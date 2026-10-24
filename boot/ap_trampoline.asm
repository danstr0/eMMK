;;; @file ap_trampoline.asm
;;; @brief Application Processor startup trampoline.
;;;
;;; This code is copied by the BSP to physical address @c AP_TRAMPOLINE_ADDR
;;; (@c 0x8000) before sending SIPI. Each AP begins execution here in 16-bit
;;; real mode and transitions through protected mode into 64-bit long
;;; mode, then jumps to the C entry point @c ap_entry().
;;;
;;; The BSP writes boot parameters (PML4, GDT, entry point, stack) into a
;;; parameter block at offset @c AP_PARAMS_OFFSET (@c 0xF00) within the same
;;; page. The trampoline reads these at runtime so nothing is hardcoded.
;;;
;;; @par Memory layout of the trampoline page (4 KiB at @c 0x8000)
;;; @code{.txt}
;;;   0x8000   Trampoline code (real → protected → long mode)
;;;   0x8F00   Parameter block (struct ap_boot_params)
;;;   0x8FFF   End of page
;;; @endcode

;;; The trampoline is assembled position-independently at address 0.
;;; The BSP copies it to @c AP_TRAMPOLINE_ADDR, so all absolute references
;;; must be relative to that base.
;;;
%define TRAMPOLINE_BASE  0x8000
%define PARAMS           (TRAMPOLINE_BASE + 0xF00)

;;; @name Parameter block field offsets
;;; @{
%define PARAM_PML4           0
%define PARAM_GDT_PTR_LOW    4
%define PARAM_GDT_PTR_HIGH   8
%define PARAM_GDT_LIMIT     12
%define PARAM_ENTRY_POINT   16
%define PARAM_STACK_TOP     24
%define PARAM_AP_READY      32
%define PARAM_AP_ID         36
;;; @}

section .ap_trampoline
global ap_trampoline_start
global ap_trampoline_end

; ── 16-bit real mode entry ──────────────────────────────────────────────

bits 16
ap_trampoline_start:
    cli
    cld

    ;; Set up real-mode segments. The trampoline runs at 0x8000,
    ;; which is segment 0x0800 : offset 0x0000 in real mode.
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ;; Enable the A20 gate via port 0x92 (fast A20)
    in al, 0x92
    or al, 2
    and al, ~1
    out 0x92, al

    ;; Load a temporary 32-bit GDT for the real -> protected transition
    lgdt [TRAMPOLINE_BASE + (gdt32_ptr - ap_trampoline_start)]

    ;; Enable protected mode (CR0.PE)
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ;; Far jump to flush the pipeline and load a 32-bit code segment
    jmp 0x08:(TRAMPOLINE_BASE + (protected_entry - ap_trampoline_start))

; ── 32-bit protected mode ───────────────────────────────────────────────

bits 32
protected_entry:
    ;; Load 32-bit data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, TRAMPOLINE_BASE + 0xE00

    ;; Load the PML4 physical address into CR3
    mov eax, [PARAMS + PARAM_PML4]
    mov cr3, eax

    ;; Enable PAE (CR4.PAE, bit 5)
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ;; Enable long mode and NXE (EFER.LME, bit 8; EFER.NXE, bit 11)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr

    ;; Enable paging (CR0.PG, bit 31) - enters compatibility mode
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ;; Load the 64-bit GDT provided by the BSP
    ;; Build a temporary GDT pointer on the stack
    sub esp, 10
    mov ax, [PARAMS + PARAM_GDT_LIMIT]
    mov [esp], ax                           ; limit
    mov eax, [PARAMS + PARAM_GDT_PTR_LOW]
    mov [esp + 2], eax                      ; base low
    mov eax, [PARAMS + PARAM_GDT_PTR_HIGH]
    mov [esp + 6], eax                      ; base high
    lgdt [esp]
    add esp, 10

    ;; Far jump into a 64-bit code segment
    jmp 0x08:(TRAMPOLINE_BASE + (long_mode_entry - ap_trampoline_start))

; ── 64-bit long mode ────────────────────────────────────────────────────

bits 64
default abs      ; [PARAMS + ...] are absolute addresses, not RIP-relative
long_mode_entry:
    ;; Load 64-bit data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ;; Load the per-AP stack
    mov rsp, [PARAMS + PARAM_STACK_TOP]

    ;; Ensure 16-byte stack alignment
    and rsp, ~0xF

    ;; Jump to the C entry point (virtual address)
    mov rax, [PARAMS + PARAM_ENTRY_POINT]
    jmp rax

; ── Temporary 32-bit GDT (only for real -> protected transition) ────────

align 16
gdt32_start:
    dq 0    ; null descriptor
    ; 32-bit code segment: base 0, limit 4G, executable, readable
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 32-bit data segment: base 0, limit 4G, writable
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt32_end:

gdt32_ptr:
    dw gdt32_end - gdt32_start - 1                            ; limit
    dd TRAMPOLINE_BASE + (gdt32_start - ap_trampoline_start)  ; base

ap_trampoline_end:

section .note.GNU-stack noalloc noexec nowrite

