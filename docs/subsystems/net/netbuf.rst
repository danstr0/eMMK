========================
Network Buffers (netbuf)
========================

Overview
========
The netbuf is eMMK's fundamental packet container, heavily inspired by
the Linux kernel's ``sk_buff``. Instead of copying data between protocol
layers, the netbuf uses a set of four pointers to "slide" the active data
window within a larger pre-allocated memory region.

This design enables zero-copy header manipulation:

- **Encapsulation (TX):** Prepend headers (like IP or Ethernet) by moving
  a pointer backward into reserved "headroom."
- **Decapsulation (RX):** Strip headers by moving a pointer forward,
  "pulling" the header off the packet.


Memory Geometry
===============
A ``netbuf`` manages a contiguous block of memory. The physical buffer is
bounded by ``head`` and ``end``, while the actual packet data is bounded by
``data`` and ``tail``.

- **Headroom:** Space between ``head`` and ``data``. Used for prepending
  headers.
- **Payload:** Space between ``data`` and ``tail``. This is the current
  packet data.
- **Tailroom:** Space between ``tail`` and ``end``. Used for appending
  data or padding.


Pointer Operations
==================
The API provides several inline functions to manipulate these pointers
safely:

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Function
     - Direction
     - Description
   * - ``netbuf_reserve``
     - Forward
     - Moves ``data`` and ``tail`` forward to create headroom. Call this
       before adding TX data.
   * - ``netbuf_put``
     - Tailward
     - Extends ``tail`` to add data to the end of the packet (e.g., adding
       a payload).
   * - ``netbuf_push``
     - Headward
     - Moves ``data`` backward into the headroom to prepend a header (e.g.,
       adding an IP header).
   * - ``netbuf_pull``
     - Tailward
     - Moves ``data`` forward to "consume" a header (e.g., stripping the
       Ethernet header).
   * - ``netbuf_trim``
     - Headward
     - Moves ``tail`` backward to remove ``data`` from the end (e.g.,
       removing Ethernet padding).


Metadata and Parsing
====================
As a ``netbuf`` travels through the stack, parsing logic populates metadata
fields so subsequent layers don't have to re-parse the packet:

- **Layer Pointers:** ``mac_header``, ``network_header``, and
  ``transport_header`` store the starting addresses of each protocol layer.
- **Protocol Info:** Fields like ``protocol`` (L3 type), ``ip_proto`` (L4
  type), and ``pkt_type`` (Unicast / Broadcast) are used for routing and
  dispatching.
- **Addressing:** Pre-parsed IP addresses (``src_ip``, ``dst_ip``) and ports
  are cached in the struct for quick access.


Lifecycle Management
====================
1. **Allocation:** ``netbuf_alloc()`` retrieves a buffer from the kernel's
   slab allocator. It initializes a reference count of 1.
2. **Reference Counting:** Use ``netbuf_copy()`` to create a deep copy or
   manually manage references if a buffer needs to be shared (e.g., for
   TCP retransmission).
3. **Release:** ``netbuf_free()`` decrements the reference count. The memory
   is returned to the slab only when the count reaches zero.


Queuing
=======
The ``netbuf_queue`` provides a simple, non-thread-safe FIFO. In eMMK's
run-to-completion model, these queues are used to pass packets between
the NIC driver and the stack's entry point or to buffer packets waiting
for ARP resolution.

.. code-block:: c

  struct netbuf_queue rx_queue;
  netbuf_queue_init(&rx_queue);

  // When a packet arrives:
  netbuf_queue_push(&rx_queue, nb);

  // In the main loop:
  while ((nb = netbuf_queue_pop(&rx_queue)))
  {
      net_input(nb);
  }


Related Files
-------------
- ``include/netbuf.h``
- ``kernel/netbuf.c``

