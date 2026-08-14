/**
 * @file idt.c
 * @brief IDT setup and interrupt dispatch.
 *
 * @par Responsibilities
 * - Build the 256-entry IDT from the ISR stub table (isr.asm).
 * - Load the IDT via LIDT.
 * - Dispatch interrupts to registered C handlers.
 * - Provide a default exception handler that dumps registers and halts.
 *
 * @par Interrupt dispatch flow
 * @code
 *   isr_stub_N (asm) → interrupt_dispatch (C) → handlers[N](frame)
 * @endcode
 *
 * If no handler is registered:
 * - Exceptions (0–31): @c default_exception_handler (panic + halt).
 * - IRQs (32+): log warning, send EOI (once PIC / APIC is initialized).
 */

#include "idt.h"
#include "serial.h"
#include "string.h"
#include "apic.h"
#include "tss.h"

// ── Static data ────────────────────────────────────────────────────────────

/// The 256-entry IDT, 16-byte aligned.
struct idt_entry idt[IDT_ENTRIES] ALIGNED(16);

/// LIDT descriptor.
static struct idt_ptr idtr;

/// Per-vector handler table. @c NULL = no handler registered.
static interrupt_handler_t handlers[IDT_ENTRIES];

/// ISR stub entry points, defined in isr.asm.
extern uint64_t isr_stub_table[IDT_ENTRIES];

// ── Exception names ────────────────────────────────────────────────────────

const char *exception_names[EXCEPTION_COUNT] =
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
    [15]            = "Reserved",
    [EXCEPTION_MF]  = "x87 Floating-Point (#MF)",
    [EXCEPTION_AC]  = "Alignment Check (#AC)",
    [EXCEPTION_MC]  = "Machine-Check (#MC)",
    [EXCEPTION_XM]  = "SIMD Floating-Point (#XM)",
    [EXCEPTION_VE]  = "Virtualization (#VE)",
    [EXCEPTION_CP]  = "Control Protection (#CP)",
    [22] = "Reserved", [23] = "Reserved",
    [24] = "Reserved", [25] = "Reserved",
    [26] = "Reserved", [27] = "Reserved",
    [28] = "Reserved", [29] = "Reserved",
    [30] = "Reserved", [31] = "Reserved",
};

// ── Gate configuration ─────────────────────────────────────────────────────

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist)
{
    struct idt_entry *entry = &idt[vector];

    entry->offset_low  = (uint16_t)(handler & 0xFFFF);
    entry->offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    entry->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    entry->selector    = selector;
    entry->ist         = ist & 0x7;
    entry->type_attr   = type_attr;
    entry->reserved    = 0;
}

void idt_set_handler(uint8_t vector, interrupt_handler_t handler)
{
    handlers[vector] = handler;
}

// ── Page fault detail decoder ──────────────────────────────────────────────

/**
 * @brief Print detailed page fault information.
 *
 * Reads CR2 for the faulting address and decodes the error code bits
 *
 * @see Intel SDM Vol. 3, Section 6.15 - Interrupt 14 (Page-Fault
 *      Exception).
 */
static void dump_page_fault(struct interrupt_frame *frame)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    kprintf("\n  Page Fault Details:\n");
    kprintf("    Faulting address (CR2): 0x%lx\n", cr2);
    kprintf("    Cause: %s\n",
            (frame->error_code & 0x01) ? "protection violation" : "page not present");
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

// ── Default exception handler ──────────────────────────────────────────────

/**
 * @brief Panic handler for unregistered exceptions.
 *
 * Dumps the full register state and exception details to serial, then
 * halts the CPU permanently.
 */
