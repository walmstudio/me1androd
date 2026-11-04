#!/usr/bin/env python3
"""Builds eh_test.dll: a 32-bit Windows DLL (Keystone-assembled, MSVC-shaped code and EH metadata, with relocations)
that exercises the real msvcr80.dll exception machinery after recompilation:
  SEH  EH3 frames (_except_handler3): __except of RaiseException, filter reading GetExceptionCode via [ebp-14h],
       integer divide by zero (hardware exception), __finally run by local unwind, nested frames + global unwind;
       EH4 frame (local _except_handler4 -> _except_handler4_common, encoded scope table, EH cookie check).
  C++  __CxxFrameHandler3 / _CxxThrowException: throw int / catch(int e) with a destructor in the unwind map,
       non-matching catch(Foo&) then catch(...), catch(...) of an SEH exception (EHFlags=0), rethrow (throw;) from a
       catch block into the caller's catch, try/catch nested inside a catch block.
  Loops (t_loop) and several threads throwing concurrently (t_mt).
The harness (runtime/main_eh.c) checks return values and the counters in .data.
Usage: python3 mk_eh_test.py out.dll"""
import struct, sys
from keystone import Ks, KS_ARCH_X86, KS_MODE_32

BASE = 0x14000000
TEXT, RDATA, DATA, RELOC, SIZE = 0x1000, 0x3000, 0x5000, 0x6000, 0x7000
IMPORTS = [('msvcr80.dll', ['_except_handler3', '_except_handler4_common', '__CxxFrameHandler3', '_CxxThrowException']),
           ('kernel32.dll', ['RaiseException', 'CreateThread', 'WaitForMultipleObjects', 'CloseHandle'])]
V = dict(cookie=0x00, dtor_count=0x04, fin_count=0x08, rethrow_count=0x0C, mt_sum=0x10, mt_handles=0x40)   # .data
COOKIE = 0xBB40E64E
EXPORTS = ['t_seh3', 't_div0', 't_finally', 't_seh4', 't_seh_nested', 't_cxx_int', 't_cxx_multi', 't_cxx_seh',
           't_cxx_rethrow', 't_cxx_nested', 't_loop', 't_mt']

# ------------------------------------------------------------------ code (units placed in order; align=16 unless noted)
CXX_PRO = '''push ebp
mov ebp, esp
push -1
push {fh}
mov eax, dword ptr fs:[0]
push eax
mov dword ptr fs:[0], esp
sub esp, 0x20
push ebx
push esi
push edi
mov dword ptr [ebp-0x10], esp
'''
CXX_EPI = '''mov ecx, dword ptr [ebp-0xc]
mov dword ptr fs:[0], ecx
pop edi
pop esi
pop ebx
mov esp, ebp
pop ebp
ret
'''
SEH3_PRO = '''push ebp
mov ebp, esp
push -1
push {st}
push {h3}
mov eax, dword ptr fs:[0]
push eax
mov dword ptr fs:[0], esp
sub esp, 0x10
push ebx
push esi
push edi
mov dword ptr [ebp-0x18], esp
'''
SEH3_EPI = '''mov dword ptr [ebp-4], -1
mov ecx, dword ptr [ebp-0x10]
mov dword ptr fs:[0], ecx
pop edi
pop esi
pop ebx
mov esp, ebp
pop ebp
ret {ret}
'''
U = []   # (name, asm, align)
def unit(name, asm, align=True): U.append((name, asm, align))

