;;; @file entry64.asm
;;; @brief 64-bit long mode entry point.
;;;
;;; Entered via the far jump in multiboot2.asm after paging and SSE have been
;;; enabled.
;;;
;;; @par Responsibilities
;;; 1. Load 64-bit data segments.
;;; 2. Zero the BSS section.
;;; 3. Transition to the higher-half virtual address space.
;;; 4. Relocate the stack pointer to its higher-half alias.
;;; 5. Call @c kernel_main() with the Multiboot2 info pointer.
;;;
;;; @par Section layout
;;; @c long_mode_start resides in @c .text_32 (VMA == LMA) because it is the
;;; target of a 32-bit far jump whose offset must be a physical address.
;;; After initial setup it jumps to @c higher_half_entry in @c .text
;;; (higher-half VMA).
;;;
;;; @par Calling convention
;;; The System V AMD64 ABI passes the first integer argument in RDI. The
;;; Multiboot2 info pointer (physical address) is preserved across setup in
;;; a callee-saved register and converted to a higher-half virtual address
;;; before being passed to @c kernel_main().

; ── Long-mode entry (physical addresses) ────────────────────────────────

section .text32
bits 64

global long_mode_start
global stack_top

extern kernel_main
extern __bss_start  ; provided by linker script (8-byte aligned)
extern __bss_end    ; provided by linker script (8-byte aligned)

;;; Higher-half virtual base: PML4[511] + PDPT[510] = -2 GiB.
KERNEL_VIRTUAL_BASE equ 0xFFFFFFFF80000000

;;; GDT data-segment selector.
GDT64_DATA_SEG equ 0x10

;;; @defgroup entry64 Long-Mode Entry
;;; @brief First code executed in 64-bit mode.
;;;
;;; @pre  Paging is active (identity + higher-half mappings).
;;; @pre  RDI contains the physical address of the Multiboot2 info structure
;;;       (zero-extended from EDI, set by multiboot2.asm).
;;; @post @c kernel_main() has been called with a higher-half pointer to the
;;;       Multiboot2 information structure.
;;; @{

long_mode_start:
    ;; 1. Save the Multiboot2 info pointer in a callee-saved register
    ;;    before anything can clobber RDI.
    mov r15, rdi

    ;; 2. Load the 64-bit data segment into all data/stack segment registers.
    ;;    CS was set by the far jump from 32-bit mode.
    mov ax, GDT64_DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ;; 3. Zero the BSS section (qword-at-a-time).
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    shr rcx, 3    ; byte count -> qword count
    xor rax, rax
    rep stosq

    ;; 4. Set up the 64-bit kernel stack.
    mov rsp, stack_top

    ;; 5. Jump to the higher-half virtual address.
    mov rax, higher_half_entry
    jmp rax

;;; @}

; ── Higher-half 64-bit code ─────────────────────────────────────────────

section .text

;;; @defgroup hh Higher-Half Entry
;;; @brief Code executing at higher-half virtual addresses.
;;;
;;; Once RIP is in the higher-half, the identity mapping is no longer
;;; required for kernel code. It remains active until the kernel
;;; explicitly tears it down.
;;; @{

higher_half_entry:
    ;; 6. Relocate the stack pointer to its higher-half alias.
    mov rax, KERNEL_VIRTUAL_BASE
    add rsp, rax

    ;; 7. Convert the Multiboot2 info pointer from physical to virtual.
    mov rdi, r15
    add rdi, KERNEL_VIRTUAL_BASE

    ;; 7. Ensure 16-byte stack alignment (System V AMD64 ABI requirement).
    and rsp, ~0xF

    ;; 8. Enter the C kernel.
    call kernel_main

    ;; If kernel_main() returns, halt permanently.
.hang:
    cli
    hlt
    jmp .hang

;;; @}

; ── Boot stack ──────────────────────────────────────────────────────────

;;; @defgroup bootstack Boot Stack
;;; @brief 16 KiB stack — 16-byte aligned, at physical addresses (VMA == LMA).
;;;
;;; @c stack_top is exported so that the 32-bit entry point in multiboot2.asm
;;; can initialize ESP before paging is available. After the transition to
;;; higher-half code, RSP is relocated to the equivalent higher-half alias
;;; so the stack remains accessible once the identity mapping is torn down.
;;; @{

section .boot_stack nobits alloc write
align 16

stack_bottom:
    resb 16384  ; 16 KiB
stack_top:

;;; @}

section .note.GNU-stack noalloc noexec nowrite

