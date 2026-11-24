"""Read-only recorder and foreground performance sampling on Windows."""

import argparse
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time
import uuid
from contextlib import ExitStack


NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def read_status(cli):
    try:
        result = subprocess.run(
            [str(cli), "--status"], capture_output=True, encoding="utf-8",
            errors="replace", timeout=4, creationflags=NO_WINDOW,
        )
        result.check_returncode()
        status = json.loads(result.stdout)
    except subprocess.CalledProcessError as error:
        return False, {"error": error.stderr.strip() or f"CLI exit {error.returncode}"}
    except (OSError, subprocess.TimeoutExpired, ValueError) as error:
        return False, {"error": str(error)}
    # 2.2.6 returns synthetic idle JSON when the service is unreachable.
    if not isinstance(status, dict):
        return False, {"error": "Recorder status was not a JSON object"}
    return status.get("ok") is True, status


def recorder_summary(samples):
    connected = [sample["status"] for sample in samples if sample["connected"]]
    active = [status for status in connected if status.get("phase") == "recording"]
    transitions = []
    previous = None
    for sample in samples:
        current = sample["status"]
        state = current.get("phase", current.get("state")) if sample["connected"] else "unreachable"
        signature = (state, current.get("error"), current.get("capture_control_error"),
                     current.get("paused"), current.get("pipeline_stage") if state != "recording" else None)
        if signature != previous:
            transitions.append({
                "elapsed_s": sample["elapsed_s"], "phase": state, "error": signature[1],
                "capture_control_error": signature[2], "paused": signature[3],
                "active_s": current.get("duration_sec"), "pipeline_stage": current.get("pipeline_stage"),
            })
        previous = signature

    # Take deltas within a continuously observed session. Ending between polls can
    # retain the final counters in the service even when the phase has become idle.
    written = repeated = intervals = 0
    seconds = 0.0
    for a, b in zip(samples, samples[1:]):
        if not (a["connected"] and b["connected"]):
            continue
        old, new = a["status"], b["status"]
        if not old.get("output") or (old.get("pid"), old["output"]) != (new.get("pid"), new.get("output")):
            continue
        if "recording" not in (old.get("phase"), new.get("phase")):
            continue
        keys = ("frames_written", "duplicate_frames", "duration_sec")
        if any(key not in old or key not in new for key in keys):
            continue
        delta_frames, delta_dups, delta_sec = (new[key] - old[key] for key in keys)
        # The two source counters update separately. Skip inconsistent snapshots.
        if not 0 <= delta_dups <= delta_frames or delta_sec <= 0:
            continue
        written += delta_frames
        repeated += delta_dups
        seconds += delta_sec
        intervals += 1
    return {
        "connected_samples": len(connected), "active_samples": len(active),
        "unreachable_samples": len(samples) - len(connected),
        "observed_intervals": intervals, "observed_active_s": round(seconds, 3),
        "frames_written_delta": written if intervals else None,
        "scheduled_duplicate_frames_delta": repeated if intervals else None,
        "scheduled_duplicate_pct": round(100 * repeated / written, 3) if written else None,
        "non_inserted_frames_per_s": round((written - repeated) / seconds, 3) if seconds else None,
        "peak_sampled_working_set_mib": max((s["mem_mb"] for s in active if "mem_mb" in s), default=None),
        "peak_sampled_recorder_cpu_pct": max((s["cpu_pct"] for s in active if "cpu_pct" in s), default=None),
        "last_connected_status": connected[-1] if connected else None,
        "transitions": transitions,
    }


def intervals_ms(rows, column):
    values = []
    for row in rows:
        try:
            value = float(row.get(column) or "")
        except ValueError:
            continue
        if math.isfinite(value) and value > 0:
            values.append(value)
    if not values:
        return None
    values.sort()
    return {
        "samples": len(values), "median_ms": round(statistics.median(values), 3),
        "p95_ms": round(values[math.ceil(len(values) * 0.95) - 1], 3),
        "p99_ms": round(values[math.ceil(len(values) * 0.99) - 1], 3),
        "max_ms": round(values[-1], 3),
    }


