#!/usr/bin/env python3
"""Differential tester: random x86-32 instruction sequences are run (1) in Unicorn (reference CPU)
and (2) through recomp.py -> C -> gcc, then registers, flags, x87 stack, XMM/MMX and memory are compared.
Usage: python3 tools/difftest.py [--n N] [--seed S] [--cats x87,x87t,sse,mmx,int,lock] [-v]"""
import sys, os, struct, random, subprocess, argparse, math, collections
sys.path.insert(0, os.path.dirname(__file__))
from keystone import Ks, KS_ARCH_X86, KS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn.x86_const import *
import recomp
if os.getenv("DIFFTEST_LOCAL_REGS") == "1":
    from recomp_local import LocalRecompiler
    recomp.Recompiler = LocalRecompiler

BAD = collections.Counter()
CC = 'gcc'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE, DATA, OUT, STK = 0x400000, 0x600000, 0x700000, 0x2FF000
ks = Ks(KS_ARCH_X86, KS_MODE_32)
def asm(lines):
    b, _ = ks.asm('; '.join(lines)); return bytes(b)

def f80(v):
    bits = struct.unpack('<Q', struct.pack('<d', v))[0]
    s, e, f = bits >> 63, (bits >> 52) & 0x7ff, bits & ((1 << 52) - 1)
    if e == 0 and f == 0: return struct.pack('<QH', 0, s << 15)
    return struct.pack('<QH', (1 << 63) | (f << 11), (s << 15) | (e - 1023 + 16383))

def make_data(r):
    d = bytearray(0x1000)
    def rf(): return r.choice([r.uniform(-100, 100), float(r.randint(-20, 20)), r.uniform(-1, 1), r.uniform(0.5, 3), 0.0, -0.0, r.uniform(-1e6, 1e6), 2.5, -3.5, 0.5])
    for k in range(32): struct.pack_into('<f', d, 0x000 + 4 * k, rf())
    for k in range(32): struct.pack_into('<d', d, 0x080 + 8 * k, rf())
    for k in range(32): struct.pack_into('<i', d, 0x180 + 4 * k, r.choice([r.randint(-1000, 1000), r.randint(-2**31, 2**31 - 1), 0, 1, -1]))
    for k in range(32): struct.pack_into('<f', d, 0x200 + 4 * k, rf())          # xmm init (8 regs)
    for k in range(64): struct.pack_into('<h', d, 0x280 + 2 * k, r.randint(-32768, 32767))
    for k in range(16): struct.pack_into('<q', d, 0x300 + 8 * k, r.choice([r.randint(-10**6, 10**6), r.randint(-2**62, 2**62)]))
    for k in range(12): d[0x380 + 10 * k:0x380 + 10 * k + 10] = f80(rf())
    for k in range(0x400, 0x800): d[k] = r.randrange(256)
    for k in range(16): struct.pack_into('<d', d, 0x700 + 8 * k, rf())         # doubles for sd/pd tests
    struct.pack_into('<4H', d, 0xFE0, 0x027F, 0x067F, 0x0A7F, 0x0E7F)          # FCW: RC near/down/up/trunc
    return bytes(d)

