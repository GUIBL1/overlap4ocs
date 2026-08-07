# HTSim OCS Phase 01–06 阶段测试报告

## 1. 测试结论

状态：`PASS`

Phase 01–05 的实现不是只定义 JSON 字段，而是已经形成可执行的 OCS
runtime。多平面独立重构已由源码路径和本次 operation trace 同时证明：

- `100000000..300000000 ps`：plane 1 执行重构，plane 0 的 group 0 仍在传输；
- `300000000..500000000 ps`：plane 0 执行重构，plane 1 的 group 2 同时传输。

三份 production-shaped Plan v2 均由正式 CLI 成功处理并生成 canonical Result
v2 和 operation JSONL。与论文 Fig.5 同 workload 的 `k=2` 强制 one-shot 输入
被稳定拒绝，failure Result 的 CCT 为 null，没有 fallback 或伪成功。

## 2. Phase 01–05 修改扫描

| Phase | commit | 后端能力 |
|---|---|---|
| 01 | `beb9767` | 独立 HTSim OCS 构建、显式最小链接闭包、vendor/upstream/patchset identity |
| 02 | `b93bd19` | 冻结 Plan v2、Result v2、operation event schema、ABI limits 和跨语言验证器 |
| 03 | `b2d0ce7` | strict/bounded C++ parser、raw SHA-256、deterministic EventList、capabilities、link closure |
| 04 | `c2ddfa7` | 真实 `Packet`/exact-coalesced 数据面、三跳 route、每 `(plane,src)` serializer、Pipe/sink、精确 tail 和 pool audit |
| 05 | `017a5a5` | 独立 plane runtime、program epoch/physical generation、重构 timer、token/group/step、三种 path policy、watchdog 和 blocked snapshot |

Phase 06 在上述 runtime 外增加正式 `run` CLI、Result/trace writer 和原子提交，
不改变 Phase 05 的调度 guard。

## 3. 多平面独立重构的实现证据

- `OcsCoordinator` 为每个 `plane_program` 分配一个 `OcsPlaneRuntime`；每个对象有
  独立 program cursor 和 `OcsReconfigurationTimer`。
- `OcsPlaneRuntime` 分别维护 program epoch 与 physical generation；只有
  `reconfigure` 增加 generation，`retain` 只推进 epoch。
- `OcsPlaneDataplane` 为每个 plane 独立创建 switch、destination Pipes/sinks、
  source serializers 和 in-flight counter。plane 之间只共享 EventList 时钟。
- `begin_current_epoch()` 只检查目标 plane 的 `plane_data_plane_idle()`；不会等待
  其他 plane drain。
- coordinator 的 fixed-point 顺序逐 plane 关闭 epoch、准备 path、更新 lockstep
  gate、释放 group；没有全局 reconfiguration lock。
- 真 cutover 前必须满足本 plane 的 group receiver-complete、serializer empty 和
  in-flight zero；switch 同时校验 plane/program epoch/configuration/generation。
- 成功条件要求所有 expected payload 在 receiver 收齐、所有 plane program 完成、
  route exact、pool returned 和 in-flight 为零。

## 4. SWOT 论文所需后端能力

