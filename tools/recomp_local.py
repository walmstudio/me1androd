#!/usr/bin/env python3
"""Experimental register-caching variant of recomp.py. Opt in by invoking this
file instead of recomp.py; the regular launcher build remains unchanged.
"""
import recomp as base
import re
from recomp import *

class LocalRecompiler(base.Recompiler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.local_register_opt = True
        self.stats_local = collections.Counter()
    def local_safe(self, ins):
        # Memory aliasing/volatile/atomic interactions:
        # TSO-sensitive paths: if instruction accesses memory (not stack), it is RDO/WRO
        # Helper calls: not safe.
        # Exceptions: setjmp/EH boundaries: instructions that can fault (e.g. division) or potentially throw (calls) must be safe-guarded.

        # Hardened check:
        # 1. No atomic/locking instructions.
        if 0xf0 in ins.prefix: return False

        # 2. No external helpers or calls.
        if ins.id in (X86_INS_CALL, X86_INS_RET, X86_INS_INT3): return False

        # 3. No division (faults).
        if ins.mnemonic in ('div', 'idiv'): return False

        # 4. Flags liveness: instructions that modify flags need care.
        #    Some instructions might not be safe if flags are live, but for now we limit to simple ALU/moves.

        # Only allow instructions that are known safe and do not interact with non-stack memory or complex state.
        allowed = {'mov','movzx','movsx','lea','add','sub','adc','sbb','cmp','and','or','xor','test',
                   'inc','dec','neg','not','bswap','cdq','cwde','cbw','cwd','xchg','xadd'}
        name = ins.mnemonic
        if name not in allowed and not name.startswith(('cmov','set')): return False

        # 5. Check operands for memory: only allow stack access (ebp/esp based).
        #    Non-stack access requires rdo/wro which breaks straight-line register optimization if not handled correctly.
        for j, op in enumerate(ins.operands):
            if op.type == X86_OP_REG: continue
            if op.type == X86_OP_MEM:
                # Allow if base is EBP or ESP.
                if op.mem.base not in (X86_REG_EBP, X86_REG_ESP): return False
            if op.type not in (X86_OP_REG, X86_OP_IMM, X86_OP_MEM): return False
        return True
    @staticmethod
    def local_combine(lines):
        regs = r'eax|ecx|edx|ebx|esp|ebp|esi|edi'
        pat = re.compile(r'c->(' + regs + r')\b')
        written = re.compile(r'c->(' + regs + r')\s*=(?!=)')
        result, chunk = [], []
        def flush():
            if not chunk: return
            if len(chunk) < 2:
                for x in chunk: result.extend(x)
            else:
                text = '\n'.join(y for x in chunk for y in x)
                used, dirty = sorted(set(pat.findall(text))), sorted(set(written.findall(text)))
                if not used: result.extend(text.split('\n'))
                else:
                    result.append('  { /* experimental straight-line guest registers */')
                    for r in used: result.append('    uint32_t lr_%s = c->%s;' % (r, r))
                    result.extend(pat.sub(lambda m: 'lr_' + m.group(1), y) for x in chunk for y in x)
                    for r in dirty: result.append('    c->%s = lr_%s;' % (r, r))
                    result.append('  }')
            chunk.clear()
        i = 0
        while i < len(lines):
            if lines[i] == '  /*@LOCAL_BEGIN*/':
                i += 1; group = []
                while i < len(lines) and lines[i] != '  /*@LOCAL_END*/': group.append(lines[i]); i += 1
                chunk.append(group); i += 1
            else:
                flush(); result.append(lines[i]); i += 1
        flush(); return result
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
            local_safe = self.local_register_opt and self.local_safe(i)
            marker_pos = len(lines)
            if local_safe:
                lines.append('  /*@LOCAL_BEGIN*/')
                self.stats_local['optimized_blocks'] += 1
            lines.append('  /* %08x: %s %s */' % (a, i.mnemonic, i.op_str.replace('*/', '* /')))
            self.stats[i.mnemonic] += 1
            try:
                try: tl = self.tr(i, f, body)
                except (KeyError, IndexError) as e: raise Unsupported(i.mnemonic)
                for l in tl: lines.append('  ' + l)
            except Unsupported as e:
                self.unimpl[str(e)] += 1
                lines.append('  UNIMPL(c, 0x%xu, "%s %s"); return;' % (a, i.mnemonic, i.op_str))
                if local_safe: del lines[marker_pos]
                continue
            if local_safe: lines.append('  /*@LOCAL_END*/')
            if not self.is_terminator(i):
                nxt = a + i.size
                if nxt not in body:
                    if nxt in self.funcs: lines.append('  { %s_%x(c); return; }' % (self.prefix, nxt))
                    else: lines.append('  dispatch_jmp(c, 0x%xu); return;' % nxt)
                elif k + 1 >= len(addrs) or addrs[k + 1] != nxt:
                    lines.append('  goto L_%x;' % nxt)
        lines.append('}')
        return self.local_combine(lines) if self.local_register_opt else lines

if __name__ == "__main__":
    base.Recompiler = LocalRecompiler
    base.main()
