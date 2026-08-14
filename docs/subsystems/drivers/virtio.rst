===========
Virtio Core
===========

Overview
========
Virtio is a standardized interface for paravirtualized devices. It allows
the eMMK kernel to communicate with a hypervisor (like QEMU or KVM) using
high-performance, shared-memory ring buffers called Virtqueues.

eMMK implements the Legacy (Transitional) PCI Transport. Configuration is
performed via I/O ports, and memory is exchanged via physically contiguous
buffers.


The Virtqueue Mechanism
=======================
A Virtqueue is a ring buffer consisting of three distinct parts shared
between the Guest (eMMK) and the Host (Hypervisor).

- **Descriptor Table:** An array of descriptors. Each points to a physical
  memory buffer, its length, and whether it is read-only or device-writable.
- **Available Ring:** An array of indices into the Descriptor Table.
  The driver writes here to tell the device: "Here is a new request for
  you to process."
- **Used Ring:** An array of indices into the Descriptor Table. The device
  writes here to tell the driver: "I have finished processing this request."


Data Flow (Driver to Device)
----------------------------
1. **Populate:** The driver takes a free descriptor from the virtqueue and
   sets the ``addr`` and ``len``.
2. **Publish:** The driver adds the descriptor index to the Available Ring
   and updates the ring's ``idx``.
3. **Kick:** The driver writes the queue index to the
   ``VIRTIO_PCI_QUEUE_NOTIFY`` I/O port. This alerts the hypervisor to
   check the ring.


Device Lifecycle
================
Virtio defines a strict "Handshake" protocol to initialize a device. A
driver must follow these steps in order:

1. **Reset:** Write 0 to the Status register.
2. **Acknowledge:** Set the ``ACKNOWLEDGE`` bit (OS found the device).
3. **Driver:** Set the ``DRIVER`` bit (OS knows how to drive it).
4. **Features:** Negotiate features (see below).
5. **Virtqueues:** Create and register one or more virtqueues.
6. **Driver OK:** Set the ``DRIVER_OK`` bit. The device is now active.


Feature Negotiation
===================
Virtio allows the Host and Guest to agree on a subset of features.

- The Host offers a bitmask of supported features.
- The Guest reads this mask and performs a bitwise AND with its desired
  features.
- The Guest writes the result back to the device.

.. note::

  eMMK currently uses the Legacy transport, which only supports a 32-bit
  feature mask. 64-bit features (like ``VIRTIO_F_VERSION_1``) are not yet
  supported by this driver.


Memory Layout
=============
Virtqueues must be stored in physically contiguous memory and are subject
to specific alignment requirements.

The ``virtqueue_mem_size()`` function calculates the total footprint,
including the required padding between the Available ring and the Used
ring to ensure the Used ring starts on a new page boundary (typically 4096
bytes).


API Reference
=============

.. list-table::
   :widths: 30 45

   * - ``virtio_device_init(pci)``
     - Performs the initial ACK / DRIVER handshake and maps I/O ports.
   * - ``virtqueue_create(vdev, idx)``
     - Allocates memory for a ring and registers it with the hardware.
   * - ``virtqueue_add_buf(...)``
     - Places a single buffer into a queue for processing.
   * - ``virtqueue_kick(vq)``
     - Notifies the hypervisor that the queue has pending work.
   * - ``virtqueue_get_buf(vq, &len)``
     - Non-blocking check for completed buffers from the device.


References
==========
- `Virtio Spec
  <https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html>`_
  — official documentation for the legacy PCI interface.
- `OSDev Wiki: Virtio
  <https://wiki.osdev.org/Virtio>`_
  — implementation tips for hobbyist kernels.


Related Files
=============
- ``include/virtio.h``
- ``drivers/virtio.c``

