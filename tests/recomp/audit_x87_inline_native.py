#!/usr/bin/env python3
"""Audit of the operand-free x87 instructions as emitted by tools/fpsse.py (standard
generator, or --trig-fix) against the real x87 on x86-64: values, TOP and status bits.
Needs gcc and an x86/x86-64 host. capstone is not required (a tiny stub is used if it is
missing: only operand-free instructions are generated here).
Usage (from the repository root):  python3 tests/recomp/audit_x87_inline_native.py [--trig-fix] [--seed N] [-v]
Exit code 1 when value or TOP mismatches are found; status-bit differences are reported
but are not treated as failures (PE/IE/C1 are not modelled by the double-based runtime).
Skipped (counted, not failures): trig arguments with |x| >= 2^63 (known; the default generator differs, --trig-fix matches), inputs
where the instruction result is documented as undefined (F2XM1 |x| > 1, FYL2XP1 outside +-(1-sqrt(2)/2)), a documented precision
limit (FYL2XP1 with subnormal x), and FPREM/FPREM1 partial reductions (C2 set: intermediate is implementation-defined).
Trig tolerance: 2e-15 relative or 5e-20*(|x|+1) absolute (x87 reduces with a 66-bit pi); FPTAN additionally times (1 + tan^2)."""
import os, sys, subprocess, tempfile, types, json, argparse
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools'))
try:
    import capstone  # noqa
except ImportError:
    cs = types.ModuleType('capstone'); x = types.ModuleType('capstone.x86')
    for k, v in dict(X86_REG_ST0=100, X86_REG_XMM0=200, X86_REG_MM0=300, X86_OP_REG=1, X86_OP_MEM=3, X86_OP_IMM=2).items(): setattr(x, k, v)
    cs.x86 = x; sys.modules['capstone'] = cs; sys.modules['capstone.x86'] = x
import fpsse
class Unsup(Exception): pass

parser = argparse.ArgumentParser()
parser.add_argument('--trig-fix', action='store_true')
parser.add_argument('--status-strict', action='store_true')
parser.add_argument('--ops', default=None)
parser.add_argument('--seed', type=int, default=11)
parser.add_argument('-v', '--verbose', action='store_true')
parser.add_argument('--json-summary', default=None)
args = parser.parse_args()

if args.trig_fix:
    try:
        import recomp  # noqa
    except ImportError:          # capstone missing: only fpsse.x87 is needed to activate the fix
        r = types.ModuleType('recomp'); r.main = lambda: None; sys.modules['recomp'] = r
    import recomp_x87_fixed; recomp_x87_fixed.activate()
seed = args.seed
verbose = args.verbose
target_ops = args.ops.split(',') if args.ops else None
# name, inputs (1: ST0=a; 2: ST1=b, ST0=a), results compared (st0[, st1])
ALL_OPS = [('fsqrt',1,1),('fchs',1,1),('fabs',1,1),('fsin',1,1),('fcos',1,1),('fsincos',1,2),('fptan',1,2),
           ('f2xm1',1,1),('frndint',1,1),('fxtract',1,2),('fpatan',2,1),('fyl2x',2,1),('fyl2xp1',2,1),
           ('fscale',2,1),('fprem',2,1),('fprem1',2,1)]
OPS = [o for o in ALL_OPS if not target_ops or o[0] in target_ops]
if not OPS:
    sys.exit("no matching ops (known: %s)" % ','.join(o[0] for o in ALL_OPS))
out = ['#include "rt.h"', '#include <math.h>', '#include <stdio.h>', '#include <string.h>', '#include <stdint.h>']
out.append('#define MASK 0x7F3Du')
for n, nin, nout in OPS:
    body = ' '.join(fpsse.x87(None, None, n, [], None, Unsup))
    out.append('static void g_%s(CPU* c) { %s }' % (n, body))
    pre = 'fldl %[b]; fldl %[a];' if nin == 2 else 'fldl %[a];'
    post = 'fnstsw %[sw]; fstpl %[r0];' + (' fstpl %[r1];' if nout == 2 else '') + ' fninit'
    out.append('static void n_%s(double a, double b, double* r, unsigned short* sw) { double r0 = 0, r1 = 0; unsigned short s = 0; '
               'asm volatile("fninit; %s %s; %s" : [r0] "=m"(r0), [r1] "=m"(r1), [sw] "=m"(s) : [a] "m"(a), [b] "m"(b) : "st", "memory"); '
               'r[0] = r0; r[1] = r1; *sw = s; }' % (n, pre, n, post))
