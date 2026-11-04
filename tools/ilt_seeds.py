#!/usr/bin/env python3
"""v0.10.13: targets of an incremental-link jump table (runs of `jmp rel32` thunks) -> --seeds file for recomp.py.
Usage: tools/ilt_seeds.py MassEffect.exe tools/seeds/MassEffect_ilt_funcs.txt"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pefile_min import PE
pe = PE(sys.argv[1]); img = bytes(pe.img); out = set(); i = 0; n = len(img)
while i < n - 5:
    if img[i] == 0xE9:
        j = i; tg = []
        while j < n - 5 and img[j] == 0xE9:
            t = (pe.base + j + 5 + int.from_bytes(img[j + 1:j + 5], 'little', signed=True)) & 0xffffffff
            if not pe.is_code(t): break
            tg.append(t); j += 5
        if len(tg) >= 64: out.update(tg); print('ILT at %08x: %d thunks' % (pe.base + i, len(tg))); i = j; continue
    i += 1
open(sys.argv[2], 'w').write(''.join('%x\n' % t for t in sorted(out)))
print('%d seeds -> %s' % (len(out), sys.argv[2]))
