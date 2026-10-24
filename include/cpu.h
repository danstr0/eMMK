/**
 * @file cpu.h
 * @brief CPU control: CPUID, MSRs, control registers, per-core setup.
 */

#ifndef EMMK_CPU_H
#define EMMK_CPU_H

#include "types.h"

/// @name Control-register bits
/// @{
#define CR0_MP          (1UL << 1)   ///< Monitor coprocessor.
#define CR0_EM          (1UL << 2)   ///< x87 emulation.
#define CR0_WP          (1UL << 16)  ///< Enforce read-only pages in ring 0.
#define CR4_OSFXSR      (1UL << 9)   ///< OS supports FXSAVE/FXRSTOR.
#define CR4_OSXMMEXCPT  (1UL << 10)  ///< OS handles SIMD FP exceptions.
/// @}

/// @name CPUID
/// @{
static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}
/// @}

/// @name Model-specific registers
/// @{
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)),
                       "c"(msr)
                     : "memory");
}
/// @}

/// @name Control registers
/// @{
static inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}
/// @}

/**
 * @brief Bring the calling core's control registers to the kernel's
 *        standard state.
 *
 * - CR0.WP: ring-0 writes honor read-only page mappings. Without it,
 *   the kernel's own .text / .rodata protections (and any read-only
 *   mapping) are silently ignored for kernel code — which in eMMK is
 *   all code. The CPU does not set it at reset; nothing did until now.
 * - SSE execution enabled (CR0.EM clear, CR0.MP, CR4.OSFXSR / OSXMMEXCPT
 *   set), matching what the BSP's boot code does, so every core behaves
 *   identically.
 *
 * Called by the BSP in @c vmm_init() and by each AP in @c ap_entry().
 */
static inline void cpu_init_core(void)
{
    write_cr0((read_cr0() & ~CR0_EM) | CR0_MP | CR0_WP);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
}

#endif // EMMK_CPU_H

