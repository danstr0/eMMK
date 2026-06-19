/**
 * @file multiboot2.h
 * @brief Multiboot2 boot protocol structures and tag parsing.
 *
 * Defines the data structures passed by a Multiboot2-compliant bootloader
 * (e.g., GRUB) to the kernel at boot time. The bootloader places a
 * @c multiboot2_info structure in physical memory and passes its address
 * in EBX (converted to a higher-half virtual address by entry64.asm).
 *
 * The info structure is immediately followed by a sequence of
 * variable-length tags, each 8-byte aligned, terminated by a tag whose
 * type is @c MULTIBOOT2_TAG_TYPE_END.
 *
 * @par Usage
 * @code
 *   struct multiboot2_tag_mmap *mmap;
 *   mmap = (struct multiboot2_tag_mmap *)
 *          multiboot2_get_tag(info, MULTIBOOT2_TAG_TYPE_MMAP);
 *   if (mmap)
 *   {
 *       struct multiboot2_mmap_entry *entry;
 *       multiboot2_mmap_foreach(mmap, entry)
 *       {
 *          // process entry->addr, entry->len, entry->type
 *       }
 *   }
 * @endcode
 *
 * @par Intentionally omitted tag types
 * The following tag types are defined by the Multiboot2 specification but
 * are not handled by this kernel:
 * - BASIC_MEMINFO (4)   : redundant with MMAP.
 * - BOOTDEV (5)         : not needed; disks are identified via ACPI.
 * - VBE (7)             : legacy video; serial console only.
 * - FRAMEBUFFER (8)     : no graphical output planned.
 * - APM (10)            : legacy power management; superseded by ACPI.
 * - SMBIOS (13)         : not needed at this stage.
 * - NETWORK (16)        : no network boot support.
 * - EFI* (11-12, 17-20) : may be added if UEFI boot is required.
 * - LOAD_BASE_ADDR (21) : not needed with a fixed-load-address kernel.
 *
 * @see Multiboot2 Specification (GNU), Section 4.4.1
 *      https://www.gnu.org/software/grub/manual/multiboot2/
 */

#ifndef EMMK_MULTIBOOT2_H
#define EMMK_MULTIBOOT2_H

#include "types.h"

/// @name Magic numbers
/// @{

/// Value placed in EAX by a Multiboot2-compliant bootloader.
#define MULTIBOOT2_BOOTLOADER_MAGIC  0x36D76289

/// Magic number in the Multiboot2 header (scanned by GRUB).
#define MULTIBOOT2_HEADER_MAGIC      0xE85250D6
/// @}

/// @name Tag type constants
/// @{
#define MULTIBOOT2_TAG_TYPE_END               0
#define MULTIBOOT2_TAG_TYPE_CMDLINE           1
#define MULTIBOOT2_TAG_TYPE_BOOT_LOADER_NAME  2
#define MULTIBOOT2_TAG_TYPE_MODULE            3
#define MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO     4
#define MULTIBOOT2_TAG_TYPE_BOOTDEV           5
#define MULTIBOOT2_TAG_TYPE_MMAP              6
#define MULTIBOOT2_TAG_TYPE_VBE               7
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER       8
#define MULTIBOOT2_TAG_TYPE_ELF_SECTIONS      9
#define MULTIBOOT2_TAG_TYPE_APM              10
#define MULTIBOOT2_TAG_TYPE_EFI32            11
#define MULTIBOOT2_TAG_TYPE_EFI64            12
#define MULTIBOOT2_TAG_TYPE_SMBIOS           13
#define MULTIBOOT2_TAG_TYPE_ACPI_OLD         14
#define MULTIBOOT2_TAG_TYPE_ACPI_NEW         15
#define MULTIBOOT2_TAG_TYPE_NETWORK          16
#define MULTIBOOT2_TAG_TYPE_EFI_MMAP         17
#define MULTIBOOT2_TAG_TYPE_EFI_BS           18
#define MULTIBOOT2_TAG_TYPE_EFI32_IH         19
#define MULTIBOOT2_TAG_TYPE_EFI64_IH         20
#define MULTIBOOT2_TAG_TYPE_LOAD_BASE_ADDR   21
/// @}

/// @name Memory map entry types
/// @{
#define MULTIBOOT2_MEMORY_AVAILABLE         1
#define MULTIBOOT2_MEMORY_RESERVED          2
#define MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE  3
#define MULTIBOOT2_MEMORY_NVS               4
#define MULTIBOOT2_MEMORY_BADRAM            5
/// @}

/// @name Core structures
/// @{

/**
 * @brief Fixed header at the start of the boot information.
 *
 * Passed to the kernel in RDI and immediately followed by a sequence
 * of 8-byte-aligned tags.
 */
struct multiboot2_info
{
    uint32_t total_size;  ///< Total size of this header + all tags (bytes).
    uint32_t reserved;
    // Tags follow immediately (8-byte aligned)
} PACKED;

/**
 * @brief Generic tag header.
 *
 * Every tag begins with this header. The @p size field includes the
 * header itself. Tags are padded to 8-byte boundaries.
 */