def present_summary(csv_path, exit_code):
    if not csv_path.is_file():
        return {"state": "collector_failed" if exit_code else "no_frames", "exit_code": exit_code}
    chains = {}
    with csv_path.open(encoding="utf-8-sig", errors="replace", newline="") as source:
        reader = csv.DictReader(source)
        reader.fieldnames = [field.casefold() for field in reader.fieldnames or []]
        for row in reader:
            key = (row.get("application"), row.get("processid"), row.get("swapchainaddress"))
            chains.setdefault(key, []).append(row)
    if not chains:
        return {"state": "no_frames", "exit_code": exit_code}
    largest = max(chains, key=lambda key: len(chains[key]))
    selected = chains[largest]
    return {
        "state": "captured" if not exit_code else "partial", "exit_code": exit_code,
        "swap_chain_count": len(chains), "selected_application": largest[0],
        "selected_pid": largest[1], "selected_swap_chain": largest[2],
        "selected_presents": len(selected),
        "present_interval": intervals_ms(selected, "msbetweenpresents"),
        "display_interval": intervals_ms(selected, "msbetweendisplaychange"),
        "dropped_presents": sum(str(row.get("dropped", "")).lower() in {"1", "true"} for row in selected)
            if "dropped" in selected[0] else None,
    }


def finish_presentmon(process, executable, trace_name, interrupted):
    try:
        return process.wait(timeout=0 if interrupted else 4)
    except subprocess.TimeoutExpired:
        try:
            # Only stop this run's trace, including when the user presses Ctrl+C.
            subprocess.run(
                [str(executable), "--terminate_existing_session", "--session_name", trace_name],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=4, creationflags=NO_WINDOW,
            )
            return process.wait(timeout=4)
        except (OSError, subprocess.TimeoutExpired):
            process.kill()
            return process.wait(timeout=4)


def sample(args):
    cli = args.bin.resolve() / "wpsrecorder_cli.exe"
    if not cli.is_file():
        raise SystemExit(f"Recorder CLI not found at {cli}")
    presentmon = args.presentmon.resolve() if args.presentmon else None
    if presentmon and not presentmon.is_file():
        raise SystemExit(f"PresentMon executable not found at {presentmon}")

    folder = args.out.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    samples = []
    foreground = {"state": "not_requested"}
    csv_path = folder / "presentmon.csv"
    trace = f"wps-recorder-qa-{uuid.uuid4().hex}"
    interrupted = False
    with ExitStack() as files:
        status_file = files.enter_context((folder / "status.jsonl").open("w", encoding="utf-8"))
        collector = None
        if presentmon:
            trace_log = files.enter_context((folder / "presentmon.log").open("w", encoding="utf-8"))
            command = [
                str(presentmon), "--process_id", str(args.target_pid),
                "--output_file", str(csv_path), "--v1_metrics", "--no_console_stats",
                "--timed", str(args.seconds), "--terminate_after_timed", "--session_name", trace,
            ]
            try:
                collector = subprocess.Popen(
                    command, stdout=trace_log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW,
                )
            except OSError as error:
                foreground = {"state": "collector_failed", "error": str(error)}

        start = time.perf_counter()
        observed_from = utc_now()
        tick = start
        try:
            while True:
                request_start = time.perf_counter()
                connected, status = read_status(cli)
                measurement = {
                    "at_utc": utc_now(), "elapsed_s": round(request_start - start, 3),
                    "query_ms": round((time.perf_counter() - request_start) * 1000, 2),
                    "connected": connected, "status": status,
                }
                samples.append(measurement)
                status_file.write(json.dumps(measurement, ensure_ascii=False) + "\n")
                status_file.flush()
                if request_start >= start + args.seconds:
                    break
                tick += args.interval
                now = time.perf_counter()
                if tick < now:
                    tick += math.ceil((now - tick) / args.interval) * args.interval
                time.sleep(max(0, min(tick, start + args.seconds) - now))
        except KeyboardInterrupt:
            interrupted = True
        finally:
            if collector:
                exit_code = finish_presentmon(collector, presentmon, trace, interrupted)
                foreground = present_summary(csv_path, exit_code)

    result = {
        "label": args.label, "started_utc": observed_from, "requested_s": args.seconds,
        "poll_interval_s": args.interval, "interrupted": interrupted,
        "recorder": recorder_summary(samples), "foreground": foreground,
    }
    result_path = folder / "summary.json"
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(result_path)
    return 130 if interrupted else 0


