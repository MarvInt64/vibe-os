/* VibeOS arm64 — kernel entry + interactive serial shell.
 *
 * On HVF (Apple Silicon), QEMU's GICv2 CPU-interface MMIO at 0x08010000
 * triggers "Assertion failed: (isv)" because the HVF trap handler cannot
 * always determine the access size from the exception syndrome (ISV=0 for
 * some load/store instructions to device memory).
 *
 * Work-around: skip GIC MMIO entirely.  The ARM generic timer exposes its
 * fired status directly via CNTP_CTL_EL0.ISTATUS (bit 2), readable without
 * any MMIO.  We poll this in a tight loop for the uptime counter and use
 * plain wfi for idle.  UART input is read by polling UART_FR (PL011 flag
 * register), which is device MMIO — but PL011 word-size reads do set ISV
 * correctly in QEMU's HVF backend (only GIC reads fail).
 *
 * This gives us: working shell, uptime, and timer ticks on HVF, TCG, and
 * (once the ISV issue is fixed upstream) full GIC.
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/alloc.h"

/* ---- Timer tick counter (incremented without GIC) --------------------- */
static volatile uint64_t g_tick = 0;

/* ---- IRQ handler (exceptions.S → TCG/GIC path only) ----------------- */
void arm64_irq_handler(void) {
    /* Only reached on the TCG path where GIC IRQs are enabled. */
    unsigned irq = arm64_gic_ack();
    switch (irq) {
    case TIMER_IRQ_PHYS:
        arm64_timer_ack();
        g_tick++;
        break;
    case 1023: break;  /* spurious */
    default:
        serial_write("[irq] "); serial_write_hex_u64(irq); serial_write("\r\n");
        break;
    }
    arm64_gic_eoi(irq);
}

/* ---- Fault handlers --------------------------------------------------- */
void arm64_sync_handler_el1(uint64_t esr, uint64_t elr, uint64_t far,
                             void *sp) {
    (void)sp;
    serial_write("\r\n!!! arm64 KERNEL FAULT !!!\r\n");
    serial_write("  ESR = "); serial_write_hex_u64(esr); serial_write("\r\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\r\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\r\n");
    serial_write("  EC  = "); serial_write_hex_u64((esr >> 26) & 0x3f);
    serial_write("\r\n");
    while (1) __asm__ volatile("wfi");
}

void arm64_sync_handler_el0(uint64_t esr, uint64_t elr, uint64_t far,
                             void *sp) {
    (void)sp; (void)esr; (void)elr; (void)far;
}

void arm64_unhandled_exception(uint64_t esr, uint64_t elr, uint64_t far) {
    serial_write("\r\n!!! arm64 UNHANDLED EXCEPTION !!!\r\n");
    serial_write("  ESR = "); serial_write_hex_u64(esr); serial_write("\r\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\r\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\r\n");
    while (1) __asm__ volatile("wfi");
}

/* ---- Polling tick ---------------------------------------------------- */
static void poll_timer(void) {
    if (arm64_timer_poll())
        g_tick++;
}