unit('dllmain', 'mov eax, 1\nret 12')
unit('h3', 'jmp dword ptr [{I__except_handler3}]')
unit('h4', '''push ebp
mov ebp, esp
push dword ptr [ebp+0x14]
push dword ptr [ebp+0x10]
push dword ptr [ebp+0xc]
push dword ptr [ebp+8]
push {check_cookie}
push {A_cookie}
call dword ptr [{I__except_handler4_common}]
add esp, 0x18
pop ebp
ret''')
unit('check_cookie', '''cmp ecx, dword ptr [{A_cookie}]
jne bad
ret
bad:
ud2''')
# filter shared by the SEH tests: GetExceptionCode() == expected ([ebp-1ch]) ? EXECUTE_HANDLER : CONTINUE_SEARCH
unit('filt', '''mov eax, dword ptr [ebp-0x14]
mov eax, dword ptr [eax]
xor ecx, ecx
mov edx, dword ptr [ebp-0x1c]
cmp dword ptr [eax], edx
sete cl
mov eax, ecx
ret''')
unit('raise_code', '''push 0
push 0
push 0
push dword ptr [esp+16]
call dword ptr [{I_RaiseException}]
ret 4''')
unit('thr_int', '''push ebp
mov ebp, esp
push ecx
mov eax, dword ptr [ebp+8]
mov dword ptr [ebp-4], eax
push {TI_int}
lea eax, [ebp-4]
push eax
call dword ptr [{I__CxxThrowException}]
ud2''')
unit('fin', 'lock inc dword ptr [{A_fin_count}]\nret')
unit('dtor', 'lock inc dword ptr [{A_dtor_count}]\nret')

# t_seh3: __try { RaiseException(E0000001) } __except(code == E0000001) { return 42 }
unit('t_seh3', SEH3_PRO.replace('{st}', '{ST_seh3}') + '''mov dword ptr [ebp-0x1c], 0xE0000001
mov esi, 0x51
mov dword ptr [ebp-4], 0
push 0
push 0
push 0
push 0xE0000001
call dword ptr [{I_RaiseException}]
mov eax, 1
jmp {t_seh3_out}''')
unit('t_seh3_h', 'mov esp, dword ptr [ebp-0x18]\nmov eax, 42\njmp {t_seh3_out}')
unit('t_seh3_out', SEH3_EPI.replace('{ret}', ''))
# t_div0: __try { 100 / zero } __except(code == C0000094) { return 0x94 }
unit('t_div0', SEH3_PRO.replace('{st}', '{ST_div0}') + '''mov dword ptr [ebp-0x1c], 0xC0000094
mov dword ptr [ebp-0x20], 0
mov dword ptr [ebp-4], 0
mov eax, 100
cdq
idiv dword ptr [ebp-0x20]
jmp {t_div0_out}''')
unit('t_div0_h', 'mov esp, dword ptr [ebp-0x18]\nmov eax, 0x94\njmp {t_div0_out}')
unit('t_div0_out', SEH3_EPI.replace('{ret}', ''))
# t_finally: __try { __try { raise E0000003 } __finally { fin_count++ } } __except(...) { return 7 }
unit('t_finally', SEH3_PRO.replace('{st}', '{ST_fin}') + '''mov dword ptr [ebp-0x1c], 0xE0000003
mov dword ptr [ebp-4], 0
mov dword ptr [ebp-4], 1
push 0xE0000003
call {raise_code}
mov eax, 1
jmp {t_finally_out}''')
unit('t_finally_h', 'mov esp, dword ptr [ebp-0x18]\nmov eax, 7\njmp {t_finally_out}')
unit('t_finally_out', SEH3_EPI.replace('{ret}', ''))
# t_seh4: EH4 frame (encoded scope table, EH cookie at ebp-30h); exception raised two calls deep
unit('t_seh4', '''push ebp
mov ebp, esp
push -2
push {ST_seh4}
push {h4}
mov eax, dword ptr fs:[0]
push eax
sub esp, 0x10
push ebx
push esi
push edi
mov eax, dword ptr [{A_cookie}]
xor dword ptr [ebp-8], eax
xor eax, ebp
push eax
mov dword ptr [ebp-0x18], esp
lea eax, [ebp-0x10]
mov dword ptr fs:[0], eax
mov dword ptr [ebp-0x1c], 0xE0000004
mov dword ptr [ebp-4], 0
push 0xE0000004
call {raise_code}
mov eax, 1
jmp {t_seh4_out}''')
unit('t_seh4_h', 'mov esp, dword ptr [ebp-0x18]\nmov eax, 0x44\njmp {t_seh4_out}')
unit('t_seh4_out', '''mov dword ptr [ebp-4], -2
mov ecx, dword ptr [ebp-0x10]
mov dword ptr fs:[0], ecx
pop ecx
pop edi
pop esi
pop ebx
mov esp, ebp
pop ebp
ret''')
# seh_inner(code): __try { __try { raise code } __finally { fin_count++ } } __except(code == E0000099) { return 9 }
unit('seh_inner', SEH3_PRO.replace('{st}', '{ST_inner}') + '''mov dword ptr [ebp-0x1c], 0xE0000099
mov dword ptr [ebp-4], 0
mov dword ptr [ebp-4], 1
push dword ptr [ebp+8]
call {raise_code}
mov eax, 1
jmp {seh_inner_out}''')
unit('seh_inner_h', 'mov esp, dword ptr [ebp-0x18]\nmov eax, 9\njmp {seh_inner_out}')
unit('seh_inner_out', SEH3_EPI.replace('{ret}', '4'))
# t_seh_nested: __try { seh_inner(E0000005) } __except(code == E0000005) { return 0x55 }  (inner filter declines,
# global unwind runs the inner __finally)
unit('t_seh_nested', SEH3_PRO.replace('{st}', '{ST_nest}') + '''mov dword ptr [ebp-0x1c], 0xE0000005
mov dword ptr [ebp-4], 0
push 0xE0000005
call {seh_inner}
mov eax, 1
jmp {t_seh_nested_out}''')
unit('t_seh_nested_h', 'mov esp, dword ptr [ebp-0x18]\nmov eax, 0x55\njmp {t_seh_nested_out}')
unit('t_seh_nested_out', SEH3_EPI.replace('{ret}', ''))

