;;;
;;; @file multiboot2.asm
;;; @brief Multiboot2 header and 32-bit entry point for x86-64 kernel bootstrap.
;;;
;;; This file contains:
;;; 1. The Multiboot2 header (must reside within the first 32KiB of the
;;;    kernel image so that GRUB can locate it during scanning).
;;; 2. The 32-bit entry point (@c _start), which validates the boot
;;;    environment, configures paging and SSE, then transitions to 64-bit 
;;;    long mode via a far jump.
;;; 3. Page table construction: identity-maps the first 1 GiB at physical
;;;    address @c 0x0 and mirrors it in the higher-half at virtual address 
;;;    @c 0xFFFF_FFFF_8000_0000 (-2 GiB) using 2MiB huge pages.
;;; 4. A minimal 64-bit GDT used solely for the protected-to-long-mode
;;;    transition.
;;;
;;; @par Virtual memory layout (after paging is enabled)
;;; @code{.txt}
;;;   0x0000_0000_0000_0000 .. 0x0000_0000_3FFF_FFFF  Identity    (1 GiB)
;;;   0xFFFF_FFFF_8000_0000 .. 0xFFFF_FFFF_BFFF_FFFF  Higher-half (1 GiB)
;;; @endcode
;;;
;;; Both ranges point to physical @c 0x0 .. @c 0x3FFF_FFFF, covering the
;;; kernel image and common MMIO regions (e.g., VGA text buffer at @c 0xB8000).
;;;
;;; @note All 32-bit code and data reside in @c .text32 / @c .rodata32, which
;;;       the linker script places at VMA == LMA (physical addresses). GRUB
;;;       boots in 32-bit protected mode without paging, so every symbol
;;;       referenced before the long-mode far jump must resolve to a physical
;;;       address.
;;;
;;; @note The boot page tables are placed in their own @c .boot_page_tables
;;;       section (NOLOAD, VMA == LMA) so that the BSS-clearing loop in
;;;       entry64.asm cannot destroy them while paging is active. Because
;;;       this section is NOLOAD, @c setup_page_tables explicitly zeroes
;;;       the tables before writing entries.

; ── Multiboot2 header ───────────────────────────────────────────────────

section .multiboot2
align 8

extern stack_top

;;; @defgroup mb2hdr Multiboot2 Header
;;; @brief 8-byte-aligned header scanned by GRUB within the first 32 KiB.
;;; @{

MULTIBOOT2_MAGIC      equ 0xE85250D6
MULTIBOOT2_ARCH_I386  equ 0           ; i386 protected mode
MULTIBOOT2_HEADER_LEN equ multiboot2_header_end - multiboot2_header_start
MULTIBOOT2_CHECKSUM   equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH_I386 + MULTIBOOT2_HEADER_LEN)

multiboot2_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH_I386
    dd MULTIBOOT2_HEADER_LEN
    dd MULTIBOOT2_CHECKSUM

    ;; Framebuffer tag - request 80x25 text mode.
    align 8
    dw 5, 1     ; type = 5 (framebuffer), flags = 1 (optional)
    dd 20       ; tag size in bytes
    dd 80       ; width  (columns)
    dd 25       ; height (rows)
    dd 0        ; depth  (0 = text mode)

    ;; End tag - terminates the header tag list.
    align 8
    dw 0, 0     ; type = 0 (end), flags = 0
    dd 8        ; tag size in bytes
multiboot2_header_end:

;;; @}

; ── 32-bit entry point ──────────────────────────────────────────────────

;; The linker script places .text32 at VMA == LMA (physical addresses),
;; so all absolute symbol references here resolve correctly in 32-bit
;; protected mode both before and after paging is enabled.

section .text32
bits 32

global _start
extern long_mode_start

;;; Magic value placed in EAX by a Multiboot2-compliant bootloader.
MULTIBOOT2_BOOTLOADER_MAGIC equ 0x36D76289

;;; @defgroup entry32 32-bit Entry Point
;;; @brief Initial control flow after the bootloader hands off execution.
;;;
;;; @pre EAX = @c MULTIBOOT2_BOOTLOADER_MAGIC
;;; @pre EBX = physical address of the Multiboot2 information structure
;;; @post CPU is in 64-bit long mode with identity + higher-half paging,
;;;       SSE enabled, and control transferred to @c long_mode_start.
;;; @{

_start:
    ;; 1. Initialize the stack pointer.
    mov esp, stack_top

    ;; 2. Clear EFLAGS (including the interrupt flag).
    push 0
    popfd

    ;; 3. Preserve the Multiboot2 info pointer.
    ;;    The bootloader passes it in EBX; this moves it to EDI so that it
    ;;    survives into long mode for kernel_main (System V AMD64 ABI
    ;;    first-argument register).
    mov edi, ebx

    ;; 4. Validate the boot environment.
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne halt_no_multiboot

    call check_cpuid
    test eax, eax
    jz halt_no_cpuid

    call check_long_mode
    test eax, eax
    jz halt_no_long_mode

    ;; 5. Prepare the long-mode environment.
    call setup_page_tables
    call enable_paging
    call enable_sse

    ;; 6. Load the 64-bit GDT and far-jump into long mode.
    lgdt [gdt64_pointer]
    jmp gdt64.code_segment:long_mode_start

;;; @}

