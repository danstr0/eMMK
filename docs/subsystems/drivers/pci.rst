=======
PCI Bus
=======

Overview
========
The PCI (Peripheral Component Interconnect) subsystem is responsible for
discovering, identifying, and configuring hardware devices on the system bus.
eMMK currently implements the legacy PCI Configuration Space access
mechanism, which is universally supported and sufficient for modern
virtualization environments like QEMU.

The driver provides a centralized device table, BAR (Base Address Register)
decoding, and support for MSI-X, allowing drivers to use high-performance
message-signaled interrupts rather than shared legacy IRQ lines.


Configuration Space Access
==========================
Access to PCI configuration space (256 bytes per function) is performed
via two 32-bit I/O ports:

- **0xCF8 (CONFIG_ADDRESS):** Specifies the Bus, Device, Function, and
  Register to access.
- **0xCFC (CONFIG_DATA):** Performs the actual 8, 16, or 32-bit Read /
  Write operation.

.. list-table:: **CONFIG_ADDRESS Bit Layout**
   :header-rows: 1
   :widths: 15 15 60

   * - Bits
     - Name
     - Description
   * - ``31``
     - Enable
     - Must be 1 to enable configuration cycles.
   * - ``23:16``
     - Bus
     - 0–255.
   * - ``15:11``
     - Device
     - 0–31.
   * - ``10:8``
     - Function
     - 0–7.
   * - ``7:2``
     - Register
     - dword-aligned offset (``0x00`` – ``0xFC``).


Enumeration
===========
The kernel performs a "cold boot" scan of the PCI bus during ``pci_init()``.
It iterates through all 256 buses, checking for the presence of a Vendor ID
at Device 0, Function 0.

1. **Scanning:** If a device is found, its header is read to check if it is
   a multi-function device.
2. **Identification:** The Vendor ID, Device ID, and Class codes are recorded.
3. **BAR Decoding:** The kernel automatically determines the size and type
   (I/O or Memory) of all Base Address Registers.
4. **Registration:** The device is added to an internal table (up to
   ``PCI_MAX_DEVICES``).


Base Address Registers (BARs)
=============================
BARs allow the kernel to discover the physical address range where a
device's registers are mapped. The PCI driver handles the complexities of
64-bit BARs (which consume two slots) and distinguishes between Memory and
I/O space.

- **I/O BARs:** Mapped into the x86 I/O port space.
- **Memory BARs:** Mapped into the system's physical memory address space
  (MMIO).

.. note::

  Drivers should use ``pci_get_bar_address()`` and ``pci_get_bar_size()``
  to retrieve these values rather than reading the configuration space
  directly, as the driver handles 64-bit address reconstruction and mask
  decoding.


MSI-X Support
=============
Message Signaled Interrupts (MSI-X) are the preferred interrupt mechanism
in eMMK. Unlike legacy interrupts which use a shared physical wire, MSI-X
allows a device to write a specific "message" to a Local APIC address to
trigger a specific IDT vector.


**Advantages:**

- Eliminates shared IRQ lines (no "IRQ sharing" performance penalties).
- Allows a single device (like a multi-queue NIC) to trigger different
  vectors on different CPU cores.
- The PCI driver manages the MSI-X Table, which is a memory-mapped
  structure usually located in one of the device's BARs.


API Reference
=============

.. list-table::
   :widths: 30 45

   * - ``pci_init()``
     - Scans the bus and populates the global device list.
   * - ``pci_find_device(vid, did, prev)``
     - Find a device by Vendor / Device ID. Used for specific drivers.
   * - ``pci_find_class(class, sub, prev)``
     - Find a device by type (e.g., Storage or Network).
   * - ``pci_enable_device(dev)``
     - Enables Memory/IO access and Bus Mastering (DMA).
   * - ``pci_msix_enable(dev)``
     - Prepares the device for MSI-X interrupts.
   * - ``pci_msix_set_entry(...)``
     - Maps a specific MSI-X table index to an IDT vector and CPU.


Example: Initializing a Virtio-Net Device
-----------------------------------------

.. code-block:: c

  struct pci_device *dev = pci_find_device(PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET, NULL);
  if (dev)
  {
      pci_enable_device(dev);
      // Map BARs and configure Virtio...
  }


References
----------
- `OSDev Wiki: PCI
  <https://wiki.osdev.org/PCI>`_
  — general overview of PCI configuration and enumeration.
- `PCI Local Bus Specification
  <https://www.pcisig.com/>`_
  — the official standard for PCI.
- `Virtio Specification
  <https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html>`_
  — Virtio Over PCI Bus (§4.1).


Related Files
-------------
- ``include/pci.h``
- ``drivers/pci.c``

