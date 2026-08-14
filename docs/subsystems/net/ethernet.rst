========
Ethernet
========

Overview
========
Ethernet is the standard link-layer protocol used by eMMK to communicate
over a local area network. The kernel specifically supports Ethernet II
frames. This layer acts as the bridge between the physical netdev and the
network layer protocols like ARP and IPv4.


Frame Structure
===============
An Ethernet frame consists of a 14-byte header followed by a payload of 46
to 1500 bytes. eMMK handles the header in software; the Frame Check Sequence
(FCS) is typically appended or verified by the virtio-net hardware.

The header contains three fields:

1. **Destination MAC (6 bytes):** The physical address of the target NIC.
2. **Source MAC (6 bytes):** The physical address of the sending NIC.
3. **EtherType (2 bytes):** Indicates which protocol is encapsulated in
   the payload (e.g., ``0x0800`` for IPv4).


Byte Order Conventions
======================
To simplify development, the Ethernet API uses a "Host-Order-In,
Host-Order-Out" convention for parameters:

- **Constants:** Constants like ``ETH_P_IP`` are defined in host byte order.
- **API Calls:** When calling ``eth_output()`` or ``eth_build_header()``,
  pass the ``ethertype`` as a standard C integer (e.g., ``0x0800``).
  The function handles the conversion to network-byte-order (big-endian)
  internally.
- **Direct Access:** When reading the ``ethhdr->ethertype`` field directly
  from a raw buffer, ``ntohs()`` must be used to convert it before comparison.


Receive Path (eth_input)
========================
When a packet is received from a netdev, it enters ``eth_input()``.
This function performs the following steps:

1. **Header Parsing:** Maps a ``struct ethhdr`` to the start of
   the netbuf.
2. **Metadata Tagging:**

   - Sets ``nb->mac_header``.
   - Identifies if the packet is destined for this host, a broadcast
     address, or a multicast group.

3. **Header Stripping:** Calls ``netbuf_pull()`` to move the data pointer
   past the 14-byte header.
4. **Dispatch:** Examines the EtherType and hands the packet off to
   the next layer:

   - ``ETH_P_IP`` → ``ip_input()``
   - ``ETH_P_ARP`` → ``arp_input()``


Transmit Path (eth_output)
==========================
To send data, the stack calls ``eth_output()``. This function does:

1. **Encapsulation:** Prepends the Ethernet header using
   ``eth_build_header()``.
2. **Source Addressing:** Automatically retrieves the MAC address of
   the outgoing ``netdev``.
3. **Padding:** If the frame is smaller than the minimum 60 bytes, it pads
   the buffer with zeros to ensure compatibility with all physical switches.
4. **Hand-off:** Passes the finalized buffer to ``netdev_send()``.


Addressing and Utilities
========================
eMMK provides several helper functions for managing MAC addresses:

- **Comparison:** ``eth_addr_equal()`` checks if two physical addresses
  match.
- **Type Checking:** ``eth_is_broadcast()``, ``eth_is_multicast()``, and
  ``eth_is_zero()``.
- **Formatting:** ``eth_addr_to_str()`` converts a 6-byte array into a
  human-readable string (e.g., ``00:11:22:33:44:55``).

.. warning::

  The ``ETH_FMT()`` macro uses an internal static buffer. Do not use it
  more than once in a single ``kprintf()`` call, as the second expansion will overwrite the first.


API Reference
=============

.. list-table::
   :header-rows: 1
   :widths: 30 50

   * - Function
     - Description
   * - ``eth_input(nb)``
     - Processes an incoming frame and dispatches to L3.
   * - ``eth_output(nb, dest, type)``
     - Wraps an L3 payload in an Ethernet frame and sends it.
   * - ``eth_build_header(...)``
     - Manually constructs a header at the start of a netbuf.
   * - ``eth_copy_addr(dst, src)``
     - Efficiently copies a 6-byte MAC address.


Reference
=========
- Digital Equipment Corp., Intel Corp., and Xerox Corp., "The Ethernet:
  Data Link Layer and Physical Layer Specifications," Version 2.0,
  Nov. 1982.


Related Files
=============
- ``include/ethernet.h``
- ``net/ethernet.c``

