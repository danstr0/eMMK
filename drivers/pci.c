/**
 * @file pci.c
 * @brief PCI bus driver — enumeration, config access, device probing,
 *        and MSI-X.
 */

#include "pci.h"
#include "io.h"
#include "irq.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"
#include "apic.h"

// ── Device table and driver registry ───────────────────────────────────

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count = 0;

static const struct pci_driver *drivers[PCI_MAX_DRIVERS];
static int driver_count = 0;

/// Set once @c pci_init() has enumerated; later registrations probe
/// at once.
static bool enumerated = false;

// ── Config space address construction ──────────────────────────────────

/**
 * Build the 32-bit address word for PCI config port @c 0xCF8.
 * The offset is masked to @c dword alignment.
 */
static inline uint32_t pci_address(uint8_t bus, uint8_t dev,
                                   uint8_t func, uint8_t offset)
{
    return (1U << 31)
         | ((uint32_t)bus << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | (offset & 0xFC);
}

// ── Configuration space read ───────────────────────────────────────────

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    return (uint8_t)(inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8));
}

// ── Configuration space write ──────────────────────────────────────────

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func,
                 uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func,
                 uint8_t offset, uint16_t value)
{
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    uint32_t tmp = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    tmp = (tmp & ~(0xFFFF << shift)) | ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, tmp);
}

void pci_write8(uint8_t bus, uint8_t dev, uint8_t func,
                uint8_t offset, uint8_t value)
{
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    uint32_t tmp = inl(PCI_CONFIG_DATA);
    int shift = (offset & 3) * 8;
    tmp = (tmp & ~(0xFF << shift)) | ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, tmp);
}

// ── BAR decoding ───────────────────────────────────────────────────────

/**
 * @brief Decode a single BAR.
 *
 * For 64-bit memory BARs, reads and restores both the low and high
 * dwords and marks the next BAR slot as consumed.
 *
 * @pre I/O and memory decode are disabled in @c COMMAND (see
 *      @c pci_size_bars()): while all-ones is written, the BAR would
 *      otherwise claim the top of the address space.
 */
static void pci_read_bar(struct pci_device *pdev, int bar)
{
    uint8_t offset = PCI_BAR0 + bar * 4;
    uint32_t orig = pci_device_read32(pdev, offset);

    // Write all-ones to determine which bits are writable (= size bits)
    pci_device_write32(pdev, offset, 0xFFFFFFFF);
    uint32_t size_mask = pci_device_read32(pdev, offset);
    pci_device_write32(pdev, offset, orig);

    if (size_mask == 0 || size_mask == 0xFFFFFFFF)
        return;  // BAR not implemented

    pdev->bar[bar].is_io = (orig & PCI_BAR_IO_MASK) != 0;

    if (pdev->bar[bar].is_io)
    {
        pdev->bar[bar].base = orig & PCI_BAR_IO_ADDR_MASK;
        pdev->bar[bar].size =
            (~(size_mask & PCI_BAR_IO_ADDR_MASK) + 1) & 0xFFFF;
    }
    else
    {
        uint8_t mem_type = orig & PCI_BAR_MEM_TYPE;
        pdev->bar[bar].is_64bit    = (mem_type == PCI_BAR_MEM_64);
        pdev->bar[bar].is_prefetch = (orig & PCI_BAR_PREFETCH) != 0;

        if (pdev->bar[bar].is_64bit && bar < 5)
        {
            // 64-bit BAR spans two consecutive BAR slots
            uint32_t orig_high = pci_device_read32(pdev, offset + 4);
            pci_device_write32(pdev, offset + 4, 0xFFFFFFFF);
            uint32_t size_high = pci_device_read32(pdev, offset + 4);
            pci_device_write32(pdev, offset + 4, orig_high);

            pdev->bar[bar].base = (orig & PCI_BAR_MEM_MASK)
                                | ((uint64_t)orig_high << 32);
            uint64_t full_mask = (size_mask & PCI_BAR_MEM_MASK)
                               | ((uint64_t)size_high << 32);
            pdev->bar[bar].size = ~full_mask + 1;

            // Next BAR slot is consumed by the high 32 bits
            pdev->bar[bar + 1].size = 0;
        }
        else
        {
            pdev->bar[bar].base = orig & PCI_BAR_MEM_MASK;
            pdev->bar[bar].size =
                ~(size_mask & PCI_BAR_MEM_MASK) + 1;
        }
    }
}

