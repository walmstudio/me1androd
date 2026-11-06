#!/usr/bin/env python3
"""ME1 static recompiler: x86-32 PE -> portable C.
Stage 0 skeleton: integer ISA, flags, calls/rets, jump tables, imports via HLE thunks.
Unsupported instructions become UNIMPL() traps so coverage can be measured."""
import sys, os, struct, collections, argparse, zlib
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import *
from pefile_min import PE
import fpsse
import ehscan

md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True

REG32 = {X86_REG_EAX:'eax',X86_REG_ECX:'ecx',X86_REG_EDX:'edx',X86_REG_EBX:'ebx',
         X86_REG_ESP:'esp',X86_REG_EBP:'ebp',X86_REG_ESI:'esi',X86_REG_EDI:'edi'}
REG16 = {X86_REG_AX:'eax',X86_REG_CX:'ecx',X86_REG_DX:'edx',X86_REG_BX:'ebx',
         X86_REG_SP:'esp',X86_REG_BP:'ebp',X86_REG_SI:'esi',X86_REG_DI:'edi'}
REG8L = {X86_REG_AL:'eax',X86_REG_CL:'ecx',X86_REG_DL:'edx',X86_REG_BL:'ebx'}
REG8H = {X86_REG_AH:'eax',X86_REG_CH:'ecx',X86_REG_DH:'edx',X86_REG_BH:'ebx'}
PARENT32 = dict([(r, r) for r in REG32] + [(r16, r32) for r16, r32 in zip(REG16, REG32)] +
                [(X86_REG_AL, X86_REG_EAX), (X86_REG_CL, X86_REG_ECX), (X86_REG_DL, X86_REG_EDX), (X86_REG_BL, X86_REG_EBX),
                 (X86_REG_AH, X86_REG_EAX), (X86_REG_CH, X86_REG_ECX), (X86_REG_DH, X86_REG_EDX), (X86_REG_BH, X86_REG_EBX)])
def written_regs(i):
    """32-bit GP registers an instruction writes (explicitly or implicitly); conservative when capstone cannot tell"""
    try: w = list(i.regs_access()[1])
    except Exception: w = list(i.regs_write) + [op.reg for op in i.operands if op.type == 1]   # every register operand
    if i.id in (X86_INS_CMPXCHG, X86_INS_CMPXCHG8B): w += [X86_REG_EAX, X86_REG_EDX]   # not reported by capstone
    return set(PARENT32[r] for r in w if r in PARENT32)
SEGSEL = {X86_REG_CS:'0x1Bu',X86_REG_DS:'0x23u',X86_REG_ES:'0x23u',X86_REG_SS:'0x23u',X86_REG_FS:'0x3Bu',X86_REG_GS:'0u'}
UT = {1:'uint8_t',2:'uint16_t',4:'uint32_t'}
ST = {1:'int8_t',2:'int16_t',4:'int32_t'}
MASK = {1:'0xFFu',2:'0xFFFFu',4:'0xFFFFFFFFu'}
CC = {'o':'of','no':'!of','b':'cf','ae':'!cf','e':'zf','ne':'!zf','be':'(cf|zf)','a':'(!cf&&!zf)',
      's':'sf','ns':'!sf','p':'pf','np':'!pf','l':'(sf!=of)','ge':'(sf==of)','le':'(zf||sf!=of)','g':'(!zf&&sf==of)'}
CCALIAS = {'c':'b','nae':'b','nc':'ae','nb':'ae','z':'e','nz':'ne','na':'be','nbe':'a','pe':'p','po':'np',
           'nge':'l','nl':'ge','ng':'le','nle':'g'}

class Unsupported(Exception): pass

