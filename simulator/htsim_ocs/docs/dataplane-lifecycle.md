# Phase 04 packet data-plane lifetime

The static Phase 04 adapter consumes only an immutable, already validated
`OcsExecutionPlanV2`. Capacity preflight completes before construction of the
EventList, Pipe, or any other EventSource in the standalone driver.

```text
run_static_dataplane
  |
  +-- OcsCapacityProof
  +-- EventList
  +-- OcsTopology
       |
       +-- OcsPacketPool<OcsPacket>          (one per run)
       +-- OcsPacketPool<OcsTransitBatch>    (one per run)
       +-- OcsFlow[] + PacketFlow bridges    (run lifetime)
       +-- OcsPlaneDataplane[plane]
            |
            +-- OcsPortSerializer[src]
            +-- OcsSwitch
            +-- Pipe[dst]
            +-- OcsSink[dst]
            +-- OcsRouteTable
                 +-- Route[dst]
                      [OcsSwitch, Pipe[dst], OcsSink[dst]]
```

Topology destruction removes planes and their Routes before flows and the two
pools. Successful execution explicitly proves every serializer idle, every
in-flight count zero, and every pool object returned. Runtime guard failures
free the current packet or batch before propagating a stable
`OcsDataplaneError`; pool destruction asserts that no live object remains.

## Handoff sequence

```text
serializer completion
  -> Packet::sendOn() to OcsSwitch
  -> validate plane / program epoch / configuration / physical generation
  -> checked(now + data_latency_ps)
  -> increment plane in-flight
  -> Packet::sendOn() to Pipe
  -> Pipe delay callback
  -> Packet::sendOn() to OcsSink
  -> validate destination and continuous flow byte range
  -> receiver-complete callback when exact final byte arrives
  -> decrement plane in-flight
  -> Packet::free() to its run-owned checked pool
```

`full_packet` emits one transit object per exact wire packet.
`exact_coalesced` emits one `OcsTransitBatch` per flow, but that batch traverses
the same Route and Pipe. Its base `Packet::_size` is only the exact wire tail;
uint64 logical bytes and packet count remain authoritative OCS metadata.

The declared equivalence scope is flow/group completion input, receiver last
byte time, payload and logical packet counts, serializer busy intervals, route
guards and drain time. Per-packet arrival traces, instantaneous Pipe occupancy
and allocator counts are intentionally outside that scope.
