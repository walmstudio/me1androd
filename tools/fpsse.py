"""x87 / SSE / SSE2 / MMX translation for recomp.py.
tr(rc, i) returns a list of C statements or None if the instruction is not an FP/SIMD instruction.
Raises Unsupported (from recomp) for recognised-but-unimplemented forms."""
from capstone.x86 import *

ST0, XMM0, MM0 = X86_REG_ST0, X86_REG_XMM0, X86_REG_MM0
def is_st(o):  return o.type == X86_OP_REG and ST0 <= o.reg < ST0 + 8
def is_xmm(o): return o.type == X86_OP_REG and XMM0 <= o.reg < XMM0 + 8
def is_mm(o):  return o.type == X86_OP_REG and MM0 <= o.reg < MM0 + 8
def is_mem(o): return o.type == X86_OP_MEM
def is_gpr(o): return o.type == X86_OP_REG and not (is_st(o) or is_xmm(o) or is_mm(o))
def stn(o): return o.reg - ST0

FARITH = {'fadd': ('+', 0), 'fsub': ('-', 0), 'fsubr': ('-', 1), 'fmul': ('*', 0), 'fdiv': ('/', 0), 'fdivr': ('/', 1)}
FCONST = {'fldz': '0.0', 'fld1': '1.0', 'fldpi': '3.14159265358979323846', 'fldl2e': '1.44269504088896340736',
          'fldl2t': '3.32192809488736234787', 'fldlg2': '0.30102999566398119521', 'fldln2': '0.69314718055994530942'}
FCMOV = {'fcmovb': 'cf', 'fcmove': 'zf', 'fcmovbe': '(cf||zf)', 'fcmovu': 'pf',
         'fcmovnb': '!cf', 'fcmovne': '!zf', 'fcmovnbe': '(!cf&&!zf)', 'fcmovnu': '!pf'}
CMPPRED = ['eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord']
CMPEXPR = {'eq': 'a_ == b_', 'lt': 'a_ < b_', 'le': 'a_ <= b_', 'unord': '(a_ != a_ || b_ != b_)',
           'neq': '!(a_ == b_)', 'nlt': '!(a_ < b_)', 'nle': '!(a_ <= b_)', 'ord': '(a_ == a_ && b_ == b_)'}

# integer SIMD lane ops: mnemonic -> (lane field, lane count per 16 bytes, C expr in a/b, result field)
def _lanes():
    L = {}
    for p, fld, n in (('b', 'b', 16), ('w', 'w', 8), ('d', 'u', 4), ('q', 'q', 2)):
        L['padd' + p] = (fld, n, 'a + b'); L['psub' + p] = (fld, n, 'a - b')
    L['paddsb'] = ('sb', 16, 'sat8(a + b)'); L['paddsw'] = ('sw', 8, 'sat16(a + b)')
    L['paddusb'] = ('b', 16, 'usat8(a + b)'); L['paddusw'] = ('w', 8, 'usat16(a + b)')
    L['psubsb'] = ('sb', 16, 'sat8(a - b)'); L['psubsw'] = ('sw', 8, 'sat16(a - b)')
    L['psubusb'] = ('b', 16, 'usat8(a - b)'); L['psubusw'] = ('w', 8, 'usat16(a - b)')
    L['pmullw'] = ('w', 8, 'a * b'); L['pmulhw'] = ('sw', 8, '(int16_t)(((int32_t)a * b) >> 16)')
    L['pmulhuw'] = ('w', 8, '(uint16_t)(((uint32_t)a * b) >> 16)')
    L['pavgb'] = ('b', 16, '(a + b + 1) >> 1'); L['pavgw'] = ('w', 8, '(a + b + 1) >> 1')
    L['pmaxsw'] = ('sw', 8, 'a > b ? a : b'); L['pminsw'] = ('sw', 8, 'a < b ? a : b')
    L['pmaxub'] = ('b', 16, 'a > b ? a : b'); L['pminub'] = ('b', 16, 'a < b ? a : b')
    for p, fld, sfld, n, m in (('b', 'b', 'sb', 16, '0xFF'), ('w', 'w', 'sw', 8, '0xFFFF'), ('d', 'u', 'i', 4, '0xFFFFFFFFu')):
        L['pcmpeq' + p] = (fld, n, 'a == b ? %s : 0' % m)
        L['pcmpgt' + p] = (sfld, n, 'a > b ? -1 : 0')
    return L