# t_cxx_int: try { Obj o; throw 7; } catch (int e) { r = e * 10; } return r + 1;     (Obj::~Obj -> dtor_count++)
unit('fh_int', 'mov eax, {FI_int}\njmp dword ptr [{I___CxxFrameHandler3}]')
unit('t_cxx_int', CXX_PRO.replace('{fh}', '{fh_int}') + '''mov dword ptr [ebp-0x18], 0
mov dword ptr [ebp-4], 0
mov dword ptr [ebp-4], 1
push 7
call {thr_int}
ud2''')
unit('t_cxx_int_catch', '''mov eax, dword ptr [ebp-0x14]
imul eax, eax, 10
mov dword ptr [ebp-0x18], eax
mov eax, {t_cxx_int_cont}
ret''')
unit('t_cxx_int_cont', 'mov dword ptr [ebp-4], -1\nmov eax, dword ptr [ebp-0x18]\ninc eax\n' + CXX_EPI)
unit('t_cxx_int_unw', 'lea ecx, [ebp-0x1c]\njmp {dtor}')
# t_cxx_multi: try { throw 3; } catch (Foo&) { r = 1; } catch (...) { r = 55; } return r;
unit('fh_multi', 'mov eax, {FI_multi}\njmp dword ptr [{I___CxxFrameHandler3}]')
unit('t_cxx_multi', CXX_PRO.replace('{fh}', '{fh_multi}') + '''mov dword ptr [ebp-4], 0
push 3
call {thr_int}
ud2''')
unit('t_cxx_multi_c1', 'mov dword ptr [ebp-0x18], 1\nmov eax, {t_cxx_multi_cont}\nret')
unit('t_cxx_multi_c2', 'mov dword ptr [ebp-0x18], 55\nmov eax, {t_cxx_multi_cont}\nret')
unit('t_cxx_multi_cont', 'mov dword ptr [ebp-4], -1\nmov eax, dword ptr [ebp-0x18]\n' + CXX_EPI)
# t_cxx_seh: try { RaiseException(E0000007) } catch (...) { r = 77; }   (EHFlags = 0: catch(...) sees SEH)
unit('fh_seh', 'mov eax, {FI_seh}\njmp dword ptr [{I___CxxFrameHandler3}]')
unit('t_cxx_seh', CXX_PRO.replace('{fh}', '{fh_seh}') + '''mov dword ptr [ebp-4], 0
push 0xE0000007
call {raise_code}
ud2''')
unit('t_cxx_seh_c', 'mov dword ptr [ebp-0x18], 77\nmov eax, {t_cxx_seh_cont}\nret')
unit('t_cxx_seh_cont', 'mov dword ptr [ebp-4], -1\nmov eax, dword ptr [ebp-0x18]\n' + CXX_EPI)
# rethrow: cxx_inner: try { throw 5; } catch (int) { rethrow_count++; throw; }
#          t_cxx_rethrow: try { cxx_inner(); } catch (int e) { r = e + 1000; } return r;
unit('fh_inner', 'mov eax, {FI_inner}\njmp dword ptr [{I___CxxFrameHandler3}]')
unit('cxx_inner', CXX_PRO.replace('{fh}', '{fh_inner}') + '''mov dword ptr [ebp-4], 0
push 5
call {thr_int}
ud2''')
unit('cxx_inner_c', '''lock inc dword ptr [{A_rethrow_count}]
push 0
push 0
call dword ptr [{I__CxxThrowException}]
ud2''')
unit('fh_rt', 'mov eax, {FI_rt}\njmp dword ptr [{I___CxxFrameHandler3}]')
unit('t_cxx_rethrow', CXX_PRO.replace('{fh}', '{fh_rt}') + '''mov dword ptr [ebp-4], 0
call {cxx_inner}
mov dword ptr [ebp-0x18], 1
jmp {t_cxx_rethrow_cont}''')
unit('t_cxx_rethrow_c', 'mov eax, dword ptr [ebp-0x14]\nadd eax, 1000\nmov dword ptr [ebp-0x18], eax\nmov eax, {t_cxx_rethrow_cont}\nret')
unit('t_cxx_rethrow_cont', 'mov dword ptr [ebp-4], -1\nmov eax, dword ptr [ebp-0x18]\n' + CXX_EPI)
# t_cxx_nested: try { throw 1; } catch (int) { try { throw 2; } catch (int e2) { r = e2 * 100; } r += 1; } return r;
unit('fh_nest', 'mov eax, {FI_nest}\njmp dword ptr [{I___CxxFrameHandler3}]')
unit('t_cxx_nested', CXX_PRO.replace('{fh}', '{fh_nest}') + '''mov dword ptr [ebp-0x18], 0
mov dword ptr [ebp-4], 0
push 1
call {thr_int}
ud2''')
unit('t_cxx_nested_c1', '''mov dword ptr [ebp-0x10], esp
mov dword ptr [ebp-4], 2
push 2
call {thr_int}''')
unit('t_cxx_nested_c1cont', 'mov dword ptr [ebp-4], 1\ninc dword ptr [ebp-0x18]\nmov eax, {t_cxx_nested_cont}\nret', align=False)
unit('t_cxx_nested_c2', 'mov eax, dword ptr [ebp-0x14]\nimul eax, eax, 100\nmov dword ptr [ebp-0x18], eax\nmov eax, {t_cxx_nested_c1cont}\nret')
unit('t_cxx_nested_cont', 'mov dword ptr [ebp-4], -1\nmov eax, dword ptr [ebp-0x18]\n' + CXX_EPI)
# t_loop(n): sum of n rounds of (t_cxx_int + t_seh4 + t_cxx_rethrow + t_finally + t_cxx_nested)
unit('t_loop', '''push ebx
push esi
xor esi, esi
mov ebx, dword ptr [esp+12]
test ebx, ebx
je done
again:
call {t_cxx_int}
add esi, eax
call {t_seh4}
add esi, eax
call {t_cxx_rethrow}
add esi, eax
call {t_finally}
add esi, eax
call {t_cxx_nested}
add esi, eax
dec ebx
jne again
done:
mov eax, esi
pop esi
pop ebx
ret 4''')
unit('mt_worker', '''push dword ptr [esp+4]
call {t_loop}
lock add dword ptr [{A_mt_sum}], eax
xor eax, eax
ret 4''')
# t_mt(nthreads, iters): threads run t_loop(iters) concurrently; result in mt_sum
unit('t_mt', '''push ebx
push esi
push edi
mov edi, dword ptr [esp+16]
mov esi, dword ptr [esp+20]
xor ebx, ebx
spawn:
push 0
push 0
push esi
push {mt_worker}
push 0
push 0
call dword ptr [{I_CreateThread}]
mov dword ptr [ebx*4+{A_mt_handles}], eax
inc ebx
cmp ebx, edi
jb spawn
push -1
push 1
push {A_mt_handles}
push edi
call dword ptr [{I_WaitForMultipleObjects}]
mov esi, eax
xor ebx, ebx
close:
push dword ptr [ebx*4+{A_mt_handles}]
call dword ptr [{I_CloseHandle}]
inc ebx
cmp ebx, edi
jb close
mov eax, esi
pop edi
pop esi
pop ebx
ret 8''')

