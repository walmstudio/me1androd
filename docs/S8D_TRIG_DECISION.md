# S8-D: trig-fix default decision

## Status

Decision recorded: **keep `--trig-fix` opt-in; default off.** No runtime/generator behavior changes.

## Evidence gathered

- `tests/recomp/audit_x87_inline_native.py` (baseline, x86-64, gcc, seed 11): values and TOP match for FSQRT, FCHS, FABS, FRNDINT, FXTRACT, FSCALE, F2XM1 (|x|<=1), FPATAN, FYL2X, FPREM, FPREM1 (non-partial), FSIN/FCOS/FSINCOS for |x|<2^63. Residual value mismatches: FPTAN at +/-inf/NaN (4 cases) and FSIN/FCOS/FSINCOS/FPTAN at |x|>=2^63 (660-710 cases per op, known, skipped as out-of-range trig). FYL2XP1 mismatches only at subnormal x with huge y (6 cases, documented precision limit).
- `--trig-fix` audit: 0 value/TOP mismatches for FSIN, FCOS, FSINCOS, FPTAN, including |x|>=2^63, +/-inf and NaN. Status-bit differences (PE/IE/C1) remain expected and non-fatal for both modes.
- `tools/recomp_x87_fixed.py`: addresses FPTAN +/-inf/NaN and large-arg trig (|x|>=2^63) by setting C2 without changing ST(0)/TOP for finite out-of-range; maps +/-inf and NaN to invalid (sets IE). Requires regeneration + rebuild.

## Decision criteria to flip default

(a) ARM real-game smoke test passes.
(b) Full regen with --trig-fix produces bit-identical or improved traces.
(c) No new faults in mt/eh/vorbis/bink tests.
(d) audit_x87 reports 0 mismatches in trigfix mode (already satisfied for value/TOP).

## Recommendation

Keep --trig-fix opt-in, default off. Rationale: full regen wall (all generated C changes), correctness-vs-speed tradeoff unmeasured, and device evidence (G2) pending. Flipping default requires ARM + real game + regen.

## Risk register

- Regen wall: entire out/ must be regenerated and rebuilt.
- Correctness vs speed: trig fix adds branches and isnan/isinf/fabs per op; perf impact unmeasured.
- Mali fragility: trig paths exercised by real game on Mali GPUs not yet tested.