; ── Feature detection ───────────────────────────────────────────────────

;;; @defgroup cpudet Feature Detection
;;; @{

EFLAGS_ID_BIT equ (1 << 21)

;;; @brief Detect CPUID support by toggling @c EFLAGS.ID (bit 21).
;;;
;;; If the bit can be flipped, the CPU supports the CPUID instruction.
;;;
;;; @return EAX = 1 if CPUID is supported, 0 otherwise.
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax            ; save original EFLAGs
    xor eax, EFLAGS_ID_BIT  ; flip ID bit
    push eax
    popfd
    pushfd
    pop eax
    push ecx                ; restore original EFLAGS
    popfd
    xor eax, ecx            ; nonzero if ID bit was toggled
    and eax, EFLAGS_ID_BIT
    shr eax, 21             ; normalize to 0 or 1
    ret

CPUID_EXT_BASE equ 0x80000000
CPUID_EXT_FEAT equ 0x80000001
CPUID_LM_BIT   equ (1 << 29)

;;; @brief Detect x86-64 (long mode) support.
;;;
;;; Queries CPUID extended leaf @c 0x8000_0001 and tests the Long Mode bit
;;; (bit 29 of EDX).
;;;
;;; @return EAX = 1 if long mode is supported, 0 otherwise.
;;;
check_long_mode:
    ;; Verify that extended CPUID leaves are available
    mov eax, CPUID_EXT_BASE
    cpuid
    cmp eax, CPUID_EXT_FEAT
    jb .no_long_mode

    ;; Check the LM bit
    mov eax, CPUID_EXT_FEAT
    cpuid
    test edx, CPUID_LM_BIT
    jz .no_long_mode

    mov eax, 1
    ret
.no_long_mode:
    xor eax, eax
    ret

;;; @}

; ── Paging and CPU mode setup ───────────────────────────────────────────

