/* Structured exception handling for recompiled guest code.
 *
 * What runs where:
 *  - guest code: everything the MSVC runtime does - _except_handler3/4 (SEH __try/__except/__finally),
 *    __CxxFrameHandler3 / _CxxThrowException (C++ try/catch/throw, destructors during unwinding), filters, funclets.
 *    These come from the recompiled msvcr80.dll (or a statically linked CRT) and need no changes.
 *  - this file: the Windows parts underneath - RaiseException (KiUserExceptionDispatcher/RtlDispatchException:
 *    walk FS:[0], call each handler with a nested-exception frame like ExecuteHandler2, handle the dispositions),
 *    RtlUnwind, Set/UnhandledExceptionFilter, VirtualQuery (msvcr80 validates SEH frames with it), and hardware
 *    exceptions raised by translated code (rt_raise_hw: #DE, int3, ud2).
 *  - the only non-local transfer, the final jump of the CRT into an __except block or a catch continuation, reaches
 *    dispatch_call with a target marked as an exception landing (FuncEntry.owner). rt_eh_land finds the live host frame
 *    of the owner function (pushed by its generated wrapper, matched by owner + EBP) and longjmps there; the landing
 *    code then runs as a tail call of the owner, so its eventual `ret` returns to the owner's real caller.
 * Env ME1_TRACE_EH=1: log every exception and landing. */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>

#define EXC_NONCONTINUABLE 1u
#define EXC_UNWINDING      2u
#define EXC_EXIT_UNWIND    4u
#define EXC_STACK_INVALID  8u
#define EXC_NESTED_CALL    0x10u
enum { ContinueExecution = 0, ContinueSearch = 1, NestedException = 2, CollidedUnwind = 3 };
#define REC_SIZE 0x50u
#define CTX_SIZE 0x2CCu

static int g_trace = -1;
static uint32_t g_top_filter;
static int trace(void) { if (g_trace < 0) { const char* e = getenv("ME1_TRACE_EH"); g_trace = e ? atoi(e) : 0; } return g_trace; }

/* scratch area on the guest stack below the current ESP: EXCEPTION_RECORD, CONTEXT, nested frame, EXCEPTION_POINTERS */
typedef struct { uint32_t base, rec, ctx, nest, xp; } Area;
static Area area_below(uint32_t esp) {
    Area a; a.base = (esp - 0x400u) & ~15u;
    a.rec = a.base; a.ctx = a.base + REC_SIZE; a.nest = a.ctx + CTX_SIZE + 4; a.nest = (a.nest + 15u) & ~15u; a.xp = a.nest + 16;
    return a;
}
static void fill_ctx(CPU* c, uint32_t x, uint32_t eip, uint32_t esp) {
    memset(M + x, 0, CTX_SIZE);
    wr32(x + 0x00, 0x10007u);                       /* CONTEXT_FULL */
    wr32(x + 0x1C, c->fcw); wr32(x + 0x20, c->fsw);
    wr32(x + 0x90, 0x3B); wr32(x + 0x94, 0x23); wr32(x + 0x98, 0x23);
    wr32(x + 0x9C, c->edi); wr32(x + 0xA0, c->esi); wr32(x + 0xA4, c->ebx); wr32(x + 0xA8, c->edx);
    wr32(x + 0xAC, c->ecx); wr32(x + 0xB0, c->eax); wr32(x + 0xB4, c->ebp); wr32(x + 0xB8, eip);
    wr32(x + 0xBC, 0x1B); wr32(x + 0xC0, 0x202); wr32(x + 0xC4, esp); wr32(x + 0xC8, 0x23);
    wr32(x + 0xCC + 24, c->mxcsr);
}
static void fill_rec(uint32_t r, uint32_t code, uint32_t flags, uint32_t addr, uint32_t n, const uint32_t* info) {
    memset(M + r, 0, REC_SIZE);
    wr32(r, code); wr32(r + 4, flags); wr32(r + 12, addr); wr32(r + 16, n);
    for (uint32_t i = 0; i < n && i < 15; i++) wr32(r + 20 + 4 * i, info[i]);
}
static const char* cxx_type(uint32_t r) {   /* C++ exception (0xE06D7363): name of the first catchable type */
    static _Thread_local char b[96];
    if (rd32(r) != 0xE06D7363u || rd32(r + 16) < 3) return "";
    uint32_t ti = rd32(r + 28), cta = ti ? rd32(ti + 12) : 0, ct = cta ? rd32(cta + 4) : 0, td = ct ? rd32(ct + 4) : 0;
    if (!td) return " (rethrow)";
    snprintf(b, sizeof b, " (C++ %.80s)", (const char*)M + td + 8);
    return b;
}
static uint32_t thunk(const char* name) { return hle_resolve_impl("rt", name); }

