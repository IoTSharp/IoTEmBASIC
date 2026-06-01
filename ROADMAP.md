# IoTEmbedded 路线图

> 整理日期：2026-06-01
>
> 状态：✅ 已完成 ｜ 🚧 未完成/进行中 ｜ ⬜ 未开始 ｜ 🔁 持续维护
>
> 判定口径：能在当前源码、工程文件或示例中确认闭环的标记为已完成；只有局部实现、缺少统一契约或缺少目标验证的标记为进行中；没有代码入口的标记为未开始。

## 0. 范围边界

IoTEmbedded 只承载开源的嵌入式 BASIC/C 运行时、板级适配、总线/网络/协议模块和目标工程入口。

- 不引入租户、计费、License、Copilot 编排、付费模板等 SaaS 商业能力。
- 不承担云端设备管理、业务遥测接收、业务数据落库或按设备/遥测量计费。
- 脚本签名格式由上层契约定义，本仓库只实现可插拔校验入口和本地运行保护。
- 新增代码以 `AGENTS.md` 里的真实目录约束为准：`Application`、`Board`、`Bus`、`Common`、`Config`、`Devices`、`Display`、`Interpreter`、`Modem`、`Network`、`Platform`、`Protocol`、`Storage`、`ThirdParty`。

## 1. 当前基线

| 模块 | 状态 | 当前内容 |
| --- | --- | --- |
| `Application` | ✅ | 应用入口、FreeRTOS 任务、调试任务、BASIC 执行任务和 Modbus 测试入口已落地。 |
| `Board` | ✅ | 通用板级初始化、看门狗、板级资源表，以及 Pandora、PowerEnvDaq 板级映射已落地。 |
| `Bus/Uart`、`Bus/Rs485` | ✅ | UART、RS485 基础收发和 BASIC 绑定已落地。 |
| `Common` | ✅ | 日志、MD5、工具函数和通用类型已落地。 |
| `Config` | ✅ | 运行配置、网络配置、命令式配置查看/修改入口已落地。 |
| `Devices` | 🚧 | 空调、电表、开关、离散量、温湿度、UPS、遥测上报等业务设备抽象已有实现，仍需收敛为可复用 profile。 |
| `Display` | ✅ | ST7789 显示抽象和 Pandora LCD BASIC 图形入口已落地。 |
| `Interpreter` | ✅ | MY-BASIC 内核、加载运行封装、脚本生命周期 managed startup、可插拔签名校验入口、IMPORT 可观测错误、错误输出、SERIAL/MODBUS/MQTT/JSON/CONFIG/FORMAT/QB4.5 图形函数注册已落地。 |
| `Modem` | ✅ | Air724 基础适配已落地。 |
| `Network` | 🚧 | CH395、Air724、AP6181 Socket 适配和网络管理已有实现，跨目标能力边界仍需固化。 |
| `Platform/stm32` | 🚧 | STM32F103VETX、STM32L475VETX + FreeRTOS/CMSIS-RTOS2 工程已落地；非 STM32 与低资源 Linux 入口未落地。 |
| `Protocol/Modbus` | ✅ | Modbus CRC、核心、Master、API 和 BASIC 绑定已落地。 |
| `Protocol/Mqtt` | ✅ | MQTT client、发布/订阅/接收、句柄模型和 BASIC 绑定已落地。 |
| `Protocol/Platform` | ✅ | 平台消息模块已落地。 |
| `Storage` | ✅ | EEPROM 配置存储、BASIC 双脚本槽、CRC 校验、脚本包 metadata、签名 envelope、active/candidate/previous 生命周期状态和回滚记录已落地，并兼容旧 v1/v2 header。 |
| `ThirdParty/Parson` | ✅ | JSON 依赖已接入，保持第三方源码原样。 |
| `projects/stm32` | ✅ | F1/F103 与 L4/L475 VisualGDB/CMake 工程入口已落地。 |
| `tools/VisualGDBBuild` | ✅ | F1/L4 命令行 build/clean/rebuild/flash/diagnose 包装工具已落地。 |

## 2. 里程碑状态

