============================
UDP (User Datagram Protocol)
============================

Overview
========

The UDP module provides lightweight, connectionless, unreliable transport
layer messaging with port-based demultiplexing over IPv4.

Characteristics
---------------
- **Zero-Copy RX/TX:** The receive path strips headers in-place and passes
  the underlying ``netbuf`` directly to upper-layer callbacks. The
  ``udp_output()`` interface accepts pre-reserved ``netbuf`` structures
  to avoid payload copying on transmission.
- **Callback-Driven Demultiplexing:** Static binding table
  (``bind_table``) mapping 16-bit host ports to ``udp_recv_func_t``
  callbacks.
- **RFC 768 Compliance:** Full pseudo-header checksum verification on RX
  and computation on TX, including ``0x0000`` → ``0xFFFF`` checksum
  inversion for transmitted packets and ``0x0000`` checksum skip on
  reception.
- **ICMP Integration:** Automatic generation of ICMP Port Unreachable
  messages for datagrams arriving on unbound ports.

Header Structure
================

.. code-block:: text

   ┏━━━━━━━━┳━━━━━━━━━┳━━━━━━━━━━━━━━━━━━━━┓
   ┃ Offset ┃ Size    ┃ Field              ┃
   ┣━━━━━━━━╇━━━━━━━━━╇━━━━━━━━━━━━━━━━━━━━┫
   ┃ 0      │ 16 bits │ Source Port        ┃
   ┠────────┼─────────┼────────────────────┨
   ┃ 2      │ 16 bits │ Destination Port   ┃
   ┠────────┼─────────┼────────────────────┨
   ┃ 4      │ 16 bits │ Length             ┃
   ┠────────┼─────────┼────────────────────┨
   ┃ 6      │ 16 bits │ Checksum           ┃
   ┗━━━━━━━━┷━━━━━━━━━┷━━━━━━━━━━━━━━━━━━━━┛


Processing Pipeline
===================

.. code-block:: text

                  [ Incoming IPv4 Packet ]
                              │
                              ▼
                         udp_input()
                              │
               ┌──────────────┴──────────────┐
         [len < 8 or                 [Valid Header]
        len > nb->len]                       │
               │                             ▼
               ▼                      Verify Checksum
          netbuf_free()        ┌─────────────┴─────────────┐
               │               │                           │
             (Drop)        [Invalid]                [Valid / 0x0000]
                               │                           │
                               ▼                           ▼
                          netbuf_free()               Strip Header
                               |                      (netbuf_pull)
                             (Drop)                        │
                                                     Lookup dst_port
                                                           │
                                             ┌─────────────┴─────────────┐
                                          [Found]                    [Not Found]
                                             │                           │
                                             ▼                           ▼
                                  Invoke Callback                Push Header Back
                                  (Handler frees nb)                     │
                                                             icmp_send_dest_unreachable()
                                                                         │
                                                                   netbuf_free()

Reference
=========
- J. Postel, "User Datagram Protocol," RFC 768, Internet Engineering Task
  Force (IETF), Aug. 1980. doi: 10.17487/RFC0768.

  
Related Files
=============
- ``include/udp.h``
- ``net/udp.c``

