"""Real offline synthesis/transcription and muxing checks; no personal recordings."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import numpy as np

sys.stdout.reconfigure(encoding='utf-8')
p = argparse.ArgumentParser()
p.add_argument('--bin', type=Path, required=True)
p.add_argument('--public-speech', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
a = p.parse_args()
b = a.bin.resolve()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
results = []


def command(exe, *args, success=True, timeout=300):
    r = subprocess.run([str(b / exe), *map(str, args)], capture_output=True, timeout=timeout)
    assert (r.returncode == 0) == success, (args, r.returncode, r.stderr.decode('utf-8', 'replace')[-2000:])
    return r


def record(name, **details):
    results.append(dict(case=name, passed=True, **details))
    a.evidence.parent.mkdir(parents=True, exist_ok=True)
    a.evidence.write_text(json.dumps(dict(passed=True, cases=results), ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(results[-1], ensure_ascii=False), flush=True)


def metadata(path):
    return json.loads(command('ffprobe.exe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', path).stdout)


def samples(path):
    return np.frombuffer(command('ffmpeg.exe', '-v', 'error', '-i', path, '-map', '0:a:0', '-ac', '1', '-ar', '16000', '-f', 'f32le', '-').stdout, np.float32)


def rms(values):
    return float(np.sqrt(np.mean(values * values)))


def synth(name, text, voice='default', speed=1, volume=100):
    source = out / (name + '.txt')
    source.write_text(text, encoding='utf-8')
    wav = out / (name + '.wav')
    start = time.monotonic()
    command('wpsrecorder_cli.exe', '--synthesize', '--text-file', source, '--output', wav,
            '--tts-engine', 'audio8', '--voice', voice, '--speed', speed, '--voice-volume', volume)
    m = metadata(wav)
    sound = samples(wav)
    assert m['streams'][0]['codec_name'] == 'pcm_s16le'
    assert m['streams'][0]['sample_rate'] == '48000'
    assert np.isfinite(sound).all() and rms(sound) > .002 and 1 < len(sound) / 16000 < 30
    return wav, dict(duration=float(m['format']['duration']), rms=rms(sound),
                     peak=float(np.max(np.abs(sound))), wall_seconds=round(time.monotonic() - start, 3))


voices = json.loads(command('wpsrecorder_cli.exe', '--voices').stdout)
assert voices['audio8_available'] and voices['audio8'] == ['default', 'mandarin-female']
zh = '欢迎使用屏幕录制工具。现在我们添加字幕，并为视频生成本地旁白。'
en = 'Welcome to the screen recorder. We can add subtitles and create an offline narration.'
normal, normal_m = synth('audio8-zh', zh)
english, en_m = synth('audio8-en', en)
female, female_m = synth('audio8-mandarin-female', zh, voice='mandarin-female')
assert hashlib.sha256(normal.read_bytes()).digest() != hashlib.sha256(female.read_bytes()).digest()
record('audio8_chinese_english_and_voices', chinese=normal_m, english=en_m, mandarin_female=female_m)
fast, fast_m = synth('audio8-zh-fast', zh, speed=1.5)
quiet, quiet_m = synth('audio8-zh-quiet', zh, volume=35)
assert fast_m['duration'] < normal_m['duration'] * .85, (fast_m, normal_m)
assert quiet_m['rms'] < normal_m['rms'] * .6
record('voice_speed_volume', normal=normal_m, faster=fast_m, quieter=quiet_m)

source = out / 'original-dual-audio.mp4'
command('ffmpeg.exe', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=s=640x360:r=30:d=20',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=20',
        '-f', 'lavfi', '-i', 'sine=frequency=880:sample_rate=48000:duration=20',
        '-map', '0:v', '-map', '1:a', '-map', '2:a', '-c:v', 'libx264', '-preset', 'ultrafast', '-c:a', 'aac', source)
original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
mixed, replaced = out / 'narrated.mp4', out / 'voice-only.mp4'
for target, volume in [(mixed, 25), (replaced, 0)]:
    command('wpsrecorder_cli.exe', '--narrate', '--input', source, '--speech', normal,
            '--output', target, '--begin', 2, '--original-volume', volume)
    m = metadata(target)
    assert abs(float(m['format']['duration']) - 20) < .06
    assert len([s for s in m['streams'] if s['codec_type'] == 'audio']) == 1
def video_hash(path):
    return command('ffmpeg.exe', '-v', 'error', '-i', path, '-map', '0:v:0', '-c', 'copy', '-f', 'hash', '-hash', 'sha256', '-').stdout.strip()
assert video_hash(source) == video_hash(mixed) == video_hash(replaced)
sound = samples(replaced)
assert rms(sound[16000:28000]) < .0001 and rms(sound[40000:60000]) > .005
assert rms(samples(mixed)[16000:28000]) > .005
record('narration_mix_alignment_and_stream_copy', begin_seconds=2, output_seconds=20,
       silent_before_voice=rms(sound[16000:28000]), voice_signal=rms(sound[40000:60000]), video_packets_unchanged=True)

command('wpsrecorder_cli.exe', '--narrate', '--input', source, '--speech', normal,
        '--output', out / 'overrun.mp4', '--begin', 19, success=False)
command('wpsrecorder_cli.exe', '--synthesize', '--text-file', out / 'audio8-zh.txt', '--output', normal, success=False)
command('wpsrecorder_cli.exe', '--synthesize', '--text-file', out / 'audio8-zh.txt', '--output', out / 'bad.wav', '--speed', 0, success=False)
assert hashlib.sha256(source.read_bytes()).hexdigest() == original_hash
record('speech_output_safety', original_sha256=original_hash, overrun_rejected=True, overwrite_rejected=True, bad_speed_rejected=True)

asr = []
for lang, phrase in [('zh', '开放时间'), ('en', 'gold'), ('ja', '中学')]:
    sample = a.public_speech / (lang + '.mp3')
    video = out / ('public-' + lang + '.mp4')
    command('ffmpeg.exe', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=0x23324a:s=640x360:r=30', '-i', sample,
            '-shortest', '-c:v', 'libx264', '-preset', 'ultrafast', '-c:a', 'aac', video)
    target = out / ('public-' + lang + '.srt')
    start = time.monotonic()
    command('wpsrecorder_cli.exe', '--transcribe', '--input', video, '--output', target,
            '--model', b / 'whisper/ggml-small-q5_1.bin', '--language', lang)
    text = target.read_text(encoding='utf-8')
    assert phrase in text and re.search(r'\d\d:\d\d:\d\d,\d{3} --> ', text)
    asr.append(dict(language=lang, source_sha256=hashlib.sha256(sample.read_bytes()).hexdigest(),
                    seconds=round(time.monotonic() - start, 3), expected_keyword=phrase, srt=text))
record('public_multilingual_asr_smoke', model='small-q5_1', samples=asr,
       boundary='Three short public examples and keyword checks; not a corpus accuracy benchmark.')