typedef struct { uint32_t ebx, esi, edi, ebp; } Saved;
static Saved save(CPU* c) { Saved s = { c->ebx, c->esi, c->edi, c->ebp }; return s; }
static void restore(CPU* c, Saved s) { c->ebx = s.ebx; c->esi = s.esi; c->edi = s.edi; c->ebp = s.ebp; }

/* RtlDispatchException: 1 = a handler returned ExceptionContinueExecution, 0 = nobody handled it.
 * Handlers that handle the exception do not return: they unwind and transfer to a landing (longjmp). */
static int dispatch(CPU* c, Area* A) {
    static uint32_t nested_thunk; if (!nested_thunk) nested_thunk = thunk("SehNested");
    uint32_t nested = 0, frame = rd32(c->fs_base), sp = c->esp;
    rt_eh_depth++;
    while (frame != 0xFFFFFFFFu) {
        uint32_t fl = rd32(A->rec + 4);
        if (!frame || (frame & 3)) { wr32(A->rec + 4, fl | EXC_STACK_INVALID); break; }
        uint32_t handler = rd32(frame + 4);
        wr32(A->nest, rd32(c->fs_base)); wr32(A->nest + 4, nested_thunk); wr32(A->nest + 8, frame); wr32(A->nest + 12, 0);
        wr32(c->fs_base, A->nest);                         /* ExecuteHandler2's frame: catches exceptions in the handler */
        uint32_t a[4] = { A->rec, frame, A->ctx, A->nest + 12 };
        Saved s = save(c); c->esp = A->base - 16;
        uint32_t disp = rt_call(c, handler, 4, a);
        restore(c, s); c->esp = sp;
        wr32(c->fs_base, rd32(A->nest));
        fl = rd32(A->rec + 4);
        if (nested == frame) { fl &= ~EXC_NESTED_CALL; nested = 0; wr32(A->rec + 4, fl); }
        if (disp == ContinueExecution) {
            if (fl & EXC_NONCONTINUABLE) rt_fault(c, rd32(A->rec + 12), "handler continued a non-continuable exception");
            rt_eh_depth--; return 1;
        }
        if (disp == NestedException) {
            uint32_t d = rd32(A->nest + 12); wr32(A->rec + 4, fl | EXC_NESTED_CALL);
            if (d > nested) nested = d;
        } else if (disp != ContinueSearch) {
            char b[80]; snprintf(b, sizeof b, "exception handler %08x returned disposition %u", handler, disp);
            rt_fault(c, handler, b);
        }
        frame = rd32(frame);
    }
    rt_eh_depth--;
    return 0;
}
/* end of the chain: top-level filter (SetUnhandledExceptionFilter), then the process dies like on Windows */
static void unhandled(CPU* c, Area* A) {
    uint32_t code = rd32(A->rec), addr = rd32(A->rec + 12);
    fprintf(stderr, "[seh] UNHANDLED exception %08x%s at %08x (thread %u)\n", code, cxx_type(A->rec), addr, rd32(c->fs_base + 0x24));
    if (g_top_filter) {
        wr32(A->xp, A->rec); wr32(A->xp + 4, A->ctx);
        uint32_t sp = c->esp; c->esp = A->base - 16;
        uint32_t r = rt_call(c, g_top_filter, 1, &A->xp);
        c->esp = sp;
        fprintf(stderr, "[seh] top-level filter %08x returned %d\n", g_top_filter, (int)r);
        if (r == 0xFFFFFFFFu) return;   /* EXCEPTION_CONTINUE_EXECUTION */
    }
    hle_report();
    exit(4);
}

static void k_RaiseException(CPU* c) {   /* (code, flags, nargs, args) */
    uint32_t code = ARG(0), flags = ARG(1) & EXC_NONCONTINUABLE, n = ARG(2), args = ARG(3), info[15] = {0};
    uint32_t esp0 = c->esp, ret = rd32(esp0);
    if (n > 15) n = 15;
    for (uint32_t i = 0; i < n && args; i++) info[i] = rd32(args + 4 * i);
    Area A = area_below(esp0);
    fill_rec(A.rec, code, flags, ret, n, info);
    fill_ctx(c, A.ctx, ret, esp0 + 4 + 16);
    if (trace()) fprintf(stderr, "[seh] RaiseException %08x%s from %08x (thread %u)\n", code, cxx_type(A.rec), ret, rd32(c->fs_base + 0x24));
    if (!dispatch(c, &A)) unhandled(c, &A);
    /* continued: registers from the (possibly modified) context */
    c->eax = rd32(A.ctx + 0xB0); c->ecx = rd32(A.ctx + 0xAC); c->edx = rd32(A.ctx + 0xA8); c->ebx = rd32(A.ctx + 0xA4);
    c->esi = rd32(A.ctx + 0xA0); c->edi = rd32(A.ctx + 0x9C); c->ebp = rd32(A.ctx + 0xB4);
    c->esp = esp0;
}