/**
 * @brief Size all six BARs of a type 0 function.
 *
 * Disables I/O and memory decode for the duration and restores the
 * original @c COMMAND value afterwards.
 */
static void pci_size_bars(struct pci_device *pdev)
{
    uint16_t cmd = pci_device_read16(pdev, PCI_COMMAND);
    pci_device_write16(pdev, PCI_COMMAND,
                       cmd & ~(PCI_CMD_IO | PCI_CMD_MEMORY));

    for (int i = 0; i < 6; ++i)
    {
        pci_read_bar(pdev, i);
        if (pdev->bar[i].is_64bit)
            ++i;  // skip next slot
    }

    pci_device_write16(pdev, PCI_COMMAND, cmd);
}

// ── Capability list walking ────────────────────────────────────────────

uint8_t pci_find_next_capability(struct pci_device *dev, uint8_t cap_id,
                                 uint8_t prev)
{
    uint16_t status = pci_device_read16(dev, PCI_STATUS);
    if (!(status & PCI_STATUS_CAPS))
        return 0;

    uint8_t ptr = prev ? pci_device_read8(dev, prev + 1) & 0xFC
                       : pci_device_read8(dev, PCI_CAPABILITIES) & 0xFC;
    int limit = 48;  // guards against infinite loops in malformed lists

    while (ptr && limit-- > 0)
    {
        uint8_t id = pci_device_read8(dev, ptr);
        if (id == cap_id)
            return ptr;
        ptr = pci_device_read8(dev, ptr + 1) & 0xFC;
    }

    return 0;
}

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id)
{
    return pci_find_next_capability(dev, cap_id, 0);
}

// ── Enumeration ────────────────────────────────────────────────────────

static void pci_enumerate_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint16_t vendor = pci_read16(bus, dev, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF)
        return;

    if (device_count >= PCI_MAX_DEVICES)
    {
        kprintf("[PCI] WARNING: Device table full, skipping %x:%x.%x\n",
                bus, dev, func);
        return;
    }

    struct pci_device *pdev = &devices[device_count++];
    memset(pdev, 0, sizeof(*pdev));

    pdev->bus       = bus;
    pdev->device    = dev;
    pdev->function  = func;
    pdev->vendor_id = vendor;
    pdev->device_id = pci_read16(bus, dev, func, PCI_DEVICE_ID);

    // Read class/revision from a single 32-bit read at offset 0x08
    uint32_t class_reg = pci_read32(bus, dev, func, PCI_REVISION);
    pdev->revision    = (uint8_t)(class_reg & 0xFF);
    pdev->prog_if     = (uint8_t)((class_reg >> 8) & 0xFF);
    pdev->subclass    = (uint8_t)((class_reg >> 16) & 0xFF);
    pdev->class_code  = (uint8_t)((class_reg >> 24) & 0xFF);

    pdev->header_type    = pci_read8(bus, dev, func, PCI_HEADER_TYPE);
    pdev->interrupt_line = pci_read8(bus, dev, func, PCI_INTERRUPT_LINE);
    pdev->interrupt_pin  = pci_read8(bus, dev, func, PCI_INTERRUPT_PIN);
    pdev->subsys_vendor  = pci_read16(bus, dev, func, PCI_SUBSYS_VENDOR);
    pdev->subsys_id      = pci_read16(bus, dev, func, PCI_SUBSYS_ID);

    // Decode BARs (Type 0 headers only)
    if ((pdev->header_type & PCI_HEADER_TYPE_MASK) == 0)
        pci_size_bars(pdev);

    // Walk capability list
    pdev->cap_msi    = pci_find_capability(pdev, PCI_CAP_MSI);
    pdev->cap_msix   = pci_find_capability(pdev, PCI_CAP_MSIX);
    pdev->cap_vendor = pci_find_capability(pdev, PCI_CAP_VENDOR);

    // Log the device
    kprintf("[PCI] %x:%x.%x  %x:%x  class %x:%x  rev %x",
            bus, dev, func, pdev->vendor_id, pdev->device_id,
            pdev->class_code, pdev->subclass, pdev->revision);
    if (pdev->cap_msix)
        kprintf("  [MSI-X]");
    else if (pdev->cap_msi)
        kprintf("  [MSI]");
    kprintf("\n");

    for (int i = 0; i < 6; ++i)
        if (pdev->bar[i].size > 0)
            kprintf("       BAR%d: %s 0x%lx, %lu bytes%s\n",
                    i,
                    pdev->bar[i].is_io ? "I/O" : "MEM",
                    pdev->bar[i].base,
                    pdev->bar[i].size,
                    pdev->bar[i].is_prefetch ? " (prefetchable)" : "");
}