class Recompiler:
    def __init__(self, pe, prefix, extra_funcs=()):
        self.pe, self.prefix = pe, prefix
        self.iat = {va:(dll,name) for va,dll,name in pe.imports}
        self.cache = {}
        self.funcs = set(); self.stats = collections.Counter(); self.unimpl = collections.Counter()
        seeds = [pe.base + pe.entry_rva] + list(pe.exports.values()) + list(extra_funcs)
        # code pointers stored in data (vtables, callbacks)
        self.data_code_ptrs = set(); self.imm_code_refs = set()
        for r in pe.relocs:
            if pe.is_data(pe.base + r):
                v = pe.u32(r)
                if pe.is_code(v): self.data_code_ptrs.add(v)
        self.seeds = [a for a in seeds if pe.is_code(a)]
        self.fp_frame = False   # current function keeps a standard EBP frame -> [ebp+..] is stack memory
        self._lk = None         # inside a LOCK CAS loop: memory operand -> o_ (read) / n_ (write)
        self.eh = ehscan.EHInfo()   # exception landing targets -> owner functions (filled by discover)
        self.use_eh = True
        self.local_regs = set()   # registers holding provably stack-derived (thread-private) addresses
    # ---------- decoding ----------
    def insn(self, va):
        rva = va - self.pe.base
        try: return next(md.disasm(self.pe.img[rva:rva+16], va, 1))
        except StopIteration: return False
    # compact per-address flow info: (size, kind, targets)  kind: 0 normal,1 call,2 jmp,3 jcc,4 stop,5 indirect-jmp
    def flow(self, va):
        r = self.cache.get(va)
        if r is not None: return r
        i = self.insn(va)
        if not i: r = (0, 4, ())
        else:
            I = i.id; m = i.mnemonic
            if I == X86_INS_CALL:
                op = i.operands[0]; r = (i.size, 1, (op.imm & 0xffffffff,) if op.type == X86_OP_IMM else ())
            elif I == X86_INS_JMP:
                op = i.operands[0]
                if op.type == X86_OP_IMM: r = (i.size, 2, (op.imm & 0xffffffff,))
                else: r = (i.size, 5, tuple(self.jump_table(i) or ()))
            elif m.startswith('j') or m in ('loop', 'loope', 'loopne'):
                r = (i.size, 3, (i.operands[0].imm & 0xffffffff,))
            elif I in (X86_INS_RET, X86_INS_RETF, X86_INS_INT3, X86_INS_HLT, X86_INS_UD2): r = (i.size, 4, ())
            else:
                r = (i.size, 0, ())
                if I in (X86_INS_MOV, X86_INS_PUSH):   # `push offset f` / `mov [x], offset f`: relocated code pointers in code
                    for op in i.operands:
                        if op.type == X86_OP_IMM:
                            v = op.imm & 0xffffffff
                            if self.pe.is_code(v) and any((a - self.pe.base) in self.pe.reloc_set for a in range(va, va + i.size - 3)):
                                self.imm_code_refs.add(v)
        self.cache[va] = r
        return r
    def jump_table(self, i):
        """jmp dword ptr [reg*4 + table] -> list of targets via relocations."""
        op = i.operands[0]
        if op.type != X86_OP_MEM or op.mem.base != 0 or op.mem.index == 0 or op.mem.scale != 4: return None
        t0 = t = op.mem.disp & 0xffffffff; out = []
        # tables indexed with negative indices (CRT memcpy: `sub ecx, 4; jb ..; jmp [ecx*4 + tbl]` reads tbl[-4..-1]):
        # take up to 4 relocated code pointers right before the displacement as well
        # `neg ecx; jmp [ecx*4 + tbl]` (CRT memmove UnwindDown: ecx = 0..-7) reads up to 7 slots before tbl, so when the
        # index register was just negated, take every relocated code pointer before it (up to 15)
        b = t0 - 4; back = 16
        a0 = i.address - self.pe.base
        nreg = {X86_REG_EAX: 0, X86_REG_ECX: 1, X86_REG_EDX: 2, X86_REG_EBX: 3, X86_REG_ESP: 4, X86_REG_EBP: 5,
                X86_REG_ESI: 6, X86_REG_EDI: 7}.get(op.mem.index)
        if nreg is not None and a0 >= 2 and self.pe.img[a0 - 2] == 0xF7 and self.pe.img[a0 - 1] == 0xD8 + nreg: back = 60
        while b >= t0 - back and (b - self.pe.base) in self.pe.reloc_set and self.pe.is_code(self.pe.u32(b - self.pe.base)):
            out.append(self.pe.u32(b - self.pe.base)); b -= 4
        # tables indexed from 1 (e.g. the CRT memcpy `jmp [eax*4 + tbl]` with eax = dst & 3 != 0): slot 0 is not a
        # relocated code pointer (it overlaps the preceding code), so skip up to 3 leading slots
        for _ in range(3):
            if (t - self.pe.base) in self.pe.reloc_set or (t + 4 - self.pe.base) not in self.pe.reloc_set: break
            t += 4
        while (t - self.pe.base) in self.pe.reloc_set:
            v = self.pe.u32(t - self.pe.base)
            if not self.pe.is_code(v): break
            out.append(v); t += 4
            if out and (t - self.pe.base) in self.func_starts_rva: break
        return out or None
    # ---------- function discovery ----------
    def discover(self):
        self.func_starts_rva = set()
        work = list(self.seeds) + sorted(self.data_code_ptrs)
        self.funcs = set(); self.bodies = {}
        def closure(work):
            while True:
                while work:
                    f = work.pop()
                    if f in self.funcs or not self.pe.is_code(f): continue
                    self.funcs.add(f); self.func_starts_rva.add(f - self.pe.base)
                    for t in self.walk(f)[1]: work.append(t)
                work = sorted(self.imm_code_refs - self.funcs)
                if not work: break
        closure(work)
        if self.use_eh:
            # exception landing targets (SEH __except blocks, C++ catch continuations) become functions of their own;
            # their owners get a setjmp frame (see emit_function / runtime/hle_seh.c)
            self.eh = ehscan.scan(self, self.funcs)
            new = sorted((set(self.eh.landings) | self.eh.owners) - self.funcs)
            if new: closure(new)
        # second pass: bodies with final function set (tail calls vs gotos)
        for f in sorted(self.funcs): self.bodies[f] = self.walk(f)[0]
        self.cache.clear()
    def walk(self, f):
        seen = set(); todo = [f]; calls = set()
        while todo:
            a = todo.pop()
            while a not in seen:
                if a != f and a in self.funcs: break        # fell/jumped into another function
                size, kind, tg = self.flow(a)
                if not size: break
                seen.add(a)
                if kind == 1:
                    calls.update(tg)
                elif kind == 2:
                    t = tg[0]
                    if t in self.funcs and t != f: break   # tail call
                    a = t; continue
                elif kind == 5:
                    todo.extend(tg); break
                elif kind == 3:
                    todo.append(tg[0])
                elif kind == 4:
                    break
                a += size
        return seen, calls
    def bodies_done_guard(self): return self.funcs
    def is_noreturn(self, t):
        # import thunks to ExitProcess etc. are handled at runtime; keep simple
        return False
    # ---------- operand helpers ----------
    def addr(self, mem, i):
        parts = []
        if mem.base:
            if mem.base == X86_REG_EIP: parts.append('0x%xu' % (i.address + i.size))
            else: parts.append('c->' + REG32[mem.base])
        if mem.index: parts.append('c->%s*%du' % (REG32[mem.index], mem.scale))
        if mem.disp or not parts: parts.append('0x%xu' % (mem.disp & 0xffffffff))
        e = '(uint32_t)(' + '+'.join(parts) + ')'
        if mem.segment == X86_REG_FS: e = '(uint32_t)(c->fs_base+%s)' % e
        elif mem.segment == X86_REG_GS: raise Unsupported('gs')
        return e
    def is_stack(self, mem):
        """thread-private memory: the stack ([esp+..], [ebp+..] in standard frames), the TEB (fs:),
        or an address held in a register proven to point at stack memory."""
        if mem.segment == X86_REG_FS: return True
        if mem.base == X86_REG_ESP: return True
        if mem.base == X86_REG_EBP and self.fp_frame: return True
        if mem.base in self.local_regs: return True
        return False
    def mem_fn(self, kind, mem, size):
        # x86 TSO for shared memory: rdoN/wroN (acquire/release), plain rdN/wrN for thread-private memory
        return '%s%s%d' % (kind, '' if self.is_stack(mem) else 'o', 8 * size)
    def rd(self, op, i, size=None):
        size = size or op.size
        if op.type == X86_OP_MEM and self._lk: return 'o_' 
        if op.type == X86_OP_IMM: return '((%s)0x%xu)' % (UT[size], op.imm & ((1 << (8*size)) - 1))
        if op.type == X86_OP_REG:
            r = op.reg
            if r in REG32: return 'c->' + REG32[r]
            if r in REG16: return '((uint16_t)c->%s)' % REG16[r]
            if r in REG8L: return '((uint8_t)c->%s)' % REG8L[r]
            if r in REG8H: return '((uint8_t)(c->%s>>8))' % REG8H[r]
            if r in SEGSEL: return '((uint16_t)%s)' % SEGSEL[r]     # flat Win32 selectors
            raise Unsupported('reg ' + i.reg_name(r))
        if op.type == X86_OP_MEM: return '%s(%s)' % (self.mem_fn('rd', op.mem, size), self.addr(op.mem, i))
        raise Unsupported('op')
    def wr(self, op, i, val, size=None):
        size = size or op.size
        if op.type == X86_OP_MEM and self._lk: return 'n_ = (%s)(%s);' % (UT[size], val)
        if op.type == X86_OP_REG:
            r = op.reg
            if r in REG32: return 'c->%s = (uint32_t)(%s);' % (REG32[r], val)
            if r in REG16: return 'c->{0} = (c->{0} & 0xFFFF0000u) | (uint16_t)({1});'.format(REG16[r], val)
            if r in REG8L: return 'c->{0} = (c->{0} & 0xFFFFFF00u) | (uint8_t)({1});'.format(REG8L[r], val)
            if r in REG8H: return 'c->{0} = (c->{0} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({1}) << 8);'.format(REG8H[r], val)
            if r in SEGSEL and r != X86_REG_FS and r != X86_REG_GS: return '(void)(%s);' % val   # loads of flat selectors
            raise Unsupported('reg ' + i.reg_name(r))
        if op.type == X86_OP_MEM: return '%s(%s, %s);' % (self.mem_fn('wr', op.mem, size), self.addr(op.mem, i), val)
        raise Unsupported('wr op')
    @staticmethod
    def szf(size, res):
        b = 8*size - 1
        return 'zf = ((%s)(%s) == 0); sf = ((%s) >> %d) & 1; pf = PARITY(%s);' % (UT[size], res, res, b, res)
    # ---------- instruction translation ----------
    def tr(self, i, f, body):
        m = i.mnemonic; ops = i.operands; I = i.id; out = []
        if not self._lk and ops and (((i.prefix[0] == 0xf0) and any(o.type == X86_OP_MEM for o in ops)) or
                                     (I == X86_INS_XCHG and any(o.type == X86_OP_MEM for o in ops))):
            return self.tr_locked(i, f, body)
        S = ops[0].size if ops else 4; T = UT.get(S, 'uint32_t'); B = 8*S - 1
        nxt = i.address + i.size
        E = out.append
        # -------- track registers holding stack-derived (thread-private) addresses --------
        # v1.2: every register the instruction writes (explicit, implicit - cdq/mul/div/xchg/cmpxchg/lods/popad/cpuid -,
        # 8/16-bit sub-registers) loses the mark; only lea/mov/add/sub below can (re)establish it. EBP counts as a stack
        # base only in functions with a standard frame.
        mark = None
        if I == X86_INS_LEA and ops[0].type == X86_OP_REG and ops[0].reg in REG32:
            mem = ops[1].mem
            base_ok = mem.base == X86_REG_ESP or (mem.base == X86_REG_EBP and self.fp_frame) or mem.base in self.local_regs
            idx_ok = mem.index == 0 or mem.index in self.local_regs
            if mem.segment == 0 and base_ok and idx_ok: mark = ops[0].reg
        elif I == X86_INS_MOV and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG \
                and ops[0].reg in REG32 and (ops[1].reg in self.local_regs or ops[1].reg == X86_REG_ESP or (ops[1].reg == X86_REG_EBP and self.fp_frame)):
            mark = ops[0].reg
        elif I in (X86_INS_ADD, X86_INS_SUB) and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM \
                and ops[0].reg in self.local_regs:
            mark = ops[0].reg
        if I == X86_INS_CALL or I in (X86_INS_POPAL, X86_INS_INT, X86_INS_INT3):
            self.local_regs.clear()   # conservative: callee may clobber anything
        elif self.local_regs:
            for r in written_regs(i): self.local_regs.discard(r)
        if mark is not None: self.local_regs.add(mark)
        def label(t): return 'L_%x' % t
        def jump(t):
            if t in body: return 'goto %s;' % label(t)
            if t in self.funcs: return '{ %s_%x(c); return; }' % (self.prefix, t)
            return '{ dispatch_jmp(c, 0x%xu); return; }' % t
        def call_target(op):
            if op.type == X86_OP_IMM:
                t = op.imm & 0xffffffff
                if t in self.funcs: return '%s_%x(c);' % (self.prefix, t)
                return 'dispatch_call(c, 0x%xu);' % t
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in self.iat:
                return 'call_import(c, 0x%xu);' % (op.mem.disp & 0xffffffff)
            return 'dispatch_call(c, %s);' % self.rd(op, i, 4)
        if I in (X86_INS_MOV,):
            E(self.wr(ops[0], i, self.rd(ops[1], i)))
        elif I in (X86_INS_MOVZX, X86_INS_MOVSX):
            src = self.rd(ops[1], i)
            if I == X86_INS_MOVSX: src = '(%s)(%s)(%s)' % (UT[S], ST[ops[1].size], src)
            E(self.wr(ops[0], i, src))
        elif I == X86_INS_LEA:
            E(self.wr(ops[0], i, self.addr(ops[1].mem, i)))
        elif I == X86_INS_PUSH:
            v = self.rd(ops[0], i, 4) if ops[0].type != X86_OP_IMM else '0x%xu' % (ops[0].imm & 0xffffffff)
            if ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP: E('{ uint32_t v = c->esp; push32(c, v); }')
            elif S == 2: E('c->esp -= 2; wr16(c->esp, %s);' % self.rd(ops[0], i))
            else: E('{ uint32_t v = %s; push32(c, v); }' % v)
        elif I == X86_INS_POP:
            if S == 2: E('{ uint32_t v = rd16(c->esp); c->esp += 2; %s }' % self.wr(ops[0], i, 'v'))
            else: E('{ uint32_t v = rd32(c->esp); c->esp += 4; %s }' % self.wr(ops[0], i, 'v'))
        elif I == X86_INS_PUSHAW:
            E('{ uint32_t s = c->esp; uint32_t r[8] = {c->eax,c->ecx,c->edx,c->ebx,s,c->ebp,c->esi,c->edi}; for (int k=0;k<8;k++){ c->esp-=2; wr16(c->esp,(uint16_t)r[k]); } }')
        elif I == X86_INS_POPAW:
            E('{ uint32_t* r[8] = {&c->edi,&c->esi,&c->ebp,0,&c->ebx,&c->edx,&c->ecx,&c->eax}; for (int k=0;k<8;k++){ uint16_t v=rd16(c->esp); c->esp+=2; if (r[k]) *r[k]=(*r[k]&0xffff0000u)|v; } }')
        elif I == X86_INS_PUSHAL:
            E('{ uint32_t s = c->esp; push32(c,c->eax); push32(c,c->ecx); push32(c,c->edx); push32(c,c->ebx); push32(c,s); push32(c,c->ebp); push32(c,c->esi); push32(c,c->edi); }')
        elif I == X86_INS_POPAL:
            E('c->edi=pop32(c); c->esi=pop32(c); c->ebp=pop32(c); c->esp+=4; c->ebx=pop32(c); c->edx=pop32(c); c->ecx=pop32(c); c->eax=pop32(c);')
        elif I == X86_INS_PUSHFD:
            E('push32(c, MKFLAGS());')
        elif I == X86_INS_POPFD:
            E('{ uint32_t v = pop32(c); SETFLAGS(v); }')
        elif I in (X86_INS_ADD, X86_INS_SUB, X86_INS_CMP, X86_INS_ADC, X86_INS_SBB):
            a = self.rd(ops[0], i); b = self.rd(ops[1], i, S)
            cin = ' + cf' if I == X86_INS_ADC else (' - cf' if I == X86_INS_SBB else '')
            if I in (X86_INS_ADD, X86_INS_ADC):
                E('{ %s a = %s, b = (%s)%s; uint64_t w = (uint64_t)a + b%s; %s r = (%s)w; cf = (w >> %d) & 1; of = (((a ^ r) & (b ^ r)) >> %d) & 1; %s %s }'
                  % (T, a, T, b, cin, T, T, 8*S, B, self.szf(S, 'r'), self.wr(ops[0], i, 'r')))
            else:
                st = '' if I == X86_INS_CMP else self.wr(ops[0], i, 'r')
                E('{ %s a = %s, b = (%s)%s; uint64_t w = (uint64_t)a - b%s; %s r = (%s)w; cf = (w >> %d) & 1; of = (((a ^ b) & (a ^ r)) >> %d) & 1; %s %s }'
                  % (T, a, T, b, cin, T, T, 8*S if S < 4 else 63, B, self.szf(S, 'r'), st))
        elif I in (X86_INS_AND, X86_INS_OR, X86_INS_XOR, X86_INS_TEST):
            opc = {X86_INS_AND:'&', X86_INS_TEST:'&', X86_INS_OR:'|', X86_INS_XOR:'^'}[I]
            if I == X86_INS_XOR and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
                E('%s cf = of = 0; zf = 1; sf = 0; pf = 1;' % self.wr(ops[0], i, '0'))
            else:
                st = '' if I == X86_INS_TEST else self.wr(ops[0], i, 'r')
                E('{ %s r = (%s)(%s %s (%s)%s); cf = of = 0; %s %s }' % (T, T, self.rd(ops[0], i), opc, T, self.rd(ops[1], i, S), self.szf(S, 'r'), st))
        elif I in (X86_INS_INC, X86_INS_DEC):
            d = '+' if I == X86_INS_INC else '-'
            ovf = ('0x%x' % (1 << B)) if I == X86_INS_INC else ('0x%x' % ((1 << B) - 1))
            E('{ %s r = (%s)(%s %s 1); of = (r == (%s)%s); %s %s }' % (T, T, self.rd(ops[0], i), d, T, ovf, self.szf(S, 'r'), self.wr(ops[0], i, 'r')))
        elif I == X86_INS_NEG:
            E('{ %s a = %s; %s r = (%s)(0 - a); cf = (a != 0); of = (a == (%s)0x%x); %s %s }' % (T, self.rd(ops[0], i), T, T, T, 1 << B, self.szf(S, 'r'), self.wr(ops[0], i, 'r')))
        elif I == X86_INS_NOT:
            E(self.wr(ops[0], i, '~%s' % self.rd(ops[0], i)))
        elif I in (X86_INS_SHL, X86_INS_SAL, X86_INS_SHR, X86_INS_SAR):
            cnt = self.rd(ops[1], i, 1) if len(ops) > 1 else '1'
            a = self.rd(ops[0], i)
            if I in (X86_INS_SHL, X86_INS_SAL):
                body_ = 'r = (%s)(a << n); cf = (a >> (%d - n)) & 1; of = ((r >> %d) & 1) ^ cf;' % (T, 8*S, B)
            elif I == X86_INS_SHR:
                body_ = 'r = (%s)(a >> n); cf = (a >> (n - 1)) & 1; of = (a >> %d) & 1;' % (T, B)
            else:
                body_ = 'r = (%s)((%s)a >> n); cf = ((%s)a >> (n - 1)) & 1; of = 0;' % (T, ST[S], ST[S])
            E('{ unsigned n = (%s) & 31; if (n) { %s a = %s, r; %s %s %s } }' % (cnt, T, a, body_, self.szf(S, 'r'), self.wr(ops[0], i, 'r')))
        elif I in (X86_INS_ROL, X86_INS_ROR):
            cnt = self.rd(ops[1], i, 1) if len(ops) > 1 else '1'
            a = self.rd(ops[0], i)
            if I == X86_INS_ROL:
                e = 'r = n ? (%s)((a << n) | (a >> (%d - n))) : a; cf = r & 1; if (m == 1) of = ((r >> %d) & 1) ^ cf;' % (T, 8*S, B)
            else:
                e = 'r = n ? (%s)((a >> n) | (a << (%d - n))) : a; cf = (r >> %d) & 1; if (m == 1) of = ((r >> %d) ^ (r >> %d)) & 1;' % (T, 8*S, B, B, B - 1)
            E('{ unsigned m = (%s) & 31; if (m) { unsigned n = m %% %d; %s a = %s, r; %s %s } }' % (cnt, 8*S, T, a, e, self.wr(ops[0], i, 'r')))
        elif I in (X86_INS_RCL, X86_INS_RCR):
            cnt = self.rd(ops[1], i, 1) if len(ops) > 1 else '1'
            step = ('of_ = (a >> %d) & 1; a = (%s)((a << 1) | cf); cf = of_;' % (B, T)) if I == X86_INS_RCL else \
                   ('of_ = a & 1; a = (%s)((a >> 1) | ((%s)cf << %d)); cf = of_;' % (T, T, B))
            ofx = 'of = ((a >> %d) & 1) ^ cf;' % B if I == X86_INS_RCL else 'of = ((a >> %d) ^ (a >> %d)) & 1;' % (B, B - 1)
            E('{ unsigned n = ((%s) & 31) %% %d; if (n) { %s a = %s; unsigned of_; while (n--) { %s } %s %s } }'
              % (cnt, 8*S + 1, T, self.rd(ops[0], i), step, ofx, self.wr(ops[0], i, 'a')))
        elif I in (X86_INS_SHLD, X86_INS_SHRD):
            cnt = self.rd(ops[2], i, 1)
            a, b = self.rd(ops[0], i), self.rd(ops[1], i)
            if S == 4:
                if I == X86_INS_SHLD: e = 'r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1;'
                else: e = 'r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1;'
                E('{ unsigned n = (%s) & 31; if (n) { uint32_t a = %s, b = %s, r; %s of = ((r ^ a) >> 31) & 1; %s %s } }' % (cnt, a, b, e, self.szf(4, 'r'), self.wr(ops[0], i, 'r')))
            elif S == 2:   # 16-bit: operate on the 32-bit concatenation (counts > 16 are architecturally undefined)
                if I == X86_INS_SHLD: e = 'uint64_t t = ((uint64_t)a << 32) | ((uint64_t)b << 16) | a; r = (uint16_t)(t >> (32 - n)); cf = (t >> (32 - n + 16)) & 1;'
                else: e = 'uint64_t t = ((uint64_t)a << 32) | ((uint64_t)b << 16) | a; r = (uint16_t)(t >> n); cf = (t >> (n - 1)) & 1;'
                E('{ unsigned n = (%s) & 31; if (n) { uint16_t a = %s, b = %s, r; %s of = ((r ^ a) >> 15) & 1; %s %s } }' % (cnt, a, b, e, self.szf(2, 'r'), self.wr(ops[0], i, 'r')))
            else: raise Unsupported('shxd8')
        elif I == X86_INS_IMUL:
            if len(ops) == 1:
                if S == 4: E('{ int64_t w = (int64_t)(int32_t)c->eax * (int32_t)%s; c->eax = (uint32_t)w; c->edx = (uint32_t)(w >> 32); cf = of = (w != (int32_t)w); }' % self.rd(ops[0], i))
                elif S == 1: E('{ int16_t w = (int16_t)(int8_t)c->eax * (int8_t)%s; c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)w; cf = of = (w != (int8_t)w); }' % self.rd(ops[0], i))
                else: E('{ int32_t w = (int32_t)(int16_t)c->eax * (int16_t)%s; c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)w; c->edx = (c->edx & 0xFFFF0000u) | (uint16_t)(w >> 16); cf = of = (w != (int16_t)w); }' % self.rd(ops[0], i))
            else:
                a = self.rd(ops[1], i) if len(ops) == 3 else self.rd(ops[0], i)
                b = self.rd(ops[2], i, S) if len(ops) == 3 else self.rd(ops[1], i, S)
                E('{ int64_t w = (int64_t)(%s)%s * (int64_t)(%s)%s; %s r = (%s)w; cf = of = (w != (%s)w); %s %s }' % (ST[S], a, ST[S], b, T, T, ST[S], self.szf(S, 'r'), self.wr(ops[0], i, 'r')))
        elif I == X86_INS_MUL:
            if S == 4: E('{ uint64_t w = (uint64_t)c->eax * %s; c->eax = (uint32_t)w; c->edx = (uint32_t)(w >> 32); cf = of = (c->edx != 0); }' % self.rd(ops[0], i))
            elif S == 1: E('{ uint16_t w = (uint16_t)((uint8_t)c->eax * %s); c->eax = (c->eax & 0xFFFF0000u) | w; cf = of = (w >> 8) != 0; }' % self.rd(ops[0], i))
            else: E('{ uint32_t w = (uint32_t)(uint16_t)c->eax * %s; c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)w; c->edx = (c->edx & 0xFFFF0000u) | (w >> 16); cf = of = (w >> 16) != 0; }' % self.rd(ops[0], i))
        elif I in (X86_INS_DIV, X86_INS_IDIV):
            v = self.rd(ops[0], i)
            if S != 4: raise Unsupported('div%d' % S)
            # #DE: divide by zero or quotient overflow -> STATUS_INTEGER_DIVIDE_BY_ZERO / STATUS_INTEGER_OVERFLOW via SEH
            if I == X86_INS_DIV: E('{ uint64_t n = ((uint64_t)c->edx << 32) | c->eax; uint32_t d = %s; if (!d) rt_raise_hw(c, 0xC0000094u, 0x%xu); uint64_t q = n / d; if (q >> 32) rt_raise_hw(c, 0xC0000095u, 0x%xu); c->eax = (uint32_t)q; c->edx = (uint32_t)(n %% d); }' % (v, i.address, i.address))
            else: E('{ int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); int32_t d = (int32_t)%s; if (!d) rt_raise_hw(c, 0xC0000094u, 0x%xu); if (d == -1 && n == INT64_MIN) rt_raise_hw(c, 0xC0000095u, 0x%xu); int64_t q = n / d; if (q != (int32_t)q) rt_raise_hw(c, 0xC0000095u, 0x%xu); c->eax = (uint32_t)(int32_t)q; c->edx = (uint32_t)(int32_t)(n %% d); }' % (v, i.address, i.address, i.address))
        elif I == X86_INS_CDQ: E('c->edx = (uint32_t)((int32_t)c->eax >> 31);')
        elif I == X86_INS_CWDE: E('c->eax = (uint32_t)(int32_t)(int16_t)c->eax;')
        elif I == X86_INS_CBW: E('c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)c->eax;')
        elif I == X86_INS_CWD: E('c->edx = (c->edx & 0xFFFF0000u) | (uint16_t)((int16_t)c->eax >> 15);')
        elif I == X86_INS_XCHG:
            E('{ %s a = %s, b = %s; %s %s }' % (T, self.rd(ops[0], i), self.rd(ops[1], i), self.wr(ops[0], i, 'b'), self.wr(ops[1], i, 'a')))
        elif I == X86_INS_XADD:
            E('{ %s a = %s, b = %s; %s r = (%s)(a + b); cf = r < a; of = (((a ^ r) & (b ^ r)) >> %d) & 1; %s %s %s }' % (T, self.rd(ops[0], i), self.rd(ops[1], i), T, T, B, self.szf(S, 'r'), self.wr(ops[1], i, 'a'), self.wr(ops[0], i, 'r')))
        elif I == X86_INS_CMPXCHG:
            acc = {1:'(uint8_t)c->eax', 2:'(uint16_t)c->eax', 4:'c->eax'}[S]
            accw = {1:'c->eax = (c->eax & 0xFFFFFF00u) | (uint8_t)d;', 2:'c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)d;', 4:'c->eax = d;'}[S]
            E('{ %s d = %s, a = %s; %s r = (%s)(a - d); cf = a < d; of = (((a ^ d) & (a ^ r)) >> %d) & 1; %s if (zf) { %s } else { %s } }'
              % (T, self.rd(ops[0], i), acc, T, T, B, self.szf(S, 'r'), self.wr(ops[0], i, self.rd(ops[1], i)), accw))
        elif I == X86_INS_BSWAP:
            E(self.wr(ops[0], i, '__builtin_bswap32(%s)' % self.rd(ops[0], i)))
        elif I in (X86_INS_BT, X86_INS_BTS, X86_INS_BTR, X86_INS_BTC):
            if ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_REG:
                # bit string addressing: the register offset is signed and may reach outside the operand
                ea = self.addr(ops[0].mem, i); o = self.rd(ops[1], i, S)
                so = '(int32_t)%s' % o if S == 4 else '(int32_t)(int16_t)%s' % o
                W_, R_ = self.mem_fn('wr', ops[0].mem, S), self.mem_fn('rd', ops[0].mem, S)
                upd = {X86_INS_BT:'', X86_INS_BTS:'%s(a_, v | ((%s)1 << n));' % (W_, T), X86_INS_BTR:'%s(a_, v & ~((%s)1 << n));' % (W_, T), X86_INS_BTC:'%s(a_, v ^ ((%s)1 << n));' % (W_, T)}[I]
                E('{ int32_t o_ = %s; uint32_t a_ = %s + (uint32_t)((o_ >> %d) * %d); unsigned n = (unsigned)o_ & %d; %s v = %s(a_); cf = (v >> n) & 1; %s }'
                  % (so, ea, {2:4, 4:5}[S], S, 8*S - 1, T, R_, upd))
                return out
            a = self.rd(ops[0], i); n = '((%s) & %d)' % (self.rd(ops[1], i, S if ops[1].type == X86_OP_REG else 1), 8*S - 1)
            upd = {X86_INS_BT:'', X86_INS_BTS:self.wr(ops[0], i, 'a | ((%s)1 << n)' % T), X86_INS_BTR:self.wr(ops[0], i, 'a & ~((%s)1 << n)' % T), X86_INS_BTC:self.wr(ops[0], i, 'a ^ ((%s)1 << n)' % T)}[I]
            E('{ %s a = %s; unsigned n = %s; cf = (a >> n) & 1; %s }' % (T, a, n, upd))
        elif I in (X86_INS_BSF, X86_INS_BSR):
            fn = '__builtin_ctz(v)' if I == X86_INS_BSF else '(31 - __builtin_clz(v))'
            E('{ uint32_t v = %s; zf = (v == 0); if (v) { %s } }' % (self.rd(ops[1], i), self.wr(ops[0], i, fn)))
        elif m.startswith('set'):
            cc = m[3:]; cc = CCALIAS.get(cc, cc)
            E(self.wr(ops[0], i, '(%s) ? 1 : 0' % CC[cc]))
        elif m.startswith('cmov'):
            cc = m[4:]; cc = CCALIAS.get(cc, cc)
            E('if (%s) { %s }' % (CC[cc], self.wr(ops[0], i, self.rd(ops[1], i))))
        elif I == X86_INS_LEAVE:
            E('c->esp = c->ebp; c->ebp = pop32(c);')
        elif I == X86_INS_NOP or m in ('prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2'):
            pass
        elif m == 'pause': E('RT_PAUSE();')
        elif m == 'mfence': E('__atomic_thread_fence(__ATOMIC_SEQ_CST);')
        elif m == 'lfence': E('__atomic_thread_fence(__ATOMIC_ACQUIRE);')
        elif m == 'sfence': E('__atomic_thread_fence(__ATOMIC_RELEASE);')
        elif I == X86_INS_CLD: E('c->df = 0;')
        elif I == X86_INS_STD: E('c->df = 1;')
        elif I == X86_INS_CLC: E('cf = 0;')
        elif I == X86_INS_STC: E('cf = 1;')
        elif I == X86_INS_CMC: E('cf = !cf;')
        elif I == X86_INS_SAHF: E('{ uint8_t h = (uint8_t)(c->eax >> 8); cf = h & 1; pf = (h >> 2) & 1; zf = (h >> 6) & 1; sf = (h >> 7) & 1; }')
        elif I == X86_INS_LAHF: E('c->eax = (c->eax & 0xFFFF00FFu) | ((uint32_t)(cf | 2 | (pf << 2) | (zf << 6) | (sf << 7)) << 8);')
        elif I in (X86_INS_MOVSB, X86_INS_MOVSW, X86_INS_MOVSD, X86_INS_STOSB, X86_INS_STOSW, X86_INS_STOSD,
                   X86_INS_LODSB, X86_INS_LODSW, X86_INS_LODSD, X86_INS_SCASB, X86_INS_SCASW, X86_INS_SCASD,
                   X86_INS_CMPSB, X86_INS_CMPSW, X86_INS_CMPSD) and not any(o.type == X86_OP_REG and o.reg >= X86_REG_XMM0 for o in ops):
            kind = m.split()[-1][:4]; w = {'b':1,'w':2,'d':4}[m.split()[-1][4]]
            rep = 0xf3 in i.prefix and 'rep' or (0xf2 in i.prefix and 'repne' or '')
            E('STR_%s%s(%d);' % (kind.upper(), '_' + rep.upper() if rep else '', w))
        elif m.startswith('j') and I != X86_INS_JMP:
            t = ops[0].imm & 0xffffffff
            if I == X86_INS_JECXZ: E('if (c->ecx == 0) %s' % jump(t))
            elif I == X86_INS_JCXZ: E('if ((uint16_t)c->ecx == 0) %s' % jump(t))
            else:
                cc = m[1:]; cc = CCALIAS.get(cc, cc)
                E('if (%s) %s' % (CC[cc], jump(t)))
        elif I in (X86_INS_LOOP, X86_INS_LOOPE, X86_INS_LOOPNE):
            t = ops[0].imm & 0xffffffff
            cond = {X86_INS_LOOP:'c->ecx', X86_INS_LOOPE:'c->ecx && zf', X86_INS_LOOPNE:'c->ecx && !zf'}[I]
            E('c->ecx--; if (%s) %s' % (cond, jump(t)))
        elif I == X86_INS_JMP:
            op = ops[0]
            if op.type == X86_OP_IMM: E(jump(op.imm & 0xffffffff))
            else:
                jt = self.jump_table(i)
                if jt and all(t in body for t in jt):
                    E('switch (%s) {' % self.rd(op, i, 4))
                    for t in sorted(set(jt)): E('  case 0x%xu: goto %s;' % (t, label(t)))
                    E('  default: dispatch_jmp(c, %s); return; }' % self.rd(op, i, 4))
                elif op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in self.iat:
                    E('{ call_import(c, 0x%xu); return; }' % (op.mem.disp & 0xffffffff))
                else:
                    E('{ uint32_t t = %s; dispatch_jmp(c, t); return; }' % self.rd(op, i, 4))
        elif I == X86_INS_CALL:
            op = ops[0]
            if op.type == X86_OP_IMM and (op.imm & 0xffffffff) == nxt:   # call $+5 ; pop (PIC)
                E('push32(c, 0x%xu);' % nxt)
            else:
                tgt = self.rd(op, i, 4) if op.type != X86_OP_IMM else None
                if tgt and op.type == X86_OP_MEM and not (op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in self.iat):
                    E('{ uint32_t t = %s; push32(c, 0x%xu); dispatch_call(c, t); }' % (tgt, nxt))
                elif tgt and op.type == X86_OP_REG:
                    E('{ uint32_t t = %s; push32(c, 0x%xu); dispatch_call(c, t); }' % (tgt, nxt))
                else:
                    E('push32(c, 0x%xu); %s' % (nxt, call_target(op)))
                E('CHECK_RET(c, 0x%xu);' % nxt)
        elif I == X86_INS_RET:
            n = ops[0].imm if ops else 0
            E('c->esp += %d; return;' % (4 + n))
        elif I == X86_INS_INT3:
            E('rt_raise_hw(c, 0x80000003u, 0x%xu); return;' % i.address)          # EXCEPTION_BREAKPOINT
        elif I == X86_INS_HLT or I == X86_INS_UD2:
            E('rt_raise_hw(c, %s, 0x%xu); return;' % ('0xC0000096u' if I == X86_INS_HLT else '0xC000001Du', i.address))
        elif I == X86_INS_CPUID: E('rt_cpuid(c);')
        elif I == X86_INS_XLATB: E('c->eax = (c->eax & 0xFFFFFF00u) | rd8(c->ebx + (uint8_t)c->eax);')
        elif I in (X86_INS_IN, X86_INS_OUT, X86_INS_INSB, X86_INS_INSD, X86_INS_OUTSB, X86_INS_OUTSD, X86_INS_OUTSW, X86_INS_INSW):
            E('rt_fault(c, 0x%xu, "port I/O"); return;' % i.address)
        elif I == X86_INS_RDTSC: E('rt_rdtsc(c);')
        elif I in (X86_INS_INT,):
            E('rt_fault(c, 0x%xu, "int"); return;' % i.address)
        else:
            r = fpsse.tr(self, i, Unsupported)
            if r is None: raise Unsupported(m)
            out.extend(r)
        return out
    def tr_locked(self, i, f, body):
        """LOCK-prefixed read-modify-write and XCHG with memory: atomic (seq_cst) on the host."""
        ops = i.operands; I = i.id
        k = next(j for j, o in enumerate(ops) if o.type == X86_OP_MEM); mem = ops[k]
        S = mem.size; N = 8 * S; B = N - 1; A = self.addr(mem.mem, i)
        if I == X86_INS_CMPXCHG8B:
            return ['{ uint32_t la_ = %s; uint64_t e_ = ((uint64_t)c->edx << 32) | c->eax; zf = lk_cas64(la_, &e_, ((uint64_t)c->ecx << 32) | c->ebx); '
                    'if (!zf) { c->eax = (uint32_t)e_; c->edx = (uint32_t)(e_ >> 32); } }' % A]
        T = UT[S]
        if I == X86_INS_XCHG:
            r = ops[1 - k]
            return ['{ uint32_t la_ = %s; %s o_ = lk_xchg%d(la_, (%s)%s); %s }' % (A, T, N, T, self.rd(r, i), self.wr(r, i, 'o_'))]
        if I == X86_INS_XADD:
            return ['{ uint32_t la_ = %s; %s b = (%s)%s; %s a = lk_xadd%d(la_, b); %s r = (%s)(a + b); cf = r < a; of = (((a ^ r) & (b ^ r)) >> %d) & 1; %s %s }'
                    % (A, T, T, self.rd(ops[1], i), T, N, T, T, B, self.szf(S, 'r'), self.wr(ops[1], i, 'a'))]
        if I == X86_INS_CMPXCHG:
            acc = {1:'(uint8_t)c->eax', 2:'(uint16_t)c->eax', 4:'c->eax'}[S]
            accw = {1:'c->eax = (c->eax & 0xFFFFFF00u) | (uint8_t)d;', 2:'c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)d;', 4:'c->eax = d;'}[S]
            return ['{ uint32_t la_ = %s; %s a = %s, d = a, s_ = (%s)%s; lk_cas%d(la_, &d, s_); %s r = (%s)(a - d); cf = a < d; of = (((a ^ d) & (a ^ r)) >> %d) & 1; %s if (!zf) { %s } }'
                    % (A, T, acc, T, self.rd(ops[1], i), N, T, T, B, self.szf(S, 'r'), accw)]
        if I in (X86_INS_BTS, X86_INS_BTR, X86_INS_BTC):
            if ops[1].type == X86_OP_IMM:
                pre = 'uint32_t a_ = %s; unsigned n = %du;' % (A, ops[1].imm & B)
            else:   # bit-string addressing with a signed register offset
                o = self.rd(ops[1], i, S); so = '(int32_t)%s' % o if S == 4 else '(int32_t)(int16_t)%s' % o
                pre = 'int32_t o_ = %s; uint32_t a_ = %s + (uint32_t)((o_ >> %d) * %d); unsigned n = (unsigned)o_ & %d;' % (so, A, {2:4, 4:5}[S], S, B)
            fn = {X86_INS_BTS:'lk_or%d(a_, (%s)((%s)1 << n))', X86_INS_BTR:'lk_and%d(a_, (%s)~((%s)1 << n))', X86_INS_BTC:'lk_xor%d(a_, (%s)((%s)1 << n))'}[I] % (N, T, T)
            return ['{ %s %s v = %s; cf = (v >> n) & 1; }' % (pre, T, fn)]
        if I in (X86_INS_ADD, X86_INS_SUB, X86_INS_ADC, X86_INS_SBB, X86_INS_AND, X86_INS_OR, X86_INS_XOR,
                 X86_INS_INC, X86_INS_DEC, X86_INS_NEG, X86_INS_NOT) and k == 0:
            # generic: compute on o_ (snapshot), publish n_ with CAS, retry on contention. The body only writes
            # flags and the memory operand, so re-running it is safe (cf is restored for adc/sbb).
            self._lk = mem
            try: inner = self.tr(i, f, body)
            finally: self._lk = None
            return ['{ uint32_t la_ = %s; unsigned cfi_ = cf; %s o_ = rd%d(la_), n_; (void)cfi_; do { cf = cfi_; n_ = o_; %s } while (!lk_cas%d(la_, &o_, n_)); }'
                    % (A, T, N, ' '.join(inner), N)]
        raise Unsupported('lock ' + i.mnemonic)
    def frame_kind(self, f, body):
        """True if f starts with push ebp; mov ebp, esp and never writes EBP otherwise (except pop ebp / leave)."""
        i0 = body.get(f)
        if not i0 or i0.id != X86_INS_PUSH or i0.operands[0].type != X86_OP_REG or i0.operands[0].reg != X86_REG_EBP: return False
        i1 = body.get(f + i0.size)
        if not i1 or i1.id != X86_INS_MOV or [o.reg if o.type == X86_OP_REG else None for o in i1.operands] != [X86_REG_EBP, X86_REG_ESP]: return False
        for a, i in body.items():
            if a == f + i0.size or i.id == X86_INS_LEAVE or (i.id == X86_INS_POP and i.operands[0].type == X86_OP_REG and i.operands[0].reg == X86_REG_EBP): continue
            try: w = i.regs_access()[1]
            except Exception: return False
            if X86_REG_EBP in w or X86_REG_BP in w: return False
        return True
    def is_terminator(self, i):
        if i.id in (X86_INS_RET, X86_INS_INT3, X86_INS_HLT, X86_INS_UD2): return True
        if i.id == X86_INS_JMP: return True
        return False
    # ---------- emission ----------
    def emit_function(self, f):
        body = {}
        for a in self.bodies[f]:
            i = self.insn(a)
            if i: body[a] = i
        targets = {f}
        for a, i in body.items():
            if i.id == X86_INS_JMP or (i.mnemonic.startswith('j') or i.mnemonic.startswith('loop')):
                if i.operands and i.operands[0].type == X86_OP_IMM: targets.add(i.operands[0].imm & 0xffffffff)
                elif i.id == X86_INS_JMP:
                    jt = self.jump_table(i)
                    if jt: targets.update(jt)
        self.fp_frame = self.frame_kind(f, body)
        lines = []
        if f in self.eh.owners:
            # owner of exception landing targets: the runtime longjmps back into this frame and the landing code runs as
            # a tail call of it (frame matched by owner + EBP, see rt_eh_land)
            lands = sorted(t for t, (o, k) in self.eh.landings.items() if o == f)
            lines += ['static void %s_%x_b(CPU* c);' % (self.prefix, f),
                      'void %s_%x(CPU* c) {' % (self.prefix, f),
                      '  RtEhFrame fr_; rt_eh_enter(c, &fr_, 0x%xu);' % f,
                      '  if (!_setjmp(fr_.jb)) %s_%x_b(c);' % (self.prefix, f),
                      '  else switch (rt_eh_target(&fr_)) {']
            lines += ['    case 0x%xu: %s_%x(c); break;' % (t, self.prefix, t) for t in lands]
            lines += ['    default: rt_fault(c, rt_eh_target(&fr_), "bad exception landing"); }',
                      '  rt_eh_leave(&fr_);', '}',
                      'static void %s_%x_b(CPU* c) {' % (self.prefix, f), '  FLAGS_DECL;']
        else:
            lines = ['void %s_%x(CPU* c) {' % (self.prefix, f), '  FLAGS_DECL;']
        addrs = sorted(body)
        if not addrs:   # start address does not decode (data reached through a relocation, e.g. .rdata merged into .text)
            return ['void %s_%x(CPU* c) {' % (self.prefix, f), '  UNIMPL(c, 0x%xu, "not code"); return;' % f, '}']
        if addrs[0] != f: lines.append('  goto L_%x;' % f)
        for k, a in enumerate(addrs):
            i = body[a]
            if a in targets:
                lines.append('L_%x:' % a)
                self.local_regs.clear()   # control may jump here from anywhere: drop local-register knowledge
            lines.append('  /* %08x: %s %s */' % (a, i.mnemonic, i.op_str.replace('*/', '* /')))
            self.stats[i.mnemonic] += 1
            try:
                try: tl = self.tr(i, f, body)
                except (KeyError, IndexError) as e: raise Unsupported(i.mnemonic)
                for l in tl: lines.append('  ' + l)
            except Unsupported as e:
                self.unimpl[str(e)] += 1
                lines.append('  UNIMPL(c, 0x%xu, "%s %s"); return;' % (a, i.mnemonic, i.op_str))
                continue
            if not self.is_terminator(i):
                nxt = a + i.size
                if nxt not in body:
                    if nxt in self.funcs: lines.append('  { %s_%x(c); return; }' % (self.prefix, nxt))
                    else: lines.append('  dispatch_jmp(c, 0x%xu); return;' % nxt)
                elif k + 1 >= len(addrs) or addrs[k + 1] != nxt:
                    lines.append('  goto L_%x;' % nxt)
        lines.append('}')
        return lines

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pe'); ap.add_argument('outdir'); ap.add_argument('--prefix', default='f')
    ap.add_argument('--chunk', type=int, default=400)
    ap.add_argument('--base', type=lambda x:int(x,16), default=None)
    ap.add_argument('--seeds', default=None, help='file with extra function start VAs (hex)')
    ap.add_argument('--stats-only', action='store_true')
    ap.add_argument('--no-eh', action='store_true', help='do not scan SEH/C++ exception metadata')
    ap.add_argument('--code-ranges', default=None, help='lo-hi[,lo-hi] code RVA ranges (hex) when .rdata is merged into .text')
    a = ap.parse_args()
    cr = [tuple(int(x, 16) for x in r.split('-')) for r in a.code_ranges.split(',')] if a.code_ranges else None
    pe = PE(a.pe, a.base, cr)
    extra = [int(x, 16) for x in open(a.seeds).read().split()] if a.seeds else []
    rc = Recompiler(pe, a.prefix, extra)
    rc.use_eh = not a.no_eh
    rc.discover()
    os.makedirs(a.outdir, exist_ok=True)
    funcs = sorted(rc.funcs)
    decls = ['#pragma once', '#include "rt.h"'] + ['void %s_%x(CPU* c);' % (a.prefix, f) for f in funcs]
    def put(name, text, mode='w'):   # keep unchanged files untouched so make only rebuilds what changed
        path = os.path.join(a.outdir, name)
        if os.path.exists(path):
            with open(path, 'rb' if 'b' in mode else 'r') as f:
                if f.read() == text: return
        with open(path, mode) as f: f.write(text)
    put('funcs.h', '\n'.join(decls) + '\n')
    for n in range(0, len(funcs), a.chunk):
        L = ['#include "funcs.h"']
        for f in funcs[n:n + a.chunk]: L += rc.emit_function(f)
        if not a.stats_only:
            put('code_%03d.c' % (n // a.chunk), '\n'.join(L) + '\n')
    # module table: functions, imports, exports
    T = ['#include "funcs.h"', 'const FuncEntry %s_funcs[] = {' % a.prefix]
    L_ = rc.eh.landings   # landing targets carry their owner (runtime: exception transfers longjmp to the owner's frame)
    T += [('  {0x%xu, %s_%x, 0x%xu},' % (f, a.prefix, f, L_[f][0])) if f in L_ else ('  {0x%xu, %s_%x},' % (f, a.prefix, f)) for f in funcs] + ['};']
    T += ['const unsigned %s_nfuncs = %d;' % (a.prefix, len(funcs))]
    T += ['const ImportEntry %s_imports[] = {' % a.prefix]
    T += ['  {0x%xu, "%s", "%s"},' % (va, dll, nm) for va, dll, nm in pe.imports] + ['};']
    T += ['const unsigned %s_nimports = %d;' % (a.prefix, len(pe.imports))]
    ex = list(pe.exports.items())
    ex += [('#%d' % pe.export_ords[k], v) for k, v in pe.exports.items() if not k.startswith('#') and k in pe.export_ords]   # ordinal aliases
    T += ['const ExportEntry %s_exports[] = {' % a.prefix] + ['  {"%s", 0x%xu},' % (k, v) for k, v in ex] + ['  {0,0}};']
    T += ['const uint32_t %s_base = 0x%xu, %s_entry = 0x%xu;' % (a.prefix, pe.base, a.prefix, pe.base + pe.entry_rva)]
    T += ['const uint32_t %s_image_size = 0x%xu;' % (a.prefix, len(pe.img))]
    T += ['const uint32_t %s_image_crc = 0x%08xu;   /* CRC-32 of image.bin: checks that original game files match this build */' % (a.prefix, zlib.crc32(bytes(pe.img)))]
    put('module.c', '\n'.join(T) + '\n')
    put('image.bin', bytes(pe.img), 'wb')
    tot = sum(rc.stats.values()); un = sum(rc.unimpl.values())
    print('functions: %d  instructions: %d  unsupported: %d (%.2f%%)' % (len(funcs), tot, un, 100.0 * un / max(tot, 1)))
    print('exceptions: %d landing targets in %d owner functions %s' % (len(rc.eh.landings), len(rc.eh.owners), rc.eh.stats))
    for k, v in rc.unimpl.most_common(25): print('   %6d  %s' % (v, k))

if __name__ == '__main__': main()
