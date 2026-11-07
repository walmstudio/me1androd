#!/usr/bin/env python3
"""Geometric smoke test for default layout assets, at several landscape ratios."""
import itertools, json
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent / 'android/assets'
paths = list((ROOT/'layouts').glob('*.icp')) + [ROOT/'controls/mass_effect.icp']
errors=[]
for f in paths:
    profile=json.loads(f.read_text()); es=profile['elements']; bindings={b for e in es for b in e.get('bindings',[])}
    for k in ['KEY_G','KEY_TAB','KEY_J','SPECIAL_KEYBOARD']:
        if k not in bindings: errors.append(f'{f.name}: missing {k}')
    for W,H in [(1600,720),(2560,1080),(1920,1080),(2000,1200)]:
        boxes=[]; s=max(W,H)/100
        for e in es:
            k=s*e['scale']; typ=e['type']; hw=hh=6*k
            if typ=='BUTTON': hw=hh=3*k
            elif typ=='D_PAD': hw=hh=7*k
            elif typ=='RANGE_BUTTON': hw=2*k*e.get('bindingCount',4); hh=2*k
            if e.get('orientation',0)==1 and typ=='RANGE_BUTTON': hw,hh=hh,hw
            x=e['x']*W; y=e['y']*H; boxes.append((x-hw,y-hh,x+hw,y+hh))
            if x-hw<0 or y-hh<0 or x+hw>W or y+hh>H: errors.append(f'{f.name} {W}x{H}: element out of bounds')
        for (i,a),(j,b) in itertools.combinations(enumerate(boxes),2):
            if min(a[2],b[2])-max(a[0],b[0])>2 and min(a[3],b[3])-max(a[1],b[1])>2:
                errors.append(f'{f.name} {W}x{H}: elements {i},{j} overlap')
print('\n'.join(errors) if errors else f'{len(paths)} layouts × 4 screen sizes: OK (bounding boxes)')
raise SystemExit(bool(errors))
