# HTSim OCS minimal link closure

The standalone `htsim_ocs` executable links an explicit source/object
allowlist. The Makefile uses no source glob, recursive HTSim build, upstream
archive, or system protocol library.

## Object allowlist

Vendored HTSim core:

```text
eventlist.o
network.o
route.o
pipe.o
trigger.o
```

Project overlay:

```text
htsim_logged_shim.o
sha256.o
ocs_execution_plan.o
ocs_plan_parser.o
ocs_packet_flow_bridge.o
ocs_packet.o
ocs_flow.o
ocs_execution_mode.o
ocs_switch.o
ocs_sink.o
ocs_route_table.o
ocs_port_serializer.o
ocs_plane_dataplane.o
ocs_topology.o
main_ocs.o
```

The Phase 4 objects implement the project-owned finite packet data plane; they
add no vendored HTSim core translation unit. The link-closure probe replaces
the parser/data-plane/CLI objects with
`test_link_closure.o` and proves one Packet traverses `Route -> Pipe -> sink`
and is freed exactly once.

The following domains are forbidden in the executable and link probe:

```text
config.o loggers.o logfile.o queue.o switch.o
tcp ndp eqds roce hpcc fat-tree generic-topology
```

`htsim_logged_shim.cpp` owns only
`LoggedManager::{LoggedManager,add_logged,dump_idmap}` and
`Logged::_logged_manager`. `network.cpp` continues to own `Logged::LASTIDNUM`,
Packet, and PacketFlow. `config.cpp` is excluded; all OCS units and arithmetic
are checked integers.

The static data-plane Route is always
`OcsSwitch -> Pipe(destination) -> OcsSink(destination)`. It does not call
`Pipe::setNext()`. Each plane owns separate source serializers, switch, Pipes,
sinks, routes and in-flight counters while sharing only the EventList clock.

## Reproducible evidence

The actual command and object list are preserved in
`build/htsim_ocs.map` and `build/test_link_closure.map`. Run:

```bash
make -C simulator/htsim_ocs clean all test-link-closure
make -C simulator/htsim_ocs check-link-closure
```

`check-link-closure` scans both maps and undefined symbols, rejects forbidden
objects/domains, verifies the manifest digests, and checks the CLI contract.
The generated `build/generated/generated_contract.h` contains only schema IDs,
raw schema/ABI digests, the six ABI limits, and the build-flags digest. It does
not duplicate schema content.
