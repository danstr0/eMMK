/**
 * @file pci.h
 * @brief PCI bus driver interface.
 *
 * Provides legacy PCI configuration space access via I/O ports
 * @c 0xCF8 / @c 0xCFC, device enumeration, BAR decoding, capability list
 * walking, and MSI-X setup.
 *
 * @par PCI Configuration Space (256 bytes per function)
 * Accessed via a 32-bit address written to port @c 0xCF8:
 * @code{.txt}
 *   Bit 31:      Enable
 *   Bits 23:16:  Bus      (0–255)
 *   Bits 15:11:  Device   (0–31)
 *   Bits 10:8:   Function (0–7)
 *   Bits 7:2:    Register (dword-aligned offset)
 *   Bits 1:0:    Always 0
 * @endcode
 * Data is read / written via port @c 0xCFC.
 *
 * @par Enumeration and drivers
 * @c pci_init() scans all buses / devices / functions and populates an
 * internal device table, then offers every device to the registered
 * drivers (@c pci_register_driver()): a driver's @c probe is called for
 * every device whose ID matches its table — every instance, not just the
 * first. A driver registered after @c pci_init() is offered the
 * still-unbound devices immediately.
 *
 * @par Interrupts
 * @c pci_msix_alloc() enables MSI-X and gives each requested table entry
 * its own IDT vector from the kernel allocator (@c irq.h); drivers never
 * hard-code vector numbers.
 *
 * TODO: legacy CF8/CFC access reaches only the first 256 bytes of config
 * space; PCIe extended capabilities need ECAM.
 */

#ifndef EMMK_PCI_H
#define EMMK_PCI_H

#include "types.h"
#include "idt.h"

/// @name PCI I/O ports
/// @{
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC
/// @}

/// @name Configuration register offsets (type 0 header)
/// @{
#define PCI_VENDOR_ID       0x00  // 16-bit
#define PCI_DEVICE_ID       0x02  // 16-bit
#define PCI_COMMAND         0x04  // 16-bit
#define PCI_STATUS          0x06  // 16-bit
#define PCI_REVISION        0x08  //  8-bit
#define PCI_PROG_IF         0x09  //  8-bit
#define PCI_SUBCLASS        0x0A  //  8-bit
#define PCI_CLASS           0x0B  //  8-bit
#define PCI_CACHE_LINE      0x0C  //  8-bit
#define PCI_LATENCY         0x0D  //  8-bit
#define PCI_HEADER_TYPE     0x0E  //  8-bit
#define PCI_BIST            0x0F  //  8-bit
#define PCI_BAR0            0x10  // 32-bit
#define PCI_BAR1            0x14  // 32-bit
#define PCI_BAR2            0x18  // 32-bit
#define PCI_BAR3            0x1C  // 32-bit
#define PCI_BAR4            0x20  // 32-bit
#define PCI_BAR5            0x24  // 32-bit
#define PCI_CARDBUS_CIS     0x28  // 32-bit
#define PCI_SUBSYS_VENDOR   0x2C  // 16-bit
#define PCI_SUBSYS_ID       0x2E  // 16-bit
#define PCI_ROM_BASE        0x30  // 32-bit
#define PCI_CAPABILITIES    0x34  //  8-bit: offset to first capability
#define PCI_INTERRUPT_LINE  0x3C  //  8-bit: IRQ line (writable by OS)
#define PCI_INTERRUPT_PIN   0x3D  //  8-bit: INT pin (1=A ... 4=D)
/// @}

/// @name Command register bits
/// @{
#define PCI_CMD_IO          (1 << 0)   ///< I/O space access enable.
#define PCI_CMD_MEMORY      (1 << 1)   ///< Memory space access enable.
#define PCI_CMD_BUS_MASTER  (1 << 2)   ///< Bus mastering (DMA) enable.
#define PCI_CMD_INTX_DIS    (1 << 10)  ///< Disable legacy INTx interrupts.
/// @}

