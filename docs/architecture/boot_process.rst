=============
Boot Process
=============

Overview
========

eMMK boots via the Multiboot2 protocol, using GRUB as the bootloader.
Execution proceeds through four stages, transitioning from 32-bit
protected mode to 64-bit long mode before entering the C kernel.

.. list-table:: **Boot stages**
   :header-rows: 1
   :widths: 10 30 30

   * - Stage
     - Location
     - Mode
   * - 1
     - ``_start`` (multiboot2.asm)
     - 32-bit protected mode
   * - 2
     - Page table setup (multiboot2.asm)
     - 32-bit protected mode
   * - 3
     - Far jump into ``long_mode_start``
     - 64-bit long mode
   * - 4
     - Runtime init (entry64.asm) → ``kernel_main()``
     - 64-bit long mode, higher-half


Boot Flow
=========

::

    GRUB (Multiboot2)
      │
      │  EAX = 0x36D76289 (magic)
      │  EBX = physical addr of boot info
      │
      ▼
    _start (32-bit)                          [multiboot2.asm]
      ├── verify Multiboot2 magic
      ├── check_cpuid()
      ├── check_long_mode()
      ├── setup_page_tables()                identity + higher-half
      ├── enable_paging()                    CR3, CR4.PAE, EFER.LME, CR0.PG
      ├── enable_sse()                       CR0.EM/MP, CR4.OSFXSR/OSXMMEXCPT
      └── lgdt + far jmp ───────────▶ long_mode_start (64-bit)
                                      │                [entry64.asm]
                                      ├── save MBI pointer (RD15)
                                      ├── load data segments
                                      ├── zero BSS
                                      ├── set up boot stack
                                      ├── jump to higher-half VMA
                                      ├── relocate RSP to higher-half alias
                                      ├── convert MBI pointer to virtual
                                      └── call kernel_main(mbi)


Stage 1: Multiboot2 Entry
=========================
GRUB scans the first 32 KiB of the kernel image for the Multiboot2
header (magic ``0xE85250D6``). On finding it, GRUB loads the ELF
image at its linked physical address (``0x00100000`` = 1 MiB) and
transfers control to the ELF entry point ``_start`` in 32-bit
protected mode with interrupts disabled.

**Machine state on entry** (per the Multiboot2 specification §3.3):

.. list-table::
   :widths: 15 50

   * - ``EAX``
     - ``0x36D76289`` — Multiboot2 bootloader magic.
   * - ``EBX``
     - Physical address of the Multiboot2 information structure.
   * - ``CS``
     - 32-bit read/execute code segment (base 0, limit 4 GiB).
   * - ``DS/ES/FS/GS/SS``
     - 32-bit read/write data segment (base 0, limit 4 GiB).
   * - ``ESP``
     - Undefined — the kernel must load its own stack immediately.
   * - ``EFLAGS``
     - IF cleared (interrupts disabled).
   * - ``CR0.PG``
     - Cleared (paging disabled).

The bootstrap code validates:

1. ``EAX`` matches the Multiboot2 bootloader magic.
2. The CPU supports the ``CPUID`` instruction (EFLAGS bit 21 toggle).
3. Extended CPUID leaf ``0x8000_0001`` reports long mode support
   (EDX bit 29).

The Multiboot2 header itself requests an 80x25 text-mode framebuffer.
The tag is marked *optional*, so a loader without text-mode support
(e.g., GRUB on UEFI) still boots the kernel; only the error stubs below
lose their output.

Any validation failure writes a two-character diagnostic to the VGA
text buffer at ``0xB8000`` and halts:

.. list-table::
   :widths: 15 40

   * - ``ER:MB``
     - Multiboot2 magic mismatch.
   * - ``ER:CP``
     - CPUID not supported.
   * - ``ER:LM``
     - Long mode not supported.


Stage 2: Paging Initialization
==============================
Before long mode can be activated, 4-level paging must be enabled.
The bootstrap code builds a minimal set of page tables using 2 MiB
huge pages:

.. list-table:: Page table wiring
   :header-rows: 1
   :widths: 25 25 30

   * - Entry
     - Points to
     - Mapping
   * - PML4[0]
     - PDPT
     - Identity (low 1 GiB)
   * - PML4[511]
     - PDPT
     - Higher-half (−2 GiB)
   * - PDPT[0]
     - PD
     - Identity
   * - PDPT[510]
     - PD
     - Higher-half
   * - PD[0..511]
     - 512 × 2 MiB pages
     - Physical 0x0 .. 0x3FFF_FFFF

Both PML4[0] and PML4[511] point to the **same** PDPT, and both
PDPT[0] and PDPT[510] point to the **same** PD. This means the
first 1 GiB of physical memory is accessible at two virtual address
ranges:

::

    0x0000_0000_0000_0000 .. 0x0000_0000_3FFF_FFFF   (identity)
    0xFFFF_FFFF_8000_0000 .. 0xFFFF_FFFF_BFFF_FFFF   (higher-half)

The identity mapping is required so that the code running at its
physical address doesn't fault when paging is first enabled.  The
higher-half mapping provides the permanent kernel address space.

Page table storage
------------------
The three tables (PML4, PDPT, PD) are placed
in a dedicated ``.boot_page_tables`` section, separate from ``.bss``,
so they are not destroyed by the BSS-clearing loop in entry64.asm.

Paging activation sequence
--------------------------
1. Load PML4 physical address into CR3.
2. Enable PAE (CR4 bit 5).
3. Set Long Mode Enable and No-Execute Enable in the EFER MSR (bits 8 and
   11).
4. Enable paging (CR0 bit 31) — the CPU enters compatibility mode.


