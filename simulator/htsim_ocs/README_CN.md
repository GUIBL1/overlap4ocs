# HTSim OCS 独立后端

[英文说明](README.md)

`htsim_ocs` 是 overlap4ocs 的独立光路交换（OCS）仿真后端。它读取一份完整、确定的 JSON 执行计划，输出一份正式 JSON 结果，以及可选的操作事件JSONL 轨迹：

```text
执行计划 v2 JSON -> htsim_ocs -> 仿真结果 v2 JSON（+ 可选操作事件 JSONL）
```

## 已实现功能

`htsim_ocs` 接收 overlap4ocs 已经生成好的调度方案，把方案放到 HTSim 的事件仿真环境中执行，再把仿真结果返回给 overlap4ocs。它主要增加了以下能力。

### 读取 overlap4ocs 的完整调度方案

- 通过固定格式的 JSON 文件接收拓扑、光平面配置、数据流、执行步骤、步骤依赖和重构安排。
- 在开始仿真前检查输入是否完整、字段之间是否一致，以及数据流能否在指定配置下到达目的端。输入有问题时直接返回明确的错误。
- 调度算法仍由 overlap4ocs 决定；后端只执行输入文件明确写出的内容。

### 仿真多个相互独立的光平面

- 在 HTSim 中为每个光平面建立独立的交换、传输和重构状态。
- 每个光平面可以使用不同的当前配置，也可以在不同时间开始和结束重构。
- 一个光平面正在重构时，其他光平面仍可继续传输，因此能够表现 SWOT 依靠多平面重叠传输与重构的核心过程。
- 光平面切换配置前会等待该平面上已经发出的数据传完，避免旧配置的数据误入新配置。

### 执行三类论文对比方案

- 一次性配置：仿真开始前安装固定配置，整个集合通信过程中不再重构。
- 一步一次重配置：当前步骤完成后统一切换到下一步需要的配置，再开始下一步传输。
- SWOT：各光平面按照自己的使用情况提前重构；一部分光平面重构时，其他光平面可以继续发送已经就绪的数据。

这三类方案使用同一套流量执行和结果统计逻辑，便于直接比较集合通信完成时间。

### 按计划执行数据流和步骤依赖

- 将输入中的每条数据流放到指定光平面，并按照该平面的带宽、传播时延和重构时延推进仿真时间。
- 支持所有步骤统一同步，也支持由 overlap4ocs 明确给出流组之间的前后依赖。
- 只有前置数据真正到达接收端后，依赖它的流组或步骤才会开始。
- 以最后一条必需数据流到达接收端的时间作为集合通信完成时间。

### 支持小规模核对和论文规模仿真

- `full_packet` 逐个模拟数据包，主要用于小规模正确性检查。
- `exact_coalesced` 合并处理连续数据包，减少论文规模用例的事件数量和内存占用。
- 两种模式使用相同的流量、时序和完成规则，并通过等价测试保证主要仿真结果一致。

### 生成可直接使用的结果

- 输出固定格式的结果 JSON，包括集合通信完成时间、各步骤和数据流的完成情况、各光平面的配置与重构时间，以及流量统计。
- 可以额外输出按时间排列的操作轨迹，用于查看数据流释放、步骤完成和光平面重构的先后顺序。
- 输入错误、运行时死锁、超过仿真限制等情况都会生成结构化失败信息，不会把失败结果伪装成有效的集合通信完成时间。
- 相同程序和相同输入会生成一致的结果，便于重复实验、保存证据和比较三种方案。

## 后端不实现什么

下列功能属于上游 overlap4ocs，而不是后端的隐藏能力：

- 定义集合通信算法、进程伙伴、数据块所有权或规约语义；
- 运行混合整数线性规划或规划器，或选择基线、一次性配置、SWOT 参数；
- 判定一次性配置是否可行，或生成“不支持”证明；
- 将集合通信与规划器分配结果转换为显式数据流、流组、令牌、配置和光平面程序；
- 重解释结果、截断集合通信完成时间，或失败后回退到解析模型。

tips：后端仅模拟无损、仅有效载荷的传输模型，不模拟重传、丢包、优先级流量控制等。

## 运行路径

