# S8-C: 80-bit extended precision arithmetic decision

## Status

Decision recorded: **keep double `st[8]` (status quo).** No runtime changes. 80-bit is retained only at memory boundary via `rt_rdf80`/`rt_wrf80`.

## Current state

- `CPU.st[8]` is `double` (`runtime/rt.h:22`); all arithmetic runs in 53-bit mantissa.
- 80-bit extended memory format is converted on load/store:
  - `rt_rdf80` (`runtime/rt_fp.c:7`): 64-bit mantissa + 15-bit exponent -> double; `(double)m` rounds 64->53 bits (`rt_fp.c:12`).
  - `rt_wrf80` (`runtime/rt_fp.c:15`): double -> 80-bit, zero-extending mantissa (no precision gained).
- Used at boundary: `rt_fnsave`/`rt_frstor` (x87 save/restore), `rt_fxsave`/`rt_fxrstor` (FXSAVE). Also any generated inline x87 memory access via 80-bit load/store.
- Precision loss points: any 80-bit value with >53 significant bits loses low 11 bits on `rt_rdf80`; round-trip double->80->double is identity only if original was double; 80-bit-only values (e.g. 64-bit mantissa integers, extended-range exponents beyond double max) are clamped/rounded. Status tags (empty/zero/NaN/inf/special) computed from double.

## Options

### (A) Keep double (status quo)
- Effort: none. Risk: low. Correctness: matches current audited baseline (audit_x87 passes for value/TOP). Performance: native double ops, optimal.
- Limits: cannot represent full 80-bit mantissa/exponent; precision loss at 80-bit memory boundary (documented, accepted).

### (B) Full 80-bit via long double / __float128
- Effort: high. Requires changing `CPU.st[8]` type, every op in `rt_fp.c` and inlined generated code, FPU control word rounding modes, exception flags, tag word, save/restore formats, and audit harness expectations. `long double` is 80-bit on x86-64 Linux but 64-bit (double) on aarch64/Android (non-portable); `__float128` lacks hardware support and libquadmath dependency. Risk: high (portability, performance, audit re-baselining). Correctness gain: theoretical full precision, but game code overwhelmingly uses 53-bit-or-less; benefit marginal. Performance: software emulation on ARM, severe.

### (C) Hybrid (80-bit only for specific ops, e.g. load/store or selected transcendentals)
- Effort: medium-high. Complexity of mixed precision domains, partial correctness, unclear benefit. Risk: medium (subtle rounding mismatches). Not recommended.

## Recommendation

Keep (A). Rationale: current double-based runtime passes x87 audit for value/TOP; ME1 game code does not depend on 80-bit mantissa precision; full 80-bit introduces portability (ARM) and performance costs with marginal correctness benefit; conversion loss at memory boundary is bounded and documented. 80-bit memory format preserved for ABI/save-state compatibility.

## Decision criteria / triggers to revisit

- Real-game trace shows precision-dependent divergence attributable to 80-bit mantissa (not reproducible in double).
- New target platform provides native 80-bit+ (x86-64 only) with acceptable perf, and ARM path can emulate cheaply.
- A specific subsystem (e.g. physics) requires >53-bit intermediate precision and is isolated.
- Audit harness extended to compare against true 80-bit reference shows actionable mismatches.

## Proof-of-concept

Optional, experimental only, NOT wired into runtime: none added. (If added later, place under `tools/experiments/` clearly marked experimental; do not touch runtime.)

## Baseline

`make test-x87-audit` run; baseline unchanged (see audit output). No runtime files modified by S8-C.