static void pci_enumerate_bus(uint8_t bus);

/// Check if a device is a PCI-to-PCI bridge and enumerate its secondary bus.
static void pci_check_bridge(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint8_t class_code = pci_read8(bus, dev, func, PCI_CLASS);
    uint8_t subclass   = pci_read8(bus, dev, func, PCI_SUBCLASS);

    if (class_code == PCI_CLASS_BRIDGE && subclass == 0x04)
    {
        // PCI-to-PCI bridge: secondary bus number is at offset 0x19
        uint8_t secondary_bus = pci_read8(bus, dev, func, 0x19);
        if (secondary_bus != 0)
        {
            kprintf("[PCI] Bridge %x:%x.%x -> bus %x\n",
                    bus, dev, func, secondary_bus);
            pci_enumerate_bus(secondary_bus);
        }
    }
}

static void pci_enumerate_bus(uint8_t bus)
{
    for (uint8_t dev = 0; dev < 32; ++dev)
    {
        uint16_t vendor = pci_read16(bus, dev, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF)
            continue;

        pci_enumerate_function(bus, dev, 0);
        pci_check_bridge(bus, dev, 0);

        uint8_t header = pci_read8(bus, dev, 0, PCI_HEADER_TYPE);
        if (header & PCI_HEADER_MULTI)
            for (uint8_t func = 1; func < 8; ++func)
                if (pci_read16(bus, dev, func, PCI_VENDOR_ID) != 0xFFFF)
                {
                    pci_enumerate_function(bus, dev, func);
                    pci_check_bridge(bus, dev, func);
                }
    }
}

// ── Driver probing ─────────────────────────────────────────────────────

bool pci_match_id(const struct pci_device_id *ids,
                  const struct pci_device *dev)
{
    if (!ids || !dev)
        return false;

    for (; ids->vendor != 0; ++ids)
        if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor_id)
         && (ids->device == PCI_ANY_ID || ids->device == dev->device_id))
            return true;

    return false;
}

/**
 * @brief Offer every unbound device that matches @p drv to its probe.
 * @return Number of devices the driver bound.
 */
static int pci_probe_driver(const struct pci_driver *drv)
{
    int bound = 0;

    for (int i = 0; i < device_count; ++i)
    {
        struct pci_device *dev = &devices[i];
        if (dev->driver || !pci_match_id(drv->ids, dev))
            continue;
    
        if (drv->probe(dev) == 0)
        {
            dev->driver = drv;
            bound++;
            kprintf("[PCI] %x:%x.%x bound to %s\n",
                    dev->bus, dev->device, dev->function, drv->name);
        }
    }

    return bound;
}

int pci_register_driver(const struct pci_driver *drv)
{
    if (!drv || !drv->ids || !drv->probe)
        return -1;

    for (int i = 0; i < driver_count; ++i)
        if (drivers[i] == drv)
            return -1;

    if (driver_count >= PCI_MAX_DRIVERS)
    {
        kprintf("[PCI] Driver registry full, cannot add %s\n", drv->name);
        return -1;
    }

    drivers[driver_count++] = drv;

    // Before enumeration pci_init() does the probing
    return enumerated ? pci_probe_driver(drv) : 0;
}

void pci_unregister_driver(const struct pci_driver *drv)
{
    for (int i = 0; i < driver_count; ++i)
        if (drivers[i] == drv)
        {
            for (int j = i; j < driver_count - 1; ++j)
                drivers[j] = drivers[j + 1];
            drivers[--driver_count] = NULL;
            break;
        }

    for (int i = 0; i < device_count; ++i)
        if (devices[i].driver == drv)
            devices[i].driver = NULL;
}

// ── Initialization ─────────────────────────────────────────────────────

void pci_init(void)
{
    kprintf("[PCI] Enumerating devices...\n");
    device_count = 0;
    enumerated = false;

    // Verify a host bridge exists on bus 0
    if (pci_read32(0, 0, 0, 0) == 0xFFFFFFFF)
    {
        kprintf("[PCI] No PCI host bridge detected\n");
        return;
    }

    // Check if the host bridge is multi-function - if so, there may
    // be multiple root buses (one per function)
    uint8_t header = pci_read8(0, 0, 0, PCI_HEADER_TYPE);
    if (header & PCI_HEADER_MULTI)
        for (uint8_t func = 0; func < 8; ++func)
        {
            if (pci_read16(0, 0, func, PCI_VENDOR_ID) != 0xFFFF)
                pci_enumerate_bus(func);
        }
    else
        pci_enumerate_bus(0);

    kprintf("[PCI] Enumeration complete: %d device(s)\n", device_count);
    enumerated = true;

    // Offer every device to the registered drivers, in registration order
    for (int i = 0; i < driver_count; ++i)
        pci_probe_driver(drivers[i]);
}

