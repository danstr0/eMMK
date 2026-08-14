========================
Network Devices (netdev)
========================

Overview
========
The ``netdev`` layer provides a unified interface for various network
hardware. By abstracting device-specific details into an operations table
(``vtable``), the IP stack can transmit and receive packets without knowing
whether the underlying hardware is a virtualized Virtio NIC, an Intel e1000,
or a loopback interface.


Architecture
============
Each network device is represented by a ``struct netdev``. This structure
tracks the device's identity, its hardware capabilities, its current IPv4
configuration, and its performance statistics.

Driver Interface
----------------
A driver "plugs into" the stack by implementing the ``netdev_ops`` structure:

- **open / close:** Enables or disables the hardware's RX/TX engines.
- **send:** Accepts a ``netbuf`` from the stack and schedules it for
  physical transmission.
- **poll:** Manually triggers the driver to check for new packets (essential
  for eMMK's non-blocking design).
- **set_mac:** Updates the hardware's physical address filter.


Data Path and Queuing
=====================
eMMK uses a two-stage receive process to decouple hardware interrupts from
protocol processing.

1. **Ingestion:** The driver (via poll or an ISR) retrieves a raw frame from
   the hardware, wraps it in a ``netbuf``, and calls ``netdev_receive()``.
   This function places the packet into the device's private ``rx_queue``.

2. **Processing:** The main kernel loop calls ``netdev_process_rx()``. This
   function drains the ``rx_queue`` of every registered device and passes
   the packets to the Ethernet layer (``eth_input``) for parsing.

.. note::

  The ``netdev_send()`` function always consumes the provided ``netbuf``.
  If the transmission succeeds, the driver eventually frees it. If it fails,
  the ``netdev`` layer frees it immediately to prevent memory leaks.


IPv4 Configuration
==================
In eMMK, each netdev supports a single IPv4 address. Configuration is
handled via ``netdev_set_ip()``, which assigns the IP address, subnet mask,
and default gateway (all in host byte order).

The API provides convenient macros for handling IP addresses:

- ``IP4_ADDR(a, b, c, d)``: Constructs a 32-bit integer address.
- ``IP4_FMT(ip):`` A helper for kprintf to print addresses in
  dotted-decimal notation.


Lifecycle
=========
1. **Allocation:** The driver calls ``netdev_alloc("eth0")``.
2. **Setup:** The driver populates ``dev->mac``, ``dev->ops``, and
   ``dev->priv`` (a pointer to the driver's internal state, like ``struct
   virtio_net_dev``).
3. **Registration:** ``netdev_register(dev)`` makes the device visible to
   the stack.
4. **Activation:** The system calls ``dev->ops->open()`` and configures
   the IP.


API Reference
=============

.. list-table::
   :header-rows: 1
   :widths: 30 50

   * - Function
     - Description
   * - ``netdev_register(dev)``
     - Adds a device to the global registry (max 4 devices).
   * - ``netdev_get_default()``
     - Returns the first registered device (usually eth0).
   * - ``netdev_send(dev, nb)``
     - The entry point for sending a packet.
   * - ``netdev_poll_all()``
     - Triggers all drivers to check for incoming data.
   * - ``netdev_process_rx()``
     - Drains all RX queues and feeds the network stack.


Related Files
-------------
- ``include/netdev.h``
- ``kernel/netdev.c``