void rt_raise_hw(CPU* c, uint32_t code, uint32_t eip) {
    uint32_t esp0 = c->esp;
    Area A = area_below(esp0);
    fill_rec(A.rec, code, 0, eip, 0, NULL);
    fill_ctx(c, A.ctx, eip, esp0);
    if (trace()) fprintf(stderr, "[seh] hardware exception %08x at %08x (thread %u)\n", code, eip, rd32(c->fs_base + 0x24));
    c->esp = A.base - 64;
    if (dispatch(c, &A)) rt_fault(c, eip, "a handler asked to continue after a hardware exception (not supported)");
    unhandled(c, &A);
    rt_fault(c, eip, "unhandled hardware exception");
}

static void k_RtlUnwind(CPU* c) {   /* (TargetFrame, TargetIp, ExceptionRecord, ReturnValue) */
    static uint32_t unwind_thunk; if (!unwind_thunk) unwind_thunk = thunk("SehUnwindNested");
    uint32_t target = ARG(0), tip = ARG(1), urec = ARG(2), retval = ARG(3);
    uint32_t esp0 = c->esp, ret = rd32(esp0);
    Saved s0 = save(c);
    Area A = area_below(esp0);
    uint32_t rec = urec;
    if (!rec) { rec = A.rec; fill_rec(rec, 0xC0000027u /* STATUS_UNWIND */, 0, ret, 0, NULL); }
    wr32(rec + 4, rd32(rec + 4) | EXC_UNWINDING | (target ? 0 : EXC_EXIT_UNWIND));
    fill_ctx(c, A.ctx, tip ? tip : ret, esp0 + 4 + 16); wr32(A.ctx + 0xB0, retval);
    if (trace() > 1) fprintf(stderr, "[seh] RtlUnwind to frame %08x\n", target);
    if (tip && tip != ret) fprintf(stderr, "[seh] warning: RtlUnwind TargetIp %08x != return address %08x (returning normally)\n", tip, ret);
    rt_eh_depth++;
    uint32_t frame = rd32(c->fs_base);
    while (frame != 0xFFFFFFFFu && frame != target) {
        if (!frame || (frame & 3)) { fprintf(stderr, "[seh] RtlUnwind: bad frame %08x\n", frame); break; }
        if (target && frame > target) { fprintf(stderr, "[seh] RtlUnwind: target %08x not on the chain (at %08x)\n", target, frame); break; }
        wr32(A.nest, rd32(c->fs_base)); wr32(A.nest + 4, unwind_thunk); wr32(A.nest + 8, frame); wr32(A.nest + 12, 0);
        wr32(c->fs_base, A.nest);
        uint32_t a[4] = { rec, frame, A.ctx, A.nest + 12 };
        c->esp = A.base - 16;
        uint32_t disp = rt_call(c, rd32(frame + 4), 4, a);
        restore(c, s0); c->esp = esp0;
        wr32(c->fs_base, rd32(A.nest));
        if (disp == CollidedUnwind) { uint32_t f = rd32(A.nest + 12); if (f) frame = f; }
        uint32_t next = rd32(frame);
        wr32(c->fs_base, next);                            /* RtlpUnlinkHandler */
        frame = next;
    }
    rt_eh_depth--;
    restore(c, s0); c->esp = esp0; c->eax = retval;
}
/* ExecuteHandler2's frame: an exception inside a handler -> ExceptionNestedException, skip to the frame being handled */
static void rt_SehNested(CPU* c) {   /* cdecl (rec, frame, ctx, dispctx) */
    if (rd32(ARG(0) + 4) & (EXC_UNWINDING | EXC_EXIT_UNWIND)) { RET(ContinueSearch); return; }
    wr32(ARG(3), rd32(ARG(1) + 8)); RET(NestedException);
}
static void rt_SehUnwindNested(CPU* c) {
    if (!(rd32(ARG(0) + 4) & (EXC_UNWINDING | EXC_EXIT_UNWIND))) { RET(ContinueSearch); return; }
    wr32(ARG(3), rd32(ARG(1) + 8)); RET(CollidedUnwind);
}