/// @name Status register bits
/// @{
#define PCI_STATUS_CAPS    (1 << 4)  ///< Capabilities list present.
#define PCI_STATUS_66MHZ   (1 << 5)  ///< 66 MHz capable.
#define PCI_STATUS_DEVSEL  (3 << 9)  ///< DEVSEL timing mask.
/// @}

/// @name Header type
/// @{
#define PCI_HEADER_TYPE_MASK  0x7F  ///< Header type (0 = standard).
#define PCI_HEADER_MULTI      0x80  ///< Multi-function device flag.
/// @}

/// @name Base Address Register (BAR) decoding
///
/// BAR bit layout:
/// @code
///   Bit 0:       0 = memory space, 1 = I/O space
///   Memory BARS:
///   Bits 2:1:  Type (00 = 32-bit, 10 = 64-bit)
///   Bit 3:     Prefetchable
///   Bits 31:4: Base address (16-byte aligned)
///   I/O BARS:
///   Bits 31:2: Base address (4-byte aligned)
/// @endcode
/// @{
#define PCI_BAR_IO_MASK       0x01         ///< Bit 0: 1 = I/O space.
#define PCI_BAR_MEM_TYPE      0x06         ///< Bits 2:1: memory type mask.
#define PCI_BAR_MEM_32        0x00         ///< 32-bit memory space.
#define PCI_BAR_MEM_64        0x04         ///< 64-bit memory space.
#define PCI_BAR_PREFETCH      0x08         ///< Bit 3: Prefetchable.
#define PCI_BAR_MEM_MASK      0xFFFFFFF0U  ///< Memory address mask.
#define PCI_BAR_IO_ADDR_MASK  0xFFFFFFFC   ///< I/O address mask.
/// @}

/// @name Class codes
/// @{
#define PCI_CLASS_STORAGE  0x01
#define PCI_CLASS_NETWORK  0x02
#define PCI_CLASS_DISPLAY  0x03
#define PCI_CLASS_BRIDGE   0x06
/// @}

/// @name Common vendor / device IDs
/// @{
#define PCI_VENDOR_INTEL    0x8086
#define PCI_VENDOR_VIRTIO   0x1AF4

// Virtio transitional device IDs (0x1000-0x103F)
#define VIRTIO_DEV_NET      0x1000  ///< Network card.
#define VIRTIO_DEV_BLOCK    0x1001  ///< Block device.
#define VIRTIO_DEV_CONSOLE  0x1003  ///< Console.
/// @}

/// @name Capability IDs
/// @{
#define PCI_CAP_PM      0x01  ///< Power management.
#define PCI_CAP_MSI     0x05  ///< Message Signaled Interrupts.
#define PCI_CAP_VENDOR  0x09  ///< Vendor-specific.
#define PCI_CAP_MSIX    0x11  ///< MSI-X.
/// @}

/// @name MSI-X structures
/// @{

/// MSI-X capability layout (at cap offset).
#define MSIX_MSG_CTRL        0x02  ///< Message Control (16-bit).
#define MSIX_TABLE_OFFSET    0x04  ///< Table offset + BIR (32-bit).
#define MSIX_PBA_OFFSET      0x08  ///< PBA offset + BIR (32-bit).

// Message Control register bits
#define MSIX_CTRL_ENABLE     (1 << 15)  ///< MSI-X enable.
#define MSIX_CTRL_MASK_ALL   (1 << 14)  ///< Function mask.
#define MSIX_CTRL_SIZE_MASK  0x07FF     ///< Table size - 1.

/// BIR field mask (bits 2:0 of table / PBA offset registers).
#define MSIX_BAR_MASK        0x07
#define MSIX_OFFSET_MASK     (~0x07U)

/// MSI-X table entry layout (16 bytes per entry, memory-mapped).
struct msix_entry
{
    uint32_t msg_addr_lo;  ///< Message address (low 32 bits).
    uint32_t msg_addr_hi;  ///< Message address (high 32 bits).
    uint32_t msg_data;     ///< Message data (vector number).
    uint32_t vector_ctrl;  ///< Bit 0: masked.
} PACKED;