LANES = _lanes()
LOGIC = {'andps': '&', 'andpd': '&', 'pand': '&', 'orps': '|', 'orpd': '|', 'por': '|',
         'xorps': '^', 'xorpd': '^', 'pxor': '^', 'andnps': 'n', 'andnpd': 'n', 'pandn': 'n'}
SHIFTS = {'psllw': ('w', 8, 16, '<<'), 'pslld': ('u', 4, 32, '<<'), 'psllq': ('q', 2, 64, '<<'),
          'psrlw': ('w', 8, 16, '>>'), 'psrld': ('u', 4, 32, '>>'), 'psrlq': ('q', 2, 64, '>>'),
          'psraw': ('sw', 8, 16, 'sar'), 'psrad': ('i', 4, 32, 'sar')}
UNPACK = {'punpcklbw': ('b', 16, 0), 'punpcklwd': ('w', 8, 0), 'punpckldq': ('u', 4, 0), 'punpcklqdq': ('q', 2, 0),
          'punpckhbw': ('b', 16, 1), 'punpckhwd': ('w', 8, 1), 'punpckhdq': ('u', 4, 1), 'punpckhqdq': ('q', 2, 1),
          'unpcklps': ('u', 4, 0), 'unpckhps': ('u', 4, 1), 'unpcklpd': ('q', 2, 0), 'unpckhpd': ('q', 2, 1)}

def tr(rc, i, Unsupported):
    m = i.mnemonic; ops = i.operands
    A = lambda o: rc.addr(o.mem, i)
    if m in ('wait', 'fwait', 'fnop', 'emms', 'femms') or m.startswith('prefetch'):
        return ['c->fempty = 0xFF;'] if m in ('emms', 'femms') else []
    if m.startswith('f') and m not in ('fxsave', 'fxrstor'):
        return x87(rc, i, m, ops, A, Unsupported)
    simd = any(is_xmm(o) or is_mm(o) for o in ops) or m.startswith('cvt')
    if m in ('stmxcsr', 'ldmxcsr', 'fxsave', 'fxrstor', 'movnti', 'clflush'):
        a = A(ops[0])
        if m == 'movnti': return ['wr32(%s, %s);' % (a, rc.rd(ops[1], i))]
        return {'stmxcsr': ['wr32(%s, c->mxcsr);' % a], 'ldmxcsr': ['c->mxcsr = rd32(%s);' % a],
                'fxsave': ['rt_fxsave(c, %s);' % a], 'fxrstor': ['rt_fxrstor(c, %s);' % a], 'clflush': []}[m]
    if not simd: return None
    return sse(rc, i, m, ops, A, Unsupported)