void rt_eh_land(CPU* c, const FuncEntry* e) {
    for (RtEhFrame* f = rt_eh_top; f; f = f->prev)
        if (f->owner == e->owner && (f->entry_esp - 4 == c->ebp || f->entry_ebp == c->ebp)) {
            if (trace()) fprintf(stderr, "[seh] landing %08x in frame of %08x (ebp %08x, esp %08x)\n", e->va, e->owner, c->ebp, c->esp);
            f->target = e->va;
            hle_unwind_locks(f->hle_depth);
            hle_hb_leave();   /* back in guest code (the RaiseException HLE call does not return) */
            rt_eh_depth = f->disp_depth;
            rt_eh_top = f;
            _longjmp(f->jb, 1);
        }
    fprintf(stderr, "[seh] warning: landing %08x: no live frame of %08x with ebp %08x - running it in place\n", e->va, e->owner, c->ebp);
}
void rt_eh_reset(void) { rt_eh_top = NULL; rt_eh_depth = 0; }

static void k_SetUnhandledExceptionFilter(CPU* c) { uint32_t o = g_top_filter; g_top_filter = ARG(0); RET(o); }
static void k_UnhandledExceptionFilter(CPU* c) {   /* (EXCEPTION_POINTERS*) - called by CRT filters (_XcptFilter) */
    uint32_t xp = ARG(0), rec = rd32(xp);
    fprintf(stderr, "[seh] UnhandledExceptionFilter: exception %08x%s at %08x\n", rd32(rec), cxx_type(rec), rd32(rec + 12));
    static _Thread_local int busy;
    if (g_top_filter && !busy) { busy = 1; uint32_t r = rt_call(c, g_top_filter, 1, &xp); busy = 0; if (r) { RET(r); return; } }
    RET(1);   /* EXCEPTION_EXECUTE_HANDLER: the caller terminates the process */
}
static void k_VirtualQuery(CPU* c) {   /* (addr, MEMORY_BASIC_INFORMATION*, len) */
    uint32_t a = ARG(0), p = ARG(1), page = a & ~0xFFFu;
    if (ARG(2) < 28) { hle_set_last_error(c, 24); RET(0); return; }
    uint32_t abase, aprot, size = 0x1000, state = 0x1000, prot, type;
    Module* m = rt_module_at(a);
    if (m) {   /* image: protection from the section headers mapped at the module base */
        abase = m->base; aprot = 0x80; type = 0x1000000; prot = 0x02;
        uint32_t pe = m->base + rd32(m->base + 0x3C), ns = rd16(pe + 6), sec = pe + 24 + rd16(pe + 20);
        for (uint32_t i = 0; i < ns; i++, sec += 40) {
            uint32_t va = m->base + rd32(sec + 12), vs = (rd32(sec + 8) + 0xFFFu) & ~0xFFFu, ch = rd32(sec + 36);
            if (a >= va && a < va + vs) {
                prot = (ch & 0x20000000u) ? ((ch & 0x80000000u) ? 0x40 : 0x20) : ((ch & 0x80000000u) ? 0x04 : 0x02);
                size = va + vs - page; break;
            }
        }
    } else if (hle_vmem_query(a, &abase, &size)) { size = abase + size - page; aprot = 0x04; prot = 0x04; type = 0x20000; }
    else if (a >= 0x10000u && a < 0x7FFE0000u) { abase = page; aprot = 0x04; prot = 0x04; type = 0x20000; }
    else { abase = 0; aprot = 0; state = 0x10000; prot = 0x01; type = 0; }
    wr32(p, page); wr32(p + 4, abase); wr32(p + 8, aprot); wr32(p + 12, size); wr32(p + 16, state); wr32(p + 20, prot); wr32(p + 24, type);
    RET(28);
}

const HleDef hle_seh_defs[] = {
    { "kernel32.dll", "RaiseException", 4, k_RaiseException, HLE_NOLOCK },
    { "kernel32.dll", "RtlUnwind", 4, k_RtlUnwind, HLE_NOLOCK },
    { "ntdll.dll", "RtlUnwind", 4, k_RtlUnwind, HLE_NOLOCK },
    { "kernel32.dll", "SetUnhandledExceptionFilter", 1, k_SetUnhandledExceptionFilter },
    { "kernel32.dll", "UnhandledExceptionFilter", 1, k_UnhandledExceptionFilter, HLE_NOLOCK },
    { "kernel32.dll", "VirtualQuery", 3, k_VirtualQuery },
    { "rt", "SehNested", 0, rt_SehNested, HLE_NOLOCK },
    { "rt", "SehUnwindNested", 0, rt_SehUnwindNested, HLE_NOLOCK },
};
const unsigned hle_seh_ndefs = sizeof hle_seh_defs / sizeof hle_seh_defs[0];
