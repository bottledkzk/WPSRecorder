"""Verify a course submission ZIP and its extracted portable files."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile


def digest(stream):
    checksum = hashlib.sha256()
    size = 0
    while chunk := stream.read(4 * 1024 * 1024):
        checksum.update(chunk)
        size += len(chunk)
    return size, checksum.hexdigest()


parser = argparse.ArgumentParser()
parser.add_argument("--archive", type=Path, required=True)
parser.add_argument("--runtime", "--installed", type=Path, required=True, help="extracted exe_cli directory")
parser.add_argument("--manifest", type=Path, help="SHA-256 sidecar, defaults to manifest.json next to the ZIP")
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--expected-version")
parser.add_argument("--expected-sample-sha256")
args = parser.parse_args()
if not __debug__:
    parser.error("run package verification without Python optimization")

runtime = args.runtime.resolve(strict=True)
package = runtime.parent
assert runtime.name == "exe_cli", runtime
manifest_path = args.manifest or args.archive.with_name("manifest.json")
manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
expected = {row["path"].replace("\\", "/"): (row["bytes"], row["sha256"].lower()) for row in manifest}
assert len(expected) == len(manifest), "Duplicate entries in hash manifest"

roots = {"exe_cli", "sample"}
forbidden_names = {"settings.ini", "ui-settings.ini", "speech-services.ini", "credentials.json", ".env",
                   "install.cmd", "install.ps1", "uninstall.ps1"}
with zipfile.ZipFile(args.archive) as archive:
    members = archive.infolist()
    names = [item.filename.rstrip("/") for item in members]
    assert {name.split("/", 1)[0] for name in names} == roots, "ZIP must have only exe_cli/ and sample/ at its root"
    assert len(set(name.casefold() for name in names)) == len(names), "Duplicate ZIP member"
    for name in names:
        assert "\\" not in name and ":" not in name and not {"", ".", ".."}.intersection(name.split("/")), name
        basename = name.rsplit("/", 1)[-1].casefold()
        assert basename not in forbidden_names and not re.fullmatch(r"recorder_.*\.exe", basename), name
        assert "kokoro" not in name.casefold(), name
    entries = {item.filename: item for item in members if not item.is_dir()}
    assert set(entries) == set(expected), set(entries) ^ set(expected)
    extracted = {file.relative_to(package).as_posix() for root in roots for file in (package / root).rglob("*") if file.is_file()}
    assert extracted == set(entries), extracted ^ set(entries)
    for name, item in entries.items():
        with archive.open(item) as stored, (package / name).open("rb") as unpacked:
            assert item.file_size == expected[name][0], name
            assert digest(stored) == expected[name] == digest(unpacked), name

required_runtime = {"wpsrecorder.exe", "wpsrecorder_cli.exe", "ffmpeg.exe", "ffprobe.exe",
                    "platforms/qwindows.dll", "studio/index.html"}
assert required_runtime <= {name.removeprefix("exe_cli/") for name in entries}, "Incomplete portable runtime"
sample_names = sorted(name for name in entries if name.startswith("sample/"))
video_suffixes = {".mp4", ".mkv", ".mov", ".avi", ".flv", ".webm", ".m4v"}
assert sample_names and all(Path(name).suffix.casefold() in video_suffixes for name in sample_names), sample_names
if args.expected_sample_sha256:
    assert len(sample_names) == 1 and expected[sample_names[0]][1] == args.expected_sample_sha256.lower(), (
        "Packaged sample differs from requested recording")

env = {key: value for key, value in os.environ.items() if not key.upper().startswith(("WPSRECORDER_", "QT_", "PYTHON"))}
env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")


def run_tool(tool, *arguments, timeout=20):
    return subprocess.run([str(runtime / tool), *map(str, arguments)], cwd=runtime, env=env,
                          capture_output=True, text=True, encoding="utf-8", check=True, timeout=timeout).stdout.strip()


with tempfile.TemporaryDirectory(prefix="wpsrecorder-package-qa-") as data:
    env["WPSRECORDER_DATA_DIR"] = data
    version = run_tool("wpsrecorder_cli.exe", "--version")
    assert re.fullmatch(r"wpsrecorder \d+\.\d+\.\d+", version), version
    if args.expected_version:
        assert version == "wpsrecorder " + args.expected_version, version
    voices = json.loads(run_tool("wpsrecorder_cli.exe", "--voices"))
    if any(name.startswith("exe_cli/tts/audio8/") for name in entries):
        assert voices["audio8_available"] and "mandarin-female" in voices["audio8"], voices
    if "exe_cli/whisper/whisper-cli.exe" in entries:
        assert "exe_cli/whisper/ggml-base.bin" in entries, "Missing bundled base ASR model"
        run_tool("whisper/whisper-cli.exe", "--help")
    durations = {}
    for name in sample_names:
        sample = package / name
        metadata = json.loads(run_tool("ffprobe.exe", "-v", "error", "-select_streams", "v:0",
                                       "-show_entries", "stream=codec_type:format=duration", "-of", "json", sample))
        duration = float(metadata["format"]["duration"])
        assert metadata.get("streams") == [{"codec_type": "video"}] and 0 < duration <= 180, name
        run_tool("ffmpeg.exe", "-nostdin", "-v", "error", "-xerror", "-i", sample, "-f", "null", "-", timeout=180)
        durations[name] = duration

with args.archive.open("rb") as encoded:
    archive_size, archive_sha = digest(encoded)
report = dict(passed=True, zip_crc=True, roots=sorted(roots), verified_files=len(entries),
              archive_size=archive_size, archive_sha256=archive_sha, app_sha256=expected["exe_cli/wpsrecorder.exe"][1],
              version=version, media_files=sample_names, sample_duration_sec=durations, sample_full_decode=True,
              sample_sha256={name: expected[name][1] for name in sample_names}, audio8_voices=voices["audio8"],
              system_path_only=True, qa_programs_in_package=False, personal_settings_in_package=False)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps(report, ensure_ascii=False))
