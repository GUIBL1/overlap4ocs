# Standalone HTSim OCS backend readiness

Status: Phase 06 acceptance passed in the retained development workspace.

## Frozen identity

| Item | Value |
|---|---|
| backend version | `0.5.0` |
| Plan schema | `swot-execution-plan/v2`, SHA-256 `b37ce8b5a87127b84967d990bd0055ef0c6cd5b5be20139e0fef05aac3f17d78` |
| Result schema | `swot-simulation-result/v2`, SHA-256 `857e2d8d9f91bd17fd140326b832a542e95f323cd243a6bb986ef84ef1390a86` |
| operation schema | `htsim-ocs-operation-event/v1`, SHA-256 `5b3d2424233220ce28669afa1f912a4b89b700930ba2c2180dd8f66e8e98277d` |
| HTSim upstream | `841d9e7be46bb968eece766aa4b6c044c7799f67` |
| local patchset | `51bece963136736a2528ddad36924d022147ebe80933d58b3307e04ef5d15d37` |
| build flags | `0e3469e20cdca5e487fad86c1c237f22c8bcc7334e26a21f4457e95e4181bbda` |
| ABI limits source | SHA-256 `e709ee450e6a57323740ae547b3ccfbc69f1e397ea6116afc0cfb658859103c2` |

Parent commit/compiler/build-state fields remain build-specific and are checked
against `capabilities --json`, not frozen into cross-commit projections.

## Static corpus and determinism

The backend-owned corpus contains 13 runtime fixtures (26 full/coalesced plans)
and 3 exact-coalesced performance fixtures. Its delivery manifest is
`tests/artifacts/phase06/delivery-manifest.json`, SHA-256
`e7c53e5c0bc3d529dfef98ef54a52df7278daaf43032a63bf6786ab99fe39f02`.
It records every manifest, plan, stable projection, trace, and README digest.

Representative raw goldens:

- exact-tail full-packet plan: `f5c898fe17d1b09b1b3a8cabb765a3be20f1824932df6994a91db809950cb9f6`;
- its stable result projection: `13c098f9a4ad2d1230f5b6b081eba264415bb76d463462fde87716ef52864faa`;
- Fig.5 SWOT full trace: `d2feb6cbb834cd46c1472145fe1d2acf0189a13fb2366137136289b2f9bef9c1`;
- Fig.5 Strawman full trace: `b8dff153e8270a84ea31c3e5810f597b6074fd1c134d161d790205c7fdb38665`.

One hundred fresh processes produced byte-identical success result/trace,
failure result/empty trace, and capabilities output. Every semantic case first
passes plan/inventory/dependency audit, then Result/trace schema and
conservation checks, timing, golden projection, and full/exact ps equivalence.

## Fig.5 evidence

The hand-authored decimal-byte fixture fixes p=8, k=2, 40,000,000 bytes,
400 Gbps per plane, 200 us reconfiguration, and zero data latency.

- SWOT uses `(15,5),(0,10),(5,0),(5,0),(0,10),(15,5)` MB per-plane splits,
  plane-0 `P1→P3→retain P3→P1`, and plane-1
  `P1→P2→retain P2→P1`; CCT is exactly 1,200,000,000 ps.
- Strawman splits every step evenly and uses
  `P1→P2→P3→retain P3→P2→P1` on both planes; 700 us transfer plus four
  200 us transitions gives exactly 1,500,000,000 ps.

Trace assertions cover P1/P2/P3, bypass/retain, independent plane progress,
P1/P2 serial-resource guards, and Step 6's previous-step data guard.

## Failure and atomicity

Acceptance covers malformed JSON, schema/permutation errors, plan/result size
limits, conservative Pipe capacity rejection, max-time/max-event snapshots,
suppressed and duplicate receiver callbacks, unwritable parents, existing
outputs, trace/result temp creation and no-replace rename failures, and SIGINT.
All known failures use the fixed exit/status taxonomy and have null CCT.
Trace commits precede result; an injected trace failure never publishes a
success result. Existing formal paths remain byte-for-byte unchanged.

## Performance evidence

Budget: 60 s and 1 GiB peak RSS per case. Evidence file:
`tests/artifacts/phase06/performance-evidence.json`, generated with
`make phase06-performance-evidence` using `/usr/bin/time -v` on Linux 6.8,
Intel Xeon Gold 6330. Normal `make test` executes the same gates without
rewriting the reviewed evidence file.

| Fixture | Events | Transit units | Wall time | Peak RSS |
|---|---:|---:|---:|---:|
| 5 GiB, p2/k1 | 2 | 1 | 0.006 s | 3,584 KiB |
| 32 program epochs | 279 | 128 | 0.018 s | 4,480 KiB |
| p256/k8, 512 MB/rank | 4,096 | 2,048 | 0.128 s | 17,920 KiB |

The 5 GiB logical flow represents 3,579,140 packets with one coalesced transit
unit. The p256 fixture has complete permutations, 2,048 explicit flows, and
exactly 512,000,000 sent bytes per rank. Result-size preflight is linear in
ports plus flows rather than their Cartesian product.

## Supported boundary

Supported: `paper_exact`, `full_packet`, `exact_coalesced`, both contracted
dependency modes, all three path policies, exact-tail payloads, independent
plane serialization, initial/retain/reconfigure epochs, and structured
runtime-limit/invariant snapshots. Stable unsupported results cover a result
too large for the ABI, unsupported execution preconditions, and an upstream
Pipe capacity proof violation. No fallback, analytical CCT, hidden collective
generation, shared NIC arbiter, retransmission, or congestion protocol is
present.

The future Python adapter may only write a deterministic plan, invoke this CLI,
and validate/read the formal result. It must not link backend internals, parse
CCT from stdout, reinterpret scheduling, or fall back to a mathematical model.
