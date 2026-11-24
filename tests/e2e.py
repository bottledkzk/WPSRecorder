"""End-to-end acceptance against real capture, FFmpeg decoding and local IPC."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--bin', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--evidence', type=Path, required=True)
parser.add_argument('--long-seconds', type=int, default=600)
args = parser.parse_args()
binaries = args.bin.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
args.evidence.mkdir(parents=True, exist_ok=True)
cli = binaries / 'wpsrecorder_cli.exe'
ffprobe = binaries / 'ffprobe.exe'
ffmpeg = binaries / 'ffmpeg.exe'
results = []

def run(*arguments, success=True, timeout=180):
    process = subprocess.run([str(cli), *map(str, arguments)], capture_output=True,
                             text=True, encoding='utf-8', errors='replace', timeout=timeout)
    if success:
        assert process.returncode == 0, (arguments, process.returncode, process.stderr)
    else:
        assert process.returncode != 0 and process.stderr.strip(), (arguments, process.stdout)
    return process

def control(command):
    return json.loads(run('--' + command).stdout)

def probe(path):
    process = subprocess.run([str(ffprobe), '-v', 'error', '-count_frames', '-show_streams',
                              '-show_format', '-of', 'json', str(path)], capture_output=True, check=True)
    return json.loads(process.stdout)

def verify_video(path, minimum, maximum, width, height, audio):
    metadata = probe(path)
    duration = float(metadata['format']['duration'])
    assert minimum <= duration <= maximum, (path, duration, minimum, maximum)
    video = next(stream for stream in metadata['streams'] if stream['codec_type'] == 'video')
    assert (video['width'], video['height']) == (width, height)
    assert int(video['nb_read_frames']) > 0
    if audio:
        stream = next(stream for stream in metadata['streams'] if stream['codec_type'] == 'audio')
        assert int(stream['sample_rate']) == 48000 and stream['channels'] == 2
    return metadata

def save_result(name, **values):
    record = dict(case=name, **values)
    results.append(record)
    (args.evidence / 'acceptance.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(record), flush=True)

version = run('--version').stdout.strip()
assert version.startswith('wpsrecorder ')
idle = control('status')
for field in ['state', 'duration_sec', 'frames_written', 'avg_fps', 'mem_mb', 'cpu_pct']:
    assert field in idle
assert idle['state'] == 'idle'
for options in [('--duration', '-1'), ('--fps', '0'), ('--res', '321x241'), ('--audio', 'unknown')]:
    run('--record', '--duration', '1', '--output', output / 'invalid.mp4', *options, success=False)
run('--unknown-option', success=False)
save_result('cli_contract', version=version, idle_json=idle, invalid_input_rejected=True)

handle_path = output / 'fixture-hwnd.txt'
fixture = subprocess.Popen([str(binaries / 'recorder_qa_surface.exe'), '--tone', '--handle-file', str(handle_path)])
try:
    for _ in range(200):
        if handle_path.exists() and handle_path.read_text().strip():
            break
        time.sleep(.05)
    assert handle_path.exists(), 'QA surface did not start'
    handle = handle_path.read_text().strip()
    time.sleep(1)
    shared = ['--source', 'window', '--window', handle, '--res', '1280x720', '--fps', '30']
    sample = output / 'sample-window-system.mp4'
    start_time = time.monotonic()
    run('--start', '--output', sample, '--audio', 'system', *shared, timeout=20)
    save_result('start_returns_with_captured_pipes', seconds=time.monotonic()-start_time)
    run('--start', '--output', output / 'overlap.mp4', *shared, success=False)
    time.sleep(2)
    before_pause = control('status')
    assert before_pause['state'] == 'recording' and before_pause['frames_written'] > 0
    assert all(math.isfinite(float(before_pause[key])) for key in ['mem_mb', 'cpu_pct', 'avg_fps'])
    control('pause')
    time.sleep(.2)
    paused = control('status')
    time.sleep(2)
    still_paused = control('status')
    assert still_paused['paused'] and abs(still_paused['duration_sec'] - paused['duration_sec']) < .06
    control('resume')
    while control('status')['duration_sec'] < 8:
        time.sleep(.2)
    stopped = control('stop')
    assert stopped['state'] == 'idle' and not stopped.get('error')
    metadata = verify_video(sample, 8, 9, 1280, 720, True)
    save_result('window_system_audio_pause_resume', final=stopped,
                paused_time_delta=still_paused['duration_sec']-paused['duration_sec'], media=metadata)

    import numpy as np
    pcm = subprocess.run([str(ffmpeg), '-v', 'error', '-i', str(sample), '-vn', '-t', '5',
                          '-ar', '48000', '-ac', '2', '-f', 'f32le', 'pipe:1'], capture_output=True, check=True).stdout
    samples = np.frombuffer(pcm, dtype='<f4').reshape(-1, 2)
    rms = np.sqrt(np.mean(samples ** 2, axis=0))
    assert np.max(rms) > .001, ('Loopback capture is silent', rms.tolist())
    frequencies = np.fft.rfftfreq(len(samples), 1/48000)
    spectrum = np.abs(np.fft.rfft(samples[:, 0]))
    tone = float(spectrum[np.argmin(abs(frequencies-440))])
    assert tone > 5, ('Expected fixture tone was not found', tone)
    save_result('system_audio_signal', rms=rms.tolist(), fft_440hz=tone)

    frames = subprocess.run([str(ffmpeg), '-v', 'error', '-i', str(sample), '-vf', 'fps=1,scale=160:90',
                             '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'], capture_output=True, check=True).stdout
    images = np.frombuffer(frames, dtype='uint8').reshape(-1, 90, 160, 3)
    motion = np.abs(np.diff(images.astype(float), axis=0)).mean(axis=(1,2,3))
    assert len(images) >= 8 and max(motion) > .2
    save_result('visual_changes', sampled_frames=len(images), mean_difference=motion.tolist())

    for source in ['mic', 'mix']:
        file = output / f'audio-{source}.mp4'
        status = json.loads(run('--record', '--duration', '2', '--output', file, '--audio', source, *shared).stdout)
        metadata = verify_video(file, 2, 3, 1280, 720, True)
        save_result(f'audio_{source}', final=status, media=metadata)

    for extension in ['mov', 'avi', 'flv', 'mkv']:
        file = output / f'container.{extension}'
        run('--record', '--duration', '1', '--output', file, *shared)
        verify_video(file, 1, 2, 1280, 720, False)
        save_result('container_' + extension, playable=True)

    region = output / 'region.mp4'
    run('--record', '--duration', '2', '--output', region, '--source', 'region', '--region', '200,200,640,360',
        '--res', '640x360', '--fps', '15', '--clicks')
    verify_video(region, 2, 3, 640, 360, False)
    save_result('region_capture', playable=True)

    trimmed = output / 'trimmed.mp4'
    run('--trim', '--input', sample, '--output', trimmed, '--begin', '1', '--end', '3')
    verify_video(trimmed, 1.95, 2.1, 1280, 720, True)
    splits = output / 'split'
    run('--split', '--input', sample, '--output', splits, '--intervals', '0-1;2-3')
    for file in sorted(splits.glob('*.mp4')):
        verify_video(file, .95, 1.1, 1280, 720, True)
    assert len(list(splits.glob('*.mp4'))) == 2
    merged = output / 'merged.mp4'
    run('--merge', '--input', trimmed, '--input', region, '--output', merged)
    verify_video(merged, 3.9, 4.5, 1280, 720, True)
    run('--trim', '--input', sample, '--output', trimmed, '--begin', '0', '--end', '1', success=False)
    run('--trim', '--input', output/'missing.mp4', '--output', output/'missing-export.mp4', '--end', '1', success=False)
    save_result('editing', trim_seconds=2, split_count=2, heterogeneous_merge=True, overwrite_rejected=True)

    for index in range(3):
        file = output / f'repeated-{index}.mp4'
        run('--record', '--duration', '1', '--output', file, *shared)
        verify_video(file, 1, 2, 1280, 720, False)
    save_result('repeat_start_stop', iterations=3)

    if args.long_seconds:
        file = output / 'stability-10min.mp4'
        run('--start', '--output', file, '--res', '1280x720', '--fps', '30', '--audio', 'system')
        beginning = time.monotonic()
        snapshots = []
        with (args.evidence/'stability-status.jsonl').open('w', encoding='utf-8') as log:
            while True:
                status = control('status')
                assert status['state'] == 'recording', status
                snapshots.append(status)
                log.write(json.dumps(status)+'\n'); log.flush()
                if status['duration_sec'] >= args.long_seconds:
                    break
                time.sleep(5)
        status = control('stop')
        metadata = verify_video(file, args.long_seconds, args.long_seconds+6, 1280, 720, True)
        save_result('stability', requested_seconds=args.long_seconds, elapsed_wall=time.monotonic()-beginning,
                    samples=len(snapshots), peak_sampled_mem_mb=max(s['mem_mb'] for s in snapshots),
                    peak_sampled_cpu_pct=max(s['cpu_pct'] for s in snapshots), final=status, media=metadata)
    control('service-exit')
finally:
    fixture.terminate()
    fixture.wait(timeout=10)