NORETURN static void default_exception_handler(struct interrupt_frame *frame)
{
    const char *name = "Unknown Exception";
    if (frame->vector < EXCEPTION_COUNT && exception_names[frame->vector])
        name = exception_names[frame->vector];

    kprintf("\n");
    kprintf("=========================================="
            "==========================================\n");
    kprintf("  KERNEL PANIC: %s (vector %lu, error code 0x%lx)\n",
            name, frame->vector, frame->error_code);
    kprintf("=========================================="
            "==========================================\n");

    kprintf("\n  Register dump:\n");
    kprintf("    RAX=%lx  RBX=%lx  RCX=%lx\n",
            frame->rax, frame->rbx, frame->rcx);
    kprintf("    RDX=%lx  RSI=%lx  RDI=%lx\n",
            frame->rdx, frame->rsi, frame->rdi);
    kprintf("    RBP=%lx  RSP=%lx  RIP=%lx\n",
            frame->rbp, frame->rsp, frame->rip);
    kprintf("    R8 =%lx  R9 =%lx  R10=%lx\n",
            frame->r8, frame->r9, frame->r10);
    kprintf("    R11=%lx  R12=%lx  R13=%lx\n",
            frame->r11, frame->r12, frame->r13);
    kprintf("    R14=%lx  R15=%lx\n",
            frame->r14, frame->r15);
    kprintf("    CS=%lx  SS=%lx  RFLAGS=%lx\n",
            frame->cs, frame->ss, frame->rflags);

    if (frame->vector == EXCEPTION_PF)
        dump_page_fault(frame);

    kprintf("\n  System halted.\n");

    for (;;)
        __asm__ volatile("cli; hlt");
}

// ── Interrupt dispatch ─────────────────────────────────────────────────────

/**
 * @brief Central interrupt dispatcher (called from isr.asm).
 *
 * Routes the interrupt to the registered handler. If no handler is
 * registered, exceptions cause a panic and IRQs are logged and dropped
 * (with EOI, once the interrupt controller is initialized).
 *
 * @param frame  Pointer to the saved register state on the stack.
 */
void interrupt_dispatch(struct interrupt_frame *frame)
{
    uint8_t vector = (uint8_t)frame->vector;

    if (handlers[vector])
    {
        handlers[vector](frame);
        return;
    }

    // Unhandled exception - panic
    if (vector < EXCEPTION_COUNT)
        default_exception_handler(frame);  // never returns

    // Unhandled IRQ / software interrupt
    kprintf("[IDT] WARNING: Unhandled interrupt vector %u\n",
            (uint32_t)vector);

    if (vector != APIC_SPURIOUS_VECTOR)
        lapic_eoi();
}

// ── Initialization ─────────────────────────────────────────────────────────

void idt_init(void)
{
    kprintf("[IDT] Initializing Interrupt Descriptor Table...\n");

    // Clear handler table and IDT
    memset(handlers, 0, sizeof(handlers));
    memset(idt, 0, sizeof(idt));

    // Install all 256 ISR stubs from the table generated by isr.asm.
    // All gates use:
    // - Kernel code segment selector (0x08, GDT entry 1)
    // - Interrupt gate type (clears IF on entry)
    for (int i = 0; i < IDT_ENTRIES; ++i)
        idt_set_gate(i, isr_stub_table[i], 0x08, IDT_GATE_INTERRUPT, 0);

    // Load the IDT
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtr));

    kprintf("[IDT] Loaded: %u entries at %p\n",
            IDT_ENTRIES, (void *)&idt);
}

void idt_configure_ist(void)
{
    // #DF (vector 8): use IST1
    idt_set_gate(EXCEPTION_DF, isr_stub_table[EXCEPTION_DF],
                 0x08, IDT_GATE_INTERRUPT, IST_DOUBLE_FAULT);

    // NMI (vector 2): use IST2
    idt_set_gate(EXCEPTION_NMI, isr_stub_table[EXCEPTION_NMI],
                 0x08, IDT_GATE_INTERRUPT, IST_NMI);

    kprintf("[IDT] IST configured: #DF=IST%d, NMI=IST%d\n",
            IST_DOUBLE_FAULT, IST_NMI);
}

