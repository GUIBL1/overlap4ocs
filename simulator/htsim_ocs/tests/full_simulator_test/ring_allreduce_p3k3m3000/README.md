# ring_allreduce_p3k3m3000 — 三节点三平面 ring allreduce 测试用例

本目录是用 `tools/generate_ring_allreduce_plan.py` 生成的手写测试夹具（`provenance.source_kind = hand_authored_fixture`），用于测试 `htsim_ocs` 后端在 3 节点、3 光平面拓扑上执行 ring allreduce 的完整流程。

## 拓扑与算法

- **拓扑**：3 个节点（rank 0..2），3 个并行光平面，每平面 400 Gb/s，传播时延 20 ps，重构时延 200 ps（本用例不触发重构）。
- **算法**：ring allreduce，每 rank 消息 3000 B，按 3 等份切块（每块 1000 B）。
  - 阶段 1 reduce-scatter：2 步；阶段 2 allgather：2 步；共 4 个逻辑步骤，使用 `global_step_barrier` 依赖模式串行推进。
  - 每个阶段内第 s 步（0 基，阶段内局部编号）中，rank i 把块 `(i - s) mod 3` 发给 rank `(i + 1) mod 3`；allgather 阶段从自己的第 0 步重新开始编号。共 12 条单向 flow，每条携带一块。
- **光路**：单一配置 `permutation = [1, 2, 0]`（0→1、1→2、2→0），`strategy = one_shot`（`static_preinstalled`），三个平面均预装该配置、全程不重构。
- **平面分配**：每步的 3 条环边按轮转分配到 3 个平面，每步三个平面并行传输，最终每平面各承载 4 条 flow（每平面 4000 B）。

## 文件

| 文件 | 说明 |
| --- | --- |
| `plan-full-packet.json` | 逐包仿真模式（`execution_mode = full_packet`）执行计划 |
| `plan-exact-coalesced.json` | 合并批量仿真模式（`execution_mode = exact_coalesced`）执行计划；与上者仅 `execution_mode` 不同 |
| `result-plan-full-packet.json` / `result-plan-exact-coalesced.json` | 对应模式的仿真结果（本机运行输出） |

## 运行

构建后端（本机无 `g++`，需指定 `CXX`；生成契约头用 `ocs` conda 环境的 python）：

```bash
cd simulator/htsim_ocs
make CXX=clang++ PYTHON=/home/code/miniconda3/envs/ocs/bin/python -j$(nproc)
```

校验与仿真：

```bash
./build/htsim_ocs validate --plan tests/full_simulator_test/ring_allreduce_p3k3m3000/plan-full-packet.json
./build/htsim_ocs run --plan tests/full_simulator_test/ring_allreduce_p3k3m3000/plan-full-packet.json \
  --result /tmp/result-full-packet.json [--trace /tmp/trace-full-packet.jsonl]
./build/htsim_ocs run --plan tests/full_simulator_test/ring_allreduce_p3k3m3000/plan-exact-coalesced.json \
  --result /tmp/result-exact-coalesced.json
```

重新生成（在 `ocs` conda 环境下）：

```bash
conda activate ocs
PYTHONPATH=. python simulator/htsim_ocs/tools/generate_ring_allreduce_plan.py
# 自定义参数：
PYTHONPATH=. python simulator/htsim_ocs/tools/generate_ring_allreduce_plan.py \
  --p 4 --planes 4 --message-bytes-per-rank 4096 --output-dir /tmp/my_plan
```

生成脚本输出为确定性 JSON（相同参数产生相同字节和 SHA-256），默认输出到本目录（`simulator/htsim_ocs/tests/full_simulator_test/`）。

## 预期结果

4 步串行，每步 1 个包 × 1000 B：串行化 1000 × 8 × 10^12 / 4×10^11 = 20000 ps，加 20 ps 传播时延。

- 各步完成时间：20020 / 40040 / 60060 / 80080 ps；CCT = 80080 ps（两种模式一致）。
- 每平面承载 4000 B；每 rank 发送 4000 B；总计 12000 B，12 条 flow 全部完成。