;;; @defgroup paging Paging and CPU Mode Setup
;;; @{

PAGE_PRESENT  equ (1 << 0)
PAGE_WRITABLE equ (1 << 1)
PAGE_HUGE     equ (1 << 7)

;;; @brief Build identity and higher-half mappings using 2 MiB pages.
;;;
;;; Because the @c .boot_page_tables section is NOLOAD, the memory is not
;;; guaranteed to be zeroed. This function clears all three tables before
;;; writing entries, ensuring that unmapped slots have their Present bit
;;; clear.
;;;
;;; Page table wiring:
;;; @code{.txt}
;;;  PML4[0]    -> PDPT  (identity mapping)
;;;   PML4[511]  -> PDPT  (higher-half: -2 GiB via PML4[511] + PDPT[510])
;;;  PDPT[0]    -> PD    (identity, 1 GiB)
;;;  PDPT[510]  -> PD    (higher-half, same physical 1 GiB)
;;;  PD[0..511]          (512 x 2 MiB = 1 GiB)
;;;@endcode
;;;
;;; Resulting virtual-to-physical translations:
;;; - @c 0x0000_0000_0000_0000 -> physical 0x0 (identity)
;;; - @c 0xFFFF_FFFF_8000_0000 -> physical 0x0 (higher-half / kernel)
;;;
;;; @note The first 2 MiB page covers both the kernel image (loaded at
;;;       1 MiB) and the legacy region below 1 MiB, whose fixed-range MTRRs
;;;       mark parts uncacheable. A large page spanning several MTRR memory
;;;       types may be treated as UC in its entirety. These tables only
;;;       live until @c vmm_init(), whose replacement tables map everything
;;;       below 2 MiB with 4 KiB pages, so at worst early boot runs slowly
;;;       on real hardware.
;;;
setup_page_tables:
    ;; Zero all three page tables (3 * 4096 bytes)
    ;;   these are contiguous - pml4 | pdpt | pd
    push edi
    mov edi, pml4_table
    mov ecx, (3 * 4096) / 4
    xor eax, eax
    rep stosd
    pop edi

    ;; PML4[0] and PML4[511] both point to the same PDPT
    mov eax, pdpt_table
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [pml4_table], eax
    mov [pml4_table + 511 * 8], eax

    ;; PDPT[0] and PDPT[510] both point to the same PD
    mov eax, pd_table
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [pdpt_table], eax
    mov [pdpt_table + 510 * 8], eax

    ;; Fill PD with 512 * 2 MiB huge-page entries (1 GiB total)
    xor ecx, ecx
.map_pd:
    mov eax, ecx
    shl eax, 21     ; eax = ecx * 0x20_0000 (2 MiB)
    or eax, (PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE)
    mov [pd_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd
    ret

MSR_EFER equ 0xC0000080
EFER_LME equ (1 << 8)    ; Long Mode Enable
EFER_NXE equ (1 << 11)   ; No-Execute Enable
CR4_PAE  equ (1 << 5)    ; Physical Address Extension
CR0_PG   equ (1 << 31)   ; Paging

;;; @brief Activate long-mode paging (CR3 / CR4 / EFER / CR0).
;;;
;;; After this function returns the CPU is in compatibility mode (32-bit code
;;; segment with paging active). A far jump into a 64-bit code segment is
;;; still required to enter true long mode.
enable_paging:
    ;; Point CR3 at the PML4
    mov eax, pml4_table
    mov cr3, eax

    ;; Enable PAE
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    ;; Set Long Mode Enable and No-Execute Enable in the EFER MSR
    mov ecx, MSR_EFER
    rdmsr
    or eax, EFER_LME | EFER_NXE
    wrmsr

    ;; Enable paging - the CPU enters compatibility mode
    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax
    ret

CR0_EM         equ (1 << 2)   ; Coprocessor Emulation
CR0_MP         equ (1 << 1)   ; Coprocessor Monitoring
CR4_OSFXSR     equ (1 << 9)   ; OS supports FXSAVE/FXRSTOR
CR4_OSXMMEXCPT equ (1 << 10)  ; OS supports unmasked SIMD FP exceptions

;;; @brief Enable SSE/SSE2 execution (CR0.EM clear, CR4.OSFXSR set).
;;;
;;; The kernel is compiled with @c -mno-sse, so compiled C never uses SSE
;;; registers. This is such that any stray SSE instruction executes
;;; instead of raising @c \#UD.
;;;
;;; @warning Any code that does use SSE must not be interruptible, or
;;;          must save and restore the state itself (@c FXSAVE, @c XSAVE).
enable_sse:
    mov eax, cr0
    and eax, ~CR0_EM  ; clear Emulation flag
    or eax, CR0_MP    ; set Monitor Coprocessor
    mov cr0, eax

    mov eax, cr4
    or eax, (CR4_OSFXSR | CR4_OSXMMEXCPT)
    mov cr4, eax
    ret

;;; @}

; ── Error handlers ──────────────────────────────────────────────────────

;;; @defgroup errhalt Error Handlers
;;;
;;; @brief Fatal-error stubs that write a short diagnostic to the VGA text 
;;;        buffer (@c 0xB8000) using white-on-red (attribute @c 0x4F) and halt.
;;; @{

halt_no_multiboot:
    mov dword [0xB8000], 0x4F524F45  ; "ER"
    mov dword [0xB8004], 0x4F3A4F52  ; "R:"
    mov dword [0xB8008], 0x4F424F4D  ; "MB"
    jmp hang

halt_no_cpuid:
     mov dword [0xB8000], 0x4F524F45  ; "ER"
     mov dword [0xB8004], 0x4F3A4F52  ; "R:"
     mov dword [0xB8008], 0x4F504F43  ; "CP"
     jmp hang

halt_no_long_mode:
     mov dword [0xB8000], 0x4F524F45  ; "ER"
     mov dword [0xB8004], 0x4F3A4F52  ; "R:"
     mov dword [0xB8008], 0x4F4D4F4C  ; "LM"
     jmp hang

hang:
    cli
    hlt
    jmp hang

;;; @}

; ── Global Descriptor Table (64-bit) ────────────────────────────────────

;;; @defgroup gdt64 64-bit GDT
;;; @brief Minimal GDT for the protected-to-long-mode transition. 
;;;
;;; Placed in @c .rodata32 (VMA == LMA) so that @c lgdt can reference it
;;; at a physical address while still in 32-bit protected mode.
;;;
;;; Contains a null descriptor, a 64-bit code segment, and a 64-bit data
;;; segment. Only the fields required by hardware in long mode are set.
;;;
;;; @par Code segment descriptor bits
;;; - 43: Executable
;;; - 44: Descriptor type (code/data)
;;; - 47: Present
;;; - 53: Long mode (64-bit)
;;;
;;; @par Data segment descriptor bits
;;; - 41: Writable
;;; - 44: Descriptor type
;;; - 47: Present
;;; @{

section .rodata32
align 16

global gdt64
global gdt64_pointer

gdt64:
    dq 0                                              ; null descriptor
.code_segment: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)  ; 64-bit code
.data_segment: equ $ - gdt64
    dq (1 << 41) | (1 << 44) | (1 << 47)              ; 64-bit data
gdt64_pointer:
    dw $ - gdt64 - 1                                  ; limit (size - 1)
    dq gdt64                                          ; base address

;;; @}

; ── Page tables (BSS) ───────────────────────────────────────────────────

;;; @defgroup pagetbl Boot Page Tables
;;; @brief Page-aligned, NOLOAD page tables (512 * 8 = 4096 bytes each).
;;;
;;; Because this section is NOLOAD, the memory content is undefined at boot.
;;; @c setup_page_tables explicitly zeroes all three tables before writing
;;; entries, so unmapped slots are guaranteed not-present on any platform.
;;;
;;; These tables live in their own @c .boot_page_tables section (VMA == LMA)
;;; so that the BSS-clearing loop in entry64.asm (which uses
;;; @c __bss_start .. @c __bss_end) cannot destroy them while paging is
;;; already active.
;;; @{

section .boot_page_tables nobits alloc write
align 4096

pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table:
    resb 4096

;;; @}

section .note.GNU-stack noalloc noexec nowrite

