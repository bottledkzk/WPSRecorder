"""Decode a timeline that combines stills, captions and external sound."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("--bin", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
bin_dir = args.bin.resolve()


def run(exe, *argv, success=True):
    result = subprocess.run([str(bin_dir / (exe + ".exe")), *map(str, argv)],
                            capture_output=True, timeout=120)
    assert (result.returncode == 0) == success, (argv, result.stderr.decode(errors="replace"))
    return result.stdout


source, audio, image = [root / n for n in ["source.mp4", "added-tone.wav", "local-picture.png"]]
run("ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=5",
    "-c:v", "libx264", "-pix_fmt", "yuv420p", source)
run("ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=f=660:r=48000:d=3.5", audio)
run("ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=red:s=256x144",
    "-frames:v", 1, "-threads", 1, image)
hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in [source, audio, image]]
plan = dict(version=1, input=str(source), sourceBytes=source.stat().st_size, duration=5000,
            width=640, height=360, crop=[64, 36, 512, 288], cropRatio=[16, 9],
            kept=[[0, 5000]], splits=[], captions=[dict(begin=1500, end=3500, text="Styled subtitle")],
            subtitleStyle=dict(font="Arial", pixels=40, color="#c000ff20"),
            audio=[dict(stream=-1, title="External", enabled=True, gain=.7,
                        file=str(audio), offset=500, length=3500, kept=[[800, 1600], [2600, 3800]])],
            stills=[dict(at=0, length=1000, image="", caption="Opening card"),
                    dict(at=2500, length=1500, image=str(image), caption="Local image")])
project, hard, soft = [root / n for n in ["project.json", "hard.mp4", "soft.mkv"]]
project.write_text(json.dumps(plan), encoding="utf-8")
for output, options in [(hard, []), (soft, ["--soft-captions"])]:
    run("wpsrecorder_cli", "--edit-project", "--input", project, "--output", output,
        "--encoder", "libx264", *options)
    run("ffmpeg", "-v", "error", "-i", output, "-f", "null", "-")
meta = json.loads(run("ffprobe", "-v", "error", "-count_frames", "-show_streams",
                      "-show_format", "-of", "json", hard))
video, sound = meta["streams"]
assert (video["width"], video["height"], int(video["nb_read_frames"])) == (512, 288, 225), video
assert abs(float(meta["format"]["duration"]) - 7.5) < .06, meta["format"]
assert sound["codec_type"] == "audio", sound


def frame(path, seconds):
    return np.frombuffer(run("ffmpeg", "-v", "error", "-ss", seconds, "-i", path, "-frames:v", 1,
                             "-pix_fmt", "rgb24", "-f", "rawvideo", "-"), np.uint8).reshape(288, 512, 3)


art = frame(soft, .35).astype(np.int16)
still_asset = Path(__file__).resolve().parents[1] / "src" / "assets" / "intermission.png"
expected_art = np.frombuffer(run("ffmpeg", "-v", "error", "-i", still_asset,
                                "-vf", "scale=512:288:force_original_aspect_ratio=decrease:force_divisible_by=2,"
                                       "pad=512:288:(ow-iw)/2:(oh-ih)/2",
                                "-frames:v", 1, "-pix_fmt", "rgb24", "-f", "rawvideo", "-"),
                             np.uint8).reshape(288, 512, 3).astype(np.int16)
art_detail = np.ptp(expected_art, axis=2) > 20
mean_art_error = float(np.abs(art[art_detail] - expected_art[art_detail]).mean())
picture = frame(hard, 4.0)
assert art_detail.sum() > 100 and mean_art_error < 8, ("Bundled still differs from artwork", mean_art_error)
assert picture[30, 100, 0] > 210 and max(picture[30, 100, 1:]) < 40, "Imported still missing"
cap = frame(hard, 2.75).astype(int)
no_cap = frame(soft, 2.75).astype(int)
green_delta = int(((cap[:, :, 1] - no_cap[:, :, 1] > 25) &
                   (cap[:, :, 1] - cap[:, :, 0] > 50)).sum())
assert green_delta > 100, green_delta
samples = np.frombuffer(run("ffmpeg", "-v", "error", "-i", hard, "-map", "0:a:0", "-ac", 1,
                           "-ar", 48000, "-f", "f32le", "-"), np.float32)


def rms(a, b):
    return float(np.sqrt(np.mean(samples[round(a * 48000):round(b * 48000)] ** 2)))


measurements = [rms(a, b) for a, b in [(.2, .8), (2, 2.4), (2.75, 3.25), (3.8, 4.7), (5.3, 6.1)]]
assert all(measurements[i] < .004 for i in [0, 2, 3]) and min(measurements[1], measurements[4]) > .03, measurements
subs = run("ffmpeg", "-v", "error", "-i", soft, "-map", "0:s:0", "-f", "srt", "-").decode()
assert all(s in subs for s in ["Opening card", "Styled subtitle", "Local image"]), subs
assert [hashlib.sha256(path.read_bytes()).hexdigest() for path in [source, audio, image]] == hashes
evidence = dict(passed=True, frames=225, duration=7.5, green_caption_pixels=green_delta, default_still_rgb_error=mean_art_error,
                external_track_rms=measurements, soft_subtitles=subs, sources_unchanged=True)
(root / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
print(json.dumps(evidence))
