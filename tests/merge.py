"""Check CLI merge resolution, stream-copy packets, and mixed input handling."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def run(executable, *arguments):
    result = subprocess.run(
        [str(executable), *map(str, arguments)], capture_output=True, timeout=90,
    )
    return result


def checked(executable, *arguments):
    result = run(executable, *arguments)
    assert result.returncode == 0, (arguments, result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def make_clip(ffmpeg, path, color, size, frequency=None):
    arguments = ["-v", "error", "-nostdin", "-f", "lavfi",
                 "-i", f"color=c={color}:s={size}:r=30:d=0.5"]
    if frequency is not None:
        arguments += ["-f", "lavfi", "-i",
                      f"sine=frequency={frequency}:sample_rate=48000:duration=0.5",
                      "-c:a", "aac", "-shortest"]
    checked(ffmpeg, *arguments, "-c:v", "libx264", "-preset", "ultrafast",
            "-pix_fmt", "yuv420p", path)


def streams(ffprobe, path, *flags):
    return json.loads(checked(ffprobe, "-v", "error", *flags,
                              "-show_streams", "-of", "json", path))["streams"]


def verify_video(ffmpeg, ffprobe, path, size, frames=30):
    tracks = streams(ffprobe, path, "-count_frames")
    video = next(stream for stream in tracks if stream["codec_type"] == "video")
    assert (video["width"], video["height"]) == size, video
    assert int(video["nb_read_frames"]) == frames, video
    checked(ffmpeg, "-v", "error", "-nostdin", "-xerror", "-i", path, "-f", "null", "-")
    return [stream for stream in tracks if stream["codec_type"] == "audio"]


def packet_hashes(ffprobe, path, stream):
    data = json.loads(checked(ffprobe, "-v", "error", "-select_streams", stream,
                              "-show_packets", "-show_entries", "packet=data_hash",
                              "-show_data_hash", "sha256", "-of", "json", path))
    return [packet["data_hash"] for packet in data["packets"]]


def pixel(ffmpeg, path, time, x, y):
    data = checked(ffmpeg, "-v", "error", "-ss", time, "-i", path, "-frames:v", 1,
                   "-vf", f"crop=2:2:{x}:{y}", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1")
    assert len(data) == 12
    return tuple(data[:3])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    cli, ffmpeg, ffprobe = [args.bin.resolve() / name for name in
                           ("wpsrecorder_cli.exe", "ffmpeg.exe", "ffprobe.exe")]

    first, second, quiet = [output / name for name in
                            ("first 1080p.mp4", "第二段 '1080p'.mp4", "无声 4x3.mp4")]
    make_clip(ffmpeg, first, "red", "1920x1080", 440)
    make_clip(ffmpeg, second, "blue", "1920x1080", 660)
    make_clip(ffmpeg, quiet, "green", "640x480")
    source_hashes = {path: hashlib.sha256(path.read_bytes()).digest() for path in (first, second, quiet)}
    cases = {}

    def merge(name, one, two, *flags):
        target = output / f"{name}.mp4"
        checked(cli, "--merge", "--input", one, "--input", two,
                "--output", target, "--encoder", "libx264", *flags)
        return target

    native = merge("default_1080p", first, second)
    assert len(verify_video(ffmpeg, ffprobe, native, (1920, 1080))) == 1
    red, blue = pixel(ffmpeg, native, "0.25", 960, 540), pixel(ffmpeg, native, "0.75", 960, 540)
    assert red[0] > 200 and red[2] < 32 and blue[2] > 200 and blue[0] < 32
    cases["default_1080p"] = {"size": [1920, 1080], "frames": 30, "input_order_verified": True}

    small = merge("default_first_canvas", quiet, first)
    assert len(verify_video(ffmpeg, ffprobe, small, (640, 480))) == 1
    cases["default_first_canvas"] = {"size": [640, 480]}

    explicit = merge("explicit_1080p", quiet, second, "--res", "1920x1080", "--fps", "60", "--bitrate", "12000")
    audio, = verify_video(ffmpeg, ffprobe, explicit, (1920, 1080), 60)
    assert audio["channels"] == 2 and audio["sample_rate"] == "48000", audio
    assert max(pixel(ffmpeg, explicit, "0.25", 10, 540)) < 10
    assert pixel(ffmpeg, explicit, "0.25", 960, 540)[1] > 80
    silence = checked(ffmpeg, "-v", "error", "-ss", "0.1", "-i", explicit, "-t", "0.2",
                      "-vn", "-f", "s16le", "pipe:1")
    assert silence and not any(silence), "Missing-audio segment was not silent"
    cases["explicit_1080p"] = {"size": [1920, 1080], "frames": 60, "letterbox": True, "silence": True}

    rotated = output / "rotated.mp4"
    checked(ffmpeg, "-v", "error", "-display_rotation", "90", "-i", quiet, "-c", "copy", rotated)
    portrait = merge("first_display_orientation", rotated, rotated)
    verify_video(ffmpeg, ffprobe, portrait, (480, 640))
    cases["first_display_orientation"] = {"size": [480, 640]}
    rotated_copy = merge("copy_display_orientation", rotated, rotated, "--copy-streams", "--res", "480x640")
    assert packet_hashes(ffprobe, rotated_copy, "v:0") == packet_hashes(ffprobe, rotated, "v:0") * 2
    assert any(abs(item.get("rotation", 0)) == 90 for item in streams(ffprobe, rotated_copy)[0].get("side_data_list", []))
    cases["copy_display_orientation"] = {"encoded_size": [640, 480], "display_size": [480, 640]}

    with_two_tracks = output / "two-tracks.mp4"
    checked(ffmpeg, "-v", "error", "-i", quiet,
            "-f", "lavfi", "-i", "sine=523:sample_rate=48000:duration=1.1",
            "-f", "lavfi", "-i", "sine=870:sample_rate=48000:duration=1.1",
            "-map", "0:v", "-map", "1:a", "-map", "2:a", "-c:v", "copy", "-c:a", "aac", with_two_tracks)
    multi = merge("two_tracks_with_gap", with_two_tracks, quiet)
    assert len(verify_video(ffmpeg, ffprobe, multi, (640, 480))) == 2
    for audio_index in (0, 1):
        track = f"0:a:{audio_index}"
        audible = checked(ffmpeg, "-v", "error", "-ss", "0.1", "-i", multi, "-t", "0.2",
                          "-map", track, "-f", "s16le", "pipe:1")
        silent = checked(ffmpeg, "-v", "error", "-ss", "0.65", "-i", multi, "-t", "0.2",
                         "-map", track, "-f", "s16le", "pipe:1")
        assert any(audible) and silent and not any(silent), track
    cases["two_tracks_with_gap"] = {"tracks": 2, "silent_second_clip_on_both_tracks": True}

    mute = merge("no_audio_streams", quiet, quiet)
    assert not verify_video(ffmpeg, ffprobe, mute, (640, 480))
    cases["no_audio_streams"] = {"tracks": 0}

    motion = output / "motion-60.mp4"
    checked(ffmpeg, "-v", "error", "-f", "lavfi", "-i",
            "testsrc2=s=320x180:r=60:d=1,noise=alls=40:allf=t+u:all_seed=77",
            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "12", motion)
    inherited = merge("inherit_60", motion, motion)
    verify_video(ffmpeg, ffprobe, inherited, (320, 180), 120)
    assert streams(ffprobe, inherited)[0]["avg_frame_rate"] == "60/1"
    low = merge("low_rate", motion, motion, "--fps", "60", "--bitrate", "300")
    high = merge("high_rate", motion, motion, "--fps", "60", "--bitrate", "12000")
    verify_video(ffmpeg, ffprobe, high, (320, 180), 120)
    low_rate = int(streams(ffprobe, low)[0]["bit_rate"])
    high_rate = int(streams(ffprobe, high)[0]["bit_rate"])
    assert high_rate > 3 * low_rate, (low_rate, high_rate)
    cases["fps_and_bitrate"] = {"inherited_fps": 60, "explicit_fps": 60,
                                "requested_kbps": [300, 12000], "measured_bps": [low_rate, high_rate]}

    srt = output / "merge.srt"
    srt.write_text("1\n00:00:00,100 --> 00:00:00,400\nMerge test\n", encoding="utf-8")
    with_srt = merge("merged_subtitles_and_denoise", first, second, "--subtitles", srt, "--denoise")
    assert [s["codec_type"] for s in streams(ffprobe, with_srt)] == ["video", "audio", "subtitle"]
    cases["merged_subtitles_and_denoise"] = {"subtitle_stream": "mov_text"}

    copied = merge("copy_1080p", first, second, "--copy-streams", "--res", "1920x1080")
    verify_video(ffmpeg, ffprobe, copied, (1920, 1080))
    copied_packets = {}
    for stream in ("v:0", "a:0"):
        expected = packet_hashes(ffprobe, first, stream) + packet_hashes(ffprobe, second, stream)
        assert expected and packet_hashes(ffprobe, copied, stream) == expected, stream
        copied_packets[stream] = len(expected)
    cases["copy_1080p"] = {"size": [1920, 1080], "unchanged_encoded_packets": copied_packets}

    for label, one, two, flags, reason in (
        ("copy_mixed", first, quiet, ["--copy-streams"], "matching input streams"),
        ("copy_resize", first, second, ["--copy-streams", "--res", "1280x720"], "cannot change resolution"),
        ("copy_denoise", first, second, ["--copy-streams", "--denoise"], "cannot apply audio cleanup"),
        ("odd_size", first, second, ["--res", "1920x1079"], "positive even WIDTHxHEIGHT"),
        ("invalid_size", first, second, ["--res", "1080p"], "positive even WIDTHxHEIGHT"),
        ("oversize_canvas", first, second, ["--res", "20000x12000"], "exceeds the supported"),
        ("invalid_fps", first, second, ["--fps", "foo"], "Invalid --fps"),
        ("copy_rate", first, second, ["--copy-streams", "--bitrate", "12000"], "change bitrate"),
        ("copy_fps", first, second, ["--copy-streams", "--fps", "60"], "cannot change the source frame rate"),
    ):
        rejected = output / f"{label}.mp4"
        result = run(cli, "--merge", "--input", one, "--input", two, "--output", rejected, *flags)
        assert result.returncode != 0 and reason in result.stderr.decode("utf-8", errors="replace"), result
        assert not rejected.exists(), rejected
        cases[label] = {"rejected": True}

    native_hash = hashlib.sha256(native.read_bytes()).digest()
    overwrite = run(cli, "--merge", "--input", first, "--input", second, "--output", native)
    assert overwrite.returncode != 0 and b"Output already exists" in overwrite.stderr
    assert hashlib.sha256(native.read_bytes()).digest() == native_hash
    assert all(hashlib.sha256(path.read_bytes()).digest() == expected for path, expected in source_hashes.items())
    assert not list(output.glob(".merge-*.mp4"))
    cases["originals_and_output_preserved"] = {"overwrite_rejected": True}

    trimmed = output / "trimmed.mp4"
    checked(cli, "--trim", "--input", first, "--output", trimmed, "--begin", 0, "--end", "0.3")
    verify_video(ffmpeg, ffprobe, trimmed, (1920, 1080), frames=9)
    cases["trim_1080p_regression"] = {"size": [1920, 1080], "frames": 9}
    for command, flags, reason in (
        ("--trim", ["--begin", "foo"], "non-negative --begin"),
        ("--trim", ["--end", "nan"], "non-negative --begin"),
        ("--split", ["--intervals", "0-0.2"], "requires a directory path"),
    ):
        target = output / (command[2:] + ".mp4")
        result = run(cli, command, "--input", first, "--output", target, *flags)
        assert result.returncode and reason in result.stderr.decode("utf-8", errors="replace") and not target.exists()
    wav = output / "export.wav"
    checked(cli, "--export", "--input", first, "--output", wav)
    audio, = streams(ffprobe, wav)
    assert audio["codec_type"] == "audio" and audio["codec_name"] == "pcm_s16le"
    cases["cli_validation_and_wav"] = {"invalid_intervals_rejected": True, "wav_codec": audio["codec_name"]}

    result = {"version": checked(cli, "--version").decode().strip(), "passed": True, "cases": cases}
    (output / "merge-check.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
