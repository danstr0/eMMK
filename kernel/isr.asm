;;; @file isr.asm
;;; @brief Interrupt Service Routine (ISR) stubs.
;;;
;;; Assembly stubs bridging hardware interrupts to the C 
;;; @c interrupt_dispatch() handler. Every IDT vector (0-255) has a
;;; dedicated stub so the vector number is embedded at compile time
;;; rather than looked up at runtime.
;;;
;;; @par Interrupt flow
;;; @code
;;;   CPU pushes:  SS, RSP, RFLAGS, CS, RIP [, error code]
;;;   Stub pushes: [dummy error code], vector number, all GPRs
;;;   Stub calls:  interrupt_dispatch(frame *rdi)
;;;   Stub pops:   all GPRs, removes vector + error code
;;;   Stub:        IRETQ
;;; @endcode
;;;
;;; The resulting stack frame matches @c interrupt_frame
;;; exactly.

section .text
bits 64

;;; External C dispatcher.
extern interrupt_dispatch

; ── Stub macros ─────────────────────────────────────────────────────────

;;; @defgroup isr_macros ISR Stub Macros
;;; @{

;;; Stub for vectors that do @b not push an error code.
;;; Pushes a dummy 0 to keep the stack frame uniform.
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push qword 0    ; dummy error code
    push qword %1   ; vector number
    jmp isr_common
%endmacro

;;; Stub for vectors where the CPU already pushed an error code.
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push qword %1   ; vector number (error code already on stack)
    jmp isr_common
%endmacro

;;; @}

; ── Exception stubs (vectors 0-31) ──────────────────────────────────────

;;; @defgroup isr_exceptions Exception Stubs
;;; @brief Vectors 0-31. Exceptions that push and error code use
;;;        @c ISR_ERR; all others use @c ISR_NOERR.
;;; @{

ISR_NOERR 0        ; #DE  Divide Error
ISR_NOERR 1        ; #DB  Debug
ISR_NOERR 2        ;      NMI
ISR_NOERR 3        ; #BP  Breakpoint
ISR_NOERR 4        ; #OF  Overflow
ISR_NOERR 5        ; #BR  Bound Range Exceeded
ISR_NOERR 6        ; #UD  Invalid Opcode
ISR_NOERR 7        ; #NM  Device Not Available
ISR_ERR   8        ; #DF  Double Fault
ISR_NOERR 9        ;      Coprocessor Segment Overrun
ISR_ERR   10       ; #TS  Invalid TSS
ISR_ERR   11       ; #NP  Segment Not Present
ISR_ERR   12       ; #SS  Stack Fault
ISR_ERR   13       ; #GP  General Protection Fault
ISR_ERR   14       ; #PF  Page Fault
ISR_NOERR 15       ;      Reserved
ISR_NOERR 16       ; #MF  x87 Floating-Point
ISR_ERR   17       ; #AC  Alignment Check
ISR_NOERR 18       ; #MC  Machine-Check
ISR_NOERR 19       ; #XM  SIMD Floating-Point
ISR_NOERR 20       ; #VE  Virtualization Exception
ISR_ERR   21       ; #CP  Control Protection Exception
ISR_NOERR 22       ;      Reserved
ISR_NOERR 23       ;      Reserved
ISR_NOERR 24       ;      Reserved
ISR_NOERR 25       ;      Reserved
ISR_NOERR 26       ;      Reserved
ISR_NOERR 27       ;      Reserved
ISR_NOERR 28       ;      Reserved
ISR_NOERR 29       ;      Reserved
ISR_NOERR 30       ;      Reserved
ISR_NOERR 31       ;      Reserved

;;; @}

; ── IRQ and remaining vector stubs (32-255) ─────────────────────────────

;;; @defgroup isr_irqs IRQ / Software Interrupt Stubs
;;; @brief Vectors 32-47: legacy PIC IRQs (remapped).
;;;        Vectors 48-255: available for APIC, MSI, software interrupts.
;;; @{

%assign i 32
%rep 224
    ISR_NOERR i
%assign i i+1
%endrep

;;; @}

;;; @defgroup isr_common Common Handler
;;; @brief All stubs jump here after pushing the error code and vector
;;;        number. Saves the full register state, calls the C dispatcher,
;;;        restores state, and returns via IRETQ.
;;;
;;; Stack on entry (low address at top):
;;; @code
;;;   [RSP+0]   vector number  (pushed by stub)
;;;   [RSP+8]   error code     (pushed by CPU or stub)
;;;   [RSP+16]  RIP            (pushed by CPU)
;;;   [RSP+24]  CS             (pushed by CPU)
;;;   [RSP+32]  RFLAGS         (pushed by CPU)
;;;   [RSP+40]  RSP            (pushed by CPU)
;;;   [RSP+48]  SS             (pushed by CPU)
;;; @endcode
;;; @{

isr_common:
    ;; Save all general-purpose registers.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ;; First argument to interrupt_dispatch: pointer to the frame.
    mov rdi, rsp

    ;; Align the stack to 16 bytes. Save the pre-alignment RSP in
    ;; RBX (callee-saved).
    mov rbx, rsp
    and rsp, ~0xF

    ;; Call the C interrupt dispatcher.
    call interrupt_dispatch

    ;; Restore the exact pre-alignment stack pointer.
    mov rsp, rbx

    ;; Restore all general-purpose registers.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ;; Remove the vector number and error code from the stack.
    add rsp, 16

    ;; Return from interrupt
    iretq

;;; @}

; ── ISR stub table ──────────────────────────────────────────────────────

;;; @defgroup isr_table ISR Stub Table
;;; @brief Array of 256 function pointers, indexed by vector number.
;;;        Used by @c idt_init() to populate all IDT entries.
;;; @{

section .data
global isr_stub_table

isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_ %+ i
%assign i i+1
%endrep

;;; @}

section .note.GNU-stack noalloc noexec nowrite