#define MSIX_ENTRY_MASKED  (1 << 0)
/// @}

/// @name Table limits
/// @{
#define PCI_MAX_DEVICES       64  ///< Enumerated functions.
#define PCI_MAX_DRIVERS       8   ///< Registered drivers.

/// MSI-X entries @c pci_msix_alloc() can set up per device.
#define PCI_MSIX_MAX_VECTORS  32
/// @}

/// @name Driver model
/// @{

/// Wildcard for @c pci_device_id fields.
#define PCI_ANY_ID  0xFFFF

/**
 * @brief One entry of a driver's ID table.
 *
 * A table is terminated by an entry with `vendor == 0`.
 */
struct pci_device_id
{
    uint16_t vendor;  ///< Vendor ID, or @c PCI_ANY_ID.
    uint16_t device;  ///< Device ID, or @c PCI_ANY_ID.
};

struct pci_device;

/**
 * @brief A PCI device driver.
 *
 * Registered with @c pci_register_driver(). The PCI layer calls @c probe
 * once per matching, not yet bound device. ID table is terminated by
 * `vendor == 0`.
 */
struct pci_driver
{
    const char *name;                 ///< Driver name (for logs).
    const struct pci_device_id *ids;  ///< ID table.

    /**
     * Take over @p dev. Return @c 0 to bind the device to this driver,
     * negative to decline (the device stays available to other drivers).
     * A driver that fails must leave the device as it found it apart
     * from config-space state (it may have reset or enabled the device).
     */
    int (*probe)(struct pci_device *dev);
};
/// @}

/// @name PCI device structure
/// @{
struct pci_device
{
    // Address on the bus
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;

    // Identification
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor;
    uint16_t subsys_id;

    // Classification
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;

    // Header info
    uint8_t  header_type;
    uint8_t  interrupt_line;
    uint8_t  interrupt_pin;

    /// Decoded BAR information (populated during enumeration).
    struct
    {
        uint64_t base;         ///< Physical base address.
        uint64_t size;         ///< Region size in bytes.
        bool     is_io;        ///< I/O space (vs memory).
        bool     is_64bit;     ///< 64-bit BAR (consumes next BAR slot).
        bool     is_prefetch;  ///< Memory is prefetchable.
        void    *virt;         ///< Mapping made by @c pci_map_bar().
    } bar[6];

    // Capability offsets (0 = not present). These record the first
    // instance only; use pci_find_next_capability() to walk all of them
    uint8_t  cap_msi;
    uint8_t  cap_msix;
    uint8_t  cap_vendor;

    // MSI-X state (populated if cap_msix != 0)
    uint16_t msix_count;                     ///< Table size.
    volatile struct msix_entry *msix_table;  ///< Virtual address of MSI-X table.
    uint16_t msix_allocated; ///< Entries set up by @c pci_msix_alloc().
    uint8_t  msix_vectors[PCI_MSIX_MAX_VECTORS];  ///< IDT vector of each allocated entry.

    /// Driver bound by the probe loop, or @c NULL.
    const struct pci_driver *driver;

    /// Opaque pointer for the owning driver.
    void *driver_data;
};
/// @}

/// @name Configuration space access
/// @{
uint8_t  pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                uint8_t value);
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint16_t value);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint32_t value);
/// @}

/// @name Per-device config access helpers
/// @{
static inline uint8_t pci_device_read8(struct pci_device *dev, uint8_t offset)
{
    return pci_read8(dev->bus, dev->device, dev->function, offset);
}

static inline uint16_t pci_device_read16(struct pci_device *dev, uint8_t offset)
{
    return pci_read16(dev->bus, dev->device, dev->function, offset);
}

static inline uint32_t pci_device_read32(struct pci_device *dev, uint8_t offset)
{
    return pci_read32(dev->bus, dev->device, dev->function, offset);
}