| 里程碑 | 状态 | 说明 |
| --- | --- | --- |
| M0 目录与工程约束 | ✅ | 真实目录结构和 `Inc`/`Src` 规范已在 `AGENTS.md` 固化。 |
| M1 BASIC 最小可运行 | ✅ | MY-BASIC 已集成，支持加载字符串脚本、执行、错误定位和打印重定向。 |
| M2 BASIC 运行时函数注册 v1 | ✅ | SERIAL、MODBUS、MQTT、JSON、CONFIG、FORMAT、QB4.5 图形函数已按模块注册。 |
| M3 首批 STM32/RTOS 工程 | ✅ | STM32F103VETX、STM32L475VETX 两个 FreeRTOS/CMSIS-RTOS2 工程已接入。 |
| M4 脚本生命周期 v1 | ✅ | EEPROM 双槽、CRC、脚本包 metadata、可插拔签名校验入口、IMPORT、active/candidate/previous 语义、启动失败回滚和状态记录已完成。 |
| M5 多目标验证 | 🚧 | 两个 STM32/RTOS 目标已具备工程入口；低资源 Linux Profile 未开始。 |
| M6 资源与稳定性基线 | 🚧 | 看门狗、任务心跳、栈余量查看已有实现；ROM/RAM/栈/启动耗时预算和回归记录未完成。 |
| M7 低资源 Linux Profile | ⬜ | 尚无 `Platform/linux` 或 `projects/linux-*` 入口。 |
| M8 面向 CodeGen 的目标能力契约 | 🚧 | 已形成 BASIC API 清单、smoke manifest 和运行时 registry；板级能力、协议能力和资源预算 profile 仍需继续补齐。 |

## 3. 已完成能力清单

| 编号 | 状态 | 能力 |
| --- | --- | --- |
| D0 | ✅ | 目录拆分已按当前真实模块落地，新增模块遵守 `Inc/` + `Src/` 结构。 |
| D1 | ✅ | STM32F103VETX / PowerEnvDaq 工程入口、CubeMX 平台代码、linker script、VisualGDB 工程已落地。 |
| D2 | ✅ | STM32L475VETX / Pandora 工程入口、CubeMX 平台代码、linker script、VisualGDB 工程已落地。 |
| D3 | ✅ | FreeRTOS + CMSIS-RTOS2 应用任务、任务心跳、看门狗线程和栈余量观测已落地。 |
| D4 | ✅ | BASIC 解释器内核、`app_basic` 加载/运行封装、错误日志和打印输出适配已落地。 |
| D5 | ✅ | EEPROM 配置读写、BASIC 脚本槽 v3 header、脚本包 metadata、签名 envelope、CRC 校验、双槽读取、managed startup、active/candidate/previous 状态和回滚记录已落地，并兼容旧 v1/v2 header。 |
| D6 | ✅ | BASIC `IMPORT` 按 EEPROM 槽内脚本名解析，兼容旧的 `app01/app02` 物理槽名称，并记录依赖循环、缺失依赖、脚本大小超限等可观测错误。 |
| D7 | ✅ | SERIAL / RS485 BASIC 函数和底层 UART/RS485 适配已落地。 |
| D8 | ✅ | Modbus RTU 读写、CRC、Master API 和 BASIC 函数已落地。 |
| D9 | ✅ | MQTT 句柄模型、connect/publish/subscribe/receive/ping/disconnect 和 BASIC 函数已落地。 |
| D10 | ✅ | JSON 解析、构造、读写、数组访问、序列化和 BASIC 函数已落地。 |
| D11 | ✅ | CONFIG / NETWORK / MQTT 链路配置 BASIC 函数已落地。 |
| D12 | ✅ | CH395、有线网络 Socket、Air724、AP6181 相关适配已有实现。 |
| D13 | ✅ | Pandora ST7789 LCD、QB4.5 风格图形 BASIC 函数和显示输出重定向已落地。 |
| D14 | ✅ | `tools/VisualGDBBuild` 支持 F1/L4 目标发现、诊断、构建、清理、重建和烧录命令拼装。 |
| D15 | ✅ | `examples/basic` 已提供 MQTT、JSON、SERIAL、MODBUS、图形等脚本示例说明。 |

## 4. 未完成任务

### P0 BASIC 能力契约与注册表收口

| 编号 | 状态 | 任务 |
| --- | --- | --- |
| P0.1 | ✅ | 已新增 `src/Interpreter/Src/app_basic_registry.c` / `Inc/app_basic_registry.h`，`app_basic.c` 只负责加载、IMPORT、打印和执行，模块注册和函数契约收口到统一 registry/profile。 |
| P0.2 | ✅ | 已新增 `docs/basic/basic-api.v1.json`，供示例文档、CodeGen 和低资源 Linux Profile 复用。 |
| P0.3 | ✅ | 已新增 `examples/basic/smoke/` 下 SERIAL、MODBUS、MQTT、JSON、CONFIG、DISPLAY smoke 脚本，以及 `smoke-manifest.v1.json` 预期结果。 |
| P0.4 | ✅ | 已新增 `docs/basic/api-contracts.md`，明确错误码、超时、句柄生命周期和内存所有权约定。 |

### P1 脚本生命周期与安全校验