# ------------------------------------------------------------------ x87
def x87(rc, i, m, ops, A, Unsupported):
    E = []
    def fmem(o, integer):
        a = A(o)
        if integer: return {2: '(double)(int16_t)rd16(%s)', 4: '(double)(int32_t)rd32(%s)', 8: '(double)(int64_t)rd64(%s)'}[o.size] % a
        return {4: '(double)rdf32(%s)', 8: 'rdf64(%s)', 10: 'rt_rdf80(%s)'}[o.size] % a
    if m in FCONST: return ['fpush(c, %s);' % FCONST[m]]
    if m in ('fld', 'fild'):
        o = ops[0]
        if is_st(o): return ['{ double v_ = FST(%d); fpush(c, v_); }' % stn(o)]
        return ['{ double v_ = %s; fpush(c, v_); }' % fmem(o, m == 'fild')]
    if m in ('fst', 'fstp'):
        o = ops[0]
        if is_st(o): E.append('FST(%d) = FST(0);' % stn(o))
        elif o.size == 4: E.append('wrf32(%s, (float)FST(0));' % A(o))
        elif o.size == 8: E.append('wrf64(%s, FST(0));' % A(o))
        elif o.size == 10: E.append('rt_wrf80(%s, FST(0));' % A(o))
        else: raise Unsupported(m)
        if m == 'fstp': E.append('fpop(c);')
        return E
    if m in ('fist', 'fistp', 'fisttp'):
        o = ops[0]; t = 1 if m == 'fisttp' else 0
        E.append('wr%d(%s, (uint%d_t)rt_fist%d(c, FST(0), %d));' % (8 * o.size, A(o), 8 * o.size, 8 * o.size, t))
        if m != 'fist': E.append('fpop(c);')
        return E
    base, pop, integer = m, False, False
    if m.startswith('fi') and ('f' + m[2:]) in FARITH: base, integer = 'f' + m[2:], True
    elif m.endswith('p') and m[:-1] in FARITH: base, pop = m[:-1], True
    if base in FARITH:
        op, rev = FARITH[base]
        if pop: dst, src = 'FST(%d)' % (stn(ops[0]) if ops else 1), 'FST(0)'
        elif ops and is_mem(ops[0]): dst, src = 'FST(0)', fmem(ops[0], integer)
        elif len(ops) == 1: dst, src = 'FST(0)', 'FST(%d)' % stn(ops[0])
        elif len(ops) == 2: dst, src = 'FST(%d)' % stn(ops[0]), 'FST(%d)' % stn(ops[1])
        else: dst, src = 'FST(1)', 'FST(0)'
        expr = ('s_ %s %s' % (op, dst)) if rev else ('%s %s s_' % (dst, op))
        E.append('{ double s_ = %s; %s = FN64(%s); }' % (src, dst, expr))
        if pop: E.append('fpop(c);')
        return E
    if m in ('fcom', 'fcomp', 'fucom', 'fucomp', 'ficom', 'ficomp'):
        if ops and is_mem(ops[0]): src = fmem(ops[0], m.startswith('fi'))
        elif ops: src = 'FST(%d)' % stn(ops[0])
        else: src = 'FST(1)'
        E.append('fcom_sw(c, FST(0), %s);' % src)
        if m.endswith('p'): E.append('fpop(c);')
        return E
    if m in ('fcompp', 'fucompp'): return ['fcom_sw(c, FST(0), FST(1));', 'fpop(c);', 'fpop(c);']
    if m == 'ftst': return ['fcom_sw(c, FST(0), 0.0);']
    if m in ('fcomi', 'fucomi', 'fcompi', 'fucompi', 'fcomip', 'fucomip'):
        n = stn(ops[-1]) if ops else 1
        E.append('FCOMI(FST(0), FST(%d));' % n)
        if m.endswith('pi') or m.endswith('ip'): E.append('fpop(c);')
        return E
    if m == 'fxam': return ['rt_fxam(c);']
    if m == 'fxch':
        n = stn(ops[-1]) if ops else 1
        return ['{ double t_ = FST(0); FST(0) = FST(%d); FST(%d) = t_; c->fempty &= ~((1u << c->ftop) | (1u << ((c->ftop + %d) & 7))); }' % (n, n, n)]
    if m in FCMOV: return ['if (%s) FST(0) = FST(%d);' % (FCMOV[m], stn(ops[-1]))]
    if m == 'fchs': return ['FST(0) = -FST(0);']
    if m == 'fabs': return ['FST(0) = fabs(FST(0));']
    if m == 'fsqrt': return ['FST(0) = FN64(sqrt(FST(0)));']
    if m in ('fsin', 'fcos'):
        return ['{ double x_ = FST(0); if (isfinite(x_) && fabs(x_) >= 0x1p63) c->fsw |= 0x400u; else { if (isinf(x_)) c->fsw |= 1u; FST(0) = FN64(%s(x_)); c->fsw &= ~0x400u; } }' % m[1:]]
    if m == 'fsincos':
        return ['{ double x_ = FST(0); if (isfinite(x_) && fabs(x_) >= 0x1p63) c->fsw |= 0x400u; else { if (isinf(x_)) c->fsw |= 1u; FST(0) = FN64(sin(x_)); fpush(c, FN64(cos(x_))); c->fsw &= ~0x400u; } }']
    if m == 'fptan': return ['rt_fptan(c);']
    if m == 'fpatan': return ['FST(1) = FN64(atan2(FST(1), FST(0))); fpop(c);']
    if m == 'fyl2x': return ['rt_fyl2x(c);']
    if m == 'fyl2xp1': return ['rt_fyl2xp1(c);']
    if m == 'f2xm1': return ['FST(0) = FN64(expm1(FST(0) * 0.69314718055994530942));']
    if m == 'frndint': return ['FST(0) = rt_frnd(c, FST(0));']
    if m == 'fscale': return ['rt_fscale(c);']
    if m == 'fxtract': return ['rt_fxtract(c);']
    if m in ('fprem', 'fprem1'): return ['rt_fprem(c, %d);' % (m == 'fprem1')]
    if m == 'fnstsw':
        o = ops[0]
        if is_mem(o): return ['wr16(%s, (uint16_t)fnstsw_val(c));' % A(o)]
        return ['c->eax = (c->eax & 0xFFFF0000u) | fnstsw_val(c);']
    if m == 'fnstcw': return ['wr16(%s, (uint16_t)c->fcw);' % A(ops[0])]
    if m == 'fldcw': return ['c->fcw = rd16(%s);' % A(ops[0])]
    if m == 'fnclex': return ['c->fsw &= 0x7F00u;']
    if m == 'fninit': return ['rt_fninit(c);']
    if m in ('fnstenv', 'fldenv', 'fnsave', 'frstor'): return ['rt_%s(c, %s);' % (m, A(ops[0]))]
    if m in ('ffree', 'ffreep'):
        E.append('c->fempty |= 1u << ((c->ftop + %d) & 7);' % stn(ops[0]))
        if m == 'ffreep': E.append('fpop(c);')
        return E
    if m == 'fincstp': return ['c->ftop = (c->ftop + 1) & 7;']
    if m == 'fdecstp': return ['c->ftop = (c->ftop - 1) & 7;']
    raise Unsupported(m)