| 能力 | 状态 | 本次证据 |
|---|---|---|
| 第一次配置免费预装 | 已实现 | 所有 initial epoch 在 `t=0` ready，generation 0 |
| 真重构按 `T_reconf` 收费 | 已实现 | Strawman/SWOT 每个 reconfigure 窗口精确 200000000 ps |
| retain 不收费但推进 program | 已实现 | 两个 Fig.5 case 均有 2 个 retain；generation 不变 |
| 多 plane 独立推进/重构 | 已实现 | SWOT 的两段交叠窗口 |
| plane drain 后原子 cutover | 已实现 | 所有 success invariant 为 true，无同 plane transfer/reconfig overlap |
| Strawman 双 lockstep gate | 已实现 | path prep 等前一步完成，transfer 等全部本步 path ready |
| SWOT early path preparation | 已实现 | plane 1 在 step 0 全局完成前 200 us 开始并完成重构 |
| global barrier / explicit group DAG | 已实现 | token tracker 只消费 plan 显式 parent；本次使用 global barrier |
| permutation 与 sparse traffic 分离 | 已实现 | 每个 flow 都逐项验证 `permutation[src]=dst`，无配置推导伪流 |
| receiver-last-byte CCT | 已实现 | Result completion、flow/group/step/token 均由 receiver callback 推进 |
| `full_packet` / `exact_coalesced` | 已实现 | 本次论文规模用 exact-coalesced；Phase 04/06 已有逐 ps 等价门 |
| deterministic trace/result | 已实现 | 三 case 第二次运行与保留输出逐字一致 |
| failure/limit/deadlock/invariant snapshot | 已实现 | 本次 invalid one-shot 产生结构化 failure、null CCT |

不属于当前后端职责、仍需 Phase 07+ 的功能：collective IR、算法 rank/chunk
ownership 证明、planner/solver、one-shot matching/unsupported certificate、Plan lowerer、
Python subprocess adapter 和 legacy result projection。后端不会从 strategy 名称生成
traffic，也不会验证显式 flows 是否完整实现某个 AllReduce；该证明必须在上游 IR/lowerer。

paper mode 也明确不模拟丢包、ACK、重传、PFC、共享 NIC arbiter 或拥塞协议；当前
模型是 lossless payload-only、每 plane 独立 endpoint rate 和一个 forward Pipe latency。

## 5. 手写输入矩阵与公平性

共同 workload：`p=8`、每 rank 原始消息 `40,000,000 B`，六步每 rank 发送量为
`[20,10,5,5,10,20] MB`，总计 `70,000,000 B`，等于
`2(p-1)/p*m`。三个 success case 均为总发送/接收 `560,000,000 B`，各 step
总字节为 `[160,80,40,40,80,160] MB`，`400 Gbps/plane`、`T_reconf=200 us`、
data latency 0。

| 输入 | strategy/policy | k | flows/groups | plane program |
|---|---|---:|---:|---|
| `one-shot-static.json` | one_shot/static_preinstalled | 3 | 96/12 | P1、P2、P3 各占一个永久 initial plane；0 次重构 |
| `fig5-stepwise-strawman.json` | baseline/step_lockstep | 2 | 96/12 | 每 plane `initial + 4 reconfigure + 1 retain` |
| `fig5-swot-overlap.json` | swot/overlap_earliest | 2 | 64/8 | 每 plane `initial + 2 reconfigure + 1 retain` |
| `fig5-forced-one-shot-infeasible.json` | one_shot/static_preinstalled | 2 | 64/8 | 非法地保留 3 configs/multi-epoch，预期失败 |

one-shot 与另外两行不是同资源公平比较：Fig.5 需要 P1/P2/P3 三种配置，D13
规定 one-shot 至少要一 plane/required configuration，因此最小可行 `k=3`。
Strawman 与 SWOT 同为 `k=2`，可以直接比较。强行把同一 workload 做成 `k=2`
one-shot 不允许用重构或 fallback 补救。

四份文件采用 `source_kind=production` 的完整 provenance、decision digest 和覆盖
全部 group 的 nominal schedule，用于测试正式输入形态；它们仍是人工阶段测试，
不冒充尚未实现的 Phase 07 lowerer 产物。

## 6. 仿真结果

| Case | status | CCT | events | reconfigure/retain | per-plane bytes |
|---|---|---:|---:|---:|---|
| one-shot static k3 | success | 1400000000 ps = 1.4 ms | 192 | 0/0 | 320/160/80 MB |
| Fig.5 Strawman k2 | success | 1500000000 ps = 1.5 ms | 200 | 8/2 | 280/280 MB |
| Fig.5 SWOT k2 | success | 1200000000 ps = 1.2 ms | 132 | 4/2 | 320/240 MB |
| forced Fig.5 one-shot k2 | failure | null | pre-runtime | n/a | n/a |

