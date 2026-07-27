# overlap4ocs local HTSim patch ledger

Upstream base: `841d9e7be46bb968eece766aa4b6c044c7799f67`.

There are currently no modifications to vendored csg-htsim source files.
Accordingly, `LOCAL_PATCHSET.json` contains an empty, canonical `patches`
array. Its exact raw SHA-256 digest is the local patchset identifier embedded
in the `htsim_ocs` binary.

Project-owned integration code under `simulator/htsim_ocs/` is an overlay and
is not a vendored-core patch. If a future core patch is unavoidable, give it a
stable ID such as `HTSIM-P001` and record its parent commit, files, reason,
semantic effect, tests, and upstream-update status here and in the machine
manifest.