/* ---- Print decimal ----------------------------------------------------- */
static void print_dec(uint64_t v) {
    char buf[21]; int i = 0;
    if (!v) { serial_write_char('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) serial_write_char(buf[i]);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* ---- Memory detection ------------------------------------------------- */
static void detect_memory(uintptr_t *hs, size_t *hz) {
    *hs = 0x40800000UL;         /* 6 MB past kernel load */
    *hz = 232UL * 1024 * 1024;
}

/* ---- Shell commands --------------------------------------------------- */
static void cmd_help(void) {
    serial_write("  help    — this message\r\n");
    serial_write("  uname   — kernel info\r\n");
    serial_write("  mem     — heap usage\r\n");
    serial_write("  uptime  — seconds since boot\r\n");
    serial_write("  cpuinfo — CPU registers\r\n");
    serial_write("  halt    — stop the machine\r\n");
}

static void cmd_uname(void) {
    serial_write("VibeOS arm64 | QEMU virt | ARM generic timer | ");
    uint64_t midr = read_sysreg(midr_el1);
    /* implementer 0x61 = Apple */
    if (((midr >> 24) & 0xFF) == 0x61)
        serial_write("Apple Silicon (HVF)\r\n");
    else
        serial_write("QEMU Cortex-A72 (TCG)\r\n");
    serial_write("  MIDR_EL1 = "); serial_write_hex_u64(midr); serial_write("\r\n");
}

static void cmd_mem(void) {
    size_t total = kmalloc_get_total();
    size_t used  = kmalloc_get_used();
    /* Derive free from total - used to avoid the free-list walk which
     * traverses all blocks and may touch unaligned pointers on arm64. */
    size_t free  = (total > used) ? (total - used) : 0;
    serial_write("  total="); print_dec(total >> 10);
    serial_write(" KB  used="); print_dec(used >> 10);
    serial_write(" KB  free="); print_dec(free >> 10);
    serial_write(" KB\r\n");
}

static void cmd_uptime(void) {
    poll_timer();  /* flush pending ticks */
    uint64_t t = g_tick;
    print_dec(t / 100); serial_write(".");
    print_dec((t % 100) / 10);
    serial_write(" s  ("); print_dec(t); serial_write(" ticks)\r\n");
}

static void cmd_cpuinfo(void) {
    serial_write("  MIDR_EL1  = "); serial_write_hex_u64(read_sysreg(midr_el1)); serial_write("\r\n");
    serial_write("  CNTFRQ    = "); print_dec(read_sysreg(cntfrq_el0) / 1000000);
    serial_write(" MHz\r\n");
    serial_write("  CNTPCT    = "); serial_write_hex_u64(read_sysreg(cntpct_el0)); serial_write("\r\n");
    serial_write("  SCTLR_EL1 = "); serial_write_hex_u64(read_sysreg(sctlr_el1)); serial_write("\r\n");
}

static void run_command(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    if      (str_eq(line, "help"))    cmd_help();
    else if (str_eq(line, "uname"))   cmd_uname();
    else if (str_eq(line, "mem"))     cmd_mem();
    else if (str_eq(line, "uptime"))  cmd_uptime();
    else if (str_eq(line, "cpuinfo")) cmd_cpuinfo();
    else if (str_eq(line, "halt")) {
        serial_write("halting.\r\n");
        __asm__ volatile("msr daifset, #3");
        while (1) __asm__ volatile("wfi");
    } else {
        serial_write("  unknown: '"); serial_write(line);
        serial_write("'  (try 'help')\r\n");
    }
}

/* ---- Non-blocking UART read, also polls timer ------------------------- */
static int uart_getc_nonblock(char *out) {
    poll_timer();
    if (!arm64_uart_can_read()) return 0;
    *out = arm64_uart_getc();
    return 1;
}

static void shell_loop(void) {
    static char line[256];
    int pos = 0;

    serial_write("VibeOS arm64 — type 'help' for commands\r\n");

    while (1) {
        serial_write("\r\narm64$ ");
        pos = 0;

        for (;;) {
            char c;
            /* Spin until a character arrives, polling timer each iteration */
            while (!uart_getc_nonblock(&c))
                ;

            if (c == '\r' || c == '\n') {
                serial_write("\r\n");
                line[pos] = '\0';
                break;
            } else if ((c == '\b' || c == 0x7f) && pos > 0) {
                pos--;
                serial_write("\b \b");
            } else if (c >= 0x20 && c < 0x7f && pos < 255) {
                line[pos++] = c;
                serial_write_char(c);
            }
        }
        run_command(line);
    }
}

/* ---- Kernel entry ----------------------------------------------------- */
void kernel_main_arm64(void) {
    serial_init();

    serial_write("\r\n");
    serial_write("  __   ___      _      ___  ____  \r\n");
    serial_write(" \\ \\ / (_)_ __| |__  / _ \\/ ___| \r\n");
    serial_write("  \\ V /| | '__| '_ \\| | | \\___ \\ \r\n");
    serial_write("   \\_/ |_|_|  |_|_|_|\\___/|____/ \r\n");
    serial_write("         arm64  |  HVF  |  M-chip   \r\n\r\n");

    /* CPU info */
    uint64_t freq = read_sysreg(cntfrq_el0);
    serial_write("[arm64] MIDR_EL1 = "); serial_write_hex_u64(read_sysreg(midr_el1));
    serial_write("  CNTFRQ = "); print_dec(freq / 1000000); serial_write(" MHz\r\n");

    /* MMU sanity check */
    if (read_sysreg(sctlr_el1) & 1)
        serial_write("[arm64] MMU on — identity mapping active\r\n");
    else
        serial_write("[arm64] WARNING: MMU off!\r\n");

    /* Heap */
    uintptr_t hs; size_t hz;
    detect_memory(&hs, &hz);
    kmalloc_init(hs, hz);
    serial_write("[arm64] heap "); print_dec(hz >> 20);
    serial_write(" MB at "); serial_write_hex_u64(hs); serial_write("\r\n");

    serial_write("[arm64] detecting platform...\r\n");
    /* Detect platform FIRST so we choose the right timer path.
     * Apple implementer = 0x61.  On HVF, GICC MMIO (0x08010000) triggers
     * an ISV=0 assertion in QEMU's HVF backend, so we skip it entirely. */
    uint64_t midr    = read_sysreg(midr_el1);
    int      is_apple = (((midr >> 24) & 0xFF) == 0x61);

    if (is_apple) {
        serial_write("[arm64] Apple HVF detected\r\n");
        /* HVF path: timer in poll mode (IMASK=1, no IRQ, no GIC MMIO). */
        serial_write("[arm64] init timer poll...\r\n");
        arm64_timer_init_poll();
        serial_write("[arm64] timer: poll mode (IMASK=1, no GIC MMIO — HVF)\r\n");
    } else {
        /* TCG / real-hardware path: GIC + IRQ-driven timer. */
        arm64_gic_init();
        arm64_timer_init();          /* ENABLE=1, IMASK=0, GIC IRQ 30 */
        __asm__ volatile("msr daifclr, #2");   /* unmask IRQ */
        serial_write("[arm64] GICv2 + timer IRQ enabled (TCG path)\r\n");
    }

    shell_loop();
}