Sanity oracle：

- one-shot：六步都只使用预装该 configuration 的一个 400 Gbps plane；
  `400+200+100+100+200+400 us = 1.4 ms`，无重构事件。
- Strawman：两 plane 每步均分，数据时间 `700 us`；P1→P2→P3→retain
  P3→P2→P1 含四个收费 transition，`700+4*200=1500 us`。
- SWOT：结果与论文 fixture 的整数 oracle 一致，为 `1200 us`；trace 证明
  early preparation、P1/P2/P3、retain/bypass 和独立 plane 推进。
- forced one-shot：CLI `validate` 精确报告 `one_shot_not_static`；formal failure
  Result 使用契约层 `schema_validation_error`、pointer
  `/plane_programs/0/epochs`、exit 2、空 trace、null CCT。

三个 success Result 均满足：96/96/64 expected flows 全完成、expected/sent/
received 均为 560 MB、每 rank sent=received=70 MB、drop/duplicate/missing/
in-flight 全为 0、12 个 mandatory invariants 全为 true。

## 7. 验证命令

均在 `conda activate ocs` 下执行：

```bash
build/htsim_ocs validate --plan <input.json>
build/htsim_ocs run --plan <input.json> --result <result.json> --trace <trace.jsonl>
PYTHONPATH=../.. python <inline contract/traffic/program audit>
```

Result 使用 Python contract validator 完成 schema、status/nullability、aggregate/detail
校验；每行 trace 使用 operation-event validator；同时检查 canonical raw bytes、raw
plan SHA、event index 连续、时间单调和第二次运行 byte determinism。全部通过。

## 8. Artifact SHA-256

| Artifact | SHA-256 |
|---|---|
| `inputs/one-shot-static.json` | `11915632173950cc4e6ff08c68f11d902c39e095035b4209d4bb3075f2d34953` |
| `inputs/fig5-stepwise-strawman.json` | `9815ed0e3748d57eb1a2bd1880c210f78c89af8d5e75062396f563cf746a5386` |
| `inputs/fig5-swot-overlap.json` | `0d50d83875db5ce83d758b51b1802e577f5a8da849402253565485daab1b82e3` |
| `inputs/fig5-forced-one-shot-infeasible.json` | `5b1bbb3ccaf9457d8db99382db94902e7f31cd067c5663d982375924961202e7` |
| `outputs/one-shot-static.result.json` | `82cf7b0bb760008760a6a90c2ff315822aace300e34c94d4c556395826c4a951` |
| `outputs/one-shot-static.operations.jsonl` | `6ec0b4561a148bbdf017ea8704d126dbb1dd9837fb324411db66ea728fbf202a` |
| `outputs/fig5-stepwise-strawman.result.json` | `cd63e11810a725d26f3c5be1a6d874e09574e25bb9ef08e0d1f8d84b7bd48553` |
| `outputs/fig5-stepwise-strawman.operations.jsonl` | `b8dff153e8270a84ea31c3e5810f597b6074fd1c134d161d790205c7fdb38665` |
| `outputs/fig5-swot-overlap.result.json` | `a219cb4090c6ee7c2d4917135ba0b4c0eb0142db1eff1a76428aa9397455153f` |
| `outputs/fig5-swot-overlap.operations.jsonl` | `6aea2d022b26bf64567ccf9f3afd3b76c8cb9e9d1af54f5080d08e1806d078cd` |
| `outputs/fig5-forced-one-shot-infeasible.result.json` | `77d7e102e5e889e1f3e73224e4a914a52007ef35160c19393c567ab9842b0dda` |
| `outputs/fig5-forced-one-shot-infeasible.operations.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |

这些文件受仓库现有 `tests/` ignore 规则覆盖，未修改 `.gitignore`。
