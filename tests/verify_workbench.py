"""Independently decode camera placement before/after the native UI drag."""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np
from PIL import Image
p=argparse.ArgumentParser()
p.add_argument('--bin',type=Path,required=True)
p.add_argument('--report',type=Path,required=True)
p.add_argument('--camera-report',type=Path,required=True)
p.add_argument('--evidence',type=Path,required=True)
a=p.parse_args()
r=json.loads(a.report.read_text(encoding='utf-8'))
assert r['passed']
source=Path(next(c['output'] for c in r['cases'] if c['case']=='floating_stop_auto_preview'))
camera=Path(str(source)+'.camera.mkv')
native=json.loads(a.camera_report.read_text(encoding='utf-8'))
ratio=native['native_camera_width']/native['native_camera_height']
def run(exe,*args):
 return subprocess.run([str(a.bin/exe),*map(str,args)],capture_output=True,check=True).stdout
def meta(path):
 return json.loads(run('ffprobe.exe','-v','error','-count_frames','-show_streams','-show_format','-of','json',path))
def frame(path,index,w,h):
 return np.frombuffer(run('ffmpeg.exe','-v','error','-i',path,'-vf',rf'select=eq(n\,{index})','-frames:v','1','-f','rawvideo','-pix_fmt','rgb24','-'),np.uint8).reshape(h,w,3)
main_meta,raw_meta=meta(source),meta(camera)
count=int(main_meta['streams'][0]['nb_read_frames'])
assert count==int(raw_meta['streams'][0]['nb_read_frames'])
assert len([s for s in main_meta['streams'] if s['codec_type']=='audio'])==2
results=[]
for index,rect in [(2,[448,216,160,126]),(min(50,count-2),[224,126,160,126])]:
 main,raw=frame(source,index,640,360),frame(camera,index,640,480)
 x,y,w,h=rect
 width,height=max(w,round(h*ratio)),max(h,round(w/ratio))
 scaled=np.asarray(Image.fromarray(raw).resize((width,height),Image.Resampling.BILINEAR))
 left,top=(width-w)//2,(height-h)//2
 expected=scaled[top:top+h,left:left+w]
 mae=float(np.mean(np.abs(main[y+9:y+h-9,x+9:x+w-9].astype(float)-expected[9:-9,9:-9].astype(float))))
 assert mae<20,(index,mae)
 results.append(dict(frame=index,rectangle=rect,interior_mae=mae))
result=dict(passed=True,main_frames=count,camera_frames=count,audio_tracks=2,placements=results)
a.evidence.write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result))
