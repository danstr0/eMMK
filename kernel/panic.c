/**
 * @file panic.c
 * @brief System-wide kernel panic.
 */

#include "panic.h"
#include "serial.h"
#include "apic.h"
#include "smp.h"

/// Core index of the panicking core, or @c -1 if no panic is in progress.
static volatile int panic_owner = -1;

// ── Helpers ────────────────────────────────────────────────────────────

/// Stop this core for good. NMIs can still wake it from hlt, so loop.
NORETURN static void halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

/**
 * @brief Claim the panic and stop the other cores.
 *
 * Returns only on the core that wins the claim; every other caller
 * halts here.
 */
static void panic_begin(void)
{
    __asm__ volatile("cli");

    int me = smp_core_id();
    int expected = -1;
    if (!__atomic_compare_exchange_n(&panic_owner, &expected, me, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        // Either another core is already panicking, or this core faulted
        // while printing its own report
        halt_forever();
    }

    serial_panic_enter();

    if (lapic_is_ready())
        lapic_send_nmi_all();
}

static const char *const separator =
    "=========================================="
    "==========================================\n";

// ── Exception names ────────────────────────────────────────────────────

static const char *const exception_names[EXCEPTION_COUNT] =
{
    [EXCEPTION_DE]  = "Divide Error (#DE)",
    [EXCEPTION_DB]  = "Debug (#DB)",
    [EXCEPTION_NMI] = "Non-Maskable Interrupt",
    [EXCEPTION_BP]  = "Breakpoint (#BP)",
    [EXCEPTION_OF]  = "Overflow (#OF)",
    [EXCEPTION_BR]  = "Bound Range Exceeded (#BR)",
    [EXCEPTION_UD]  = "Invalid Opcode (#UD)",
    [EXCEPTION_NM]  = "Device Not Available (#NM)",
    [EXCEPTION_DF]  = "Double Fault (#DF)",
    [EXCEPTION_CSO] = "Coprocessor Segment Overrun",
    [EXCEPTION_TS]  = "Invalid TSS (#TS)",
    [EXCEPTION_NP]  = "Segment Not Present (#NP)",
    [EXCEPTION_SS]  = "Stack Fault (#SS)",
    [EXCEPTION_GP]  = "General Protection Fault (#GP)",
    [EXCEPTION_PF]  = "Page Fault (#PF)",
    [EXCEPTION_MF]  = "x87 Floating-Point (#MF)",
    [EXCEPTION_AC]  = "Alignment Check (#AC)",
    [EXCEPTION_MC]  = "Machine-Check (#MC)",
    [EXCEPTION_XM]  = "SIMD Floating-Point (#XM)",
    [EXCEPTION_VE]  = "Virtualization (#VE)",
    [EXCEPTION_CP]  = "Control Protection (#CP)",
};

/**
 * @brief Print detailed page fault information.
 *
 * Reads CR2 for the faulting address and decodes the error code bits.
 *
 * @see Intel SDM Vol. 3, Interrupt 14 (Page-Fault Exception).
 */
static void dump_page_fault(struct interrupt_frame *frame)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    kprintf("\n  Page Fault Details:\n");
    kprintf("    Faulting address (CR2): 0x%016lx\n", cr2);
    kprintf("    Cause: %s\n",
            (frame->error_code & 0x01) ? "protection violation"
                                       : "page not present");
    kprintf("    Access: %s\n",
            (frame->error_code & 0x02) ? "write" : "read");
    kprintf("    Context: %s\n",
            (frame->error_code & 0x04) ? "user mode" : "supervisor mode");
    if (frame->error_code & 0x08)
        kprintf("    Reserved bit set in page table entry\n");
    if (frame->error_code & 0x10)
        kprintf("    Caused by instruction fetch (NX violation)\n");
    if (frame->error_code & 0x20)
        kprintf("    Protection key violation\n");
    if (frame->error_code & 0x40)
        kprintf("    Shadow stack access\n");
}

// ── Public API ───────────────────────────────────────────────────────── 

void panic(const char *fmt, ...)
{
    panic_begin();

    kprintf("\n%s  KERNEL PANIC on core %d: ", separator, panic_owner);

    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(COM1_PORT, fmt, args);
    __builtin_va_end(args);

    kprintf("\n%s  System halted.\n", separator);
    halt_forever();
}

void panic_exception(struct interrupt_frame *frame)
{
    panic_begin();

    const char *name = "Reserved";
    if (frame->vector < EXCEPTION_COUNT && exception_names[frame->vector])
        name = exception_names[frame->vector];

    kprintf("\n%s", separator);
    kprintf("  KERNEL PANIC on core %d: %s (vector %lu, error code 0x%lx)\n",
            panic_owner, name, frame->vector, frame->error_code);
    kprintf("%s", separator);

    kprintf("\n  Register dump:\n");
    kprintf("    RAX=%016lx  RBX=%016lx  RCX=%016lx\n",
            frame->rax, frame->rbx, frame->rcx);
    kprintf("    RDX=%016lx  RSI=%016lx  RDI=%016lx\n",
            frame->rdx, frame->rsi, frame->rdi);
    kprintf("    RBP=%016lx  RSP=%016lx  RIP=%016lx\n",
            frame->rbp, frame->rsp, frame->rip);
    kprintf("    R8 =%016lx  R9 =%016lx  R10=%016lx\n",
            frame->r8, frame->r9, frame->r10);
    kprintf("    R11=%016lx  R12=%016lx  R13=%016lx\n",
            frame->r11, frame->r12, frame->r13);
    kprintf("    R14=%016lx  R15=%016lx\n",
            frame->r14, frame->r15);
    kprintf("    CS=%lx  SS=%lx  RFLAGS=%lx\n",
            frame->cs, frame->ss, frame->rflags);

    if (frame->vector == EXCEPTION_PF)
        dump_page_fault(frame);

    kprintf("\n  System halted.\n");
    halt_forever();
}

void panic_nmi_handler(struct interrupt_frame *frame)
{
    if (panic_owner != -1)
        halt_forever();

    panic_exception(frame);
}

bool panic_in_progress(void)
{
    return panic_owner != -1;
}