// ── Device lookup ──────────────────────────────────────────────────────

struct pci_device *pci_find_device(uint16_t vendor, uint16_t device,
                                   struct pci_device *from)
{
    int start = 0;
    if (from)
        for (int i = 0; i < device_count; ++i)
            if (&devices[i] == from)
            {
                start = i + 1;
                break;
            }

    for (int i = start; i < device_count; ++i)
        if ((vendor == 0xFFFF || devices[i].vendor_id == vendor)
         && (device == 0xFFFF || devices[i].device_id == device))
            return &devices[i];

    return NULL;
}

struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass,
                                  struct pci_device *from)
{
    int start = 0;
    if (from)
        for (int i = 0; i < device_count; ++i)
            if (&devices[i] == from)
            {
                start = i + 1;
                break;
            }

    for (int i = start; i < device_count; ++i)
        if (devices[i].class_code == class_code
         && (subclass == 0xFF || devices[i].subclass == subclass))
            return &devices[i];

    return NULL;
}

// ── Device enable ──────────────────────────────────────────────────────

void pci_enable_device(struct pci_device *dev)
{
    uint16_t cmd = pci_device_read16(dev, PCI_COMMAND);
    cmd |= PCI_CMD_IO | PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER;
    pci_device_write16(dev, PCI_COMMAND, cmd);
}

