#!/usr/bin/env python3
# Diagnostic only: requires capstone, keystone-engine and unicorn.
# Run from the repository root; does not mask failures in difftest.
import sys,random,struct,math
sys.path.insert(0,'tools'); import difftest as dt
from unicorn import *
from unicorn.x86_const import *
from capstone import *
r=random.Random(11); data=dt.make_data(r); cases=[]
for cat in ['x87','x87t']:
 for _ in range(200):
  pro,c=dt.gen_x87(r,cat=='x87t');
  try: dt.asm(dt.PRO0+pro+c.body+dt.EPI(c))
  except Exception: continue
  c.regs=[r.choice([r.randrange(1<<32),r.randrange(256),0x80000000,0xffffffff,r.randrange(1,100)]) for _ in range(4)]+[dt.STK,0x600c00,dt.DATA,dt.DATA+0x800]
  if cat=='x87t' and (c.body == ['fsincos','fprem1','frndint','fcos','fxtract','fprem'] or c.body == ['fabs','fld1','faddp st(1), st(0)','fyl2x','fld qword ptr [esi+0x110]','frndint','fld1','fxch st(1)','fprem','fstp st(1)','f2xm1','fprem1']): cases.append((pro,c))
md=Cs(CS_ARCH_X86,CS_MODE_32)
def d80(pair):
 m,se=pair; ex=se&0x7fff
 if ex==0x7fff: return float('nan') if (m<<1)&0xffffffffffffffff else -math.inf if se&0x8000 else math.inf
 try: v=math.ldexp(m, (ex or 1)-16383-63)
 except OverflowError: v=math.inf
 return -v if se&0x8000 else v
for pro,c in cases:
 code=dt.asm(dt.PRO0+pro+c.body+dt.EPI(c)); ins=list(md.disasm(code,dt.CODE)); fps=[i for i in ins if i.mnemonic in ('fprem','fprem1')]; i=fps[-1]; addr=i.address
 u=Uc(UC_ARCH_X86,UC_MODE_32)
 for a,sz in ((0x200000,0x100000),(dt.CODE,0x200000),(dt.DATA,0x1000),(dt.OUT,0x1000)):u.mem_map(a,sz)
 u.mem_write(dt.CODE,code);u.mem_write(dt.DATA,data)
 for reg,v in zip((UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EDX,UC_X86_REG_EBX,UC_X86_REG_ESP,UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI),c.regs):u.reg_write(reg,v)
 u.reg_write(UC_X86_REG_EFLAGS,2); u.reg_write(UC_X86_REG_MXCSR,0x1f80);u.mem_write(c.regs[4]-4,struct.pack('<I',0xfffffff0));u.reg_write(UC_X86_REG_ESP,c.regs[4]-4)
 u.emu_start(dt.CODE,addr); sw0=u.reg_read(UC_X86_REG_FPSW);top=sw0>>11&7; ar=u.reg_read(UC_X86_REG_FP0+top);br=u.reg_read(UC_X86_REG_FP0+((top+1)&7));a=d80(ar);b=d80(br)
 u.emu_start(addr,addr+i.size);us=u.reg_read(UC_X86_REG_FPSW); val=d80(u.reg_read(UC_X86_REG_FP0+top))
 print(' ; '.join(c.body));print('input',a,b,'raw',ar,br,'Unicorn',val,'status',hex(us),'prior status',hex(sw0)); assert b == 0.0
assert len(cases) == 2, 'the seeded generator or dependencies changed'