struct multiboot2_tag
{
    uint32_t type;  ///< One of @c MULTIBOOT2_TAG_TYPE_*.
    uint32_t size;  ///< Total size of this tag (bytes), including header.
    // Tag-specific payload follows
} PACKED;
/// @}

/// @name Tag structures
/// @{

/// Command line string passed to the kernel.
struct multiboot2_tag_cmdline
{
    uint32_t type;
    uint32_t size;
    char string[];  ///< Null-terminated command line.
} PACKED;

/// Name of the bootloader that loaded the kernel.
struct multiboot2_tag_bootloader
{
    uint32_t type;
    uint32_t size;
    char string[];  ///< Null-terminated bootloader name.
} PACKED;

/**
 * @brief Memory map from the bootloader.
 *
 * This header is followed by a variable number of 
 * @c multiboot2_mmap_entry structures, each @p entry_size bytes long.
 * Use @c multiboot2_mmap_foreach to iterate.
 */
struct multiboot2_tag_mmap
{
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;     ///< Size of each entry (bytes).
    uint32_t entry_version;  ///< Entry format version (currently 0).
    // Entries follow
} PACKED;

/// A single region in the physical memory map.
struct multiboot2_mmap_entry
{
    uint64_t addr;      ///< Physical start address.
    uint64_t len;       ///< Region length in bytes.
    uint32_t type;      ///< One of @c MULTIBOOT2_MEMORY_*.
    uint32_t reserved;
} PACKED;

/// ACPI 1.0 RSDP (Root System Description Pointer).
struct multiboot2_tag_acpi_old
{
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[];  ///< Raw RSDP bytes (20 bytes for ACPI 1.0).
} PACKED;

/// ACPI 2.0+ XSDP (Extended System Description Pointer).
struct multiboot2_tag_acpi_new
{
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[];  ///< Raw XSDP bytes (≥ 36 bytes for ACPI 2.0+)
} PACKED;

struct multiboot2_tag_elf_sections
{
    uint32_t type;
    uint32_t size;
    uint32_t num;      ///< Number of section header entries.
    uint32_t entsize;  ///< Size of each section header entry (bytes).
    uint32_t shndx;    ///< Index of the section name string table.
    // Section header entries follow
} PACKED;

/// @name Tag lookup
/// @{

/**
 * @brief Find a tag by type in the boot information.
 * 
 * Walks the tag list and returns a pointer to the first tag matching
 * @p type, or @c NULL if not found or if the end tag is reached.
 * Includes a bounds check against @c info->total_size to guard 
 * against zero-size tag (which would cause an infinite loop on
 * corrupted data).
 *
 * @param info  Pointer to the @c multiboot2_info structure.
 * @param type  Tag type to search for (@c MULTIBOOT2_TAG_TYPE_*).
 * @return Pointer to the matching tag, or @c NULL.
 *
 * The returned pointer can be cast to the appropriate tag structure:
 * @code
 *   struct multiboot2_tag_mmap *mmap =
 *       (struct multiboot2_tag_mmap *)
 *       multiboot2_get_tag(info, MULTIBOOT2_TAG_TYPE_MMAP);
 * @endcode
 */
static inline struct multiboot2_tag *
multiboot2_get_tag(struct multiboot2_info *info, uint32_t type)
{
    uintptr_t base = (uintptr_t)info;
    uintptr_t end  = base + info->total_size;

    // First tag starts immediately after the 8-byte info header
    struct multiboot2_tag *tag = (struct multiboot2_tag *)(base + 8);

    while ((uintptr_t)tag + sizeof(*tag) <= end
           && tag->type != MULTIBOOT2_TAG_TYPE_END)
    {
        if (tag->type == type)
            return tag;

        // Guard against corrupted zero-size tags
        if (tag->size < sizeof(struct multiboot2_tag))
            return NULL;

        uintptr_t next = (uintptr_t)tag + tag->size;
        tag = (struct multiboot2_tag *)((next + 7) & ~7ULL);
    }

    return NULL;
}
/// @}

/// @name Memory map iterator
/// @{

/**
 * @brief Iterate over memory map entries.
 *
 * @param mmap   Pointer to a multiboot2_tag_mmap.
 * @param entry  Loop variable (type: @c struct @c multiboot2_mmap_entry *).
 *
 * Example:
 * @code
 *   struct multiboot2_mmap_entry *entry;
 *   multiboot2_mmap_foreach(mmap, entry)
 *   {
 *       kprintf("  region: 0x%lx, %lu bytes, type %u\n",
 *               entry->addr, entry->len, entry->type);
 *   }
 * @endcode
 */
#define multiboot2_mmap_foreach(mmap, entry)                                 \
    for ((entry) = (struct multiboot2_mmap_entry *)                          \
                   ((uint8_t *)(mmap) + sizeof(struct multiboot2_tag_mmap)); \
         (uint8_t *)(entry) < (uint8_t *)(mmap) + (mmap)->size;              \
         (entry) = (struct multiboot2_mmap_entry *)                          \
                  ((uint8_t *)(entry) + (mmap)->entry_size))

#endif // EMMK_MULTIBOOT2_H

