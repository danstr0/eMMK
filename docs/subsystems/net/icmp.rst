========================================
ICMP (Internet Control Message Protocol)
========================================

Overview
========
ICMP is a companion protocol to IPv4. Unlike TCP or UDP, it is not typically
used for application data transport. Instead, it is used by the network stack
to communicate diagnostic information (such as "Ping") and error conditions
(such as "Port Unreachable").


Header Structure
================
An ICMP header is 8 bytes long. The first 4 bytes are fixed (Type, Code,
and Checksum), while the last 4 bytes vary depending on the message type.

The ``un`` union in ``struct icmphdr`` provides specific layouts for
different types:

- **Echo:** Used for Ping (contains an ID and Sequence number).
- **Frag:** Used for Path MTU Discovery (contains the next-hop MTU).
- **Gateway:** Used for Route Redirects.


Echo (Ping) Support
===================
eMMK supports the full "Ping" cycle:

1. **Echo Request (RX):** When an ``ICMP_ECHO_REQUEST`` is received via
   ``icmp_input()``, the kernel automatically generates a response.
2. **Echo Reply (TX):** The ``icmp_send_echo_reply()`` function reuses
   the incoming ``netbuf``. It swaps the IP addresses, changes the ICMP
   type to 0 (Reply), recomputes the checksum, and sends it back.
3. **Client Support:** Applications can use ``icmp_send_echo_request()`` to
   send a ping to a remote host.


Error Reporting
===============
ICMP is the primary way the kernel tells a remote host that something went
wrong. The most common error is Destination Unreachable (Type 3).

When ``icmp_send_dest_unreachable()`` is called, the kernel constructs a
new packet. Per RFC 792, this packet includes:

- The standard 8-byte ICMP header.
- The full IP header of the packet that caused the error.
- The first 8 bytes of the original packet's payload (usually containing
  the TCP / UDP ports).

This allows the remote host to identify exactly which socket or process
triggered the error.


Receive Path (icmp_input)
=========================
Dispatched from ``ip_input()`` when the protocol field is ``1``.

1. **Validation:** The kernel validates the ICMP checksum (calculated over
   the header and the entire payload).
2. **Echo Requests:** Handled automatically by the stack.
3. **Other Types:** Currently logged to the serial console for debugging
   and then dropped.


Statistics
==========
The kernel maintains a global ``icmp_statistics`` structure to track
network health. This includes counters for received / transmitted pings,
checksum errors, and unreachable notifications. These are retrievable via
``icmp_get_stats()``.


API Reference
=============

.. list-table::
   :header-rows: 1
   :widths: 30 50

   * - Function
     - Description
   * - **icmp_input(nb)**
     - Entry point for incoming ICMP packets.
   * - **icmp_send_echo_request(...)**
     - Sends a ping to a target IP.
   * - **icmp_send_dest_unreachable(nb, code)**
     - Sends an error response for a failed packet.
   * - **icmp_get_stats(&stats)**
     - Copies current counters into a local buffer.


Reference
=========
- J. Postel, "Internet Control Message Protocol," RFC 792, Internet
  Engineering Task Force (IETF), Sep. 1981. doi: 10.17487/RFC0792.


Related Files
=============
- ``include/icmp.h``
- ``net/icmp.c``

