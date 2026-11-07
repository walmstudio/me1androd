#!/usr/bin/env python3
"""Compare x87t sequence prefixes from difftest against actual x86-64.

Run from the repository root, with capstone, keystone-engine and unicorn:
  PYTHONPATH=/path/to/deps C_INCLUDE_PATH=/path/to/vulkan/include \
    python3 tests/recomp/audit_x87t_native.py [--trig-fix] [--unicorn] [--n 200] [--seed 11]

Tests generated snippets, not the game. Requires x86-64, gcc and executable
anonymous memory. Only masked exceptions, stack TOP and values in hardware
nonempty registers are checked. No comparison of condition/exception flags,
tag mismatches, NaN payloads, zero signs or exact 80-bit values is made.
CPU x87 transcendental functions and double/libm can differ in precision.
The opt-in emitter changes generated game code and requires full regeneration.
"""
import argparse
import ctypes
import math
import mmap
import os
import platform
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.abspath('tools'))
import difftest as dt
from keystone import Ks, KS_ARCH_X86, KS_MODE_64


def double_from_ext(m, se):
    e = se & 0x7fff
    sign = -1 if se & 0x8000 else 1
    if e == 0x7fff:
        return float('nan') if m & 0x7fffffffffffffff else math.copysign(math.inf, sign)
    if not m:
        return math.copysign(0.0, sign)
    try:
        return sign * math.ldexp(m, (e or 1) - 16383 - 63)
    except OverflowError:
        return math.copysign(math.inf, sign)


class Native:
    def __init__(self, data):
        self.ks = Ks(KS_ARCH_X86, KS_MODE_64)
        self.code = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        address = ctypes.addressof(ctypes.c_char.from_buffer(self.code))
        self.call = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)(address)
        self.output = ctypes.create_string_buffer(528)
        self.address = (ctypes.addressof(self.output) + 15) & ~15
        self.data = ctypes.create_string_buffer(data)

    def run(self, lines):
        lines = [s.replace('esi', 'rsi') for s in lines] + ['fxsave [rdi]', 'fninit', 'ret']
        code = bytes(self.ks.asm('; '.join(lines))[0])
        if len(code) > 4096:
            raise ValueError('native test snippet exceeds buffer')
        self.code.seek(0)
        self.code.write(code)
        ctypes.memset(self.address, 0, 512)
        self.call(self.address, ctypes.addressof(self.data))
        raw = ctypes.string_at(self.address, 512)
        sw = struct.unpack_from('<H', raw, 2)[0]
        top, tag = (sw >> 11) & 7, raw[4]
        values = [double_from_ext(*struct.unpack_from('<QH', raw, 32 + 16 * i))
                  if tag & (1 << ((top + i) & 7)) else None for i in range(8)]
        return top, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--n', type=int, default=200)
    parser.add_argument('--seed', type=int, default=11)
    parser.add_argument('--trig-fix', action='store_true')
    parser.add_argument('--unicorn', action='store_true', help='also trace Unicorn differences in its failed final cases')
    args = parser.parse_args()
    if platform.machine() not in ('x86_64', 'amd64'):
        parser.error('native reference requires x86-64')
    if args.n < 1:
        parser.error('--n must be positive')
    if args.trig_fix:
        import recomp_x87_fixed
        recomp_x87_fixed.activate()
    r = random.Random(args.seed)
    data = dt.make_data(r)
    cases = []
    # Consume x87 cases first, just as --cats x87,x87t in difftest.py does.
    for trans in (False, True):
        for _ in range(args.n):
            pro, case = dt.gen_x87(r, trans)
            try:
                dt.asm(dt.PRO0 + pro + case.body + dt.EPI(case))
            except Exception:
                continue
            case.regs = [r.choice([r.randrange(1 << 32), r.randrange(256), 0x80000000,
                                   0xffffffff, r.randrange(1, 100)]) for _ in range(4)] + [dt.STK, 0x600c00, dt.DATA, dt.DATA + 0x800]
            if trans:
                cases.append((pro, case))
    prefixes, indices = [], []
    for number, (pro, case) in enumerate(cases):
        for i in range(len(case.body)):
            prefix = dt.Case('x87t')
            prefix.body, prefix.depth, prefix.regs = case.body[:i+1], 8, case.regs
            prefixes.append((pro, prefix))
            indices.append((number, i))
    native = Native(data)
    with tempfile.TemporaryDirectory(prefix='me1-x87-audit-') as work:
        img, starts, unimplemented = dt.build(prefixes, data, work)
        if unimplemented:
            raise RuntimeError('unimplemented instructions: %r' % unimplemented)
        result = subprocess.run([work + '/harness', work + '/data.bin', work + '/cases.bin'],
                                stdout=subprocess.PIPE, check=True).stdout
        if len(result) != len(prefixes) * 0x1400:
            raise RuntimeError('unexpected harness output size')
    unicorn = dt.run_unicorn(img, starts, prefixes, data) if args.unicorn else None
    bad = {}
    for index, ((pro, case), (number, op)) in enumerate(zip(prefixes, indices)):
        if number in bad:
            continue
        rec = result[index*0x1400:(index+1)*0x1400]
        if rec[:16] == b'\xee' * 16:
            bad[number] = (op, 'recompiled code faulted')
            continue
        top, values = native.run(dt.PRO0 + pro + case.body)
        actual_top = (struct.unpack_from('<H', rec, 4)[0] >> 11) & 7
        diffs = []
        if top != actual_top:
            diffs.append('TOP native=%d runtime=%d' % (top, actual_top))
        for j, expected in enumerate(values):
            actual = struct.unpack_from('<d', rec, 0x40 + 8*j)[0]
            if expected is not None and not dt.feq(expected, actual, 1e-9):
                diffs.append('ST(%d) native=%r runtime=%r' % (j, expected, actual))
        if diffs:
            bad[number] = (op, '; '.join(diffs))
    if unicorn is not None:
        final = {number: index for index, (number, _) in enumerate(indices)}
        failed = set()
        for number, index in final.items():
            record = result[index*0x1400:(index+1)*0x1400]
            if dt.compare(cases[number][1], unicorn[index], record):
                failed.add(number)
        first = {}
        for index, ((pro, case), (number, op)) in enumerate(zip(prefixes, indices)):
            if number not in failed or number in first:
                continue
            ref = unicorn[index]
            if isinstance(ref, tuple):
                first[number] = 'Unicorn error'
                continue
            top, values = native.run(dt.PRO0 + pro + case.body)
            other_top = (struct.unpack_from('<H', ref, 4)[0] >> 11) & 7
            differs = top != other_top or any(
                x is not None and not dt.feq(x, struct.unpack_from('<d', ref, 0x40+8*j)[0], 1e-9)
                for j, x in enumerate(values))
            if differs:
                first[number] = case.body[op].split()[0]
        from collections import Counter
        print('Unicorn/runtime final differences: %d; first native/Unicorn TOP/value difference: %s; no TOP/value difference: %d' %
              (len(failed), dict(Counter(first.values())), len(failed)-len(first)))
        print('Native/Unicorn comparison does not check exception or condition flags or empty tags.')
    print('x87t prefix audit: %d sequences, %d prefixes; %d sequences with differences' %
          (len(cases), len(prefixes), len(bad)))
    print('Checks: TOP and nonempty native stack values only; 80-bit precision and flags not fully modelled.')
    for number, (op, reason) in sorted(bad.items()):
        case = cases[number][1]
        print('case %d, instruction %d %s: %s' % (number, op, case.body[op], reason))
        print('  ' + '; '.join(case.body))
    return bool(bad)


if __name__ == '__main__':
    sys.exit(main())