# ------------------------------------------------------------------ .rdata layout (fixed offsets)
R = {}
def ralloc(name, size, off=[0x400]):
    R[name] = RDATA + off[0]; off[0] += (size + 3) & ~3
for n, sz in [('TD_int', 12), ('TD_foo', 20), ('CT_int', 28), ('CTA_int', 8), ('TI_int', 16),
              ('ST_seh3', 12), ('ST_div0', 12), ('ST_fin', 24), ('ST_seh4', 16 + 12), ('ST_inner', 24), ('ST_nest', 12),
              ('UM_int', 24), ('HA_int', 16), ('TB_int', 20), ('FI_int', 36),
              ('UM_multi', 16), ('HA_multi', 32), ('TB_multi', 20), ('FI_multi', 36),
              ('UM_seh', 16), ('HA_seh', 16), ('TB_seh', 20), ('FI_seh', 36),
              ('UM_inner', 16), ('HA_inner', 16), ('TB_inner', 20), ('FI_inner', 36),
              ('UM_rt', 16), ('HA_rt', 16), ('TB_rt', 20), ('FI_rt', 36),
              ('UM_nest', 32), ('HA_nest1', 16), ('HA_nest2', 16), ('TB_nest', 40), ('FI_nest', 36)]:
    ralloc(n, sz)