static inline void pci_device_write8(struct pci_device *dev, uint8_t offset, uint8_t val)
{
    pci_write8(dev->bus, dev->device, dev->function, offset, val);
}

static inline void pci_device_write16(struct pci_device *dev, uint8_t offset, uint16_t val)
{
    pci_write16(dev->bus, dev->device, dev->function, offset, val);
}

static inline void pci_device_write32(struct pci_device *dev, uint8_t offset, uint32_t val)
{
    pci_write32(dev->bus, dev->device, dev->function, offset, val);
}
/// @}

/// @name Initialization and enumeration
/// @{

/**
 * @brief Scan all PCI buses, populate the device table, and probe drivers.
 *
 * BARs are sized with I/O and memory decode disabled in @c COMMAND (and
 * the previous @c COMMAND value restored), so the all-ones sizing write
 * can never make a BAR decode a bogus address range. After enumeration,
 * every registered driver is offered every matching device.
 */
void pci_init(void);

/// @return The number of devices found during enumeration.
int pci_get_device_count(void);

/// @return The device at @p index in the device table (0-based).
struct pci_device *pci_get_device(int index);
/// @}

/// @name Device lookup
/// @{

/**
 * @brief Find a device by vendor and device ID.
 *
 * @param vendor  Vendor ID (or @c 0xFFFF for any).
 * @param device  Device ID (or @c 0xFFFF for any).
 * @param from    Previous result to continue searching from, or @c NULL.
 * @return Pointer to the matching device, or @c NULL.
 *
 * @code
 *   struct pci_device *dev = NULL;
 *   while ((dev = pci_find_device(0x1AF4, 0x1000, dev)) != NULL)
 *      // process dev
 * @endcode
 */
struct pci_device *pci_find_device(uint16_t vendor, uint16_t device,
                                   struct pci_device *from);

/**
 * @brief Find a device by class code.
 *
 * @param class_code  PCI class code.
 * @param subclass    Subclass (or @c 0xFF for any).
 * @param from        Previous result to continue searching, or @c NULL.
 * @return Pointer to the matching device, or @c NULL.
 */
struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass,
                                  struct pci_device *from);
/// @}

/// @name Driver registry
/// @{

/**
 * @brief Register a PCI driver.
 *
 * Before @c pci_init(), the driver is only recorded; @c pci_init() probes
 * it. After @c pci_init(), the driver is offered every matching device
 * that is not yet bound, immediately.
 *
 * @param drv  Driver (must stay valid while registered).
 * @return Number of devices bound by this call, or @c -1 if @p drv is
 *         invalid, already registered, or the registry is full.
 */
int pci_register_driver(const struct pci_driver *drv);

/**
 * @brief Remove a driver from the registry.
 *
 * Devices bound to it are marked unbound.
 */
void pci_unregister_driver(const struct pci_driver *drv);

/**
 * @brief Check @p dev against a driver ID table.
 * @return @c true if any entry of @p ids matches.
 */
bool pci_match_id(const struct pci_device_id *ids,
                  const struct pci_device *dev);
/// @}

/// @name Device configuration
/// @{

/// Enable I/O, memory, and bus mastering for @p dev.
void pci_enable_device(struct pci_device *dev);

/**
 * @brief Enable bus mastering (DMA) for @p dev.
 *
 * @note Also done by @c pci_enable_device(), but can be called
 *       separately if only DMA is needed.
 */
void pci_enable_bus_master(struct pci_device *dev);
/// @}

/// @name Capability list
/// @{

/**
 * @brief Walk the capability list to find a specific ID.
 *
 * @param dev     PCI device.
 * @param cap_id  Capability ID to find.
 * @return Config space offset of the capability, or @c 0 if not found.
 */
uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id);

