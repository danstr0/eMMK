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
Because eMMK lacks a traditional scheduler, the driver supports two modes
of operation:

Polling Mode
------------
The kernel manually calls ``virtio_net_poll()`` at a high frequency (e.g.,
in the main packet-processing loop). This is the most efficient method for
high-throughput network appliances as it avoids the overhead of interrupt
context switching.

Interrupt Mode
--------------
The driver registers ``virtio_net_irq_handler`` to the IDT at vector 50.
When a packet arrives, the device triggers an MSI-X interrupt. The ISR then
calls the poll function to drain the rings.

.. note::

  Regardless of the trigger (Poll vs. Interrupt), the driver uses the
  Receive Callback mechanism. A function must be registered via
  ``virtio_net_set_rx_callback()`` to process incoming data.


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

