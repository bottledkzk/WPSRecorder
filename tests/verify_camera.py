"""Decode the native camera QA recordings independently; publish metrics only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument('--bin', type=Path, required=True)
p.add_argument('--recordings', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
a = p.parse_args()
report = json.loads((a.recordings / 'camera-acceptance.json').read_text(encoding='utf-8-sig'))
assert report['complete'] and len(report['cases']) in [8, 10], report
assert report['integrity_rid'] >= 8192 and not report['app_container'], report
assert report['camera_track_collision_rejected'], report
ffmpeg, ffprobe = (a.bin / name for name in ['ffmpeg.exe', 'ffprobe.exe'])
results = []


def metadata(path):
    r = subprocess.run([str(ffprobe), '-v', 'error', '-count_frames', '-show_streams', '-show_format', '-of', 'json', str(path)], capture_output=True, check=True)
    return json.loads(r.stdout)


def frame(path, index, size):
    r = subprocess.run([str(ffmpeg), '-v', 'error', '-i', str(path), '-vf', f'select=eq(n\\,{index})', '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], capture_output=True, check=True)
    return np.frombuffer(r.stdout, np.uint8).reshape(size[1], size[0], 3)


def mae(left, right):
    return float(np.abs(left.astype(float) - right.astype(float)).mean())


def resized(pixels, size):
    return np.asarray(Image.fromarray(pixels).resize(size, Image.Resampling.BILINEAR))


def expected_pip(raw, rect, mirror):
    x, y, w, h = rect
    native_ratio = report['native_camera_width'] / report['native_camera_height']
    width, height = max(w, round(h * native_ratio)), max(h, round(w / native_ratio))
    if mirror:
        raw = raw[:, ::-1]
    scaled = resized(raw, (width, height))
    left, top = (width - w) // 2, (height - h) // 2
    return scaled[top:top + h, left:left + w]


for case in report['cases']:
    assert not case['error'], case
    if 'preview_valid' in case:
        assert case['preview_valid'] and case['preview_updates'] >= 2, case
    config = case['config']
    path = a.recordings / Path(config['output']).name
    original = Path(str(path) + '.camera.mkv')
    main_meta, raw_meta = metadata(path), metadata(original)
    main_video = next(s for s in main_meta['streams'] if s['codec_type'] == 'video')
    raw_video = next(s for s in raw_meta['streams'] if s['codec_type'] == 'video')
    count, raw_count = int(main_video['nb_read_frames']), int(raw_video['nb_read_frames'])
    assert count == raw_count == case['frames_written'], (case['case'], count, raw_count, case)
    assert (main_video['width'], main_video['height']) == (640, 360)
    assert (raw_video['width'], raw_video['height']) == (640, 480)
    assert main_video['r_frame_rate'] == raw_video['r_frame_rate'] == '30/1'
    assert len(raw_meta['streams']) == 1, raw_meta
    duration = float(main_meta['format']['duration'])
    tolerance = .25 if case['case'].startswith('cli_') else 1 / 30 + .002
    assert abs(count / 30 - config['maxDuration']) <= tolerance, case
    assert abs(duration - count / 30) < .06, main_meta
    assert abs(float(main_video.get('start_time', 0)) - float(raw_video.get('start_time', 0))) < 1 / 30 + .002
    first, later = frame(original, 2, (640, 480)), frame(original, min(count - 1, 27), (640, 480))
    change = mae(first, later)
    assert change > .2, ('Camera samples did not change', case['case'], change)
    result = dict(case=case['case'], frames=count, camera_frames=raw_count, duration=duration, temporal_change_mae=change,
                  main_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), camera_sha256=hashlib.sha256(original.read_bytes()).hexdigest())
    index = min(40, count // 2)
    main, raw = frame(path, index, (640, 360)), frame(original, index, (640, 480))
    if case['case'] in ['camera_only', 'cli_camera_only']:
        correct, incorrect = mae(main, resized(raw[:, ::-1], (640, 360))), mae(main, resized(raw, (640, 360)))
        assert correct < 18 and incorrect > correct + 2, (correct, incorrect)
        result.update(mirror_mae=correct, unmirrored_mae=incorrect)
    elif case['case'].startswith('pip_') or case['case'] == 'cli_pip_tracks':
        x, y, w, h = (round(v * d) for v, d in zip(config['cameraRect'], [640, 360, 640, 360]))
        yy, xx = np.mgrid[0:h, 0:w]
        inside = (xx > 8) & (xx < w - 9) & (yy > 8) & (yy < h - 9)
        if config['cameraShape'] == 'circle':
            side = min(w, h)
            x += (w - side) // 2
            y += (h - side) // 2
            w = h = side
            yy, xx = np.mgrid[0:h, 0:w]
            inside = (xx - w / 2) ** 2 + (yy - h / 2) ** 2 < (w / 2 - 9) ** 2
        roi = main[y:y + h, x:x + w]
        expected = expected_pip(raw, (x, y, w, h), config['cameraMirror'])
        error = mae(roi[inside], expected[inside])
        assert error < 20, (case['case'], error)
        result.update(pip_position=[x, y, w, h], pip_interior_mae=error, shape=config['cameraShape'])
        if config['cameraMirror']:
            wrong = expected_pip(raw, (x, y, w, h), False)
            wrong_error = mae(roi[inside], wrong[inside])
            assert wrong_error > error + 2, (error, wrong_error)
            result['unmirrored_pip_mae'] = wrong_error
        if config['cameraShape'] in ['circle', 'rounded']:
            # The 3px stroke and antialiasing enter the rounded corner near (4,4).
            # The extreme 2x2 corner is outside both the clipping path and stroke.
            corner = roi[:2, :2]
            assert mae(corner, np.full_like(corner, [37, 55, 79])) < 10
            result['outside_shape_corner_mae'] = mae(corner, np.full_like(corner, [37, 55, 79]))
        if config['cameraBorder'] == '#ed4e62':
            border = roi[:3, w // 4:w * 3 // 4]
            red_fraction = float(((border[:, :, 0] > 150) & (border[:, :, 1] < 140)).mean())
            assert red_fraction > .4, red_fraction
            result['red_border_fraction'] = red_fraction
        background = main[180:250, 30:45]
        if not (x < 45 and y < 250 and x + w > 30 and y + h > 180):
            assert mae(background, np.full_like(background, [37, 55, 79])) < 5
    if case['case'] in ['pip_rectangle_audio_pause', 'cli_pip_tracks']:
        audio = [s for s in main_meta['streams'] if s['codec_type'] == 'audio']
        assert [s.get('tags', {}).get('title', s.get('tags', {}).get('handler_name')) for s in audio] == ['mic', 'system'], audio
        for stream in audio:
            assert abs(float(stream['duration']) - count / 30) < .08, stream
        if case['case'] == 'pip_rectangle_audio_pause':
            assert 0 <= case['pause_delta_frames'] <= 1, case
        result.update(audio_tracks=['mic', 'system'], pause_delta_frames=case['pause_delta_frames'])
    if case['case'] == 'live_source_switch':
        fixture = np.asarray(Image.open(a.recordings / 'fixture.png').convert('RGB'))
        initial, camera, final = [frame(path, n, (640, 360)) for n in [10, 65, 130]]
        assert mae(initial, fixture) < 4
        assert mae(camera, fixture) > 8
        assert mae(final[180:250, 30:45], fixture[180:250, 30:45]) < 4
        assert mae(final[150:280, 400:540], fixture[150:280, 400:540]) > 8
        result['live_image_camera_pip_switch'] = True
    results.append(result)
    print(json.dumps(result), flush=True)
summary = dict(passed=True, integrity_rid=report['integrity_rid'], app_container=report['app_container'],
               native_camera_size=[report['native_camera_width'], report['native_camera_height']], collision_rejected=True,
               cases=results, media_in_repository=False)
a.evidence.parent.mkdir(parents=True, exist_ok=True)
a.evidence.write_text(json.dumps(summary, indent=2), encoding='utf-8')
