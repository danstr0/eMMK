=================================
ARP (Address Resolution Protocol)
=================================

Overview
========
ARP is used to discover the hardware (MAC) address associated with a given
IPv4 address on the local network segment. Since Ethernet hardware cannot
understand IP addresses, the ARP layer maintains a mapping table (the ARP
Cache) to bridge the two.


Packet Format
=============
The ARP header is 28 bytes long. eMMK specifically implements ARP for
Ethernet hardware and IPv4 protocols.

Unlike the ethernet header, the ARP API does not perform automatic
byte-swapping for structure members.

- **Reading:** Use ``ntohs()`` / ``ntohl()`` on header fields.
- **Writing:** Use ``htons()`` / ``htonl()`` when filling the header for
  transmission.


The ARP Cache
=============
The system maintains a global cache of up to 64 entries (``ARP_CACHE_SIZE``).
Each entry exists in one of four states:

- **FREE:** The slot is empty and available for a new mapping.
- **PENDING:** An ARP Request has been broadcast, but no reply has been
  received yet. Outgoing packets for this IP are stored in the entry's
  pending queue.
- **RESOLVED:** A valid MAC address is known. Packets can be sent immediately.
- **STATIC:** A manually configured entry that never expires (e.g., for a
  gateway or a known server).


Resolution Flow
===============
When the IP layer attempts to send a packet to a local destination, it
calls ``arp_resolve()``:

- **Lookup:** The cache is checked for the target IP.
- **Hit (Resolved):** The MAC address is returned immediately.
- **Miss (Pending):**

  - An ARP Request is broadcast to the network.
  - The packet is placed into a pending queue (up to 8 packets per IP).
  - When the ARP Reply arrives, the MAC is saved, and all queued packets
    are automatically transmitted.


Receive Path (arp_input)
========================
The ``arp_input()`` function handles incoming ARP traffic dispatched from
``eth_input()``:

- **Requests:** If the "Target IP" matches our device's IP, the kernel
  generates an ARP Reply and sends it directly back to the requester's MAC
  address.
- **Replies:** If the reply matches a PENDING entry in our cache, the entry
  is upgraded to RESOLVED, and the pending packet queue is flushed to
  the hardware.


Cache Maintenance
=================
To prevent the cache from becoming stale (e.g., if a neighbor changes their
NIC), entries have a lifetime of 300 seconds (``ARP_CACHE_TIMEOUT``).

The kernel does not have a background thread for this; instead, the main
loop or a timer must periodically call ``arp_cache_expire()``. This function:

- Removes entries older than 5 minutes.
- Drops packets for PENDING requests that never received a response.


API Reference
=============

.. list-table::
   :header-rows: 1
   :widths: 30 50

   * - Function
     - Description
   * - ``arp_resolve(dev, ip, mac)``
     - The primary entry point for L3. Resolves an IP or initiates a request.
   * - ``arp_input(nb)``
     - Processes incoming ARP packets.
   * - ``arp_add_static(ip, mac, dev)``
     - Manually inserts a permanent mapping into the cache.
   * - ``arp_cache_expire()``
     - Performs house-cleaning on the cache. Should be called every ~30s.
   * - ``arp_cache_dump()``
     - Prints the current state of the ARP table to the serial console.


Reference
=========
- D. Plummer, "An Ethernet Address Resolution Protocol," RFC 826, Internet
  Engineering Task Force (IETF), Nov. 1982.

Related Files
=============
- ``include/arp.h``
- ``net/arp.c``