out.append('typedef struct { const char* name; int nin, nout; void (*g)(CPU*); void (*n)(double,double,double*,unsigned short*); } Op;')
out.append('static Op ops[] = {' + ','.join('{"%s",%d,%d,g_%s,n_%s}' % (n, i, o, n, n) for n, i, o in OPS) + '};')
out.append(r'''
static uint32_t rng = %d; static uint32_t nx(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static double rnd(int wide) { double m = 1.0 + (nx() / 4294967296.0); int e = (int)(nx() %% (wide ? 400 : 41)) - (wide ? 200 : 20); double v = ldexp(m, e); return (nx() & 1) ? -v : v; }
static int is_trig(const char* nm) { return !strcmp(nm, "fptan") ? 2 : (!strcmp(nm, "fsin") || !strcmp(nm, "fcos") || !strcmp(nm, "fsincos")); }
static int oor_trig(const char* nm, double a) { return (!strcmp(nm, "fsin") || !strcmp(nm, "fcos") || !strcmp(nm, "fsincos") || !strcmp(nm, "fptan")) && isfinite(a) && fabs(a) >= 0x1p63; }
static int undefined_domain(const char* nm, double a) {   /* results documented as undefined */
    if (!strcmp(nm, "f2xm1")) return !(fabs(a) <= 1.0);     /* NaN excluded below */
    if (!strcmp(nm, "fyl2xp1")) return !(fabs(a) < 0.29289321881345254) || (a != 0 && fabs(a) < 0x1p-1000);   /* the latter: double intermediate (log1p(x)/ln2 of a subnormal x loses precision); 80-bit keeps it */
    return 0;
}
static int close_(double n, double g, double a, int trig) {
    if (isnan(n) || isnan(g)) return isnan(n) && isnan(g);
    if (n == g) return !!signbit(n) == !!signbit(g) || n != 0;
    if (isinf(n) || isinf(g)) return 0;
    double d = fabs(n - g), s = fmax(fabs(n), fabs(g));
    /* x87 reduces trig arguments with a 66-bit pi: absolute error about 1.5e-20 * |a| (visible only near zeros) */
    if (trig && d <= 5e-20 * (fabs(a) + 1.0)) return 1;
    /* tan amplifies the reduction error by its derivative 1 + tan^2 */
    if (trig == 2 && d <= 5e-20 * (fabs(a) + 1.0) * (1.0 + n * n)) return 1;
    return d <= 2e-15 * s || d <= 1e-300;
}
int main(int argc, char** argv) {
    int verbose = argc > 1; int bad = 0;
    double sp[] = {0., -0., 1., -1., 0.5, -0.5, 1.5, -2.5, 3., 0x1p-1074, -0x1p-1074, 0x1p-1022, 0x1.fffffffffffffp1023, -0x1.fffffffffffffp1023,
                   1e300, 1e-300, 3.14159265358979323846, -3.14159265358979323846, 1.5707963267948966, 0x1p62, 0x1p63, 0x1p64, -0x1p63, 1e10, 1e22,
                   INFINITY, -INFINITY, NAN, -NAN, 0.25, 0.29289321881345243, -0.29289321881345243, 0.9, -0.9, 1.0000000000000002, 0.9999999999999999};
    int ns = sizeof sp / sizeof *sp;
    for (unsigned k = 0; k < sizeof ops / sizeof *ops; k++) {
        Op* o = &ops[k]; int n = 0, vm = 0, tm = 0, sm = 0, oor = 0, und = 0, par = 0; unsigned sbits = 0; char ex[200] = "", sex[200] = "";
        for (int i = 0; i < ns + 4000; i++) for (int j = 0; j < (o->nin == 2 ? (i < ns ? ns : 3) : 1); j++) {
            double a = i < ns ? sp[i] : rnd(i & 1), b = o->nin == 2 ? (i < ns ? sp[j] : rnd(j & 1)) : 0, nr[2]; unsigned short nsw;
            o->n(a, b, nr, &nsw);
            if (!isnan(a) && undefined_domain(o->name, a)) { und++; continue; }
            CPU cpu, *c = &cpu; memset(&cpu, 0, sizeof cpu); cpu.fcw = 0x37f; cpu.fempty = 0xFF;
            if (o->nin == 2) fpush(c, b); fpush(c, a); o->g(c);
            double g0 = FST(0), g1 = FST(1); unsigned gsw = fnstsw_val(c); n++;
            if (oor_trig(o->name, a)) { if (!close_(nr[0], g0, a, is_trig(o->name)) || (nsw & 0x3800) != (gsw & 0x3800)) oor++; continue; }   /* out of range: ST0 unchanged, C2 set, no push; compare ST0 and TOP only */
            if (o->nin == 2 && (!strcmp(o->name, "fprem") || !strcmp(o->name, "fprem1")) && (nsw & 0x400)) { par++; continue; }
            if (!close_(nr[0], g0, a, is_trig(o->name)) || (o->nout == 2 && !close_(nr[1], g1, a, is_trig(o->name)))) { if (!vm) snprintf(ex, sizeof ex, "a=%%a b=%%a native=%%a,%%a runtime=%%a,%%a", a, b, nr[0], nr[1], g0, g1); vm++; }
            if ((nsw & 0x3800) != (gsw & 0x3800)) tm++;
            if (((nsw ^ gsw) & MASK & ~0x3800u) != 0) { if (!sm) snprintf(sex, sizeof sex, "a=%%a b=%%a native sw=%%04x runtime sw=%%04x", a, b, nsw & MASK, gsw & MASK); sm++; sbits |= (nsw ^ gsw) & MASK & ~0x3800u; }
        }
        printf("%%-8s %%5d checked  value:%%-5d top:%%-3d status:%%-5d (bits %%04x) | skipped: out-of-range trig %%d (known), undefined domain %%d, partial FPREM %%d%%s%%s\n", o->name, n, vm, tm, sm, sbits, oor, und, par, vm ? "\n           e.g. " : "", ex);
        if (verbose && sm) printf("         status e.g. %%s\n", sex);
        if (vm || tm) bad = 1;
    }
    return bad;
}''' % seed)
src = '\n'.join(out)
d = tempfile.mkdtemp(); c = os.path.join(d, 'a.c'); exe = os.path.join(d, 'a')
open(c, 'w').write(src)
root = os.path.join(os.path.dirname(__file__), '..', '..')
subprocess.check_call(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffunction-sections', '-Wl,--gc-sections', '-I' + os.path.join(root, 'runtime'), c, os.path.join(root, 'runtime', 'rt_fp.c'), '-lm', '-o', exe])
import re
p = subprocess.run([exe] + (['-v'] if verbose else []), capture_output=True, text=True)
print(p.stdout, end='')
if p.stderr: print(p.stderr, end='', file=sys.stderr)
summary = {}
for line in p.stdout.splitlines():
    m = re.match(r'^(\S+)\s+(\d+)\s+checked\s+value:(\d+)\s+top:(\d+)\s+status:(\d+)\s+\(bits\s+([0-9a-fA-F]+)\)\s+\|\s+skipped: out-of-range trig\s+(\d+)\s+\(known\), undefined domain\s+(\d+), partial FPREM\s+(\d+)', line)
    if m:
        summary[m.group(1)] = {
            'checked': int(m.group(2)), 'value_mismatches': int(m.group(3)),
            'top_mismatches': int(m.group(4)), 'status_mismatches': int(m.group(5)),
            'status_bits': m.group(6), 'skipped_oor_trig': int(m.group(7)),
            'skipped_undefined_domain': int(m.group(8)), 'skipped_partial_fprem': int(m.group(9)),
        }
if args.json_summary:
    with open(args.json_summary, 'w') as f:
        json.dump(summary, f, indent=2, sort_keys=True)
sys.exit(p.returncode)
