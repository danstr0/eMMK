==========
Virtio Net
==========

Overview
========
The Virtio-Net driver provides eMMK with high-performance Ethernet
capabilities. It utilizes the Virtio transport to exchange raw Ethernet
frames with the hypervisor. This driver is designed for a run-to-completion
architecture, offering both polling and interrupt-driven receive paths.

Queue Architecture
==================
The driver initializes two mandatory virtqueues for data transfer:

- **Queue 0 (RX):** The "Receive" queue. The driver pre-allocates empty
  buffers and places them here. When the device receives a packet from the
  outside world, it fills one of these buffers and moves it to the Used ring.
- **Queue 1 (TX):** The "Transmit" queue. The driver places packets to be
  sent onto this queue. The device consumes them and moves them to the Used
  ring once they are physically transmitted.


Packet Format
=============
Every packet exchanged via Virtio-Net is prefixed with a Virtio-Net Header
(``struct virtio_net_hdr``). This header contains metadata for advanced
features like checksum offloading and segmentation.

In the current eMMK implementation, Virtio-Net hardware offloading is
disabled for simplicity. Therefore:

- **Outgoing packets:** The driver prepends a 10-byte zeroed header to
  every frame.
- **Incoming packets:** The driver strips the 10-byte header before passing
  the Ethernet frame to the receive callback.


The Receive Model
=================
Packets are only ever processed by polling: the kernel calls
``virtio_net_poll()`` from its packet-processing loop, which drains the
rings and invokes the receive callback (registered via
``virtio_net_set_rx_callback()``) for each frame.

Wake-up Interrupt
-----------------
The driver also registers ``virtio_net_irq_handler`` at vector 50
(MSI-X, targeted at the BSP). The handler only acknowledges the interrupt:
its purpose is to wake a core idling in ``sti; hlt``. It is used to call
``virtio_net_poll()`` itself, which re-entered the rings and the slab
allocator whenever the interrupt arrived while the same core was already
polling.


Initialization Sequence
=======================
When ``virtio_net_init()`` is called:

1. **PCI Discovery:** Locates the Virtio-Net device via its Vendor / Device
   ID.
2. **Virtio Handshake:** Performs the ACK / DRIVER / FEATURES reset sequence.
3. **Feature Negotiation:** Specifically requests ``VIRTIO_NET_F_MAC`` and
   ``VIRTIO_NET_F_STATUS``.
4. **Buffer Allocation:** Pre-allocates 64 buffers of 2048 bytes each
   (``VIRTIO_NET_RX_BUF_SIZE``) for the RX pool.
5. **Queue Fill:** The RX queue is pre-loaded with these buffers so
   the device can begin receiving immediately.
6. **Interrupt Setup:** Configures MSI-X entry 0 to map to the CPU's Local
   APIC at vector 50.


API Reference
=============

.. list-table::
   :widths: 30 45

   * - ``virtio_net_init()``
     - Probes the bus and initializes the first available NIC.
   * - ``virtio_net_send(dev, data, len)``
     - Copies a raw Ethernet frame into a Virtio buffer and kicks
       the TX queue.
   * - ``virtio_net_poll(dev)``
     - Drains both RX and TX rings. Calls the RX callback for any new data.
   * - ``virtio_net_set_rx_callback(...)``
     - Attaches a C function to be called whenever a packet is received.
   * - ``virtio_net_link_up(dev)``
     - Checks if the virtual cable is "plugged in."


Constants and Limits
====================
- **MTU Support:** Standard 1500-byte frames are supported.
- **RX Buffer Count:** 64 (tuned for low-memory environments).
- **Buffer Size:** 2048 bytes (allows for the header + maximum Ethernet
  frame + headroom).

Related Files
=============
- ``include/virtio_net.h``
- ``drivers/net/virtio_net.c``