```text
执行计划原始字节
  -> 命令行路径/输入输出预检和 SHA-256
  -> 严格的执行计划 v2 解析器与语义验证
  -> 不可变 OcsExecutionPlanV2
  -> OcsCoordinator
       -> 令牌/流组/步骤依赖运行时
       -> 每个光平面一个 OcsPlaneRuntime
            -> 程序时期与重构定时器
            -> 每个光平面一个 OcsPlaneDataplane
                 -> 每个源端一个串行化器
                 -> OCS 交换器 -> 目的端管道 -> 接收端
  -> OcsRunSummary + 操作事件
  -> 规范化结果/JSONL 写入器
  -> 先原子提交轨迹，再原子提交结果
```

成功的集合通信完成时间从 `0 ps` 的集合通信开始令牌起算，到最后一个必需有效载荷字节到达接收端为止。

## 仓库结构

```text
simulator/htsim_ocs/
├── README.md                 # 英文说明
├── README_CN.md              # 本文档
├── Makefile                  # 显式正式/测试目标文件闭包
├── include/                  # 声明、公开数据类型和运行时结构
├── src/                      # C++ 实现与命令行入口
├── docs/                     # 生命周期、命令行、数据字典和就绪状态文档
├── tools/                    # 构建时契约头文件生成器
├── tests/                    # 单元、契约、语义、失败和规模测试
├── third_party/nlohmann/     # 随仓库提供的 JSON 头文件与许可信息
└── build/                    # 被忽略的目标文件、映射、测试程序和正式程序

simulator/schemas/            # 执行计划/结果/操作事件的 JSON 模式规范
simulator/contracts/          # Python 编解码器、验证器和应用二进制接口上限镜像
third_party/csg-htsim/        # 随仓库提供的 HTSim 源码与本地补丁登记表
```

### 源文件职责

除特别说明外，`include/` 中的头文件声明由 `src/` 同名文件实现的类型。

| 文件或文件对                             | 职责                                                                        |
| ---------------------------------------- | --------------------------------------------------------------------------- |
| `src/main_ocs.cpp`                     | 命令行解析与分发、路径预检、原始计划快照、错误/退出映射和运行提交顺序       |
| `include/version.h`                    | `version` 和结果来源信息使用的后端/模式规范身份常量                       |
| `include/checked_arithmetic.h`         | 字节、计数器、容量和时间的带检查无符号整数运算                              |
| `include/sha256.h`、`src/sha256.cpp` | 项目自有的流式 SHA-256 实现                                                 |
| `include/ocs_run_status.h`             | 稳定进程退出类别                                                            |
| `include/ocs_validation_error.h`       | 含稳定错误码和 JSON 指针的解析/模式规范错误                                 |
| `include/ocs_dataplane_error.h`        | 运行时/数据面错误与稳定错误码                                               |
| `ocs_execution_plan.*`                 | 不可变强类型执行计划 v2 模型和按标识符查找接口                              |
| `ocs_plan_parser.*`                    | 严格 JSON/模式规范/语义解析器、容量证明、环检测、结果大小预检和原始字节散列 |
| `ocs_execution_mode.*`                 | 两种执行模式选择、数据包/尾包算法和容量证明                                 |
| `ocs_packet.*`                         | OCS 数据包元数据、完整数据包/在途批次和带检查对象池                         |
| `ocs_packet_flow_bridge.*`             | 项目数据流身份与 HTSim 数据包流元数据之间的安全桥接                         |
| `ocs_flow.*`                           | 每条数据流的释放、有序发送/接收记账、精确完成和回调                         |
| `ocs_switch.*`                         | 安装置换/时期/代次的验证，以及进入 HTSim 路径的转发                         |
| `ocs_sink.*`                           | 目的端检查、接收回调、在途计数递减和对象归还                                |
| `ocs_route_table.*`                    | 精确交换器/管道/接收端路径所有权与路径审计                                  |
| `ocs_port_serializer.*`                | 每平面/每源端先进先出队列、整数串行化、精确尾包、积压和忙碌区间             |
| `ocs_plane_dataplane.*`                | 单光平面的交换器、管道、接收端、串行化器、路径安装、数据流释放和统计        |
| `ocs_topology.*`                       | `k` 个光平面构造、数据流所有权、流组释放、全局数据面审计和失败清理        |
| `ocs_trace_collector.*`                | 内存中的确定性操作事件收集                                                  |
| `ocs_flow_group.*`                     | 流组与逻辑步骤的接收完成聚合                                                |
| `ocs_dependency_tracker.*`             | 强类型就绪令牌发布和剩余父依赖计数器                                        |
| `ocs_program_epoch.*`                  | 程序时期状态机、时序字段、转换身份和物理代次                                |
| `ocs_reconfiguration.*`                | 带检查的光平面局部 HTSim 重构`EventSource` 事件源定时器                   |
| `ocs_plane_runtime.*`                  | 光平面程序游标、时期生命周期、代次和运行时快照                              |
| `ocs_guards.*`                         | 令牌、时期、排空和同步模式“全部路径就绪”条件                              |
| `ocs_watchdog.*`                       | 包含边界的最大时间/最大事件数与事件列表为空决策                             |
| `ocs_coordinator.*`                    | 规范化不动点调度器、回调、完成判定、失败快照和成功审计                      |
| `include/ocs_run_summary.h`            | 结果写入器消费的完整内存成功/失败摘要                                       |
| `ocs_result_writer.*`                  | 规范化结果 v2/操作 JSONL 编码与禁止覆盖的原子发布                           |
| `src/htsim_logged_shim.cpp`            | 最小链接闭包使用的启动安全 HTSim`Logged` 登记表适配层                     |