void pci_enable_bus_master(struct pci_device *dev)
{
    uint16_t cmd = pci_device_read16(dev, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_device_write16(dev, PCI_COMMAND, cmd);
}

// ── BAR access wrappers ────────────────────────────────────────────────

uint64_t pci_get_bar_address(struct pci_device *dev, int bar)
{
    if (bar < 0 || bar > 5) return 0;
    return dev->bar[bar].base;
}

uint64_t pci_get_bar_size(struct pci_device *dev, int bar)
{
    if (bar < 0 || bar > 5) return 0;
    return dev->bar[bar].size;
}

void *pci_map_bar(struct pci_device *dev, int bar)
{
    if (!dev || bar < 0 || bar > 5)
        return NULL;

    // The upper half of a 64-bit BAR has size 0 and is rejected here
    if (dev->bar[bar].is_io || dev->bar[bar].size == 0
     || dev->bar[bar].base == 0)
        return NULL;

    if (!dev->bar[bar].virt)
        dev->bar[bar].virt = vmm_map_mmio(dev->bar[bar].base,
                                          dev->bar[bar].size);
    return dev->bar[bar].virt;
}

// ── Device table access ────────────────────────────────────────────────

int pci_get_device_count(void)
{
    return device_count;
}

struct pci_device *pci_get_device(int index)
{
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

// ── MSI-X configuration ────────────────────────────────────────────────

int pci_msix_enable(struct pci_device *dev)
{
    if (!dev->cap_msix)
    {
        kprintf("[PCI] %x:%x.%x: No MSI-X capability\n",
                dev->bus, dev->device, dev->function);
        return -1;
    }

    if (dev->msix_table)
        return 0;  // already enabled

    uint8_t cap = dev->cap_msix;

    // Read message control to get table size
    uint16_t msg_ctrl = pci_device_read16(dev, cap + MSIX_MSG_CTRL);
    uint16_t count = (msg_ctrl & MSIX_CTRL_SIZE_MASK) + 1;

    // Read table location: BAR index and offset
    uint32_t table_info   = pci_device_read32(dev, cap + MSIX_TABLE_OFFSET);
    uint8_t  table_bir    = table_info & MSIX_BAR_MASK;
    uint32_t table_offset = table_info & MSIX_OFFSET_MASK;

    if (table_bir > 5
        || (uint64_t)table_offset + count * sizeof(struct msix_entry)
           > dev->bar[table_bir].size)
    {
        kprintf("[PCI] %x:%x.%x: MSI-X table outside BAR%u\n",
                dev->bus, dev->device, dev->function, table_bir);
        return -1;
    }

    // Map the table's BAR (uncached). Each device has its own mapping
    uint8_t *bar_virt = pci_map_bar(dev, table_bir);
    if (!bar_virt)
    {
        kprintf("[PCI] %x:%x.%x: cannot map MSI-X table BAR%u\n",
                dev->bus, dev->device, dev->function, table_bir);
        return -1;
    }

    dev->msix_count = count;
    dev->msix_table = (volatile struct msix_entry *)(bar_virt + table_offset);

    // Mask all vectors initially
    for (uint16_t i = 0; i < dev->msix_count; ++i)
        dev->msix_table[i].vector_ctrl = MSIX_ENTRY_MASKED;

    // Enable MSI-X and disable legacy INTx
    msg_ctrl |= MSIX_CTRL_ENABLE;
    msg_ctrl &= ~MSIX_CTRL_MASK_ALL;
    pci_device_write16(dev, cap + MSIX_MSG_CTRL, msg_ctrl);

    uint16_t cmd = pci_device_read16(dev, PCI_COMMAND);
    cmd |= PCI_CMD_INTX_DIS;
    pci_device_write16(dev, PCI_COMMAND, cmd);

    kprintf("[PCI] %x:%x.%x: MSI-X enabled, %u vectors\n",
            dev->bus, dev->device, dev->function, dev->msix_count);

    return 0;
}

void pci_msix_set_entry(struct pci_device *dev, uint16_t index,
                        uint8_t vector, uint32_t apic_id)
{
    if (!dev->msix_table || index >= dev->msix_count)
        return;

    volatile struct msix_entry *entry = &dev->msix_table[index];

    // MSI address format:
    //   Bits 31:20 = 0xFEE (fixed prefix)
    //   Bits 19:12 = Destination APIC ID
    //   Bit 3      = Redirection hint (0 = no)
    //   Bit 2      = Destination mode (0 = physical)
    entry->msg_addr_lo = 0xFEE00000 | (apic_id << 12);
    entry->msg_addr_hi = 0;

    // MSI data format:
    //   Bit 15    = Trigger mode (0 = edge)
    //   Bit 14    = Level (0 = deassert for edge)
    //   Bits 10:8 = Delivery mode (000 = fixed)
    //   Bits 7:0  = Vector number
    entry->msg_data = vector;

    // Unmask this vector
    entry->vector_ctrl = 0;
}

int pci_msix_alloc(struct pci_device *dev, uint16_t n,
                   interrupt_handler_t handler, void *ctx)
{
    if (!dev || !handler || n == 0 || n > PCI_MSIX_MAX_VECTORS
        || dev->msix_allocated != 0)
        return -1;

    if (pci_msix_enable(dev) < 0)
        return -1;

    if (n > dev->msix_count)
    {
        kprintf("[PCI] %x:%x.%x: %u MSI-X entries requested, "
                "%u available\n",
                dev->bus, dev->device, dev->function, n, dev->msix_count);
        return -1;
    }

    for (uint16_t i = 0; i < n; ++i)
    {
        int vector = irq_alloc_vector(handler, ctx);
        if (vector < 0)
        {
            kprintf("[PCI] %x:%x.%x: out of interrupt vectors\n",
                    dev->bus, dev->device, dev->function);
            dev->msix_allocated = i;
            pci_msix_free(dev);
            return -1;
        }
        dev->msix_vectors[i] = (uint8_t)vector;
    }
    dev->msix_allocated = n;

    // Program the entries only once every vector exists
    uint32_t apic = lapic_id();
    for (uint16_t i = 0; i < n; ++i)
        pci_msix_set_entry(dev, i, dev->msix_vectors[i], apic);

    return 0;
}

void pci_msix_free(struct pci_device *dev)
{
    if (!dev)
        return;

    for (uint16_t i = 0; i < dev->msix_allocated; ++i)
    {
        pci_msix_mask(dev, i);
        irq_free_vector(dev->msix_vectors[i]);
        dev->msix_vectors[i] = 0;
    }
    dev->msix_allocated = 0;
}

void pci_msix_mask(struct pci_device *dev, uint16_t index)
{
    if (!dev->msix_table || index >= dev->msix_count)
        return;
    dev->msix_table[index].vector_ctrl = MSIX_ENTRY_MASKED;
}

void pci_msix_unmask(struct pci_device *dev, uint16_t index)
{
    if (!dev->msix_table || index >= dev->msix_count)
        return;
    dev->msix_table[index].vector_ctrl = 0;
}

