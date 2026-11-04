#!/usr/bin/env python3
"""Builds mt_test.dll: a tiny 32-bit Windows DLL (Keystone-assembled) that stresses guest threads and sync.
mt_run(n, iters) starts n threads (CREATE_SUSPENDED + ResumeThread) that all wait on a manual-reset event, then each
does `iters` rounds of: lock xadd, lock inc, misaligned lock add, CRITICAL_SECTION counter, lock-cmpxchg spinlock
with a plain-store release, xchg spinlock, InterlockedIncrement, 64-bit lock cmpxchg8b, and every 16th round a
mutex-protected counter. Also: lock bts bitmap, TLS per thread, unique thread ids, ReleaseSemaphore, exit codes via
ExitThread (odd threads) / return (even), DllMain THREAD_ATTACH/DETACH counts, wait timeouts, auto-reset events,
mutex recursion. The harness (runtime/main_mt.c) checks the counters in .data afterwards.
Usage: python3 mk_mt_test.py out.dll"""
import struct, sys
from keystone import Ks, KS_ARCH_X86, KS_MODE_32

BASE = 0x13000000
TEXT, RDATA, DATA, SIZE = 0x1000, 0x3000, 0x4000, 0x5000
IMPORTS = ['CreateThread', 'ResumeThread', 'ExitThread', 'GetExitCodeThread', 'WaitForSingleObject', 'WaitForMultipleObjects',
           'CloseHandle', 'CreateEventA', 'SetEvent', 'CreateSemaphoreA', 'ReleaseSemaphore', 'CreateMutexA', 'ReleaseMutex',
           'InitializeCriticalSection', 'EnterCriticalSection', 'LeaveCriticalSection', 'DeleteCriticalSection',
           'TlsAlloc', 'TlsFree', 'TlsSetValue', 'TlsGetValue', 'InterlockedIncrement', 'GetCurrentThreadId', 'Sleep']
# .data layout (offsets from BASE+DATA); keep in sync with runtime/main_mt.c
V = dict(c_xadd=0x00, c_inc=0x04, c_cs=0x08, c_mutex=0x0C, c_spin=0x10, spin=0x14, c_spin2=0x18, spin2=0x1C, c_api=0x20,
         bitmap=0x24, c64=0x28, tls_index=0x30, tls_fail=0x34, start_ev=0x38, sem=0x3C, mutex=0x40, iters=0x44,
         nthreads=0x48, c_plain=0x4C, res_wait_all=0x50, res_sem_ok=0x54, res_sem_last=0x58, res_timeout=0x5C,
         res_auto1=0x60, res_auto2=0x64, res_mtx=0x68, exitcodes_ok=0x6C, c_mis=0x71, n_attach=0x78, n_detach=0x7C,
         handles=0x80, tids=0x100, cs=0x180, tmp=0x1A0, tmp2=0x1A4)

# ---- .rdata: import + export tables ----
rd = bytearray(0x1000)
def put(off, b): rd[off:off + len(b)] = b
idt, ilt, iat, names = 0x000, 0x040, 0x100, 0x200
p = names; hint_rva = []
for n in IMPORTS:
    hint_rva.append(RDATA + p); put(p, struct.pack('<H', 0) + n.encode() + b'\0'); p += 2 + len(n) + 1; p += p & 1
dllname = RDATA + p; put(p, b'kernel32.dll\0'); p += 14
for k, r in enumerate(hint_rva): put(ilt + 4 * k, struct.pack('<I', r)); put(iat + 4 * k, struct.pack('<I', r))
put(idt, struct.pack('<5I', RDATA + ilt, 0, 0, dllname, RDATA + iat))
IAT = {n: BASE + RDATA + iat + 4 * k for k, n in enumerate(IMPORTS)}
exp = 0x800

def A(name): return BASE + DATA + V[name]
def I(name): return 'dword ptr [0x%x]' % IAT[name]
def D(name): return 'dword ptr [0x%x]' % A(name)

