"""Verify a real desktop recording's duration, frame timestamps and audio alignment."""
import argparse
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--bin', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--seconds', type=int, default=600)
p.add_argument('--res', default='1280x720')
p.add_argument('--fps', type=int, default=30)
p.add_argument('--bitrate', type=int, default=6000)
p.add_argument('--monitor', type=int, default=0)
p.add_argument('--encoder', default='h264_nvenc')
p.add_argument('--audio', choices=('system', 'none'), default='system')
a = p.parse_args()
cli = a.bin / 'wpsrecorder_cli.exe'
a.evidence.mkdir(parents=True, exist_ok=True)

def run(*args, timeout=180):
    r = subprocess.run([str(cli), *map(str, args)], capture_output=True, text=True, encoding='utf-8', timeout=timeout)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)

started = time.monotonic()
run('--start', '--output', a.output, '--res', a.res, '--fps', a.fps, '--bitrate', a.bitrate,
    '--monitor', a.monitor, '--encoder', a.encoder, '--audio', a.audio, '--max-duration', a.seconds)
samples = []
try:
    with (a.evidence / 'nvenc-stability-status.jsonl').open('w', encoding='utf-8') as log:
        while time.monotonic() - started < a.seconds + 180:
            s = run('--status')
            samples.append(s)
            log.write(json.dumps(s) + '\n')
            log.flush()
            if s['state'] == 'idle':
                assert not s.get('error'), s
                break
            time.sleep(5)
        else:
            raise AssertionError('Recording did not finalize')
    r = subprocess.run([str(a.bin / 'ffprobe.exe'), '-v', 'error', '-count_frames', '-show_streams', '-show_format', '-of', 'json', str(a.output)], capture_output=True, check=True)
    media = json.loads(r.stdout)
    video = next(s for s in media['streams'] if s['codec_type'] == 'video')
    final = samples[-1]
    assert abs(float(video['duration']) - a.seconds) < 1 / a.fps + .005
    assert (video['width'], video['height']) == tuple(map(int, a.res.split('x')))
    assert int(video['nb_read_frames']) == final['frames_written']
    assert final['frames_written'] + final['missed_frame_slots'] == a.seconds * a.fps
    audio = [stream for stream in media['streams'] if stream['codec_type'] == 'audio']
    if a.audio == 'system':
        assert len(audio) == 1 and abs(float(video['duration']) - float(audio[0]['duration'])) < .1
    else:
        assert not audio
    packets = subprocess.check_output([str(a.bin / 'ffprobe.exe'), '-v', 'error', '-select_streams', 'v:0',
                                       '-show_entries', 'packet=pts_time', '-of', 'json', str(a.output)])
    timestamps = [float(packet['pts_time']) for packet in json.loads(packets)['packets']]
    slots = [round(timestamp * a.fps) for timestamp in timestamps]
    assert len(slots) == final['frames_written']
    assert slots[0] == 0 and slots[-1] == a.seconds * a.fps - 1
    assert all(left < right for left, right in zip(slots, slots[1:]))
    assert all(abs(timestamp - slot / a.fps) < .0015 for timestamp, slot in zip(timestamps, slots))
    result = dict(requested_seconds=a.seconds, elapsed_wall=time.monotonic()-started,
                  samples=len(samples), peak_sampled_mem_mb=max(s['mem_mb'] for s in samples),
                  peak_sampled_cpu_pct=max(s['cpu_pct'] for s in samples),
                  first_pts_sec=timestamps[0], last_pts_sec=timestamps[-1], final=final, media=media)
    (a.evidence / 'nvenc-stability.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
finally:
    run('--stop')
    run('--service-exit')
