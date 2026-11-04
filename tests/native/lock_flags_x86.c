/* Native x86-64 check of the flag semantics the difftest reference (Unicorn/QEMU) gets wrong:
 * LOCK NEG flags come from the negated value; CMPXCHG8B changes only ZF. gcc -O1 lock_flags_x86.c && ./a.out */
#include <stdio.h>
#include <stdint.h>
int main(void) {
    uint16_t w = 0x1234; uint64_t fl;
    __asm__ volatile("lock negw %1\n\tpushfq\n\tpopq %0" : "=r"(fl), "+m"(w) :: "cc");
    printf("lock neg word 0x1234 -> %04x flags %03llx (CF PF ZF SF OF mask 8c5: %03llx)\n", w, (unsigned long long)fl, (unsigned long long)(fl & 0x8c5));
    /* cmpxchg8b must keep CF/PF/SF/OF: set them with an add, then cmpxchg8b (success and failure) */
    uint64_t q = 0x1111222233334444ull; uint32_t eax = 0x33334444, edx = 0x11112222, ebx = 5, ecx = 6;
    uint64_t f1, f2;
    __asm__ volatile("movl $0x7fffffff, %%esi\n\taddl $0x7fffffff, %%esi\n\t"   /* OF=1 SF=1 PF? CF=0 */
                     "lock cmpxchg8b %1\n\tpushfq\n\tpopq %0" : "=r"(f1), "+m"(q), "+a"(eax), "+d"(edx) : "b"(ebx), "c"(ecx) : "esi", "cc");
    printf("cmpxchg8b ok   flags %03llx  (add flags + ZF expected)\n", (unsigned long long)(f1 & 0x8c5));
    __asm__ volatile("movl $0xffffffff, %%esi\n\taddl $2, %%esi\n\t"            /* CF=1 */
                     "lock cmpxchg8b %1\n\tpushfq\n\tpopq %0" : "=r"(f2), "+m"(q), "+a"(eax), "+d"(edx) : "b"(ebx), "c"(ecx) : "esi", "cc");
    printf("cmpxchg8b fail flags %03llx  (CF=1 kept, ZF=0 expected)\n", (unsigned long long)(f2 & 0x8c5));
    return 0;
}
