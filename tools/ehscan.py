"""Exception-handling metadata scan for the recompiler.

Finds *landing targets*: code addresses that are entered by a non-local transfer from the exception machinery
(SEH __except blocks reached from _except_handler3/4, C++ catch continuations reached from _JumpToContinuation),
and for each one the *owner*: the function whose still-active frame the transfer resumes.

  SEH  (EH3/EH4 scope tables)   owner F pushes the table address in its prologue (inline or via __SEH_prolog4);
                                records with a filter (__except) give landing targets (the handler address).
  C++  (FuncInfo 0x1993052x)    owner F references a handler stub `mov eax, offset FuncInfo; jmp __CxxFrameHandler3`
                                in its prologue; catch funclets end with `mov eax, offset continuation; ret`.
                                A continuation inside another catch funclet of the same function (try/catch nested in
                                a catch block) is owned by that funclet.

At run time a landing target is reached through dispatch (jmp/call reg), the runtime finds the innermost active
frame of its owner and longjmps there (see runtime/hle_seh.c)."""
from capstone.x86 import *

KIND_SEH, KIND_CXX = 1, 2
CXX_MAGIC = (0x19930520, 0x19930521, 0x19930522)

class EHInfo:
    def __init__(self):
        self.landings = {}      # target -> (owner, kind)
        self.owners = set()
        self.conflicts = []
        self.stats = dict(seh_tables=0, seh_targets=0, cxx_funcinfo=0, cxx_try=0, cxx_targets=0, conflicts=0)

