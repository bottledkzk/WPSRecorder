"""Independent decode acceptance of timeline ranges, crop, captions and per-track edits."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import re
import sys
import numpy as np
sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')

p = argparse.ArgumentParser()
p.add_argument('--bin', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
a.evidence.parent.mkdir(parents=True, exist_ok=True)
b = a.bin.resolve()
cli, ffmpeg, ffprobe = [b / n for n in ['wpsrecorder_cli.exe','ffmpeg.exe','ffprobe.exe']]
results = []

def command(exe, *args, ok=True):
    r = subprocess.run([str(exe), *map(str,args)], capture_output=True, timeout=120)
    assert (r.returncode == 0) == ok, (args, r.returncode, r.stderr.decode('utf-8',errors='replace')[-4000:])
    return r

def save(case, **data):
    results.append(dict(case=case, **data))
    a.evidence.write_text(json.dumps(dict(passed=False, cases=results),ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(results[-1],ensure_ascii=False),flush=True)

def meta(path):
    return json.loads(command(ffprobe,'-v','error','-count_frames','-show_streams','-show_format','-of','json',path).stdout)

def frame(path, at, width, height):
    return np.frombuffer(command(ffmpeg,'-v','error','-ss',at,'-i',path,'-frames:v','1','-pix_fmt','rgb24','-f','rawvideo','pipe:1').stdout,np.uint8).reshape(height,width,3)

source = a.output / 'source 双音轨.mp4'
command(ffmpeg,'-v','error','-nostdin','-y','-f','lavfi','-i','testsrc2=size=640x360:rate=30:duration=6',
    '-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=6','-f','lavfi','-i','sine=frequency=880:sample_rate=48000:duration=6',
    '-map','0:v','-map','1:a','-map','2:a','-c:v','libx264','-preset','ultrafast','-pix_fmt','yuv420p','-c:a','aac',
    '-metadata:s:a:0','handler_name=mic','-metadata:s:a:1','handler_name=system',source)
original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
project = dict(version=1,input=str(source.resolve()),sourceBytes=source.stat().st_size,duration=6000,width=640,height=360,
    crop=[80,40,480,240],kept=[[1000,2500],[4000,5500]],splits=[2500,4000],audio=[
        dict(stream=0,title='mic',enabled=True,gain=1,kept=[[0,2000],[4500,6000]]),
        dict(stream=1,title='system',enabled=True,gain=.35,kept=[[0,6000]])],
    captions=[dict(begin=1500,end=4500,text='讲解重点 · Cut 01')])
plan = a.output / 'timeline 工程.json'
plan.write_text(json.dumps(project,ensure_ascii=False),encoding='utf-8')
burned, soft = a.output/'edited-burned.mp4',a.output/'edited-soft.mkv'
for path, options in [(burned,[]),(soft,['--soft-captions'])]:
    command(cli,'--edit-project','--input',plan,'--output',path,'--encoder','libx264',*options)
    m=meta(path);v=next(s for s in m['streams'] if s['codec_type']=='video')
    assert (v['width'],v['height'])==(480,240),v
    assert int(v['nb_read_frames'])==90,v
    assert abs(float(m['format']['duration'])-3)<.1,m
    assert len([s for s in m['streams'] if s['codec_type']=='audio'])==2,m
save('timeline_concat_crop',frames=90,duration=3,crop=[80,40,480,240],source_unchanged=True)

for out_time,source_time in [(.2,1.2),(2.2,4.7)]:
    expected=frame(source,source_time,640,360)[40:280,80:560].astype(float)
    got=frame(burned,out_time,480,240).astype(float)
    error=float(np.mean(np.abs(expected-got)))
    assert error<10,(out_time,error)
save('source_frame_mapping',source_times=[1.2,4.7],output_times=[.2,2.2],crop_mae_below=10)

caption=frame(burned,.8,480,240).astype(float);plain=frame(soft,.8,480,240).astype(float)
caption_diff=float(np.mean(np.abs(caption[150:230]-plain[150:230])))
assert caption_diff>1,caption_diff
subtitle=command(ffmpeg,'-v','error','-copyts','-i',soft,'-map','0:s:0','-f','srt','pipe:1').stdout.decode('utf-8')
timestamps = re.findall(r'(\d+):(\d+):(\d+),(\d+)', subtitle)
times = [int(h)*3600+int(m)*60+int(s)+int(ms)/1000 for h,m,s,ms in timestamps]
video_start = float(next(s for s in meta(soft)['streams'] if s['codec_type']=='video').get('start_time',0))
assert '讲解重点' in subtitle and len(times)==4 and max(abs(t-video_start-want) for t,want in zip(times,[.5,1.5,1.5,2]))<.003,subtitle
save('burned_and_optional_captions',pixel_diff=caption_diff,trimmed_and_remapped_srt=subtitle)

def audio(path, track):
    return np.frombuffer(command(ffmpeg,'-v','error','-i',path,'-map',f'0:a:{track}','-ac','1','-ar','48000','-f','f32le','pipe:1').stdout,np.float32)

def rms(samples,start,end):
    clip=samples[int(start*48000):int(end*48000)]
    return float(np.sqrt(np.mean(clip**2)))

mic,system=audio(burned,0),audio(burned,1)
assert rms(mic,.1,.7)>.04 and rms(mic,2.3,2.8)>.04
assert rms(mic,1.1,1.9)<.003
assert rms(system,1.1,1.9)>.015
assert .28<rms(system,.1,.7)/rms(mic,.1,.7)<.42
save('independent_audio_edits',mic_before=rms(mic,.1,.7),mic_cut=rms(mic,1.1,1.9),mic_after=rms(mic,2.3,2.8),system_during_mic_cut=rms(system,1.1,1.9))

mixed=a.output/'mixed.mp4'
command(cli,'--edit-project','--input',plan,'--output',mixed,'--encoder','libx264','--mix-tracks')
assert len([s for s in meta(mixed)['streams'] if s['codec_type']=='audio'])==1
mixed_audio=audio(mixed,0)
assert rms(mixed_audio,1.1,1.9)>.015
save('mixed_audio_export',tracks=1,signal_during_mic_cut=rms(mixed_audio,1.1,1.9))

one_track=json.loads(json.dumps(project));one_track['audio'][0]['enabled']=False
one_plan=a.output/'one-track.json';one_plan.write_text(json.dumps(one_track),encoding='utf-8')
one_output=a.output/'one-track.mp4';command(cli,'--edit-project','--input',one_plan,'--output',one_output,'--encoder','libx264')
one_audio=[s for s in meta(one_output)['streams'] if s['codec_type']=='audio']
assert len(one_audio)==1 and one_audio[0]['tags']['handler_name']=='系统声音',one_audio
one_track['audio'][1]['enabled']=False;one_plan.write_text(json.dumps(one_track),encoding='utf-8')
silent=a.output/'no-audio.mp4';command(cli,'--edit-project','--input',one_plan,'--output',silent,'--encoder','libx264')
assert not [s for s in meta(silent)['streams'] if s['codec_type']=='audio']
save('track_enable_disable',single_track_name='系统声音',all_disabled_audio_streams=0)

for invalid in [dict(crop=[80,40,481,240]),dict(kept=[]),dict(kept=[[4000,5000],[1000,2000]]),dict(captions=[dict(begin=0,end=9000,text='bad')]),dict(sourceBytes=1)]:
    bad=dict(project,**invalid);bad_path=a.output/'invalid.json';bad_path.write_text(json.dumps(bad),encoding='utf-8')
    command(cli,'--edit-project','--input',bad_path,'--output',a.output/'invalid.mp4',ok=False)
command(cli,'--edit-project','--input',plan,'--output',burned,ok=False)
assert hashlib.sha256(source.read_bytes()).hexdigest()==original_hash
save('validation_and_original_safety',invalid_plans_rejected=5,overwrite_rejected=True,source_sha256=original_hash)
a.evidence.write_text(json.dumps(dict(passed=True,cases=results),ensure_ascii=False,indent=2),encoding='utf-8')