| 编号 | 状态 | 任务 |
| --- | --- | --- |
| P1.1 | ✅ | 已在 EEPROM BASIC 脚本槽 header 中补齐脚本包 metadata：版本、创建时间、构建号、入口名、依赖脚本名；旧 v1 header 仍可读取。 |
| P1.2 | ✅ | 已增加 opaque 签名 envelope 与 `app_basic_set_signature_verifier(...)` 可插拔校验入口，运行时只消费上层下发的签名格式，不内置商业授权逻辑。 |
| P1.3 | ✅ | 已明确 active / candidate / previous 槽位语义，新增 EEPROM 生命周期状态记录和 RTOS managed startup；candidate 加载、运行或启动复位失败会回滚 previous 并记录结果。 |
| P1.4 | ✅ | 已为 IMPORT 增加 `last_import_status` 状态、日志和 `status` 输出，可区分依赖循环、缺失依赖、脚本大小超限、无效 header、读失败、签名拒绝和解析失败。 |

### P2 构建验证与资源基线

| 编号 | 状态 | 任务 |
| --- | --- | --- |
| P2.1 | 🚧 | 固化 F1、L4 的 VisualGDB build 验证记录，区分本地工具链缺失和真实编译失败。 |
| P2.2 | ⬜ | 记录每个目标的 Flash、RAM、堆、关键任务栈、BASIC 脚本槽大小和 MQTT/JSON 峰值内存。 |
| P2.3 | ⬜ | 增加可重复的 size/map 摘要输出，作为后续资源优化基线。 |
| P2.4 | ⬜ | 为 BASIC 核心建立 host smoke harness，避免所有验证都依赖硬件烧录。 |

### P3 平台与驱动抽象补齐

| 编号 | 状态 | 任务 |
| --- | --- | --- |
| P3.1 | 🚧 | 将现有 CH395、Air724、AP6181 能力收敛为清晰的 Network Profile，明确目标差异和 fallback 规则。 |
| P3.2 | ⬜ | 补齐 I2C、SPI 的统一总线接口，保持与现有 UART/RS485 风格一致。 |
| P3.3 | ⬜ | 补齐 GPIO、Timer、Flash 的跨板级最小抽象。 |
| P3.4 | ⬜ | 把 Devices 中的业务设备抽象拆出目标能力依赖，避免设备逻辑绑定单一板卡。 |

### P4 低资源 Linux Profile

| 编号 | 状态 | 任务 |
| --- | --- | --- |
| P4.1 | ⬜ | 定义低资源 Linux Profile 边界：进程模型、文件存储、串口、网络、日志、配置和资源预算。 |
| P4.2 | ⬜ | 新增 POSIX/Linux 平台适配入口和最小 `projects/linux-low-resource` 构建入口。 |
| P4.3 | ⬜ | 复用 BASIC API 清单验证 MQTT、SERIAL、MODBUS、JSON 在 Linux Profile 下的一致行为。 |
| P4.4 | ⬜ | 记录低资源 Linux 的启动耗时、RSS、脚本执行耗时和异常恢复策略。 |

### P5 文档同步

| 编号 | 状态 | 任务 |
| --- | --- | --- |
| P5.1 | 🚧 | 同步 README、ARCHITECTURE 和 pages 中的目录描述，统一到当前真实模块名。 |
| P5.2 | ⬜ | 从统一 BASIC API 清单生成或维护示例文档，减少函数说明与源码漂移。 |
| P5.3 | ⬜ | 为新增目标工程补齐接线、构建、烧录、调试和资源限制说明。 |

## 5. 下一步建议

最适合继续做 **P2.4 host smoke harness** 和 **P4 低资源 Linux Profile**。

原因：

- P0 已经把 BASIC registry、机器可读 API 清单、smoke manifest 和契约文档收口完成，后续验证可以直接消费这些文件。
- JSON / CONFIG 等纯逻辑 smoke 可先在 host harness 跑通，SERIAL / MODBUS / MQTT 再用 mock 或真实外设夹具覆盖，避免每次都依赖硬件烧录。
- 低资源 Linux Profile 现在可以按 `docs/basic/basic-api.v1.json` 实现同一组 API，不需要边移植边重新定义函数语义。

推荐执行顺序：

1. 建立 host smoke harness，读取 `examples/basic/smoke/smoke-manifest.v1.json` 并先跑 JSON / CONFIG。
2. 为 SERIAL、MODBUS、MQTT 增加 host mock 适配，覆盖超时、错误和句柄生命周期。
3. 定义低资源 Linux Profile 的串口、网络、配置和资源预算。
4. 新增 `Platform/linux` 与 `projects/linux-low-resource` 最小构建入口。
5. 最后补齐板级能力、协议能力和资源预算 profile，把 M8 从 🚧 推到 ✅。

## 6. 持续维护规则

- `src/Platform/stm32/**` 保持 CubeMX 生成结构，除 `USER CODE` 区外不做目录重排。
- `src/ThirdParty/**` 保持第三方源码原样。
- 新增模块必须同步 CMake 源文件列表、示例文档和路线图状态。
- 每次完成 P0/P1/P2/P3/P4 中的任务，都要在本文件把状态从 ⬜ 或 🚧 更新为 ✅，并补一句实际落地点。
