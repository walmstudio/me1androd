#!/usr/bin/env python3
"""Exercise branches, loops, guest-memory boundaries and division in both emitters.
Requires Python capstone/keystone/unicorn and a C compiler. No game files needed.
"""
import os, sys, tempfile, subprocess, random
sys.path.insert(0, os.path.abspath('tools'))
import difftest as dt
from recomp_local import LocalRecompiler
BaseRecompiler = dt.recomp.Recompiler
r = random.Random(124)
data = dt.make_data(r)
cases = []
for n in range(1, 65):
    c = dt.Case('int'); c.fmask = dt.FL_ALL
    c.regs = [n, 0, 0, n*3, dt.STK, 0x600c00, dt.DATA, dt.DATA+0x800]
    c.body = ['mov eax, %d' % n, 'mov ecx, %d' % (1+n%7), 'xor edx, edx',
              'again:', 'add eax, ebx', 'xor al, 0x55', 'dec ecx', 'jnz again',
              'test eax, 1', 'jz even', 'add ebx, 3', 'jmp join',
              'even:', 'sub ebx, 4', 'join:', 'mov dword ptr [edi], eax',
              'mov edx, 0', 'mov ecx, 7', 'div ecx', 'add eax, ebx',
              'mov dword ptr [edi+4], eax']
    cases.append(([], c))
with tempfile.TemporaryDirectory(prefix='me1-local-reg-') as root:
    results = []
    for name, enabled in [('base', '0'), ('opt', '1')]:
        dt.recomp.Recompiler = LocalRecompiler if enabled == '1' else BaseRecompiler
        path = os.path.join(root, name)
        img, starts, unimpl = dt.build(cases, data, path)
        if unimpl: raise AssertionError('unsupported: %r' % unimpl)
        out = subprocess.check_output([path+'/harness', path+'/data.bin', path+'/cases.bin'], stderr=subprocess.DEVNULL)
        assert len(out) == 0x1400 * len(cases)
        if enabled == '1':
            assert 'experimental straight-line guest registers' in open(path+'/tests.c').read()
        refs = dt.run_unicorn(img, starts, cases, data)
        for i, (_, c) in enumerate(cases):
            diff = dt.compare(c, refs[i], out[i*0x1400:(i+1)*0x1400])
            if diff: raise AssertionError('%s case %d: %s' % (name, i, diff))
        results.append(out)
    assert results[0] == results[1], 'baseline and optimized results differ'
print('64 branch/loop/memory/division cases: baseline == optimized == Unicorn')
