#!/usr/bin/env python3
"""Opt-in x87 trig fix for recomp.py; normal build and generated code unchanged.
Enabling this emits different game C and requires regeneration and rebuilding.
"""
import fpsse
import recomp

_original_x87 = fpsse.x87

def fixed_x87(rc, i, m, ops, A, Unsupported):
    # The finite out-of-range case leaves ST(0)/TOP unchanged and sets C2.
    # Infinity and quiet NaN are separate cases, with masked invalid assumed.
    if m in ('fsin', 'fcos', 'fsincos', 'fptan'):
        if m in ('fsin', 'fcos'):
            calc = 'FST(0) = FN64(%s(x_));' % m[1:]
        elif m == 'fsincos':
            calc = 'FST(0) = FN64(sin(x_)); fpush(c, FN64(cos(x_)));'
        else:
            calc = 'FST(0) = FN64(tan(x_)); fpush(c, isnan(x_) || isinf(x_) ? FST(0) : 1.0);'
        return ['{ double x_ = FST(0); if (isfinite(x_) && fabs(x_) >= 0x1p63) c->fsw |= 0x400u; else { if (isinf(x_)) c->fsw |= 1u; %s c->fsw &= ~0x400u; } }' % calc]
    return _original_x87(rc, i, m, ops, A, Unsupported)

def activate():
    fpsse.x87 = fixed_x87

if __name__ == '__main__':
    activate()
    recomp.main()
