# overlap4ocs local HTSim patch ledger

Upstream base: `841d9e7be46bb968eece766aa4b6c044c7799f67`.

`LOCAL_PATCHSET.json` is canonical UTF-8 JSON with one trailing LF. Its raw
SHA-256 is the only local patchset identifier embedded in `htsim_ocs`.

## HTSIM-P001: deterministic and observable EventList

- Files: `sim/eventlist.h`, `sim/eventlist.cpp`.
- Reason: upstream uses equivalent `multimap` keys without an explicit
  tie-break and dispatches immediate triggers LIFO. The OCS runtime requires a
  replayable order and must inspect the next callback before applying inclusive
  simulation-time and event-count limits.
- Semantic effect: timed events use `(time_ps, insertion_order)`; triggers are
  FIFO and precede timed callbacks; callback insertion at the current time is
  ordered after already-pending callbacks. Checked uint64 insertion/dispatch
  counters and read-only pending/next-time queries are exposed. Existing
  end-time, cancel-by-source, cancel-by-time, cancel-by-handle, and reschedule
  behavior is retained.
- Affected symbols: `EventList::Handle`, `_pendingsources`,
  `_pending_triggers`, `doNextEvent`, all scheduling/cancel paths, and the new
  observation methods.
- Tests: `phase03.eventlist_order`,
  `phase03.eventlist_100_process_determinism`, and
  `phase03.handle_callers_compile`.
- Parent commit: pending reviewer approval; intentionally uncommitted.
- Upstream-update status: local; must be rebased and revalidated on an upstream
  update.

## HTSIM-P002: startup-safe Logged registry

- File: `sim/loggertypes.h` only. `network.cpp` is deliberately unchanged.
- Reason: `network.cpp` constructs `Packet::_defaultFlow` during dynamic
  initialization, while the project overlay defines `Logged::_logged_manager`
  in another translation unit. The C++ standard does not order those dynamic
  initializations across translation units. A shim-only or link-order solution
  therefore cannot prove safety.
- Semantic effect: `Logged` construction and `dump_idmap()` use one
  function-local `LoggedManager`, whose initialization is sequenced before its
  first use. The upstream `_logged_manager` member is retained solely for source
  compatibility with `loggers.cpp`; OCS registration does not depend on its
  initialization order.
- Affected symbols: inline `Logged::Logged`, `Logged::dump_idmap`, and the new
  private inline `Logged::logged_manager`.
- Tests: `phase03.link_closure` and 100 fresh `version + link-probe` processes
  built with ASan/UBSan (`phase03.startup_sanitizer_100`). LeakSanitizer is
  disabled because it is unsupported under the execution harness ptrace;
  AddressSanitizer and UndefinedBehaviorSanitizer remain enabled and fail-fast.
- Parent commit: pending reviewer approval; intentionally uncommitted.
- Upstream-update status: local; must be rebased and revalidated on an upstream
  update.

Project-owned integration code under `simulator/htsim_ocs/` remains an overlay,
not a vendored-core patch.