src = '''
dllmain:
  mov eax, [esp+8]
  cmp eax, 2
  jne dm1
  lock inc {n_attach}
dm1:
  cmp eax, 3
  jne dm2
  lock inc {n_detach}
dm2:
  mov eax, 1
  ret 12

mt_worker:
  push ebx
  push esi
  push edi
  push ebp
  mov ebp, [esp+20]
  call {GetCurrentThreadId}
  mov [0x{tids:x} + ebp*4], eax
  lea eax, [ebp+100]
  push eax
  push {tls_index}
  call {TlsSetValue}
  push -1
  push {start_ev}
  call {WaitForSingleObject}
  lock bts {bitmap}, ebp
  mov esi, {iters}
wloop:
  mov eax, 1
  lock xadd {c_xadd}, eax
  lock inc {c_inc}
  add {c_plain}, 1
  lock add {c_mis}, 1
  push 0x{cs:x}
  call {EnterCriticalSection}
  inc {c_cs}
  push 0x{cs:x}
  call {LeaveCriticalSection}
spin1:
  xor eax, eax
  mov edx, 1
  lock cmpxchg {spin}, edx
  jz got1
  pause
  jmp spin1
got1:
  mov eax, {c_spin}
  inc eax
  mov {c_spin}, eax
  mov {spin}, 0
spin2:
  mov eax, 1
  xchg {spin2}, eax
  test eax, eax
  jz got2
  pause
  jmp spin2
got2:
  inc {c_spin2}
  mov {spin2}, 0
  push 0x{c_api:x}
  call {InterlockedIncrement}
  mov eax, dword ptr [0x{c64:x}]
  mov edx, [0x{c64:x}+4]
c8:
  mov ebx, eax
  mov ecx, edx
  add ebx, 1
  adc ecx, 0
  lock cmpxchg8b qword ptr [0x{c64:x}]
  jnz c8
  test esi, 15
  jnz skipm
  push -1
  push {mutex}
  call {WaitForSingleObject}
  inc {c_mutex}
  push {mutex}
  call {ReleaseMutex}
skipm:
  dec esi
  jnz wloop
  push {tls_index}
  call {TlsGetValue}
  lea ecx, [ebp+100]
  cmp eax, ecx
  je tlsok
  lock inc {tls_fail}
tlsok:
  push 0
  push 1
  push {sem}
  call {ReleaseSemaphore}
  lea eax, [ebp*8]
  sub eax, ebp
  inc eax
  test ebp, 1
  jz wret
  push eax
  call {ExitThread}
wret:
  pop ebp
  pop edi
  pop esi
  pop ebx
  ret 4

mt_run:
  push ebx
  push esi
  push edi
  push ebp
  mov eax, [esp+20]
  mov {nthreads}, eax
  mov eax, [esp+24]
  mov {iters}, eax
  push 0x{cs:x}
  call {InitializeCriticalSection}
  call {TlsAlloc}
  mov {tls_index}, eax
  push 0
  push 0
  push 1
  push 0
  call {CreateEventA}
  mov {start_ev}, eax
  push 0
  push 100
  push 0
  push 0
  call {CreateSemaphoreA}
  mov {sem}, eax
  push 0
  push 0
  push 0
  call {CreateMutexA}
  mov {mutex}, eax
  xor esi, esi
create:
  push 0x{tmp:x}
  push 4
  push esi
  push 0x{mt_worker_va:x}
  push 0
  push 0
  call {CreateThread}
  mov [0x{handles:x} + esi*4], eax
  push eax
  call {ResumeThread}
  inc esi
  cmp esi, {nthreads}
  jb create
  push 20
  call {Sleep}
  push {start_ev}
  call {SetEvent}
  push -1
  push 1
  push 0x{handles:x}
  push {nthreads}
  call {WaitForMultipleObjects}
  mov {res_wait_all}, eax
  xor esi, esi
ec:
  push 0x{tmp2:x}
  push dword ptr [0x{handles:x} + esi*4]
  call {GetExitCodeThread}
  lea eax, [esi*8]
  sub eax, esi
  inc eax
  cmp eax, dword ptr [0x{tmp2:x}]
  jne ecbad
  inc {exitcodes_ok}
ecbad:
  push dword ptr [0x{handles:x} + esi*4]
  call {CloseHandle}
  inc esi
  cmp esi, {nthreads}
  jb ec
  xor esi, esi
sm:
  push 0
  push {sem}
  call {WaitForSingleObject}
  test eax, eax
  jnz smbad
  inc {res_sem_ok}
smbad:
  inc esi
  cmp esi, {nthreads}
  jb sm
  push 0
  push {sem}
  call {WaitForSingleObject}
  mov {res_sem_last}, eax
  push 0
  push 0
  push 0
  push 0
  call {CreateEventA}
  mov edi, eax
  push 30
  push edi
  call {WaitForSingleObject}
  mov {res_timeout}, eax
  push edi
  call {SetEvent}
  push 0
  push edi
  call {WaitForSingleObject}
  mov {res_auto1}, eax
  push 0
  push edi
  call {WaitForSingleObject}
  mov {res_auto2}, eax
  push edi
  call {CloseHandle}
  push -1
  push {mutex}
  call {WaitForSingleObject}
  push -1
  push {mutex}
  call {WaitForSingleObject}
  push {mutex}
  call {ReleaseMutex}
  mov ebx, eax
  push {mutex}
  call {ReleaseMutex}
  shl eax, 1
  or ebx, eax
  push {mutex}
  call {ReleaseMutex}
  shl eax, 2
  or ebx, eax
  mov {res_mtx}, ebx
  push {mutex}
  call {CloseHandle}
  push {sem}
  call {CloseHandle}
  push {start_ev}
  call {CloseHandle}
  push 0x{cs:x}
  call {DeleteCriticalSection}
  push {tls_index}
  call {TlsFree}
  xor eax, eax
  pop ebp
  pop edi
  pop esi
  pop ebx
  ret 8
'''
fmt = {n: I(n) for n in IMPORTS}
for k in V: fmt[k] = D(k)
for k in ('tids', 'cs', 'handles', 'tmp', 'tmp2', 'c_api', 'c64'): fmt[k] = A(k)
ks = Ks(KS_ARCH_X86, KS_MODE_32)
def label_va(src, lbl):   # address of a label = size of the code before it
    pre = src[:src.index('\n' + lbl + ':')]
    b, _ = ks.asm(pre, BASE + TEXT) if pre.strip() else ([], 0)
    return BASE + TEXT + len(b or [])