assert max(R.values()) < RDATA + 0xB00

def build(base):
    """Whole image (without PE headers / .reloc) for a given image base."""
    rb = lambda rva: base + rva
    img = bytearray(SIZE)
    # imports
    rd = bytearray(0x1000); put = lambda off, b: rd.__setitem__(slice(off, off + len(b)), b)
    p = 0x200; iat = 0x100; ilt = 0x40; idt = 0; IAT = {}; k = 0
    for d, (dll, names) in enumerate(IMPORTS):
        ilt0, iat0 = ilt + 4 * k, iat + 4 * k
        for n in names:
            put(p, struct.pack('<H', 0) + n.encode() + b'\0'); hint = RDATA + p; p += 2 + len(n) + 1; p += p & 1
            put(ilt + 4 * k, struct.pack('<I', hint)); put(iat + 4 * k, struct.pack('<I', hint)); IAT[n] = rb(RDATA + iat + 4 * k); k += 1
        k += 1   # null terminator
        put(p, dll.encode() + b'\0'); dn = RDATA + p; p += len(dll) + 1; p += p & 1
        put(idt + 20 * d, struct.pack('<5I', RDATA + ilt0, 0, 0, dn, RDATA + iat0))
    assert p < 0x400
    fmt = {'I_' + n: '0x%x' % a for n, a in IAT.items()}
    for n, o in V.items(): fmt['A_' + n] = '0x%x' % rb(DATA + o)
    for n, r in R.items(): fmt[n] = '0x%x' % rb(r)
    # code: iterate unit placement until sizes are stable
    ks = Ks(KS_ARCH_X86, KS_MODE_32)
    addr = {n: rb(TEXT) + 0x800 for n, _, _ in U}
    for it in range(8):
        f2 = dict(fmt); f2.update({n: '0x%x' % a for n, a in addr.items()})
        a = rb(TEXT); new = {}; blobs = []
        for n, asm, al in U:
            if al: a = (a + 15) & ~15
            new[n] = a
            code, _ = ks.asm(asm.format(**f2), a); code = bytes(code or b'')
            blobs.append((a, code)); a += len(code)
        if new == addr: break
        addr = new
    else: raise SystemExit('unit layout did not converge')
    assert a - rb(TEXT) < 0x2000
    for va, code in blobs: o = va - base; img[o:o + len(code)] = code
    # EH metadata
    def W(name, *vals, off=0):
        r = R[name] + off; struct.pack_into('<%dI' % len(vals), rd, r - RDATA, *[v & 0xffffffff for v in vals])
    A = lambda n: rb(R[n]); C = lambda n: addr[n]
    def td(name, s):
        W(name, 0, 0); b = s.encode() + b'\0'; rd[R[name] - RDATA + 8:R[name] - RDATA + 8 + len(b)] = b
    td('TD_int', '.H'); td('TD_foo', '.?AVFoo@@')
    W('CT_int', 1, A('TD_int'), 0, -1, 0, 4, 0); W('CTA_int', 1, A('CT_int')); W('TI_int', 0, 0, 0, A('CTA_int'))
    W('ST_seh3', -1, C('filt'), C('t_seh3_h'))
    W('ST_div0', -1, C('filt'), C('t_div0_h'))
    W('ST_fin', -1, C('filt'), C('t_finally_h'), 0, 0, C('fin'))
    W('ST_seh4', -2, 0, -0x30, 0, -2, C('filt'), C('t_seh4_h'))
    W('ST_inner', -1, C('filt'), C('seh_inner_h'), 0, 0, C('fin'))
    W('ST_nest', -1, C('filt'), C('t_seh_nested_h'))
    def fi(sfx, maxst, um, tb, ntb, flags):
        W('FI_' + sfx, 0x19930522, maxst, A(um), ntb, A(tb), 0, 0, 0, flags)
    W('UM_int', -1, 0, 0, C('t_cxx_int_unw'), -1, 0)
    W('HA_int', 0, A('TD_int'), -0x14, C('t_cxx_int_catch')); W('TB_int', 0, 1, 2, 1, A('HA_int')); fi('int', 3, 'UM_int', 'TB_int', 1, 1)
    W('UM_multi', -1, 0, -1, 0)
    W('HA_multi', 8, A('TD_foo'), 0, C('t_cxx_multi_c1'), 0, 0, 0, C('t_cxx_multi_c2'))
    W('TB_multi', 0, 0, 1, 2, A('HA_multi')); fi('multi', 2, 'UM_multi', 'TB_multi', 1, 1)
    W('UM_seh', -1, 0, -1, 0); W('HA_seh', 0, 0, 0, C('t_cxx_seh_c')); W('TB_seh', 0, 0, 1, 1, A('HA_seh')); fi('seh', 2, 'UM_seh', 'TB_seh', 1, 0)
    W('UM_inner', -1, 0, -1, 0); W('HA_inner', 0, A('TD_int'), 0, C('cxx_inner_c')); W('TB_inner', 0, 0, 1, 1, A('HA_inner'))
    fi('inner', 2, 'UM_inner', 'TB_inner', 1, 1)
    W('UM_rt', -1, 0, -1, 0); W('HA_rt', 0, A('TD_int'), -0x14, C('t_cxx_rethrow_c')); W('TB_rt', 0, 0, 1, 1, A('HA_rt'))
    fi('rt', 2, 'UM_rt', 'TB_rt', 1, 1)
    W('UM_nest', -1, 0, -1, 0, 1, 0, 1, 0)
    W('HA_nest2', 0, A('TD_int'), -0x14, C('t_cxx_nested_c2')); W('HA_nest1', 0, A('TD_int'), 0, C('t_cxx_nested_c1'))
    W('TB_nest', 2, 2, 3, 1, A('HA_nest2'), 0, 0, 3, 1, A('HA_nest1')); fi('nest', 4, 'UM_nest', 'TB_nest', 2, 1)
    # exports
    exp = 0xC00; en = sorted(EXPORTS); af, an, ao, nm = exp + 40, exp + 40 + 4 * len(en), exp + 40 + 8 * len(en), exp + 40 + 10 * len(en)
    q = nm; dname = RDATA + q; put(q, b'eh_test.dll\0'); q += 12
    put(exp, struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, dname, 1, len(en), len(en), RDATA + af, RDATA + an, RDATA + ao))
    for k, n in enumerate(en):
        put(an + 4 * k, struct.pack('<I', RDATA + q)); put(q, n.encode() + b'\0'); q += len(n) + 1
        put(af + 4 * k, struct.pack('<I', addr[n] - base)); put(ao + 2 * k, struct.pack('<H', k))
    assert q < 0x1000
    img[RDATA:RDATA + 0x1000] = rd
    struct.pack_into('<I', img, DATA + V['cookie'], COOKIE)
    meta = dict(entry=addr['dllmain'] - base, exp=(RDATA + exp, q - exp), idt=(RDATA, 20 * (len(IMPORTS) + 1)),
                iat=(RDATA + 0x100, 4 * (sum(len(n) + 1 for _, n in IMPORTS))), addr=addr)
    return img, meta

