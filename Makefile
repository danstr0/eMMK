# eMMK Makefile
#
# Targets:
#   make         Build the kernel ISO
#   make run     Build and boot in QEMU with virtio-net
#   make test    Build and run the test kernel
#   make clean   Remove all build artifacts
#
# Prerequisites:
#   gcc, nasm, ld, grub-mkrescue (+ xorriso), qemu-system-x86_64
#

# ── Toolchain ───────────────────────────────────────────────────────────

CC     := gcc
LD     := ld
NASM   := nasm
QEMU   := qemu-system-x86_64
GRUB   := grub-mkrescue

# ── Compiler / assembler / linker flags ─────────────────────────────────

CFLAGS := -std=c17 -ffreestanding       \
          -mno-red-zone -mcmodel=kernel \
          -fno-pic -fno-pie             \
          -mno-sse -mno-mmx -mno-avx    \
          -fno-stack-protector          \
          -Wall -Wextra -Werror         \
          -Iinclude                     \
          -MMD -MP

NASMFLAGS := -f elf64

LDFLAGS := -nostdlib               \
           -T kernel/linker.ld     \
           -z max-page-size=0x1000

# ── QEMU flags ──────────────────────────────────────────────────────────

QEMU_BASE := -m 128M -serial stdio -display none
QEMU_NET  := -device virtio-net-pci,netdev=net0 -netdev user,id=net0
QEMU_TEST := -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot

# ── Source files ────────────────────────────────────────────────────────

ASM_SRC  := boot/multiboot2.asm boot/entry64.asm kernel/isr.asm

KERN_SRC := kernel/serial.c kernel/string.c kernel/idt.c            \
			kernel/pic.c kernel/apic.c kernel/timer.c               \
			mm/pmm.c mm/vmm.c mm/heap.c mm/slab.c                   \
			drivers/pci.c drivers/virtio.c drivers/net/virtio_net.c \
			net/netbuf.c net/ethernet.c net/netdev.c                \
			net/arp.c net/ip.c net/icmp.c

TEST_SRC := test/test_main.c test/test_types.c test/test_string.c \
			test/test_pmm.c test/test_vmm.c test/test_heap.c      \
			test/test_slab.c test/test_interrupts.c               \
			test/test_pci_net.c                                   \
			test/test_netbuf.c test/test_net.c

# ── Object files ────────────────────────────────────────────────────────

ASM_OBJ  := $(patsubst %.asm, build/%.o, $(ASM_SRC))
KERN_OBJ := $(patsubst %.c,   build/%.o, $(KERN_SRC))
TEST_OBJ := $(patsubst %.c,   build/%.o, $(TEST_SRC))

# ── Top-level targets ───────────────────────────────────────────────────

.PHONY: all run test clean

all: build/os.iso

run: build/os.iso
	$(QEMU) -cdrom $< $(QEMU_BASE) $(QEMU_NET)

## Run the test kernel in QEMU
## success -> exit 1, failure -> exit 3
test: build/test.iso
	@$(QEMU) -cdrom $< $(QEMU_BASE) $(QEMU_NET) $(QEMU_TEST) 2>/dev/null; \
	rc=$$?;                                                               \
	if [ $$rc -eq 1 ]; then exit 0;                                       \
	else echo "\nTEST RUN FAILED (qemu exit $$rc)"; exit 1; fi

clean:
	rm -rf build

# ── Normal kernel build ─────────────────────────────────────────────────

build/kernel.bin: $(ASM_OBJ) build/kernel/main.o $(KERN_OBJ)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

build/os.iso: build/kernel.bin grub.cfg
	@mkdir -p build/iso/boot/grub
	cp build/kernel.bin build/iso/boot/
	cp grub.cfg build/iso/boot/grub/
	$(GRUB) -o $@ build/iso 2>/dev/null

# ── Test kernel build ───────────────────────────────────────────────────

build/test-kernel.bin: $(ASM_OBJ) build/test/main.o $(KERN_OBJ) $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

build/test.iso: build/test-kernel.bin grub.cfg
	@mkdir -p build/test-iso/boot/grub
	cp build/test-kernel.bin build/test-iso/boot/kernel.bin
	cp grub.cfg build/test-iso/boot/grub/
	$(GRUB) -o $@ build/test-iso 2>/dev/null

# ── Compilation rules ───────────────────────────────────────────────────

## Assembly (NASM) - boot directory
build/boot/%.o: boot/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

## Assembly (NASM) - kernel directory
build/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

## C - kernel sources
build/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

## C - mm sources
build/mm/%.o: mm/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

## C - driver sources
build/drivers/%.o: drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

## C - driver subdirectory sources
build/drivers/net/%.o: drivers/net/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

## C - net sources
build/net/%.o: net/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

## C - main.c recompiled with KERNEL_TEST
build/test/main.o: kernel/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DKERNEL_TEST -c $< -o $@

## C - test sources
build/test/%.o: test/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DKERNEL_TEST -c $< -o $@

# ── Auto-generated header dependencies ──────────────────────────────────

-include $(wildcard build/kernel/*.d) $(wildcard build/mm/*.d)           \
         $(wildcard build/drivers/*.d) $(wildcard build/drivers/net/*.d) \
         $(wildcard build/net/*.d) $(wildcard build/test/*.d)