### 其他目录

| 路径                                                                                         | 职责                                                                                                    |
| -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `docs/cli.md`                                                                              | 正式命令、退出类别、输出路径规则和原子提交协议                                                          |
| `docs/data-dictionary.md`                                                                  | 执行计划、结果、轨迹字段、单位和完成语义的操作索引                                                      |
| `docs/dataplane-lifecycle.md`                                                              | 数据包/批次、路径、串行化器、对象池和数据面生命周期                                                     |
| `docs/runtime-lifecycle.md`                                                                | 光平面程序、依赖、路径策略、协调器和监控器生命周期                                                      |
| `docs/minimal-link-closure.md`                                                             | 允许的 HTSim 目标文件/符号范围与链接映射检查                                                            |
| `docs/backend-readiness.md`                                                                | 版本/模式规范身份、验收语料、确定性和规模证据                                                           |
| `tools/generate_contract_header.py`                                                        | 将权威模式规范、应用二进制接口原始摘要和构建参数生成 C++ 头文件                                         |
| `third_party/nlohmann/`                                                                    | 仅头文件 JSON 解析器与上游/许可信息；这里不是 HTSim 源码目录                                            |
| `tests/fixtures/v2/`                                                                       | 静态可审计的数据面/运行时/性能计划、清单、结果投影和轨迹                                                |
| `tests/artifacts/`                                                                         | 保留的阶段证据和交付清单                                                                                |
| `tests/run_tests.py`、`test_cli.py`、`test_validate_cli.py`                            | 命令行、能力、链接闭包、第三方来源和解析器集成测试                                                      |
| `tests/test_eventlist_order.cpp`、`test_plan_parser.cpp`、`test_sha256.cpp`            | 内核顺序、解析器和 SHA 单元测试                                                                         |
| `tests/test_packet_adapter.cpp`、`test_route_handoff.cpp`、`test_static_dataplane.cpp` | 数据包、路径交接、串行化器、平面隔离和模式等价测试                                                      |
| `tests/test_runtime_driver.cpp`、`run_runtime_tests.py`                                  | 程序时期、依赖、路径策略、监控器和运行时确定性测试                                                      |
| `tests/test_result_writer.cpp`、`test_failure_contract.py`                               | 写入器故障注入、失败分类和原子性测试                                                                    |
| `tests/phase06_acceptance.py`、`test_standalone_cases.py`                                | 公共测试用例审计和独立语义验收                                                                          |
| `tests/test_determinism.py`、`test_performance_gate.py`                                  | 独立进程字节确定性与资源/复杂度门                                                                       |
| `build/`                                                                                   | 生成的契约头文件、目标文件、依赖文件、链接映射、测试程序和`build/htsim_ocs`；可用 `make clean` 删除 |

## 构建