fmt['mt_worker_va'] = BASE + TEXT + 0x100              # pass 1: placeholder of the same encoded size
syms = {n: label_va(src.format(**fmt), n) for n in ('dllmain', 'mt_worker', 'mt_run')}
fmt['mt_worker_va'] = syms['mt_worker']
src = src.format(**fmt)
code, _ = ks.asm(src, BASE + TEXT)
code = bytes(code)
assert len(code) < 0x2000

# exports (sorted names)
enames = sorted(['mt_run', 'mt_worker'])
af, an, ao, nm = exp + 40, exp + 48, exp + 56, exp + 64
q = nm; name_rvas = []
dname = RDATA + q; put(q, b'mt_test.dll\0'); q += 12
for n in enames: name_rvas.append(RDATA + q); put(q, n.encode() + b'\0'); q += len(n) + 1
put(exp, struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, dname, 1, 2, 2, RDATA + af, RDATA + an, RDATA + ao))
for k, n in enumerate(enames):
    put(af + 4 * k, struct.pack('<I', syms[n] - BASE)); put(an + 4 * k, struct.pack('<I', name_rvas[k])); put(ao + 2 * k, struct.pack('<H', k))
exp_size = q - exp

# ---- headers ----
img = bytearray(SIZE)
img[0:2] = b'MZ'; struct.pack_into('<I', img, 0x3C, 0x80)
o = 0x80; img[o:o + 4] = b'PE\0\0'
struct.pack_into('<HHIIIHH', img, o + 4, 0x14C, 3, 0x4D543031, 0, 0, 0xE0, 0x2102)
opt = o + 24
struct.pack_into('<HBBIIIIIII', img, opt, 0x10B, 8, 0, 0x2000, 0x2000, 0, syms['dllmain'] - BASE, TEXT, RDATA, BASE)
struct.pack_into('<IIHHHHHHIIIIHHIIIIII', img, opt + 32, 0x1000, 0x1000, 4, 0, 0, 0, 4, 0, 0, SIZE, 0x1000, 0, 2, 0,
                 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
dirs = [(0, 0)] * 16
dirs[0] = (RDATA + exp, exp_size); dirs[1] = (RDATA + idt, 40); dirs[12] = (RDATA + iat, 4 * len(IMPORTS) + 4)
for k, (r, s) in enumerate(dirs): struct.pack_into('<II', img, opt + 96 + 8 * k, r, s)
sec = opt + 0xE0
for k, (n, va, fl) in enumerate([(b'.text', TEXT, 0x60000020), (b'.rdata', RDATA, 0x40000040), (b'.data', DATA, 0xC0000040)]):
    vs = 0x2000 if n == b'.text' else 0x1000
    struct.pack_into('<8sIIIIIIHHI', img, sec + 40 * k, n, vs, va, vs, va, 0, 0, 0, 0, fl)
img[TEXT:TEXT + len(code)] = code
img[RDATA:RDATA + 0x1000] = rd
open(sys.argv[1] if len(sys.argv) > 1 else 'mt_test.dll', 'wb').write(img)
print('mt_test.dll: %d bytes code, exports %s' % (len(code), {k: hex(v) for k, v in syms.items()}))
