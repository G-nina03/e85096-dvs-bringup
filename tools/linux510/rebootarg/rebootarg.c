/* rebootarg ARG -- sync, then reboot(2) with LINUX_REBOOT_CMD_RESTART2 and ARG (e.g. "bootloader").
 * busybox `reboot` cannot pass an argument; the Exynos reboot driver turns "bootloader" into the
 * reboot reason that makes LK stop in fastboot ("Entering fastboot: reboot bootloader command").
 * Freestanding: raw syscalls, no libc (same build as tools/ov5647-i2crd). */
static long sc(long n, long a, long b, long c, long d) {
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b,
                  x2 __asm__("x2") = c, x3 __asm__("x3") = d;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

__attribute__((used)) void cmain(long *sp) {
    char **argv = (char **)(sp + 1);
    long rc = 1;
    if (sp[0] == 2) {
        sc(81, 0, 0, 0, 0);                                            /* sync */
        rc = sc(142, 0xfee1dead, 672274793, 0xa1b2c3d4, (long)argv[1]); /* reboot RESTART2 */
    }
    sc(93, rc < 0 ? -rc : rc, 0, 0, 0);                                /* exit */
}
__asm__(".globl _start\n_start:\n mov x0, sp\n bl cmain\n");