# ------------------------------------------------------------------ SSE / MMX
def sse(rc, i, m, ops, A, Unsupported):
    def R(o):  # register lvalue (XMM union)
        if is_xmm(o): return 'c->xmm[%d]' % (o.reg - XMM0)
        if is_mm(o): return 'c->mm[%d]' % (o.reg - MM0)
        raise Unsupported(m)
    def V(o, width=None):  # full-vector rvalue; memory loads 16 bytes for xmm, 8 for mm
        if is_xmm(o) or is_mm(o): return R(o)
        if is_mem(o):
            w = width or o.size
            return {16: 'rdx(%s)', 8: 'rdx64(%s)', 4: 'rdx32(%s)'}[w] % A(o)
        raise Unsupported(m)
    d, s = (ops + [None, None])[:2]
    W = 8 if (d is not None and is_mm(d)) or (s is not None and is_mm(s)) else 16
    E = []
    def store_full(dst, val):
        if is_mem(dst):
            if dst.size == 16: return 'wrx(%s, %s);' % (A(dst), val)
            if dst.size == 8: return 'wr64(%s, (%s).q[0]);' % (A(dst), val)
            if dst.size == 4: return 'wr32(%s, (%s).u[0]);' % (A(dst), val)
            raise Unsupported(m)
        return '%s = %s;' % (R(dst), val)
    # ---- moves
    if m in ('movaps', 'movups', 'movapd', 'movupd', 'movdqa', 'movdqu', 'lddqu', 'movntps', 'movntpd', 'movntdq', 'movntq'):
        if W == 8 and m != 'movntq': raise Unsupported(m)
        return [store_full(d, V(s))]
    if m in ('movss', 'movsd'):
        f, w = ('u', 4) if m == 'movss' else ('q', 8)
        k = 'u[0]' if w == 4 else 'q[0]'
        if is_mem(s): return ['%s = %s;' % (R(d), ('rdx32(%s)' if w == 4 else 'rdx64(%s)') % A(s))]
        if is_mem(d): return ['wr%d(%s, %s.%s);' % (8 * w, A(d), R(s), k)]
        return ['%s.%s = %s.%s;' % (R(d), k, R(s), k)]
    if m == 'movd':
        if is_xmm(d) or is_mm(d): return ['%s = xmm_u32(%s);' % (R(d), rc.rd(s, i, 4))]
        return [rc.wr(d, i, '%s.u[0]' % R(s), 4)]
    if m == 'movq':
        if is_mem(d): return ['wr64(%s, %s.q[0]);' % (A(d), R(s))]
        if is_mem(s): return ['%s = rdx64(%s);' % (R(d), A(s))]
        return ['%s = xmm_u64(%s.q[0]);' % (R(d), R(s))]
    if m in ('movq2dq',): return ['%s = xmm_u64(%s.q[0]);' % (R(d), R(s))]
    if m in ('movdq2q',): return ['%s = xmm_u64(%s.q[0]);' % (R(d), R(s))]
    if m in ('movlps', 'movlpd', 'movhps', 'movhpd'):
        k = 0 if m.startswith('movl') else 1
        if is_mem(d): return ['wr64(%s, %s.q[%d]);' % (A(d), R(s), k)]
        return ['%s.q[%d] = rd64(%s);' % (R(d), k, A(s))]
    if m == 'movhlps': return ['%s.q[0] = %s.q[1];' % (R(d), R(s))]
    if m == 'movlhps': return ['%s.q[1] = %s.q[0];' % (R(d), R(s))]
    if m == 'movmskps': return [rc.wr(d, i, '((%s.u[0] >> 31) | ((%s.u[1] >> 31) << 1) | ((%s.u[2] >> 31) << 2) | ((%s.u[3] >> 31) << 3))' % ((R(s),) * 4), 4)]
    if m == 'movmskpd': return [rc.wr(d, i, '((uint32_t)(%s.q[0] >> 63) | ((uint32_t)(%s.q[1] >> 63) << 1))' % (R(s), R(s)), 4)]
    if m == 'pmovmskb':
        return ['{ XMM s_ = %s; uint32_t r_ = 0; for (int k = 0; k < %d; k++) r_ |= (uint32_t)(s_.b[k] >> 7) << k; %s }' % (R(s), W, rc.wr(d, i, 'r_', 4))]
    # ---- scalar float / double arithmetic
    scal = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/'}
    if len(m) > 2 and m[-2:] in ('ss', 'sd') and m[:-2] in scal:
        f, sz = ('f', 4) if m[-2:] == 'ss' else ('d', 8)
        src = ('rdf32(%s)' if sz == 4 else 'rdf64(%s)') % A(s) if is_mem(s) else '%s.%s[0]' % (R(s), f)
        return ['{ %s s_ = %s; %s.%s[0] = %s(%s.%s[0] %s s_); }' % ('float' if sz == 4 else 'double', src, R(d), f, 'FN32' if sz == 4 else 'FN64', R(d), f, scal[m[:-2]])]
    if len(m) > 2 and m[-2:] in ('ps', 'pd') and m[:-2] in scal:
        f, n = ('f', 4) if m[-2:] == 'ps' else ('d', 2)
        return ['{ XMM s_ = %s; for (int k = 0; k < %d; k++) %s.%s[k] = %s(%s.%s[k] %s s_.%s[k]); }' % (V(s, 16), n, R(d), f, 'FN32' if f == 'f' else 'FN64', R(d), f, scal[m[:-2]], f)]
    if m in ('minss', 'maxss', 'minsd', 'maxsd', 'minps', 'maxps', 'minpd', 'maxpd'):
        f = 'f' if m[-1] == 's' else 'd'; T = 'float' if f == 'f' else 'double'
        n = 1 if m[-2] == 's' else (4 if f == 'f' else 2)
        cmp = '<' if m.startswith('min') else '>'
        if n == 1: src = ('rdf32(%s)' if f == 'f' else 'rdf64(%s)') % A(s) if is_mem(s) else '%s.%s[0]' % (R(s), f)
        else: src = None
        if n == 1:
            return ['{ %s a_ = %s.%s[0], b_ = %s; %s.%s[0] = (a_ %s b_) ? a_ : b_; }' % (T, R(d), f, src, R(d), f, cmp)]
        return ['{ XMM s_ = %s; for (int k = 0; k < %d; k++) { %s a_ = %s.%s[k], b_ = s_.%s[k]; %s.%s[k] = (a_ %s b_) ? a_ : b_; } }' % (V(s, 16), n, T, R(d), f, f, R(d), f, cmp)]
    if m in ('sqrtss', 'rcpss', 'rsqrtss', 'sqrtsd'):
        f = 'd' if m == 'sqrtsd' else 'f'
        src = ('rdf64(%s)' if f == 'd' else 'rdf32(%s)') % A(s) if is_mem(s) else '%s.%s[0]' % (R(s), f)
        ex = {'sqrtss': 'FN32(sqrtf(x_))', 'rcpss': '1.0f / x_', 'rsqrtss': 'FN32(1.0f / sqrtf(x_))', 'sqrtsd': 'FN64(sqrt(x_))'}[m]
        return ['{ %s x_ = %s; %s.%s[0] = %s; }' % ('double' if f == 'd' else 'float', src, R(d), f, ex)]
    if m in ('sqrtps', 'rcpps', 'rsqrtps', 'sqrtpd'):
        f, n = ('d', 2) if m == 'sqrtpd' else ('f', 4)
        ex = {'sqrtps': 'FN32(sqrtf(s_.f[k]))', 'rcpps': '1.0f / s_.f[k]', 'rsqrtps': 'FN32(1.0f / sqrtf(s_.f[k]))', 'sqrtpd': 'FN64(sqrt(s_.d[k]))'}[m]
        return ['{ XMM s_ = %s; for (int k = 0; k < %d; k++) %s.%s[k] = %s; }' % (V(s, 16), n, R(d), f, ex)]
    # ---- logic
    if m in LOGIC:
        op = LOGIC[m]
        if op == '^' and is_xmm(d) and is_xmm(s) and d.reg == s.reg or op == '^' and is_mm(d) and is_mm(s) and d.reg == s.reg:
            return ['memset(&%s, 0, sizeof(XMM));' % R(d)]
        expr = '(~%s.q[k] & s_.q[k])' % R(d) if op == 'n' else '(%s.q[k] %s s_.q[k])' % (R(d), op)
        return ['{ XMM s_ = %s; for (int k = 0; k < %d; k++) %s.q[k] = %s; }' % (V(s, W), W // 8, R(d), expr)]
    # ---- compares
    if m.startswith('cmp') and m[-2:] in ('ss', 'sd', 'ps', 'pd') and m[3:-2] in CMPPRED:
        pred = m[3:-2]; kind = m[-2:]
        f, T, mask = ('f', 'float', 'u') if kind[1] == 's' else ('d', 'double', 'q')
        n = 1 if kind[0] == 's' else (4 if f == 'f' else 2)
        allones = '0xFFFFFFFFu' if f == 'f' else '0xFFFFFFFFFFFFFFFFull'
        if n == 1:
            src = ('rdf32(%s)' if f == 'f' else 'rdf64(%s)') % A(s) if is_mem(s) else '%s.%s[0]' % (R(s), f)
            return ['{ %s a_ = %s.%s[0], b_ = %s; %s.%s[0] = (%s) ? %s : 0; }' % (T, R(d), f, src, R(d), mask, CMPEXPR[pred], allones)]
        return ['{ XMM s_ = %s; for (int k = 0; k < %d; k++) { %s a_ = %s.%s[k], b_ = s_.%s[k]; %s.%s[k] = (%s) ? %s : 0; } }' % (V(s, 16), n, T, R(d), f, f, R(d), mask, CMPEXPR[pred], allones)]
    if m in ('comiss', 'ucomiss', 'comisd', 'ucomisd'):
        f = 'f' if m.endswith('ss') else 'd'
        src = ('rdf32(%s)' if f == 'f' else 'rdf64(%s)') % A(s) if is_mem(s) else '%s.%s[0]' % (R(s), f)
        return ['FCOMI(%s.%s[0], %s);' % (R(d), f, src)]
    # ---- conversions
    if m in ('cvtsi2ss', 'cvtsi2sd'):
        f = 'f' if m.endswith('ss') else 'd'
        v = '(int32_t)%s' % rc.rd(s, i, 4)
        return ['%s.%s[0] = %s;' % (R(d), f, ('rt_sse_i2f(c, %s)' % v) if f == 'f' else '(double)%s' % v)]
    if m in ('cvttss2si', 'cvtss2si', 'cvttsd2si', 'cvtsd2si'):
        f = 'f' if 'ss2' in m else 'd'
        src = ('(double)rdf32(%s)' if f == 'f' else 'rdf64(%s)') % A(s) if is_mem(s) else '(double)%s.%s[0]' % (R(s), f)
        return [rc.wr(d, i, '(uint32_t)rt_cvt_f2i32(c, %s, %d)' % (src, m.startswith('cvtt')), 4)]
    if m == 'cvtss2sd':
        src = 'rdf32(%s)' % A(s) if is_mem(s) else '%s.f[0]' % R(s)
        return ['%s.d[0] = (double)%s;' % (R(d), src)]
    if m == 'cvtsd2ss':
        src = 'rdf64(%s)' % A(s) if is_mem(s) else '%s.d[0]' % R(s)
        return ['%s.f[0] = (float)%s;' % (R(d), src)]
    if m == 'cvtdq2ps': return ['{ XMM s_ = %s; for (int k = 0; k < 4; k++) %s.f[k] = (float)s_.i[k]; }' % (V(s, 16), R(d))]
    if m in ('cvttps2dq', 'cvtps2dq'):
        return ['{ XMM s_ = %s; for (int k = 0; k < 4; k++) %s.i[k] = rt_cvt_f2i32(c, (double)s_.f[k], %d); }' % (V(s, 16), R(d), m == 'cvttps2dq')]
    if m == 'cvtps2pd': return ['{ XMM s_ = %s; %s.d[0] = s_.f[0]; %s.d[1] = s_.f[1]; }' % (V(s, 8), R(d), R(d))]
    if m == 'cvtpd2ps': return ['{ XMM s_ = %s; %s.f[0] = (float)s_.d[0]; %s.f[1] = (float)s_.d[1]; %s.q[1] = 0; }' % (V(s, 16), R(d), R(d), R(d))]
    if m == 'cvtdq2pd': return ['{ XMM s_ = %s; %s.d[0] = s_.i[0]; %s.d[1] = s_.i[1]; }' % (V(s, 8), R(d), R(d))]
    if m in ('cvttpd2dq', 'cvtpd2dq'):
        return ['{ XMM s_ = %s; %s.i[0] = rt_cvt_f2i32(c, s_.d[0], %d); %s.i[1] = rt_cvt_f2i32(c, s_.d[1], %d); %s.q[1] = 0; }' % (V(s, 16), R(d), m[4] == 't', R(d), m[4] == 't', R(d))]
    if m == 'cvtpi2ps': return ['{ XMM s_ = %s; %s.f[0] = (float)s_.i[0]; %s.f[1] = (float)s_.i[1]; }' % (V(s, 8), R(d), R(d))]
    if m in ('cvttps2pi', 'cvtps2pi'):
        return ['{ XMM s_ = %s; %s.i[0] = rt_cvt_f2i32(c, s_.f[0], %d); %s.i[1] = rt_cvt_f2i32(c, s_.f[1], %d); }' % (V(s, 8), R(d), m[4] == 't', R(d), m[4] == 't')]
    # ---- shuffles / unpacks
    if m == 'shufps':
        imm = ops[2].imm & 0xff
        return ['{ XMM a_ = %s, b_ = %s; %s.u[0] = a_.u[%d]; %s.u[1] = a_.u[%d]; %s.u[2] = b_.u[%d]; %s.u[3] = b_.u[%d]; }'
                % (R(d), V(s, 16), R(d), imm & 3, R(d), (imm >> 2) & 3, R(d), (imm >> 4) & 3, R(d), (imm >> 6) & 3)]
    if m == 'shufpd':
        imm = ops[2].imm & 3
        return ['{ XMM a_ = %s, b_ = %s; %s.q[0] = a_.q[%d]; %s.q[1] = b_.q[%d]; }' % (R(d), V(s, 16), R(d), imm & 1, R(d), (imm >> 1) & 1)]
    if m == 'pshufd':
        imm = ops[2].imm & 0xff
        return ['{ XMM s_ = %s; %s.u[0] = s_.u[%d]; %s.u[1] = s_.u[%d]; %s.u[2] = s_.u[%d]; %s.u[3] = s_.u[%d]; }'
                % (V(s, 16), R(d), imm & 3, R(d), (imm >> 2) & 3, R(d), (imm >> 4) & 3, R(d), (imm >> 6) & 3)]
    if m in ('pshuflw', 'pshufhw', 'pshufw'):
        imm = ops[2].imm & 0xff; off = 4 if m == 'pshufhw' else 0
        sel = '; '.join('r_.w[%d] = s_.w[%d]' % (off + k, off + ((imm >> (2 * k)) & 3)) for k in range(4))
        return ['{ XMM s_ = %s, r_ = s_; %s; %s = r_; }' % (V(s, W), sel, R(d))]
    if m in UNPACK:
        fld, n, hi = UNPACK[m]; n = n * W // 16; h = n // 2
        return ['{ XMM a_ = %s, b_ = %s, r_; memset(&r_, 0, 16); for (int k = 0; k < %d; k++) { r_.%s[2*k] = a_.%s[%d + k]; r_.%s[2*k+1] = b_.%s[%d + k]; } %s = r_; }'
                % (R(d), V(s, W), h, fld, fld, h * hi, fld, fld, h * hi, R(d))]
    if m in ('packsswb', 'packuswb', 'packssdw'):
        if m == 'packssdw': sf, n, df, fn = 'i', 4, 'sw', 'sat16'
        else: sf, n, df, fn = 'sw', 8, ('sb' if m == 'packsswb' else 'b'), ('sat8' if m == 'packsswb' else 'usat8')
        n = n * W // 16
        return ['{ XMM a_ = %s, b_ = %s, r_; memset(&r_, 0, 16); for (int k = 0; k < %d; k++) { r_.%s[k] = %s(a_.%s[k]); r_.%s[%d + k] = %s(b_.%s[k]); } %s = r_; }'
                % (R(d), V(s, W), n, df, fn, sf, df, n, fn, sf, R(d))]
    # ---- integer lane arithmetic
    if m in LANES:
        fld, n, ex = LANES[m]; n = n * W // 16
        return ['{ XMM s_ = %s; for (int k = 0; k < %d; k++) { __typeof__(s_.%s[0]) a = %s.%s[k], b = s_.%s[k]; %s.%s[k] = %s; } }'
                % (V(s, W), n, fld, R(d), fld, fld, R(d), fld, ex)]
    if m == 'pmuludq':
        n = W // 16 if W == 16 else 1
        if W == 8: return ['%s.q[0] = (uint64_t)%s.u[0] * (%s).u[0];' % (R(d), R(d), V(s, W))]
        return ['{ XMM s_ = %s; %s.q[0] = (uint64_t)%s.u[0] * s_.u[0]; %s.q[1] = (uint64_t)%s.u[2] * s_.u[2]; }' % (V(s, 16), R(d), R(d), R(d), R(d))]
    if m == 'pmaddwd':
        n = W // 4
        return ['{ XMM a_ = %s, s_ = %s; for (int k = 0; k < %d; k++) %s.i[k] = (int32_t)a_.sw[2*k] * s_.sw[2*k] + (int32_t)a_.sw[2*k+1] * s_.sw[2*k+1]; }' % (R(d), V(s, W), n, R(d))]
    if m == 'psadbw':
        if W == 8: return ['{ XMM a_ = %s, s_ = %s; uint32_t t_ = 0; for (int k = 0; k < 8; k++) t_ += abs((int)a_.b[k] - s_.b[k]); %s = xmm_u64(t_); }' % (R(d), V(s, 8), R(d))]
        return ['{ XMM a_ = %s, s_ = %s; uint32_t t0 = 0, t1 = 0; for (int k = 0; k < 8; k++) { t0 += abs((int)a_.b[k] - s_.b[k]); t1 += abs((int)a_.b[8+k] - s_.b[8+k]); } memset(&%s, 0, 16); %s.q[0] = t0; %s.q[1] = t1; }' % (R(d), V(s, 16), R(d), R(d), R(d))]
    if m in SHIFTS:
        fld, n, bits, op = SHIFTS[m]; n = n * W // 16
        cnt = '%du' % (ops[1].imm & 0xff) if ops[1].type == X86_OP_IMM else '(%s).q[0]' % V(s, W)
        if op == 'sar': body = '%s.%s[k] = (__typeof__(%s.%s[0]))(%s.%s[k] >> (n_ > %d ? %d : n_));' % (R(d), fld, R(d), fld, R(d), fld, bits - 1, bits - 1)
        else: body = '%s.%s[k] = n_ > %d ? 0 : (__typeof__(%s.%s[0]))(%s.%s[k] %s n_);' % (R(d), fld, bits - 1, R(d), fld, R(d), fld, op)
        return ['{ uint64_t n_ = %s; for (int k = 0; k < %d; k++) %s }' % (cnt, n, body)]
    if m in ('pslldq', 'psrldq'):
        n = ops[1].imm & 0xff
        if m == 'pslldq': return ['{ XMM s_ = %s, r_; memset(&r_, 0, 16); for (int k = %d; k < 16; k++) r_.b[k] = s_.b[k - %d]; %s = r_; }' % (R(d), n, n, R(d))] if n < 16 else ['memset(&%s, 0, 16);' % R(d)]
        return ['{ XMM s_ = %s, r_; memset(&r_, 0, 16); for (int k = 0; k + %d < 16; k++) r_.b[k] = s_.b[k + %d]; %s = r_; }' % (R(d), n, n, R(d))] if n < 16 else ['memset(&%s, 0, 16);' % R(d)]
    if m == 'pextrw':
        n = ops[2].imm & (W // 2 - 1)
        return [rc.wr(d, i, '(uint32_t)%s.w[%d]' % (R(s), n), 4)]
    if m == 'pinsrw':
        n = ops[2].imm & (W // 2 - 1)
        return ['%s.w[%d] = (uint16_t)%s;' % (R(d), n, rc.rd(s, i, 4) if is_gpr(s) else rc.rd(s, i, 2))]
    raise Unsupported(m)
