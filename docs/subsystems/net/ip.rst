===========================
IPv4 (Internet Protocol v4)
===========================

Overview
========

The IPv4 layer is responsible for routing packets between hosts across
different network segments. It handles packet validation, header
construction, checksum calculation, and basic routing logic.

Header Structure
================

.. code-block :: text

   ┏━━━━━━━┯━━━━━━━┯━━━━━━━━━━━━━━━┯━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
   ┃Version│  IHL  │      TOS      │          Total Length         ┃
   ┠───────┴───────┴───────────────┼─────┬─────────────────────────┨
   ┃         Identification        │Flags│    Fragment Offset      ┃
   ┠───────────────┬───────────────┼─────┴─────────────────────────┨
   ┃      TTL      │   Protocol    │       Header Checksum         ┃
   ┠───────────────┴───────────────┴───────────────────────────────┨
   ┃                       Source Address                          ┃
   ┠───────────────────────────────────────────────────────────────┨
   ┃                    Destination Address                        ┃
   ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛

eMMK supports the standard IPv4 base header, but does not process optional
fields.

.. note::

  The ``ihl`` (header length) and ``version`` fields share a single byte.
  Because x86 is little-endian, these are defined using a conditional
  bitfield in ``iphdr`` to ensure the nibbles are in the correct order on
  the wire.

Receive Path (``ip_input``)
===========================

Incoming packets from ``eth_input()`` enter this stage. The kernel performs
several safety checks:

1. **Integrity:** Verifies the IPv4 version and validates the 16-bit header
   checksum.
2. **Addressing:** Checks if the packet is addressed to our IP, a broadcast
   address, or the loopback range. If not, the packet is dropped (eMMK does
   not act as a router / forwarder).
3. **Trimming:** Ethernet frames often contain padding to meet minimum size
   requirements. ``ip_input()`` uses the ``tot_len`` field to trim
   the ``netbuf`` to the exact size of the IP datagram.
4. **Dispatch:** Passes the payload to the appropriate Layer 4 handler
   (ICMP or UDP).

Transmit Path (``ip_output``)
=============================

When an application or protocol wants to send data:

1. **Encapsulation:** Prepends the IP header and calculates the checksum.
2. **Routing:** Calls ``ip_route()`` to find the interface and next-hop IP.
3. **ARP:** Calls ``arp_resolve()``.
    - If the MAC is known, the packet is sent immediately via
      ``eth_output()``.
    - If the MAC is unknown, the packet is queued by the ARP layer
      until a reply is received.

Checksum Calculation
====================

The IP header checksum is a 16-bit ones-complement sum. The function
``ip_checksum()`` provides a generic implementation used by the IP header
and as a building block for the UDP "Pseudo-header" checksums.


Reference
=========
- C. Hornig, "A Standard for the Transmission of IP Datagrams over
  Ethernet Networks," RFC 894, Internet Engineering Task Force (IETF),
  Apr. 1984.


Related Files
-------------

- `include/ip.h`
- `net/ip.c`