img, meta = build(BASE)
img2, _ = build(BASE + 0x10000)
# relocations: dwords that move with the image base
rel = []; i = 0x1000
while i < RELOC - 3:
    a, b = struct.unpack_from('<I', img, i)[0], struct.unpack_from('<I', img2, i)[0]
    if (b - a) & 0xffffffff == 0x10000: rel.append(i); i += 4
    else: i += 1
covered = set(j for r in rel for j in range(r, r + 4))
bad = [j for j in range(0x1000, RELOC) if img[j] != img2[j] and j not in covered]
if bad: raise SystemExit('unexplained diffs at ' + ' '.join('%x' % j for j in bad[:8]))
blk = bytearray(); pages = sorted(set(r & ~0xfff for r in rel))
for pg in pages:
    ent = [0x3000 | (r - pg) for r in rel if r & ~0xfff == pg]
    if len(ent) & 1: ent.append(0)
    blk += struct.pack('<II', pg, 8 + 2 * len(ent)) + struct.pack('<%dH' % len(ent), *ent)
img[RELOC:RELOC + len(blk)] = blk
# headers
img[0:2] = b'MZ'; struct.pack_into('<I', img, 0x3C, 0x80)
o = 0x80; img[o:o + 4] = b'PE\0\0'
struct.pack_into('<HHIIIHH', img, o + 4, 0x14C, 4, 0x45483031, 0, 0, 0xE0, 0x2102)
opt = o + 24
struct.pack_into('<HBBIIIIIII', img, opt, 0x10B, 8, 0, 0x2000, 0x3000, 0, meta['entry'], TEXT, RDATA, BASE)
struct.pack_into('<IIHHHHHHIIIIHHIIIIII', img, opt + 32, 0x1000, 0x1000, 4, 0, 0, 0, 4, 0, 0, SIZE, 0x1000, 0, 2, 0x40,
                 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
dirs = [(0, 0)] * 16
dirs[0] = meta['exp']; dirs[1] = meta['idt']; dirs[5] = (RELOC, len(blk)); dirs[12] = meta['iat']
for k, (r, s) in enumerate(dirs): struct.pack_into('<II', img, opt + 96 + 8 * k, r, s)
sec = opt + 0xE0
for k, (n, va, vs, fl) in enumerate([(b'.text', TEXT, 0x2000, 0x60000020), (b'.rdata', RDATA, 0x2000, 0x40000040),
                                     (b'.data', DATA, 0x1000, 0xC0000040), (b'.reloc', RELOC, 0x1000, 0x42000040)]):
    struct.pack_into('<8sIIIIIIHHI', img, sec + 40 * k, n, vs, va, vs, va, 0, 0, 0, 0, fl)
open(sys.argv[1] if len(sys.argv) > 1 else 'eh_test.dll', 'wb').write(img)
print('eh_test.dll: %d relocations, exports %s' % (len(rel), ' '.join('%s=%x' % (n, meta['addr'][n]) for n in EXPORTS)))