使用 Python 3.10+、C++17 编译器、GNU Make 和对应 Conda 环境。从仓库根目录执行：

```bash
source "$(conda info --base)/etc/profile.d/conda.sh"
conda activate ocs
make -C simulator/htsim_ocs clean all
```

正式可执行文件位于：

```text
simulator/htsim_ocs/build/htsim_ocs
```

查看准确构建信息与契约身份：

```bash
simulator/htsim_ocs/build/htsim_ocs version
simulator/htsim_ocs/build/htsim_ocs capabilities --json
```

运行后端全部验收门：

```bash
make -C simulator/htsim_ocs \
  test check-link-closure check-vendor
```

## 命令行使用方法

```text
htsim_ocs run --plan PATH --result PATH [--trace PATH]
htsim_ocs validate --plan PATH
htsim_ocs capabilities --json
htsim_ocs version
htsim_ocs help
```

### 验证执行计划

```bash
simulator/htsim_ocs/build/htsim_ocs validate \
  --plan simulator/htsim_ocs/tests/fixtures/v2/runtime/paper_fig5_swot/plan-exact-coalesced.json
```

`validate` 读取原始字节，执行全部模式规范、语义、容量、环、路径、程序时期、依赖和来源信息检查，但不会启动仿真。

### 运行执行计划

```bash
run_dir="$(mktemp -d)"

simulator/htsim_ocs/build/htsim_ocs run \
  --plan simulator/htsim_ocs/tests/fixtures/v2/runtime/paper_fig5_swot/plan-exact-coalesced.json \
  --result "$run_dir/result.json" \
  --trace "$run_dir/operations.jsonl"
```

规则：

- `--plan` 和 `--result` 必填，`--trace` 可选。
- 执行计划必须是可读普通文件。
- 输出父目录必须已存在且可写。
- 执行计划、结果、轨迹路径必须彼此不同。
- 结果/轨迹目标不能已存在；后端绝不覆盖正式输出。
- 命令行不允许覆盖执行模式、随机种子、依赖策略、路径策略、最大仿真时间或最大事件数。
- 不要从标准输出或标准错误解析状态或集合通信完成时间；必须读取并验证正式结果文件。

### 退出类别

| 退出码 | 含义                   | 常见正式停止原因                             |
| -----: | ---------------------- | -------------------------------------------- |
|      0 | 成功                   | `collective_complete`                      |
|      2 | 输入无效或语义不支持   | `invalid_input`、`unsupported_semantics` |
|      3 | 运行时死锁或不变量失败 | `deadlock`、`invariant_violation`        |
|      4 | 到达执行计划的仿真预算 | `max_simulation_time`、`max_event_count` |
|      5 | 输入输出或内部失败     | `io_error`、`internal_error`             |

若结果路径本身无法提交，非零退出码和标准错误可能是唯一诊断。其他已知失败会生成结构化结果 v2，且集合通信完成时间为空值。

## JSON 接口与带注释示例

只有执行计划是后端输入；仿真结果和操作轨迹由 `htsim_ocs`生成：

| 接口         | 正式机器模式文件                                                                                   | 带注释字段示例                                                                                         |
| ------------ | -------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| 执行计划     | [`swot-execution-plan-v2.schema.json`](../schemas/swot-execution-plan-v2.schema.json)             | [`swot-execution-plan-v2.example.jsonc`](../schemas/swot-execution-plan-v2.example.jsonc)             |
| 仿真结果     | [`swot-simulation-result-v2.schema.json`](../schemas/swot-simulation-result-v2.schema.json)       | [`swot-simulation-result-v2.example.jsonc`](../schemas/swot-simulation-result-v2.example.jsonc)       |
| 单个操作事件 | [`htsim-ocs-operation-event-v1.schema.json`](../schemas/htsim-ocs-operation-event-v1.schema.json) | [`htsim-ocs-operation-event-v1.example.jsonc`](../schemas/htsim-ocs-operation-event-v1.example.jsonc) |

运行前先验证完成的计划：

```bash
simulator/htsim_ocs/build/htsim_ocs validate --plan /path/to/plan.json
```

运行细节请阅读 [命令行契约](docs/cli.md)、
[数据字典](docs/data-dictionary.md) 和
[后端就绪证据](docs/backend-readiness.md)。