/**
 * @brief Find the next instance of a capability.
 *
 * Some devices carry several capabilities with the same ID.
 *
 * @param dev     PCI device.
 * @param cap_id  Capability ID to find.
 * @param prev    Offset of the previous match, or @c 0 to start at the
 *                head of the list.
 * @return Config space offset of the next match, or @c 0 if none.
 *
 * @code{.c}
 *   for (uint8_t c = pci_find_next_capability(dev, PCI_CAP_VENDOR, 0); c;
 *        c = pci_find_next_capability(dev, PCI_CAP_VENDOR, c))
 *       // inspect the capability at c
 * @endcode
 */
uint8_t pci_find_next_capability(struct pci_device *dev, uint8_t cap_id,
                                 uint8_t prev);
/// @}

/// @name BAR access
/// @{

/**
 * @brief Get the physical base address of a BAR.
 *
 * @param dev  PCI device.
 * @param bar  BAR index (0-5).
 * @return Decoded physical address, or @c 0 if the BAR is not valid.
 *         For 64-bit BARs, combines @c BAR[n] and @c BAR[n+1].
 */
uint64_t pci_get_bar_address(struct pci_device *dev, int bar);

/**
 * @brief Get the size of a BAR region.
 *
 * @param dev  PCI device.
 * @param bar  BAR index (0-5).
 * @return Region size in bytes.
 */
uint64_t pci_get_bar_size(struct pci_device *dev, int bar);

/**
 * @brief Map a memory BAR uncached (via @c vmm_map_mmio()).
 *
 * The whole BAR is mapped once; later calls return the same mapping.
 *
 * @param dev  PCI device.
 * @param bar  BAR index (0-5).
 * @return Virtual address of the BAR's first byte, or @c NULL for an
 *         I/O BAR, an unimplemented BAR (or the upper half of a 64-bit
 *         BAR), or a mapping failure.
 */
void *pci_map_bar(struct pci_device *dev, int bar);
/// @}

/// @name MSI-X configuration
/// @{

/**
 * @brief Enable MSI-X for a @p dev.
 *
 * Maps the MSI-X table, enables MSI-X in the message control register,
 * and disables legacy INTx. All entries start masked. Calling it again
 * on an enabled device does nothing.
 *
 * Most drivers call @c pci_msix_alloc() instead, which enables MSI-X
 * as needed.
 *
 * @return @c 0 on success, negative on failure.
 */
int pci_msix_enable(struct pci_device *dev);

/**
 * @brief Enable MSI-X and give entries 0, ..., @p n - 1 their own vectors.
 *
 * Allocates @p n vectors from the kernel allocator
 * (@c irq_alloc_vector()), all with @p handler and @p ctx, programs entry
 * @e i with vector @c dev->msix_vectors[i], targets the calling core's
 * LAPIC, and unmasks the entries. Retarget an entry with
 * `pci_msix_set_entry(dev, i, dev->msix_vectors[i], apic_id)`.
 *
 * @param dev
 * @param n
 * @param handler
 * @param ctx
 * @return @c 0 on success; @c -1 on failure (no MSI-X, @p n out of range,
 *         already allocated, or vectors exhausted), in which case nothing
 *         stays allocated.
 */
int pci_msix_alloc(struct pci_device *dev, uint16_t n,
                   interrupt_handler_t handler, void *ctx);

/**
 * @brief Mask the entries set up by @c pci_msix_alloc() and release
 *        their vectors.
 */
void pci_msix_free(struct pci_device *dev);

/**
 * @brief Program a single MSI-X table entry.
 *
 * Configures the entry for edge-triggered, fixed delivery to the
 * specified APIC. The entry is left unmasked after configuration.
 *
 * @param dev      PCI device.
 * @param index    MSI-X vector index (0-based).
 * @param vector   IDT vector number to deliver.
 * @param apic_id  Destination Local APIC ID.
 */
void pci_msix_set_entry(struct pci_device *dev, uint16_t index,
                        uint8_t vector, uint32_t apic_id);

/// Mask a single MSI-X vector.
void pci_msix_mask(struct pci_device *dev, uint16_t index);

/// Unmask a single MSI-X vector.
void pci_msix_unmask(struct pci_device *dev, uint16_t index);
/// @}

#endif // EMMK_PCI_H

