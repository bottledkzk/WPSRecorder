"""Compare frames actually shown by the editor with independent source frame-index decoding."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument('--bin', type=Path, required=True)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--editor-output', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
a = p.parse_args()
cases = []
for index in [9, 10]:
    raw = subprocess.check_output([
        str(a.bin / 'ffmpeg.exe'), '-v', 'error', '-i', str(a.source), '-vf',
        rf'select=eq(n\,{index}),scale=960:540:force_original_aspect_ratio=decrease',
        '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'])
    expected = np.frombuffer(raw, np.uint8).reshape(540, 960, 3)
    actual = np.asarray(Image.open(str(a.editor_output) + f'-frame{index}.png').convert('RGB'))
    mae = float(np.mean(np.abs(expected.astype(float) - actual.astype(float))))
    assert mae < .01, (index, mae)
    cases.append(dict(source_frame=index, pixel_mae=mae, sha256=hashlib.sha256(actual.tobytes()).hexdigest()))
assert cases[0]['sha256'] != cases[1]['sha256'], 'Forward button did not change the picture'
a.evidence.write_text(json.dumps(dict(passed=True, cases=cases), indent=2), encoding='utf-8')
print(json.dumps(dict(passed=True, cases=cases)))