def count_frames(ffmpeg, media, retain_filter):
    command = [str(ffmpeg), "-hide_banner", "-nostdin", "-v", "error", "-xerror", "-i", str(media),
               "-map", "0:v:0", "-an", "-sn", "-dn"]
    if retain_filter:
        command += ["-vf", "scale=320:-2:flags=fast_bilinear,mpdecimate"]
    command += ["-fps_mode", "passthrough", "-progress", "pipe:1", "-f", "null", "-"]
    result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    frames = [int(line.split("=", 1)[1]) for line in result.stdout.splitlines() if line.startswith("frame=")]
    if not frames or frames[-1] <= 0:
        raise RuntimeError("No video frames were decoded")
    return frames[-1]


def analyze(args):
    bin_path = args.bin.resolve()
    ffmpeg, ffprobe = (bin_path / tool for tool in ("ffmpeg.exe", "ffprobe.exe"))
    media = args.media.resolve()
    info = subprocess.check_output(
        [str(ffprobe), "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=duration:format=duration", "-of", "json", str(media)],
        encoding="utf-8", creationflags=NO_WINDOW,
    )
    metadata = json.loads(info)
    duration = None
    for stream in metadata.get("streams", []) + [metadata.get("format", {})]:
        try:
            value = float(stream.get("duration", 0))
        except ValueError:
            continue
        if math.isfinite(value) and value > 0:
            duration = value
            break
    total = count_frames(ffmpeg, media, retain_filter=False)
    visually_changed = count_frames(ffmpeg, media, retain_filter=True)
    result = {
        "media": str(media), "bytes": media.stat().st_size, "duration_s": duration, "decoded_frames": total,
        "visual_change_proxy_frames": visually_changed,
        "visual_change_proxy_pct": round(100 * visually_changed / total, 3),
        "visual_change_proxy_fps": round(visually_changed / duration, 3) if duration else None,
        "method": "FFmpeg mpdecimate with original timestamps and 320px downscale. Static content and subtle changes both affect this proxy.",
    }
    if args.out:
        with args.out.open("x", encoding="utf-8") as output:
            json.dump(result, output, ensure_ascii=False, indent=2)
            output.write("\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    monitor = commands.add_parser("sample", help="Sample recorder status and optional PresentMon frame intervals")
    monitor.add_argument("--bin", type=Path, required=True, help="directory containing wpsrecorder_cli.exe")
    monitor.add_argument("--out", type=Path, required=True, help="new evidence directory")
    monitor.add_argument("--seconds", type=int, default=60)
    monitor.add_argument("--interval", type=float, default=1.0, help="recorder polling interval in seconds")
    monitor.add_argument("--label", default="")
    monitor.add_argument("--presentmon", type=Path, help="Intel PresentMon 2.x console executable")
    monitor.add_argument("--target-pid", type=int, help="foreground app or game process ID")
    offline = commands.add_parser("analyze", help="Count decoded and visually changing frames after recording")
    offline.add_argument("--bin", type=Path, required=True, help="directory containing ffmpeg.exe and ffprobe.exe")
    offline.add_argument("--media", type=Path, required=True)
    offline.add_argument("--out", type=Path, help="new JSON evidence file")
    args = parser.parse_args()
    if args.command == "analyze":
        return analyze(args)
    if os.name != "nt":
        parser.error("Recorder sampling needs Windows")
    if args.seconds <= 0 or not math.isfinite(args.interval) or args.interval < 0.5:
        parser.error("--seconds must be positive and --interval at least 0.5 seconds")
    if bool(args.presentmon) != (args.target_pid is not None) or (args.target_pid is not None and args.target_pid <= 0):
        parser.error("--presentmon and a positive --target-pid must be supplied together")
    return sample(args)


if __name__ == "__main__":
    raise SystemExit(main())
