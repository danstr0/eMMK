/**
 * @file linker.ld
 * @brief Kernel linker script - physical/virtual memory layout.
 *
 * @par Memory layout
 * @code
 *   Physical 0x0010_0000 (1 MiB), VMA = LMA:
 *     .boot             - Multiboot2 header, 32-bit bootstrap, GDT, boot
 *                         page tables, and boot stack. Everything here
 *                         has VMA == LMA so that symbols resolve to the
 *                         physical addresses the 32-bit code requires
 *                         before (and during) the paging transition
 *
 *   Virtual 0xFFFF_FFFF_8010_0000+ (higher-half, mapped to physical 1 MiB+):
 *     .text             - 64-bit Executable code
 *     .rodata           - Read-only data (string literals, etc.)
 *     .data             - Initialized read-write data
 *     .bss              - Zero-initialized data
 * @endcode
 *
 * @par Exported symbols
 * - @c _kernel_physical_start / @c _kernel_physical_end
 * - @c _kernel_virtual_start  / @c _kernel_virtual_end
 * - @c __bss_start            / @c __bss_end
 */

ENTRY(_start)

KERNEL_VIRTUAL_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    /* ───────────────────────────────────────────────────────────────────
     * Boot sections (VMA = LMA, identity-mapped)
     *
     * The kernel image is loaded at 1 MiB by GRUB. The Multiboot2 header
     * must appear within the first 32 KiB of the file.
     *
     * All 32-bit bootstrap code, the transitional GDT, the boot page
     * tables, and the boot stack reside here at physical addresses so that
     * they are valid both before paging is enabled and afterwards (via
     * the identity mapping).
     * ─────────────────────────────────────────────────────────────────── */
    . = 1M;
    _kernel_physical_start = .;

    .boot ALIGN(4K) : AT(ADDR(.boot))
    {
        KEEP(*(.multiboot2))
        *(.text32)
        *(.rodata32)
    }

    .boot_page_tables ALIGN(4K) (NOLOAD) : AT(ADDR(.boot_page_tables))
    {
        *(.boot_page_tables)
    }

    .boot_stack ALIGN(16) (NOLOAD) : AT(ADDR(.boot_stack))
    {
        *(.boot_stack)
    }

    /* 
     * AP trampoline - kept in the kernel image at its linked address
     *
     * smp_init() copies it to AP_TRAMPOLINE_ADDR(0x8000) at runtime 
     */

    .ap_trampoline ALIGN(4K) : AT(ADDR(.ap_trampoline))
    {
        _ap_trampoline_lma = .;
        KEEP(*(.ap_trampoline))
    }

    /* ───────────────────────────────────────────────────────────────────
     * Higher-half kernel sections
     *
     * VMA is now bumped into the higher-half. Every subsequent section has
     * virtual addresses starting at KERNEL_VIRTUAL_BASE + current
     * physical offset, with AT() translating back to physical for
     * loading.
     * ─────────────────────────────────────────────────────────────────── */
    . += KERNEL_VIRTUAL_BASE;
    _kernel_virtual_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE)
    {
        *(.text)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE)
    {
        *(.rodata)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE)
    {
        *(.data)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = ALIGN(8);
    }

    _kernel_virtual_end = .;
    _kernel_physical_end = . - KERNEL_VIRTUAL_BASE;

    /* ───────────────────────────────────────────────────────────────────
     * Discarded sections
     * ─────────────────────────────────────────────────────────────────── */
    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}