F32 = lambda r: 'dword ptr [esi+0x%x]' % (4 * r.randrange(32))
F64 = lambda r: 'qword ptr [esi+0x%x]' % (0x80 + 8 * r.randrange(32))
I32 = lambda r: 'dword ptr [esi+0x%x]' % (0x180 + 4 * r.randrange(32))
I16 = lambda r: 'word ptr [esi+0x%x]' % (0x280 + 2 * r.randrange(64))
I64 = lambda r: 'qword ptr [esi+0x%x]' % (0x300 + 8 * r.randrange(16))
F80 = lambda r: 'tbyte ptr [esi+0x%x]' % (0x380 + 10 * r.randrange(12))
RB = lambda r, sz: '%s ptr [esi+0x%x]' % ({1: 'byte', 2: 'word', 4: 'dword', 8: 'qword', 16: 'xmmword'}[sz], 0x400 + sz * r.randrange(0x400 // sz // 2))

class Case:
    def __init__(s, cat): s.cat, s.body, s.depth, s.fmask, s.stores, s.c1, s.swdef = cat, [], 0, 0x8C5, [], False, True
    def store(s, kind):
        a = 0x800 + 16 * len(s.stores); s.stores.append((a, kind)); return a

# ---------------------------------------------------------------- generators
FL_ALL = 0x8C5   # CF PF ZF SF OF (AF not modelled)
def gen_x87(r, trans=False):
    c = Case('x87t' if trans else 'x87'); d0 = r.randint(2, 4)
    pro = ['fld %s' % F64(r) for _ in range(d0)]; c.depth = d0
    E = c.body.append
    for _ in range(r.randint(1, 7)):
        if c.depth < 2: E('fld ' + F64(r)); c.depth += 1
        d = c.depth; ch = r.random()
        if trans:
            op = r.choice(['fsin', 'fcos', 'fsqrt', 'fpatan', 'fyl2x', 'f2xm1', 'fptan', 'fsincos', 'fscale', 'fxtract', 'fprem', 'fprem1', 'frndint', 'fabs'])
            if op in ('fpatan', 'fyl2x', 'fscale', 'fprem', 'fprem1') and d < 2: continue
            if op in ('fptan', 'fsincos', 'fxtract') and d > 6: continue
            if op == 'fyl2x': E('fabs'); E('fld1'); E('faddp st(1), st(0)')
            if op == 'f2xm1': E('fld1'); E('fxch st(1)'); E('fprem'); E('fstp st(1)')
            if op == 'fscale': E('fxch st(1)'); E('frndint'); E('fxch st(1)')
            E(op); c.depth += {'fpatan': -1, 'fyl2x': -1, 'fptan': 1, 'fsincos': 1, 'fxtract': 1}.get(op, 0)
            c.c1 = op in ('fprem', 'fprem1'); c.swdef = c.c1
            continue
        c.c1 = False; c.swdef = False
        if ch < 0.18 and d < 7:
            E(r.choice(['fld ' + F32(r), 'fld ' + F64(r), 'fld ' + F80(r), 'fild ' + I32(r), 'fild ' + I16(r), 'fild ' + I64(r),
                        'fld st(%d)' % r.randrange(d), r.choice(['fldz', 'fld1', 'fldpi', 'fldl2e', 'fldln2', 'fldlg2', 'fldl2t'])])); c.depth += 1
        elif ch < 0.55:
            op = r.choice(['fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr']); k = r.random()
            if k < 0.2: E('%s %s' % (op, F32(r)))
            elif k < 0.35: E('%s %s' % (op, F64(r)))
            elif k < 0.45: E('fi%s %s' % (op[1:], r.choice([I32(r), I16(r)])))
            elif k < 0.65: E('%s st(0), st(%d)' % (op, r.randrange(d)))
            elif k < 0.8: E('%s st(%d), st(0)' % (op, r.randrange(d)))
            elif d >= 2: E('%sp st(%d), st(0)' % (op, r.randrange(1, d))); c.depth -= 1
        elif ch < 0.65:
            E(r.choice(['fxch st(%d)' % r.randrange(1, d), 'fchs', 'fabs', 'fsqrt', 'frndint']))
        elif ch < 0.72:
            E('fldcw word ptr [esi+0x%x]' % (0xFE0 + 2 * r.randrange(4))); E(r.choice(['frndint', 'fld st(0)', 'fadd st(0), st(1)']))
            if c.body[-1] == 'fld st(0)': c.depth += 1
            if c.depth > 7: c.body = c.body[:-1]; c.depth -= 1
        elif ch < 0.86:
            k = r.random(); n = r.randrange(1, d)
            if k < 0.3:
                E(r.choice(['fcomi st(0), st(%d)' % n, 'fucomi st(0), st(%d)' % n]))
                E(r.choice(['fcmovb st(0), st(%d)', 'fcmove st(0), st(%d)', 'fcmovbe st(0), st(%d)', 'fcmovu st(0), st(%d)',
                            'fcmovnb st(0), st(%d)', 'fcmovne st(0), st(%d)', 'fcmovnbe st(0), st(%d)', 'fcmovnu st(0), st(%d)']) % r.randrange(d))
                c.fmask = 0x45   # QEMU keeps OF/SF across FCOMI (real CPUs clear them)
            elif k < 0.45 and d >= 2: E(r.choice(['fcomip st(0), st(%d)' % n, 'fucomip st(0), st(%d)' % n])); c.depth -= 1; c.fmask = 0x45
            else:
                cm = r.choice(['fcom st(%d)' % n, 'fucom st(%d)' % n, 'fcom ' + F32(r), 'fcom ' + F64(r), 'ficom ' + I32(r), 'ftst', 'fxam'])
                if r.random() < 0.3 and d >= 2: cm = r.choice(['fcomp st(%d)' % n, 'fucomp st(%d)' % n, 'fcomp ' + F64(r)]); c.depth -= 1
                elif r.random() < 0.15 and d >= 3: cm = r.choice(['fcompp', 'fucompp']); c.depth -= 2
                E(cm); c.c1 = cm == 'fxam'; c.swdef = True
                if r.random() < 0.6:
                    E('fnstsw ax'); E('and eax, 0xFFFF%04X' % (0x4700 if c.c1 else 0x4500)); c.fmask = FL_ALL
                    if r.random() < 0.5: E('sahf'); E(r.choice(['setb dl', 'sete dl', 'setbe dl', 'setp dl', 'seta dl']))
        else:
            k = r.random()
            if k < 0.6:
                kind = r.choice(['m32', 'm64', 'm80', 'i16', 'i32', 'i64', 't16', 't32', 't64'])
                pop = kind in ('m80', 'i64', 't16', 't32', 't64') or r.random() < 0.5
                if pop and d < 2: continue
                a = c.store(kind)
                spec = {'m32': 'dword', 'm64': 'qword', 'm80': 'tbyte', 'i16': 'word', 'i32': 'dword', 'i64': 'qword', 't16': 'word', 't32': 'dword', 't64': 'qword'}[kind]
                mn = {'m': 'fst', 'i': 'fist', 't': 'fisttp'}[kind[0]]
                if mn != 'fisttp' and pop: mn += 'p'
                E('%s %s ptr [edi+0x%x]' % (mn, spec, a - 0x800)); c.depth -= 1 if pop else 0
            elif d >= 2:
                E('fstp st(%d)' % r.randrange(d)); c.depth -= 1
    return pro, c

def gen_sse(r):
    c = Case('sse'); E = c.body.append
    pro = ['movups xmm%d, xmmword ptr [esi+0x%x]' % (k, 0x200 + 16 * k) for k in range(8)]
    if r.random() < 0.3: pro += ['movupd xmm%d, xmmword ptr [esi+0x%x]' % (k, 0x700 + 16 * k) for k in range(r.randint(1, 4))]
    X = lambda: 'xmm%d' % r.randrange(8)
    for _ in range(r.randint(1, 6)):
        ch = r.random(); x, y = X(), X()
        if ch < 0.3:
            op = r.choice(['addss', 'subss', 'mulss', 'divss', 'minss', 'maxss', 'sqrtss', 'addps', 'subps', 'mulps', 'divps', 'minps', 'maxps', 'sqrtps',
                           'addsd', 'subsd', 'mulsd', 'divsd', 'minsd', 'maxsd', 'sqrtsd', 'addpd', 'mulpd', 'subpd', 'divpd'])
            if r.random() < 0.3:
                src = F32(r) if op.endswith('ss') else F64(r) if op.endswith('sd') else 'xmmword ptr [esi+0x%x]' % (0x200 + 16 * r.randrange(8))
            else: src = y
            E('%s %s, %s' % (op, x, src))
        elif ch < 0.4: E('%s %s, %s' % (r.choice(['andps', 'orps', 'xorps', 'andnps', 'andpd', 'xorpd', 'pand', 'por', 'pxor', 'pandn']), x, y))
        elif ch < 0.48: E('cmp%s%s %s, %s' % (r.choice(recomp.fpsse.CMPPRED), r.choice(['ps', 'ss', 'pd', 'sd']), x, y))
        elif ch < 0.55: op_ = r.choice(['comiss', 'ucomiss', 'comisd', 'ucomisd']); E('%s %s, %s' % (op_, x, r.choice([y, F32(r) if op_.endswith('ss') else F64(r)]))); c.fmask = FL_ALL
        elif ch < 0.68:
            E(r.choice(['cvtsi2ss %s, eax' % x, 'cvtsi2ss %s, %s' % (x, I32(r)), 'cvtsi2sd %s, ebx' % x, 'cvttss2si eax, %s' % y, 'cvtss2si ebx, %s' % y,
                        'cvttsd2si ecx, %s' % y, 'cvtsd2si edx, %s' % y, 'cvtss2sd %s, %s' % (x, y), 'cvtsd2ss %s, %s' % (x, y), 'cvtdq2ps %s, %s' % (x, y),
                        'cvttps2dq %s, %s' % (x, y), 'cvtps2dq %s, %s' % (x, y), 'cvtps2pd %s, %s' % (x, y), 'cvtpd2ps %s, %s' % (x, y),
                        'cvtdq2pd %s, %s' % (x, y), 'cvttpd2dq %s, %s' % (x, y)]))
        elif ch < 0.8:
            E(r.choice(['shufps %s, %s, 0x%x' % (x, y, r.randrange(256)), 'shufpd %s, %s, %d' % (x, y, r.randrange(4)), 'pshufd %s, %s, 0x%x' % (x, y, r.randrange(256)),
                        'unpcklps %s, %s' % (x, y), 'unpckhps %s, %s' % (x, y), 'unpcklpd %s, %s' % (x, y), 'unpckhpd %s, %s' % (x, y),
                        'movhlps %s, %s' % (x, y), 'movlhps %s, %s' % (x, y), 'movss %s, %s' % (x, y), 'movsd %s, %s' % (x, y), 'movaps %s, %s' % (x, y),
                        'movss %s, %s' % (x, F32(r)), 'movsd %s, %s' % (x, F64(r)), 'movlps %s, %s' % (x, F64(r)), 'movhps %s, %s' % (x, F64(r)),
                        'movd %s, eax' % x, 'movd ecx, %s' % y, 'movq %s, %s' % (x, y), 'movq %s, %s' % (x, F64(r)), 'movmskps eax, %s' % y, 'movmskpd edx, %s' % y,
                        'pshuflw %s, %s, 0x%x' % (x, y, r.randrange(256)), 'pshufhw %s, %s, 0x%x' % (x, y, r.randrange(256))]))
        elif ch < 0.93:
            op = r.choice(list(recomp.fpsse.LANES) + list(recomp.fpsse.UNPACK)[:8] + ['packsswb', 'packuswb', 'packssdw', 'pmuludq', 'pmaddwd', 'psadbw'])
            E('%s %s, %s' % (op, x, r.choice([y, RB(r, 16)])))
        else:
            E(r.choice(['%s %s, %d' % (r.choice(list(recomp.fpsse.SHIFTS)), x, r.randrange(40)), '%s %s, %s' % (r.choice(list(recomp.fpsse.SHIFTS)), x, y),
                        'pslldq %s, %d' % (x, r.randrange(18)), 'psrldq %s, %d' % (x, r.randrange(18)), 'pextrw eax, %s, %d' % (y, r.randrange(8)),
                        'pinsrw %s, ebx, %d' % (x, r.randrange(8))]))
        if r.random() < 0.15:
            a = c.store('x16'); E(r.choice(['movups xmmword ptr [edi+0x%x], %s', 'movlps qword ptr [edi+0x%x], %s', 'movhps qword ptr [edi+0x%x], %s',
                                             'movss dword ptr [edi+0x%x], %s', 'movsd qword ptr [edi+0x%x], %s', 'movq qword ptr [edi+0x%x], %s',
                                             'movd dword ptr [edi+0x%x], %s']) % (a - 0x800, X()))
    return pro, c

def gen_mmx(r):
    c = Case('mmx'); E = c.body.append
    pro = ['movq mm%d, qword ptr [esi+0x%x]' % (k, 0x400 + 8 * k) for k in range(8)]
    M_ = lambda: 'mm%d' % r.randrange(8)
    for _ in range(r.randint(1, 6)):
        x, y = M_(), M_(); ch = r.random()
        if ch < 0.6: E('%s %s, %s' % (r.choice([o for o in recomp.fpsse.LANES if not o.endswith('q')] + ['pand', 'por', 'pxor', 'pandn', 'punpcklbw', 'punpckhwd', 'punpckldq', 'packsswb', 'packuswb', 'packssdw', 'pmaddwd', 'psadbw', 'pmuludq']), x, r.choice([y, RB(r, 8)])))
        elif ch < 0.8: E('%s %s, %d' % (r.choice(list(recomp.fpsse.SHIFTS)), x, r.randrange(70)))
        else: E(r.choice(['movd %s, eax' % x, 'movd ecx, %s' % y, 'movq %s, %s' % (x, y), 'pshufw %s, %s, 0x%x' % (x, y, r.randrange(256)), 'pextrw eax, %s, %d' % (y, r.randrange(4)), 'pmovmskb edx, %s' % y]))
    for k in range(8): E('movq qword ptr [edi+0x%x], mm%d' % (0x200 + 8 * k, k))
    E('emms')
    return pro, c

R32 = ['eax', 'ecx', 'edx', 'ebx']; R16 = ['ax', 'cx', 'dx', 'bx']; R8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
CF_, PF_, ZF_, SF_, OF_ = 0x1, 0x4, 0x40, 0x80, 0x800
CCREAD = {'o': OF_, 'no': OF_, 'b': CF_, 'ae': CF_, 'e': ZF_, 'ne': ZF_, 'be': CF_ | ZF_, 'a': CF_ | ZF_, 's': SF_, 'ns': SF_,
          'p': PF_, 'np': PF_, 'l': SF_ | OF_, 'ge': SF_ | OF_, 'le': ZF_ | SF_ | OF_, 'g': ZF_ | SF_ | OF_}
def gen_int(r):
    c = Case('int'); E = c.body.append; st = {'d': FL_ALL}
    def need(m):
        if st['d'] & m != m: E('cmp %s, %s' % (r.choice(R32), r.choice(R32))); st['d'] = FL_ALL
    def setd(defined, undef=0): st['d'] = (st['d'] | defined) & ~undef
    for _ in range(r.randint(1, 8)):
        sz = r.choice([1, 2, 4, 4, 4]); RR = {1: R8, 2: R16, 4: R32}[sz]; a, b = r.choice(RR), r.choice(RR); W = 8 * sz
        mem = RB(r, sz); ch = r.random()
        imm = r.choice([0, 1, r.randrange(1 << W), 1 << (W - 1), (1 << W) - 1]) if sz < 4 else r.choice([0, 1, 0x7fffffff, 0x80000000, r.randrange(1 << 32)])
        if ch < 0.3:
            op = r.choice(['add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'cmp', 'test'])
            if op in ('adc', 'sbb'): need(CF_)
            src = r.choice([b, '0x%x' % imm, mem]) if op != 'test' else r.choice([b, '0x%x' % imm])
            dst = a if src == mem or r.random() < 0.7 else mem
            E('%s %s, %s' % (op, dst, src)); setd(FL_ALL)
        elif ch < 0.4:
            op = r.choice(['inc', 'dec', 'neg', 'not']); E('%s %s' % (op, r.choice([a, mem])))
            # QEMU/Unicorn bug: LOCK NEG computes flags from the old value -> SF/PF wrong there (CF/ZF/OF happen to match).
            # Real CPUs agree with the recompiler (tests/native/lock_flags_x86.c).
            if op == 'neg': setd(CF_ | ZF_ | OF_, SF_ | PF_)
            else: setd({'inc': 0x8C4, 'dec': 0x8C4, 'not': 0}[op])
        elif ch < 0.55:
            op = r.choice(['shl', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr']); n = r.choice([1, r.randrange(32), 0, W, W + 1])
            if op in ('rcl', 'rcr'): need(CF_)
            E('%s %s, %d' % (op, r.choice([a, mem]), n)); m = n & 31
            if m:
                if op in ('shl', 'shr', 'sar'): setd(SF_ | ZF_ | PF_ | (CF_ if (m < W or op == 'sar') else 0) | (OF_ if m == 1 else 0), (OF_ if m != 1 else 0) | (CF_ if (m >= W and op != 'sar') else 0))
                elif op in ('rcl', 'rcr') and m % (W + 1) == 0: setd(OF_ if m == 1 else 0, 0 if m == 1 else OF_)
                else: setd(CF_ | (OF_ if m == 1 else 0), 0 if m == 1 else OF_)
        elif ch < 0.62 and sz > 1:
            n = r.randrange(1, W + 1); E('%s %s, %s, %d' % (r.choice(['shld', 'shrd']), a, b, n))
            setd(CF_ | SF_ | ZF_ | PF_ | (OF_ if n == 1 else 0), 0 if n == 1 else OF_)
        elif ch < 0.7:
            if sz == 1: E('%s %s' % (r.choice(['imul', 'mul']), r.choice(R8)))
            else: E(r.choice(['imul %s, %s' % (a, b), 'imul %s, %s, 0x%x' % (a, r.choice([b, mem]), imm & 0x7fff), 'mul %s' % b, 'imul %s' % b]))
            setd(CF_ | OF_, SF_ | ZF_ | PF_)
        elif ch < 0.77 and sz > 1:
            E('%s %s, %s' % (r.choice(['bt', 'bts', 'btr', 'btc']), a, r.choice([b, '%d' % r.randrange(W)]))); setd(CF_, OF_ | SF_ | PF_)
        elif ch < 0.8 and sz > 1:
            if r.random() < 0.5: E('%s %s, %s' % (r.choice(['bsf', 'bsr']), a, b)); st['d'] = ZF_
            else:
                E('and ecx, 0x3ff'); E('sub ecx, 0x200'); E('%s %s ptr [esi+0x600], %s' % (r.choice(['bt', 'bts', 'btr', 'btc']), {2: 'word', 4: 'dword'}[sz], {2: 'cx', 4: 'ecx'}[sz])); setd(CF_, OF_ | SF_ | PF_ | ZF_)
                if r.random() < 0.3: E('lea ebx, [esi+0x400]'); E('xlatb')
        elif ch < 0.86:
            cc = r.choice(list(CCREAD)); need(CCREAD[cc]); E('set%s %s' % (cc, r.choice(R8)))
        elif ch < 0.9 and sz > 1:
            cc = r.choice(['b', 'e', 'a', 'l', 'g', 'ns', 'p', 'le', 'o']); need(CCREAD[cc]); E('cmov%s %s, %s' % (cc, a, r.choice([b, mem])))
        elif ch < 0.95:
            ins = r.choice(['xchg %s, %s' % (a, b), 'xadd %s, %s' % (a, b), 'cmpxchg %s, %s' % (a, b), 'movzx eax, %s' % r.choice(R8 + R16[:3]), 'movsx ecx, %s' % r.choice(R8 + R16[:3]),
                            'lea edx, [eax+ecx*4+0x%x]' % (imm & 0xff), 'cdq', 'cwde', 'cbw', 'cwd', 'bswap ebx', 'lahf', 'sahf', 'cmc', 'stc', 'clc'])
            op = ins.split()[0]
            if op in ('lahf', 'cmc'): need(FL_ALL if op == 'lahf' else CF_)
            E(ins)
            if op == 'lahf': E('and ah, 0xEF')       # AF is not modelled
            if op in ('xadd', 'cmpxchg', 'lahf'): setd(FL_ALL)
            elif op == 'sahf': setd(CF_ | PF_ | ZF_ | SF_)
            elif op in ('stc', 'clc', 'cmc'): setd(CF_)
        else:
            E('mov ecx, 0x%x' % r.randrange(1, 1 << 16)); E('xor edx, edx'); E(r.choice(['div ecx', 'mov edx, 0', 'idiv ecx'])); st['d'] = 0 if c.body[-1] != 'mov edx, 0' else FL_ALL
        if r.random() < 0.1: a_ = c.store('i32'); E('mov dword ptr [edi+0x%x], %s' % (a_ - 0x800, r.choice(R32)))
    c.fmask = st['d']
    return [], c

def gen_lock(r):
    """LOCK-prefixed RMW, XCHG with memory, cmpxchg8b, plus plain shared-memory accesses (rdo/wro paths);
    includes misaligned operands (host spinlock fallback)."""
    c = Case('lock'); E = c.body.append; st = {'d': FL_ALL}
    def need(m):
        if st['d'] & m != m: E('cmp %s, %s' % (r.choice(R32), r.choice(R32))); st['d'] = FL_ALL
    def setd(defined, undef=0): st['d'] = (st['d'] | defined) & ~undef
    for _ in range(r.randint(1, 6)):
        sz = r.choice([1, 2, 4, 4]); RR = {1: R8, 2: R16, 4: R32}[sz]; b = r.choice(RR); W = 8 * sz
        off = 0x400 + sz * r.randrange(0x40) + (r.choice([0, 0, 0, 1, 3]) if sz > 1 else 0)
        mem = '%s ptr [esi+0x%x]' % ({1: 'byte', 2: 'word', 4: 'dword'}[sz], off)
        imm = r.choice([0, 1, r.randrange(1 << W), (1 << W) - 1]) if sz < 4 else r.choice([0, 1, 0x7fffffff, 0x80000000, r.randrange(1 << 32)])
        acc = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        ch = r.random()
        if ch < 0.3:
            op = r.choice(['add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor'])
            if op in ('adc', 'sbb'): need(CF_)
            E('lock %s %s, %s' % (op, mem, r.choice([b, '0x%x' % imm]))); setd(FL_ALL)
        elif ch < 0.42:
            op = r.choice(['inc', 'dec', 'neg', 'not']); E('lock %s %s' % (op, mem))
            # QEMU/Unicorn bug: LOCK NEG computes flags from the old value -> SF/PF wrong there (CF/ZF/OF happen to match).
            # Real CPUs agree with the recompiler (tests/native/lock_flags_x86.c).
            if op == 'neg': setd(CF_ | ZF_ | OF_, SF_ | PF_)
            else: setd({'inc': 0x8C4, 'dec': 0x8C4, 'not': 0}[op])
        elif ch < 0.54:
            E('lock xadd %s, %s' % (mem, b)); setd(FL_ALL)
        elif ch < 0.68:
            if r.random() < 0.5: E('mov %s, %s' % (acc, mem))       # make the compare succeed half of the time
            src = r.choice([x for x in RR if x != acc and not (sz == 1 and x == 'ah')])
            E('lock cmpxchg %s, %s' % (mem, src)); setd(FL_ALL)
        elif ch < 0.78:
            E(r.choice(['xchg %s, %s' % (mem, b), 'xchg %s, %s' % (b, mem)]))
        elif ch < 0.86 and sz > 1:
            if r.random() < 0.5: E('lock %s %s, %d' % (r.choice(['bts', 'btr', 'btc']), mem, r.randrange(W)))
            else:
                E('and ecx, 0x3ff'); E('sub ecx, 0x200')
                E('lock %s %s ptr [esi+0x600], %s' % (r.choice(['bts', 'btr', 'btc']), {2: 'word', 4: 'dword'}[sz], {2: 'cx', 4: 'ecx'}[sz]))
            setd(CF_, OF_ | SF_ | PF_ | ZF_)
        elif ch < 0.93:
            q = 0x400 + 8 * r.randrange(0x40) + r.choice([0, 0, 0, 4, 2])
            if r.random() < 0.5: E('mov eax, dword ptr [esi+0x%x]' % q); E('mov edx, dword ptr [esi+0x%x]' % (q + 4))
            # QEMU/Unicorn bug: the cmpxchg8b helper recomputes EFLAGS from a stale cc_op, so only ZF is reliable there;
            # real CPUs keep CF/PF/SF/OF (checked natively in tests/native/lock_flags_x86.c).
            E('lock cmpxchg8b qword ptr [esi+0x%x]' % q); setd(ZF_, FL_ALL & ~ZF_)
        else:   # plain shared-memory traffic through the ordered (rdo/wro) accessors
            E(r.choice(['mov %s, %s' % (mem, b), 'mov %s, %s' % (b, mem)]))
            if sz == 4 and r.random() < 0.3: E('push %s' % mem); E('pop %s' % r.choice(R32))
    c.fmask = st['d']
    return [], c

EPI = lambda c: (['pushfd', 'pop dword ptr [0x%x]' % OUT, 'fnstsw word ptr [0x%x]' % (OUT + 4)] +
                 ['mov dword ptr [0x%x], %s' % (OUT + 0x10 + 4 * k, g) for k, g in enumerate(['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'])] +
                 ['fstp qword ptr [0x%x]' % (OUT + 0x40 + 8 * k) for k in range(c.depth)] +
                 ['movups xmmword ptr [0x%x], xmm%d' % (OUT + 0x100 + 16 * k, k) for k in range(8)] + ['ret'])
PRO0 = ['fninit', 'fldcw word ptr [esi+0xFE0]']

# ---------------------------------------------------------------- fake PE for the recompiler
class FakePE:
    def __init__(s, img):
        s.base, s.img, s.entry_rva = CODE, img, 0
        s.relocs, s.reloc_set, s.imports, s.exports = [], set(), [], {}
    def is_code(s, va): return CODE <= va < CODE + len(s.img)
    def section_of(s, rva): return None
    def u32(s, rva): return struct.unpack_from('<I', s.img, rva)[0]

def build(cases, data, workdir):
    img = bytearray(); starts = []
    for pro, c in cases:
        code = asm(PRO0 + pro + c.body + EPI(c))
        starts.append(CODE + len(img)); img += code + b'\xcc' * (16 - len(code) % 16)
    pe = FakePE(bytes(img))
    rc = recomp.Recompiler(pe, 't', starts); rc.use_eh = False; rc.discover()
    os.makedirs(workdir, exist_ok=True)
    L = ['#include "rt.h"'] + ['void t_%x(CPU* c);' % f for f in sorted(rc.funcs)]
    unimpl = collections.Counter()
    for f in sorted(rc.funcs): L += rc.emit_function(f)
    unimpl.update(rc.unimpl)
    L += ['const FuncEntry t_funcs[] = {'] + ['  {0x%xu, t_%x},' % (f, f) for f in sorted(rc.funcs)] + ['};', 'const unsigned t_nfuncs = %d;' % len(rc.funcs)]
    open(os.path.join(workdir, 'tests.c'), 'w').write('\n'.join(L) + '\n')
    open(os.path.join(workdir, 'harness.c'), 'w').write(HARNESS)
    open(os.path.join(workdir, 'data.bin'), 'wb').write(data)
    with open(os.path.join(workdir, 'cases.bin'), 'wb') as f:
        f.write(struct.pack('<I', len(cases)))
        for s, (pro, c) in zip(starts, cases): f.write(struct.pack('<9I', s, *c.regs))
    rt = ' '.join([os.path.join(ROOT, 'runtime', x) for x in ('rt.c', 'rt_fp.c', 'hle_win32.c', 'hle_sys.c', 'hle_fs.c', 'hle_mod.c', 'hle_thread.c', 'hle_seh.c', 'hle_exe.c', 'hle_vmem.c', 'hle_d3d9.c', 'hle_d3d9_cmd.c', 'gearmass.c', 'sm3_glsl.c', 'hle_hb.c', 'hle_input.c', 'hle_al.c')] +
                  [os.path.join(ROOT, 'settings', x) for x in ('ini.c', 'me1settings.c')])
    cmd = '%s -O1 -w -fwrapv -fno-strict-aliasing -ffp-contract=off -fsigned-char -I%s/runtime -I%s/runtime/khr -I%s/settings %s -o %s/harness %s/harness.c %s/tests.c %s -lm -ldl -pthread' % (
        CC, ROOT, ROOT, ROOT, os.environ.get('DIFFTEST_CFLAGS', ''), workdir, workdir, workdir, rt)   # DIFFTEST_CFLAGS: e.g. -I<Vulkan-Headers>/include
    subprocess.check_call(cmd, shell=True)
    return bytes(img), starts, unimpl

HARNESS = r'''
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
extern const FuncEntry t_funcs[]; extern const unsigned t_nfuncs;
int main(int argc, char** argv) {
    rt_init();
    static uint8_t data[0x1000]; FILE* f = fopen(argv[1], "rb"); fread(data, 1, 0x1000, f); fclose(f);
    f = fopen(argv[2], "rb"); unsigned n; fread(&n, 4, 1, f);
    uint8_t* res = mmap(NULL, (size_t)n * 0x1400, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    for (unsigned k = 0; k < n; k++) {
        uint32_t in[9]; fread(in, 4, 9, f);
        GuestFn fn = 0; for (unsigned j = 0; j < t_nfuncs; j++) if (t_funcs[j].va == in[0]) fn = t_funcs[j].fn;
        pid_t p = fork();
        if (p == 0) {
            CPU* c = &g_cpu; uint32_t teb = c->fs_base; memset(c, 0, sizeof *c); c->fs_base = teb;
            rt_fninit(c); c->mxcsr = 0x1F80;
            memcpy(M + 0x600000, data, 0x1000); memset(M + 0x700000, 0, 0x400);
            c->eax = in[1]; c->ecx = in[2]; c->edx = in[3]; c->ebx = in[4]; c->esp = in[5]; c->ebp = in[6]; c->esi = in[7]; c->edi = in[8];
            push32(c, 0xFFFFFFF0u);
            if (fn) fn(c);
            memcpy(res + (size_t)k * 0x1400, M + 0x700000, 0x400); memcpy(res + (size_t)k * 0x1400 + 0x400, M + 0x600000, 0x1000);
            _exit(0);
        }
        int st; waitpid(p, &st, 0);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) memset(res + (size_t)k * 0x1400, 0xEE, 0x1400);
    }
    fwrite(res, 0x1400, n, stdout);
    return 0;
}
'''

def run_unicorn(img, starts, cases, data):
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    for a, sz in ((0x200000, 0x100000), (CODE, 0x200000), (DATA, 0x1000), (OUT, 0x1000)): mu.mem_map(a, sz)
    mu.mem_write(CODE, img); res = []
    for s, (pro, c) in zip(starts, cases):
        n = len(asm(PRO0 + pro + c.body + EPI(c)))
        mu.mem_write(DATA, data); mu.mem_write(OUT, b'\0' * 0x400)
        for reg, v in zip((UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI), c.regs):
            mu.reg_write(reg, v)
        mu.reg_write(UC_X86_REG_EFLAGS, 0x2); mu.reg_write(UC_X86_REG_MXCSR, 0x1F80)
        mu.mem_write(c.regs[4] - 4, struct.pack('<I', 0xFFFFFFF0)); mu.reg_write(UC_X86_REG_ESP, c.regs[4] - 4)
        try: mu.emu_start(s, s + n - 1); res.append(bytes(mu.mem_read(OUT, 0x400)) + bytes(mu.mem_read(DATA, 0x1000)))
        except UcError as e: res.append(('uc', str(e)))
    return res

def feq(a, b, tol):
    if math.isnan(a) and math.isnan(b): return True
    if a == b: return True
    if math.isinf(a) or math.isinf(b): return False
    return abs(a - b) <= tol * max(abs(a), abs(b), 1e-300)

def compare(c, U, C):
    if isinstance(U, tuple): return 'unicorn error: ' + U[1]
    if C[:16] == b'\xee' * 16: return 'recompiled code faulted'
    tol = {'x87': 1e-11, 'x87t': 1e-9}.get(c.cat, 0)
    diffs = []
    fu, fc = struct.unpack_from('<I', U, 0)[0], struct.unpack_from('<I', C, 0)[0]
    if (fu ^ fc) & c.fmask: diffs.append('eflags %03x vs %03x (mask %03x)' % (fu & c.fmask, fc & c.fmask, c.fmask))
    if c.cat.startswith('x87'):
        swm = 0x3800 | ((0x4500 | (0x200 if c.c1 else 0)) if c.swdef else 0)
        su, sc = struct.unpack_from('<H', U, 4)[0], struct.unpack_from('<H', C, 4)[0]
        if (su ^ sc) & swm: diffs.append('fsw %04x vs %04x' % (su & swm, sc & swm))
    for k, g in enumerate(['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']):
        a, b = struct.unpack_from('<I', U, 0x10 + 4 * k)[0], struct.unpack_from('<I', C, 0x10 + 4 * k)[0]
        if a != b: diffs.append('%s %08x vs %08x' % (g, a, b))
    for k in range(c.depth):
        a, b = struct.unpack_from('<d', U, 0x40 + 8 * k)[0], struct.unpack_from('<d', C, 0x40 + 8 * k)[0]
        if not feq(a, b, tol): diffs.append('st(%d) %r vs %r' % (k, a, b))
    if c.cat == 'sse':
        for k in range(8):
            a, b = U[0x100 + 16 * k:0x110 + 16 * k], C[0x100 + 16 * k:0x110 + 16 * k]
            if a != b: diffs.append('xmm%d %s vs %s' % (k, a.hex(), b.hex()))
    du, dc = bytearray(U[0x400:]), bytearray(C[0x400:])
    for a, kind in c.stores:
        if kind[0] in 'it':
            n = int(kind[1:]) // 8; x, y = int.from_bytes(du[a:a + n], 'little'), int.from_bytes(dc[a:a + n], 'little')
            if x != y and y == 1 << (8 * n - 1) and x in ((1 << (8 * n - 1)) - 1, 1 << (8 * n - 1)): du[a:a + n] = dc[a:a + n]
        if tol and kind in ('m32', 'm64', 'm80'):
            fmt = {'m32': '<f', 'm64': '<d'}.get(kind)
            if fmt: x, y = struct.unpack_from(fmt, du, a)[0], struct.unpack_from(fmt, dc, a)[0]
            else:
                x, y = [m * 2.0 ** ((se & 0x7fff) - 16383 - 63) * (-1 if se & 0x8000 else 1) if (se & 0x7fff) != 0x7fff else float('nan')
                        for m, se in (struct.unpack_from('<QH', du, a), struct.unpack_from('<QH', dc, a))]
            if not feq(x, y, tol if kind != 'm32' else 1e-6): diffs.append('store@%x %s %r vs %r' % (a, kind, x, y))
            n = {'m32': 4, 'm64': 8, 'm80': 10}[kind]; du[a:a + n] = dc[a:a + n] = b'\0' * n
    if du != dc:
        k = next(j for j in range(len(du)) if du[j] != dc[j]); diffs.append('memory @%x: %s vs %s' % (DATA + k, du[k:k + 8].hex(), dc[k:k + 8].hex()))
    return '; '.join(diffs)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--n', type=int, default=300); ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--cats', default='x87,x87t,sse,mmx,int,lock'); ap.add_argument('-v', action='store_true'); ap.add_argument('--work', default='/tmp/difftest')
    ap.add_argument('--cc', default='gcc', help='compiler for the harness, e.g. NDK aarch64-linux-android29-clang -static')
    ap.add_argument('--run', default='', help='runner prefix, e.g. qemu-aarch64-static')
    a = ap.parse_args(); r = random.Random(a.seed); data = make_data(r)
    global CC; CC = a.cc
    G = {'x87': lambda: gen_x87(r), 'x87t': lambda: gen_x87(r, True), 'sse': lambda: gen_sse(r), 'mmx': lambda: gen_mmx(r), 'int': lambda: gen_int(r), 'lock': lambda: gen_lock(r)}
    cases = []
    for cat in a.cats.split(','):
        for _ in range(a.n):
            pro, c = G[cat]()
            try: asm(PRO0 + pro + c.body + EPI(c))
            except Exception:
                for l in pro + c.body:
                    try: asm([l])
                    except Exception: BAD[l.split()[0]] += 1
                continue
            c.regs = [r.choice([r.randrange(1 << 32), r.randrange(256), 0x80000000, 0xFFFFFFFF, r.randrange(1, 100)]) for _ in range(4)] + [STK, 0x600C00, DATA, DATA + 0x800]
            cases.append((pro, c))
    if BAD: print('keystone could not assemble (cases skipped):', dict(BAD))
    img, starts, unimpl = build(cases, data, a.work)
    if unimpl: print('UNIMPLEMENTED in tests:', dict(unimpl))
    U = run_unicorn(img, starts, cases, data)
    out = subprocess.run(a.run.split() + [os.path.join(a.work, 'harness'), os.path.join(a.work, 'data.bin'), os.path.join(a.work, 'cases.bin')], capture_output=True).stdout
    stats = collections.Counter(); fails = collections.defaultdict(list)
    for k, (pro, c) in enumerate(cases):
        d = compare(c, U[k], out[k * 0x1400:(k + 1) * 0x1400]); stats[c.cat, bool(d)] += 1
        if d: fails[c.cat].append((c, d))
    for cat in a.cats.split(','):
        print('%-5s pass %4d  fail %4d' % (cat, stats[cat, False], stats[cat, True]))
        for c, d in fails[cat][:(50 if a.v else 4)]:
            print('   FAIL [%s]\n        %s' % (' ; '.join(c.body), d))
    return 1 if any(fails.values()) else 0

if __name__ == '__main__': sys.exit(main())