def scan(rc, funcs):
    pe = rc.pe; base = pe.base; info = EHInfo()
    def inimg(va): return 0 <= va - base <= len(pe.img) - 4
    def u32(va): return pe.u32(va - base)
    def reloc(va): return (va - base) in pe.reloc_set
    def is_data(va):
        if not inimg(va): return False
        return pe.is_data(va)
    def code_at(va):   # relocated pointer to code stored at va
        return inimg(va) and reloc(va) and pe.is_code(u32(va))
    def add(t, owner, kind):
        old = info.landings.get(t)
        if old and old != (owner, kind): info.stats['conflicts'] += 1; info.conflicts.append((t, old, (owner, kind))); return
        info.landings[t] = (owner, kind); info.owners.add(owner)
    def prologue_imms(f, n=16):
        out = []; a = f
        for _ in range(n):
            i = rc.insn(a)
            if not i: break
            if i.id in (X86_INS_PUSH, X86_INS_MOV):
                for op in i.operands:
                    if op.type == X86_OP_IMM: out.append(op.imm & 0xffffffff)
            if i.id in (X86_INS_RET, X86_INS_JMP, X86_INS_CALL) or (i.mnemonic.startswith('j')): break
            a += i.size
        return out
    def stub_funcinfo(t):
        a = t
        for _ in range(12):
            i = rc.insn(a)
            if not i: return None
            if i.id == X86_INS_MOV and i.operands[0].type == X86_OP_REG and i.operands[0].reg == X86_REG_EAX and i.operands[1].type == X86_OP_IMM:
                v = i.operands[1].imm & 0xffffffff
                if is_data(v) and u32(v) in CXX_MAGIC:
                    j = rc.insn(a + i.size)
                    if j and j.id == X86_INS_JMP: return v
            if i.id in (X86_INS_RET, X86_INS_JMP, X86_INS_INT3): return None
            a += i.size
        return None
    def reg_const(addrs, k, reg):
        """Value of `reg` at addrs[k] from a short backward scan (xor r,r / sub r,r / mov r,imm / or r,-1); None if unknown."""
        for j in range(k - 1, max(-1, k - 60), -1):
            i = rc.insn(addrs[j])
            if not i or not i.operands: continue
            d = i.operands[0]
            if d.type != X86_OP_REG or d.reg != reg:
                if i.id == X86_INS_CALL and reg in (X86_REG_EAX, X86_REG_ECX, X86_REG_EDX): return None
                continue
            if i.id in (X86_INS_XOR, X86_INS_SUB) and len(i.operands) == 2 and i.operands[1].type == X86_OP_REG and i.operands[1].reg == reg: return 0
            if i.id == X86_INS_MOV and i.operands[1].type == X86_OP_IMM: return i.operands[1].imm & 0xffffffff
            if i.id == X86_INS_OR and i.operands[1].type == X86_OP_IMM and (i.operands[1].imm & 0xffffffff) == 0xffffffff: return 0xffffffff
            if i.id in (X86_INS_PUSH, X86_INS_CMP, X86_INS_TEST): continue
            return None
        return None
    def max_trylevel(f):
        """Highest try level the function stores to [ebp-4] (`mov dword ptr [ebp-4], N|reg` / `and dword ptr [ebp-4], 0`),
        or None when a stored value is unknown. Scope tables of neighbouring functions are adjacent in .rdata and look
        alike, so the record count has to come from the owner's code (or the next known table start)."""
        best = -1; addrs = sorted(rc.walk(f)[0])
        for k, a in enumerate(addrs):
            i = rc.insn(a)
            if not i or i.id not in (X86_INS_MOV, X86_INS_AND) or len(i.operands) != 2: continue
            d, v = i.operands
            if not (d.type == X86_OP_MEM and d.mem.base == X86_REG_EBP and d.mem.index == 0 and d.mem.disp == -4 and d.size == 4): continue
            if v.type == X86_OP_IMM: n = v.imm & 0xffffffff
            elif v.type == X86_OP_REG and i.id == X86_INS_MOV:
                n = reg_const(addrs, k, v.reg)
                if n is None: return None
            else: continue
            if i.id == X86_INS_AND: n = 0 if n == 0 else 0xffffffff
            if n < 256: best = max(best, n)
        return best
    def scope_targets(s, f):
        """EH4 (16-byte cookie header, first record enclosing -2) or EH3 (first record enclosing -1)."""
        m = max_trylevel(f); lim = 256 if m is None else m + 1
        if lim <= 0: return 0, []
        for hdr, top in ((16, 0xFFFFFFFE), (0, 0xFFFFFFFF)):
            if not inimg(s + hdr + 8) or u32(s + hdr) != top: continue
            out = []; k = 0
            while k < lim:
                r = s + hdr + 12 * k
                if not inimg(r + 8) or (k and r in table_starts): break
                enc, flt, hnd = u32(r), u32(r + 4), u32(r + 8)
                if not (enc == top or enc < k): break
                if not code_at(r + 8): break
                if flt and not code_at(r + 4): break
                if flt: out.append(hnd)
                k += 1
            if k: return k, out
        return 0, []
    def funclet_walk(h, limit=20000):
        seen = set(); todo = [h]; conts = []
        while todo and len(seen) < limit:
            a = todo.pop()
            while a not in seen:
                i = rc.insn(a)
                if not i: break
                seen.add(a)
                if (i.id == X86_INS_MOV and i.operands[0].type == X86_OP_REG and i.operands[0].reg == X86_REG_EAX
                        and i.operands[1].type == X86_OP_IMM):
                    v = i.operands[1].imm & 0xffffffff; j = rc.insn(a + i.size)
                    if pe.is_code(v) and j and j.id == X86_INS_RET: conts.append(v)
                if i.id in (X86_INS_RET, X86_INS_INT3, X86_INS_HLT): break
                if i.id == X86_INS_JMP:
                    if i.operands[0].type == X86_OP_IMM: a = i.operands[0].imm & 0xffffffff; continue
                    break
                if (i.mnemonic.startswith('j') or i.mnemonic.startswith('loop')) and i.operands[0].type == X86_OP_IMM:
                    todo.append(i.operands[0].imm & 0xffffffff)
                a += i.size
        return seen, conts
    table_starts = set()
    for f in funcs:
        for v in prologue_imms(f):
            if is_data(v) and u32(v) in (0xFFFFFFFE, 0xFFFFFFFF): table_starts.add(v)
    done_fi = set()
    for f in sorted(funcs):
        for v in prologue_imms(f):
            if pe.is_code(v):
                fi = stub_funcinfo(v)
                if fi is None or (f, fi) in done_fi: continue
                done_fi.add((f, fi)); info.stats['cxx_funcinfo'] += 1
                ntry, ptry = u32(fi + 12), u32(fi + 16)
                if not ntry or ntry > 4096 or not is_data(ptry): continue
                handlers = []
                for t in range(ntry):
                    e = ptry + 20 * t
                    if not inimg(e + 16): break
                    nc, ph = u32(e + 12), u32(e + 16)
                    if nc > 256 or not is_data(ph): continue
                    info.stats['cxx_try'] += 1
                    for h in range(nc):
                        a = u32(ph + 16 * h + 12)
                        if pe.is_code(a): handlers.append(a)
                walks = {h: funclet_walk(h) for h in sorted(set(handlers))}
                for h, (seen, conts) in walks.items():
                    for t in conts:
                        owner = f
                        for h2, (seen2, _) in walks.items():
                            if h2 != h and t in seen2: owner = h2; break
                        add(t, owner, KIND_CXX); info.stats['cxx_targets'] += 1
            elif is_data(v):
                n, targets = scope_targets(v, f)
                if not n: continue
                info.stats['seh_tables'] += 1
                for t in targets: add(t, f, KIND_SEH); info.stats['seh_targets'] += 1
    return info
