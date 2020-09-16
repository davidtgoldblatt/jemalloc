#include <stdio.h>
#ifdef _WIN32
#include <limits.h>
#include <intrin.h>
typedef unsigned __int32 uint32_t;
#else
#include <stdint.h>
#endif

int main() {
    uint32_t r[4];
    uint32_t eax_in = 0x80000008U;
#ifdef _WIN32
    __cpuid((int *)r, (int)eax_in);
#else
    asm volatile(
        "cpuid"
        : "=a" (r[0]), "=b" (r[1]), "=c" (r[2]), "=d" (r[3])
        : "a" (eax_in), "c"(0));
#endif
    uint32_t eax_out = r[0];
    uint32_t vaddr = ((eax_out & 0x0000ff00U) >> 8);
    if (vaddr > (sizeof(void *) << 3)) {
        vaddr = sizeof(void *) * 8;
    }
    int printed = printf("%u", vaddr);
    if (printed < 0) {
        return 1;
    }
    return 0;
}
