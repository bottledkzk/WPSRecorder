"""Independently decode the actual HDR display recording; validate automatic mapping and live adjustments."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import numpy as np
p=argparse.ArgumentParser()
p.add_argument('--bin',type=Path,required=True)
p.add_argument('--native',type=Path,required=True)
p.add_argument('--evidence',type=Path,required=True)
a=p.parse_args()
r=json.loads(a.native.read_text(encoding='utf-8'))
assert r['passed'],r
source=r['recording']['output']
def run(exe,*args):
    return subprocess.check_output([str(a.bin/exe),*map(str,args)],stderr=subprocess.PIPE)
meta=json.loads(run('ffprobe.exe','-v','error','-count_frames','-show_streams','-show_format','-of','json',source))
v=next(s for s in meta['streams'] if s['codec_type']=='video')
frames=int(v['nb_read_frames'])
missed=r['recording'].get('missed_frame_slots',0)
assert frames==r['recording']['frames_written'] and frames+missed==210
assert abs(float(meta['format']['duration'])-7) < .034 and (v['width'],v['height'])==(2560,1440)
assert (v.get('color_range'), v.get('color_space'), v.get('color_primaries'), v.get('color_transfer')) == (
    'tv', 'bt709', 'bt709', 'iec61966-2-1'), v
run('ffmpeg.exe','-v','error','-xerror','-i',source,'-f','null','-')
base=np.array([[0,0,0],[255,255,255],[128,128,128],[255,0,0],[0,255,0],[0,0,255],[64,64,64],[192,192,192]],float)
def decode(x):return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)
def encode(x):return np.where(x<=.0031308,x*12.92,1.055*np.maximum(x,0)**(1/2.4)-.055)
dark=encode(decode(base/255)*.5)*255
compressed=base.copy();compressed[base==255]=encode(np.array([.875]))[0]*255
references=[base,dark,compressed]
samples=[];labels=[]
for t in np.arange(.5,6.6,.5):
    data=run('ffmpeg.exe','-v','error','-ss',t,'-i',source,'-frames:v','1','-vf','scale=800:450','-f','rawvideo','-pix_fmt','rgb24','-')
    image=np.frombuffer(data,np.uint8).reshape(450,800,3)
    observed=np.array([image[225,50+100*i] for i in range(8)],float)
    errors=[float(np.max(np.abs(observed-ref))) for ref in references]
    label=int(np.argmin(errors));assert errors[label]<12,(t,errors,observed)
    labels.append(label);samples.append(dict(time=float(t),mode=['auto','exposure_minus_one','highlight_compression'][label],max_rgb_error=errors[label],white=observed[1].astype(int).tolist(),gray=observed[2].astype(int).tolist()))
assert set(labels)=={0,1,2} and labels==sorted(labels),labels
result=dict(passed=True,frames=frames,missed_frame_slots=missed,width=v['width'],height=v['height'],color_space=v['color_space'],
            color_range=v['color_range'],color_primaries=v['color_primaries'],color_transfer=v['color_transfer'],
            full_decode=True,live_modes_observed=['auto','exposure_minus_one','highlight_compression'],samples=samples)
a.evidence.write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result))
