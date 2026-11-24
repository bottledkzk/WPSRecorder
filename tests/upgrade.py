"""V2 device, composition, scheduling, export and offline subtitle acceptance.

Media stays in --output; only JSON metrics go into --evidence.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import time
import numpy as np

p = argparse.ArgumentParser()
p.add_argument('--bin', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--speech', type=Path, required=True)
p.add_argument('--skip-camera', action='store_true', help='Explicitly leave camera acceptance pending, e.g. device access denied')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
a.evidence.mkdir(parents=True, exist_ok=True)
b = a.bin.resolve()
cli, ffmpeg, ffprobe = (b / name for name in ['wpsrecorder_cli.exe', 'ffmpeg.exe', 'ffprobe.exe'])
os.environ.pop('QT_PLUGIN_PATH', None)
results = []

def save(case, **values):
    result = dict(case=case, **values)
    results.append(result)
    (a.evidence / 'upgrade-acceptance.json').write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False), flush=True)

def command(*args, ok=True, timeout=180):
    r = subprocess.run([str(cli), *map(str, args)], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
    assert (r.returncode == 0) == ok, (args, r.returncode, r.stderr, r.stdout)
    return r

def status():
    return json.loads(command('--status').stdout)

def probe(path):
    r = subprocess.run([str(ffprobe), '-v', 'error', '-count_frames', '-show_format', '-show_streams', '-of', 'json', str(path)], capture_output=True, check=True)
    return json.loads(r.stdout)

def frame(path, at=0):
    r = subprocess.run([str(ffmpeg), '-v', 'error', '-ss', str(at), '-i', str(path), '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], capture_output=True, check=True)
    return np.frombuffer(r.stdout, np.uint8).reshape(360, 640, 3)

def preset(name, scene):
    path = a.output / (name + '.json')
    path.write_text(json.dumps(dict(version=1, scene=scene)), encoding='utf-8')
    return path

def stopped(timeout=45):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        s = status()
        if s['state'] == 'idle' and not s.get('scheduled_start'):
            assert not s.get('error') and not s.get('schedule_error'), s
            return s
        time.sleep(.15)
    raise AssertionError('Recording or scheduled task did not finish')

devices = json.loads(command('--devices').stdout)
assert devices['monitors']
save('device_enumeration', **devices)
try:
    # High frame rate and the selected physical monitor are tested against real capture.
    screen = a.output / 'screen-120.mp4'
    result = json.loads(command('--record', '--duration', '3', '--fps', '120', '--res', '640x360', '--monitor', len(devices['monitors']) - 1, '--encoder', 'h264_nvenc', '--output', screen).stdout)
    metadata = probe(screen)
    video = metadata['streams'][0]
    assert result['encoder'] == 'h264_nvenc' and video['r_frame_rate'] == '120/1'
    assert int(video['nb_read_frames']) >= 360
    save('monitor_nvenc_120fps', status=result, frames=video['nb_read_frames'], duration=metadata['format']['duration'])

    audio = a.output / 'separate-audio.mkv'
    command('--record', '--duration', '3', '--res', '640x360', '--audio', 'mix', '--separate-audio', '--denoise', '--output', audio)
    tracks = probe(audio)['streams']
    names = [s.get('tags', {}).get('title') for s in tracks if s['codec_type'] == 'audio']
    assert names == ['mic', 'system'], names
    save('independent_audio_tracks', track_titles=names, streams=len(tracks))

    # A public procedural fixture avoids testing privacy masks on private desktop data.
    image = a.output / 'fixture.png'
    subprocess.run([str(ffmpeg), '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=s=640x360', '-frames:v', '1', str(image)], check=True)
    effects = dict(masks=[dict(type='solid', rect=[.1, .1, .3, .3])], magnifier=True)
    masked = a.output / 'masked.mp4'
    mask = preset('masked', dict(source='image', image=str(image), effects=effects))
    command('--record', '--duration', '2', '--res', '640x360', '--preset', mask, '--output', masked)
    pixels = frame(masked)
    assert np.mean(np.std(pixels[45:135, 75:245].astype(float), axis=(0, 1))) < 5
    save('mask_before_encoding', mask_pixel_std=float(np.std(pixels[45:135, 75:245], axis=(0, 1)).mean()))

    live = a.output / 'live-scene.mp4'
    command('--start', '--res', '640x360', '--preset', mask, '--output', live)
    time.sleep(.7)
    white = preset('whiteboard', dict(effects=dict(whiteboard=True, shapes=[dict(type='rectangle', color='#e9424c', points=[[.2, .2], [.6, .6]], width=4)])))
    command('--apply-scene', '--preset', white)
    time.sleep(1)
    command('--stop')
    pixels = frame(live, 1.2)
    assert pixels[30:60, 30:60].mean() > 240
    invalid = preset('invalid', dict(effects=dict(masks=[dict(rect=[-1, 0, 1, 1])])))
    command('--record', '--duration', '1', '--res', '640x360', '--preset', invalid, '--output', a.output / 'invalid.mp4', ok=False)
    save('live_scene_and_whiteboard', whiteboard_mean=float(pixels[30:60, 30:60].mean()), invalid_mask_rejected=True)

    future = lambda seconds: (dt.datetime.now(dt.timezone.utc) + dt.timedelta(seconds=seconds)).isoformat(timespec='seconds')
    scheduled = a.output / 'scheduled.mp4'
    command('--schedule', '--start-at', future(6), '--max-duration', '4', '--res', '640x360', '--output', scheduled)
    assert status()['scheduled_start']
    manual = a.output / 'manual-before-schedule.mp4'
    command('--record', '--duration', '1', '--res', '640x360', '--output', manual)
    assert manual.is_file() and status()['scheduled_start']
    until = time.monotonic() + 9
    while time.monotonic() < until:
        active = status()
        if active['state'] == 'recording' and Path(active['output']).name == scheduled.name:
            break
        time.sleep(.10)
    else:
        raise AssertionError('The scheduled recording did not start')
    command('--start', '--res', '640x360', '--output', a.output / 'overlap.mp4', ok=False)
    stopped()
    assert 3.9 <= float(probe(scheduled)['format']['duration']) <= 4.2
    command('--schedule', '--start-at', future(20), '--max-duration', '2', '--output', a.output / 'cancelled.mp4')
    command('--cancel-schedule')
    assert not status().get('scheduled_start')
    command('--schedule', '--start-at', future(-5), '--max-duration', '2', '--output', a.output / 'past.mp4', ok=False)
    save('scheduled_start_stop_cancel', duration=probe(scheduled)['format']['duration'],
         manual_before_start=True, overlap_rejected=True, past_time_rejected=True)

    source = a.output / 'export-source.mp4'
    subprocess.run([str(ffmpeg), '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=s=640x360:r=30', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '3', '-c:v', 'h264_nvenc', '-c:a', 'aac', str(source)], check=True)
    exported = {}
    for ext in ['mp4', 'mov', 'mkv', 'webm', 'gif', 'm4a', 'mp3']:
        output = a.output / ('export.' + ext)
        command('--export', '--input', source, '--output', output, '--begin', '.25', '--end', '2.25', '--denoise')
        metadata = probe(output)
        exported[ext] = dict(duration=metadata['format']['duration'], codecs=[s['codec_name'] for s in metadata['streams']])
        assert 1.9 <= float(metadata['format']['duration']) <= 2.2
    command('--export', '--input', source, '--output', a.output / 'export.mp4', ok=False)
    save('seven_export_formats_and_audio_cleanup', formats=exported, overwrite_rejected=True)

    subtitles = a.output / 'speech.srt'
    command('--transcribe', '--input', a.speech, '--output', subtitles, '--language', 'en', timeout=240)
    text = subtitles.read_text(encoding='utf-8')
    assert '-->' in text and 'country' in text.lower(), text
    embedded = a.output / 'subtitled.mp4'
    command('--export', '--input', source, '--output', embedded, '--subtitles', subtitles)
    assert any(s['codec_type'] == 'subtitle' for s in probe(embedded)['streams'])
    save('offline_subtitles_and_embedding', srt_bytes=subtitles.stat().st_size, expected_word_found=True, embedded=True)

    if devices['cameras'] and not a.skip_camera:
        camera = a.output / 'camera.mp4'
        result = command('--record', '--duration', '2', '--res', '640x360', '--source', 'camera', '--camera-track', '--output', camera, timeout=45)
        tracks = probe(camera)
        separate = probe(Path(str(camera) + '.camera.mkv'))
        assert int(tracks['streams'][0]['nb_read_frames']) == int(separate['streams'][0]['nb_read_frames'])
        pip = a.output / 'pip.mp4'
        camera_style = preset('camera-style', dict(source='image', image=str(image), cameraEnabled=True, cameraShape='circle', cameraMirror=True, cameraRect=[.55, .4, .4, .5]))
        command('--record', '--duration', '2', '--res', '640x360', '--preset', camera_style, '--output', pip)
        assert int(probe(pip)['streams'][0]['nb_read_frames']) >= 60
        save('camera_pip_and_original_track', camera_frames=tracks['streams'][0]['nb_read_frames'], original_frames=separate['streams'][0]['nb_read_frames'], pip_duration=probe(pip)['format']['duration'])
finally:
    command('--stop')
    command('--cancel-schedule')
    command('--service-exit')
if a.skip_camera:
    save('camera_acceptance_pending', reason='Explicit --skip-camera; hardware access is not accepted')
save('upgrade_checks_complete', count=len(results), camera_accepted=bool(devices['cameras']) and not a.skip_camera)