Stage 3: Long Mode Transition
=============================
After paging is active, the CPU is in 32-bit compatibility mode (a
sub-mode of long mode). True 64-bit execution requires a far jump
into a 64-bit code segment:

::

    lgdt [gdt64.pointer]
    jmp gdt64.code_segment:long_mode_start

The GDT contains three entries: a null descriptor, a 64-bit code segment
(bits 43, 44, 47, 53), and a 64-bit data segment (bits 41, 44, 47). Only
the fields required by hardware in long mode are set; base and limit are
ignored.

SSE/SSE2 is enabled before the transition (CR0.EM cleared, CR0.MP set,
CR4.OSFXSR and CR4.OSXMMEXCPT set) because the C compiler may emit SSE
instructions even in freestanding code.

The GDT labels ``gdt64`` and ``gdt64_pointer`` are exported as globals so
the SMP trampoline can reference them when bringing up Application
Processors (see ``smp.rst``).

Stage 4: 64-bit Runtime Initialization
======================================
``long_mode_start`` is the first code to execute in true 64-bit mode.
It resides in ``.text32`` (VMA == LMA) because the far jump's offset must
be a physical address. It performs the minimum setup required before
calling C:

1. **Save the MBI pointer** — ``mov r15d, edi``. Only EDI was written in
   32-bit mode, and the upper half of every GPR is architecturally
   undefined across the switch into 64-bit mode; the 32-bit move
   zero-extends.

2. **Data segments** — loads the GDT data segment selector into
   DS, ES, FS, GS, SS. CS was set by the far jump.

3. **BSS clearing** — zeroes the region between ``__bss_start`` and
   ``__bss_end`` (linker-provided symbols, 8-byte aligned) using
   ``rep stosq``.

4. **Stack** — sets RSP to ``stack_top``: a 16 KiB region in its own
   ``.boot_stack`` section (NOLOAD, VMA == LMA).

5. **Higher-half jump** — ``mov rax, higher_half_entry; jmp rax``. The
   label is linked at its higher-half VMA, so the absolute 64-bit
   immediate moves RIP into the higher half.

6. **Stack relocation** — adds ``KERNEL_VIRTUAL_BASE`` to RSP, so the
   stack is reached through its higher-half alias.

7. **Multiboot2 pointer conversion** — adds ``KERNEL_VIRTUAL_BASE``
   to R15 to produce a higher-half virtual address in RDI.

8. **Stack alignment** — masks RSP to 16-byte alignment per the
   System V AMD64 ABI.

9. **Call** ``kernel_main(mbi)`` — the Multiboot2 info pointer is
   passed in RDI (first argument, System V AMD64 ABI).

Physical Layout of the Boot Image
=================================
Starting at 1 MiB, the linker emits the boot sections in this order
(all VMA = LMA), followed by the higher-half sections:

.. list-table::
   :header-rows: 1
   :widths: 25 15 50

   * - Section
     - Size
     - Contents
   * - ``.boot``
     - < 4 KiB
     - Multiboot2 header, 32-bit entry, ``long_mode_start``, boot GDT
   * - ``.boot_page_tables``
     - 12 KiB
     - PML4, PDPT, PD (NOLOAD). **Still the live page tables**.
   * - ``.boot_stack``
     - 16 KiB
     - BSP stack (NOLOAD)
   * - ``.ap_trampoline``
     - < 4 KiB
     - AP startup code, copied to ``0x8000`` by ``smp_init()``
   * - ``.text`` ... ``bss``
     - —
     - Higher-half kernel (VMA ``0xFFFF_FFFF_8000_0000`` + physical)

The linker script also places the linker-synthesized dynamic sections
(``.got``, ``.plt``, ``.rela.*``) and asserts that they are empty, and
the build links with ``--orphan-handling=error`` so that any section the
script does not name fails the link instead of being placed arbitrarily.


Postconditions
==============
When ``kernel_main()`` begins executing, the following invariants hold:

- 64-bit long mode is active.
- Paging is enabled (identity + higher-half mappings).
- SSE/SSE2 is enabled.
- The kernel stack is initialized (16 KiB, 16-byte aligned).
- The BSS section is zeroed.
- Execution is in the higher-half address space.
- RDI contains a higher-half pointer to the Multiboot2 info structure.
- Interrupts are disabled (IF = 0).
- CR3 points at the boot PML4, which the rest of the kernel keeps using.


Multiboot2 Information Structure
================================
The Multiboot2 information structure is a variable-length sequence of
8-byte-aligned tags, terminated by a tag of type 0 (end). The kernel
parses it via ``multiboot2_get_tag()`` (defined in ``multiboot2.h``).

**Tags consumed by eMMK:**

.. list-table::
   :header-rows: 1
   :widths: 10 25 40

   * - Type
     - Name
     - Usage
   * - 1
     - Command line
     - Boot flags
   * - 2
     - Bootloader name
     - Diagnostic logging
   * - 6
     - Memory map
     - PMM initialisation
   * - 9
     - ELF sections
     - Not consumed
   * - 14/15
     - ACPI RSDP / XSDP
     - Not consumed yet — required for real hardware.


References
==========
- `Multiboot2 Specification (GNU)
  <https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html>`_
  — §3 (machine state), §4 (header format), §5 (information structure).
- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — §4.1 (paging modes), §9.8 (long mode activation), §10.2 (APIC).
- `AMD64 Architecture Programmer's Manual Vol. 2
  <https://www.amd.com/en/search/documentation/hub.html>`_
  — §5.1 (long mode), §5.3 (page translation).
- `System V AMD64 ABI
  <https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf>`_
  — §3.2 (function calling conventions).


Related Files
=============
- ``boot/multiboot2.asm``
- ``boot/entry64.asm``
- ``kernel/linker.ld``
- ``include/multiboot2.h``
- ``include/kernel.h``

