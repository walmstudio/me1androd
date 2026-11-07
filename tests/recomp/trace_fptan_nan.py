#!/usr/bin/env python3
"""Explain five added seed-11 Unicorn differences after the FPTAN NaN fix.
Diagnostic only; does not hide differences. Requires keystone/capstone/unicorn.
"""
import sys, random, struct
sys.path.insert(0, 'tools')
import difftest as dt
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

wanted = {
 'fprem1 ; fsqrt ; fptan ; frndint ; fld1 ; fxch st(1) ; fprem ; fstp st(1) ; f2xm1',
 'fsin ; fld1 ; fxch st(1) ; fprem ; fstp st(1) ; f2xm1 ; fsincos ; fprem ; fsqrt ; fptan',
 'fabs ; fprem ; fabs ; fprem ; fprem1 ; fptan',
 'fprem ; fsincos ; fptan',
 'fprem ; fcos ; fptan',
}
r = random.Random(11); data = dt.make_data(r); cases = []
for cat in ['x87', 'x87t']:
    for _ in range(200):
        pro, c = dt.gen_x87(r, cat == 'x87t')
        try: dt.asm(dt.PRO0 + pro + c.body + dt.EPI(c))
        except Exception: continue
        c.regs = [r.choice([r.randrange(1<<32), r.randrange(256), 0x80000000, 0xffffffff, r.randrange(1,100)]) for _ in range(4)] + [dt.STK, 0x600c00, dt.DATA, dt.DATA+0x800]
        if cat == 'x87t' and ' ; '.join(c.body) in wanted: cases.append((pro,c))
assert len(cases) == len(wanted), 'seeded generator or dependencies changed'
for pro,c in cases:
    code = dt.asm(dt.PRO0 + pro + c.body + dt.EPI(c))
    ins = list(Cs(CS_ARCH_X86, CS_MODE_32).disasm(code, dt.CODE))
    u = Uc(UC_ARCH_X86, UC_MODE_32)
    for a,sz in ((0x200000,0x100000), (dt.CODE,0x200000), (dt.DATA,0x1000), (dt.OUT,0x1000)): u.mem_map(a,sz)
    u.mem_write(dt.CODE,code); u.mem_write(dt.DATA,data)
    for reg,v in zip((UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EDX,UC_X86_REG_EBX,UC_X86_REG_ESP,UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI), c.regs): u.reg_write(reg,v)
    u.reg_write(UC_X86_REG_EFLAGS,2); u.reg_write(UC_X86_REG_MXCSR,0x1f80)
    u.mem_write(c.regs[4]-4,struct.pack('<I',0xfffffff0)); u.reg_write(UC_X86_REG_ESP,c.regs[4]-4)
    i = next(q for q in ins if q.mnemonic == 'fptan')
    u.emu_start(dt.CODE,i.address)
    top = (u.reg_read(UC_X86_REG_FPSW)>>11)&7; m,se = u.reg_read(UC_X86_REG_FP0+top)
    assert (se&0x7fff)==0x7fff and (m&0x7fffffffffffffff), 'expected NaN input'
    u.emu_start(i.address,i.address+i.size)
    top = (u.reg_read(UC_X86_REG_FPSW)>>11)&7; m,se = u.reg_read(UC_X86_REG_FP0+top)
    assert m==0x8000000000000000 and se==0x3fff, 'expected Unicorn to push 1.0'
    print('Unicorn NaN -> 1.0:', ' ; '.join(c.body))
print('All 5 sequences reproduced; compare native FPTAN in test_x87_trig.py.')
