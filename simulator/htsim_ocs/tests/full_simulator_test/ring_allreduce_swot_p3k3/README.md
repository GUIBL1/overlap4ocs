# ring_allreduce_swot_p3k3 — 三节点三平面重构与传输交叠测试用例

本目录是用 `tools/generate_swot_overlap_plan.py` 生成的 SWOT 交叠测试夹具（`strategy = swot` / `path_preparation_policy = overlap_earliest`），结构参照论文 Fig.5 fixture（`tests/fixtures/v2/runtime/paper_fig5_swot`）：**不同平面完成传输的时刻错开，先完成的平面立即开始重构，重构时间被其他平面的传输掩盖**。

## 流量结构

3 节点、3 平面、每平面 400 Gb/s、传播时延 20 ps、重构时延 2000 ps（放大以便在时序上清楚看到交叠）。

- **配置 0** `[1,2,0]`：正向环（0→1、1→2、2→0），用于 reduce-scatter；
- **配置 1** `[2,0,1]`：反向环（0→2、1→0、2→1），用于 allgather（NCCL 式换向）；
- 4 个逻辑步（`global_step_barrier` 串行数据依赖），每步字节为规划器分配（不等块，同 paper_fig5_swot）：

| 步 | 相位 | 配置 | 平面 0 | 平面 1 | 平面 2 |
|---|---|---|---|---|---|
| 0 | reduce_scatter_0 | 0 | 0→1 1000 B | 1→2 1000 B | 2→0 1000 B |
| 1 | reduce_scatter_1 | 0 | 1→2 1000 B + 2→0 1000 B | 0→1 4000 B | — |
| 2 | allgather_0 | 1 | 0→2 2000 B + 2→1 2000 B | — | 1→0 2000 B |
| 3 | allgather_1 | 1 | 0→2 1000 B + 2→1 1000 B | — | 1→0 1000 B |

同一平面上同一步的多条流来自不同源端口，并行传输；交叠效果来自**各平面在步内的工作量不相等**。

## 重构交叠时序（仿真结果，`result-plan-full-packet.json`）

| 平面 | 程序 | 关键时间（ps） |
|---|---|---|
| 0 | ep0 initial cfg0 [g0,g3] → ep1 reconfigure cfg1 [g5] → ep2 retain [g7] | ep0 排空 40040；**重构 40040→42040**；ep1 关闭 140060 |
| 1 | ep0 initial cfg0 [g1,g4]（无 cfg1 流量，不需重构） | ep0 排空 100040，程序结束 |
| 2 | ep0 initial cfg0 [g2] → ep1 reconfigure cfg1 [g6,g8] | ep0 排空 20020；**重构 20020→22020**；ep1 关闭 160080 |

交叠效果：

- 平面 0、2 的重构（40040→42040、20020→22020）都发生在平面 1 的 4000 B 传输窗口（20020→100040）之内；
- 两步 allgather 在屏障（100040）到来时路径早已就绪，释放不被重构拖慢；
- 最终 CCT = **160080 ps**：各步完成 20020 / 100040 / 140060 / 160080，两种执行模式一致；
- 对比：同样的流量在 `step_lockstep`（baseline）下，平面 0 的重构要等步 1 屏障后才开始（100040→102040），CCT = 162080。SWOT 通过交叠省下临界路径上的 2000 ps 重构时延。

## 文件与运行

| 文件 | 说明 |
| --- | --- |
| `plan-full-packet.json` / `plan-exact-coalesced.json` | 两种执行模式计划（仅 `execution_mode` 不同） |
| `result-plan-full-packet.json` / `result-plan-exact-coalesced.json` | 仿真结果 |

```bash
cd simulator/htsim_ocs
./build/htsim_ocs validate --plan tests/full_simulator_test/ring_allreduce_swot_p3k3/plan-full-packet.json
./build/htsim_ocs run --plan tests/full_simulator_test/ring_allreduce_swot_p3k3/plan-full-packet.json \
  --result /tmp/result-swot.json [--trace /tmp/trace-swot.jsonl]
```

重新生成（`ocs` conda 环境）：

```bash
conda activate ocs
PYTHONPATH=. python simulator/htsim_ocs/tools/generate_swot_overlap_plan.py
# 自定义拓扑参数：
PYTHONPATH=. python simulator/htsim_ocs/tools/generate_swot_overlap_plan.py \
  --reconfiguration-delay-ps 1000 --per-plane-bps 200000000000
```

生成确定性 JSON（相同参数产生相同字节与 SHA-256），默认输出到本目录（`simulator/htsim_ocs/tests/full_simulator_test/`）。
