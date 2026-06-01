# BASIC Examples

本目录放可以写入 EEPROM 脚本槽的 `.bas` 示例。
当前固件默认从 EEPROM 里的 `app01.bas` / `app02.bas` 读取脚本；这里的示例文件用于编写和演示脚本内容。
`IMPORT` 参数是包内脚本名，不是物理槽位名；STM32 会按 EEPROM 槽里保存的脚本名做映射，例如把 `common.bas` 存入备份槽后，主脚本可使用 `IMPORT "common.bas"`。

EEPROM BASIC 脚本槽使用 v3 header 时会随脚本保存包级 metadata：`package_version`、`created_at_unix`、`build_number`、`entry_name` 和最多 4 个依赖脚本名，并预留 opaque 签名 envelope：`format`、`flags` 和签名字节。旧 v1/v2 header 仍可读取，运行时会把槽内脚本名作为默认入口名；新写入接口是 `eeprom_write_basic_script_package(...)`，不需要签名时可继续使用 `eeprom_write_basic_script_with_metadata(...)` 或 `eeprom_write_basic_script(...)`。

签名算法、密钥和授权语义由上层契约或产品集成提供；IoTEmbedded 只通过 `app_basic_set_signature_verifier(...)` 注册可插拔 verifier，并在主脚本和 `IMPORT` 脚本加载前消费签名 envelope。

脚本生命周期在两个物理槽之外另存一个带 CRC 的 EEPROM 状态记录：`active` 是默认启动槽，`candidate` 是下一次优先试运行的新槽，`previous` 是 candidate 发布前最后一个已知可运行槽。`eeprom_set_basic_script_candidate(...)` 会把当前 active 记为 previous；RTOS 启动时会先试 candidate，加载或运行失败会自动回滚 previous，并记录 `boot_count`、`rollback_count`、最近尝试/成功/失败槽和启动结果。若 candidate 启动过程中被看门狗复位，下次上电会把上一次未完成的 `booting` 视作失败并回滚，避免坏候选反复启动。

`IMPORT` 失败会记录可观测错误到 `app_basic_status_t.last_import_status`，并在日志和 `status` 命令中输出最近依赖名、槽位和大小。当前可区分 `missing-dependency`、`dependency-cycle`、`script-size-exceeded`、`invalid-header`、`read-failed`、`signature-rejected`、`parse-failed` 和 `depth-exceeded`。

统一 BASIC API 清单位于 `../../docs/basic/basic-api.v1.json`，其中 `functionSignatures` 是逐函数 canonical 列表；错误码、超时、句柄生命周期和内存所有权约定位于 `../../docs/basic/api-contracts.md`。关键函数 smoke 脚本位于 `smoke/`，预期输出由 `smoke/smoke-manifest.v1.json` 描述，后续 host harness、CodeGen 和低资源 Linux Profile 应优先消费这些机器可读文件。

## MQTT 函数

- `MQTT_CONNECT(endpoint$[, port, client_id$, username$, password$, keep_alive_seconds])`：连接服务器，成功返回 MQTT 句柄，失败返回 `0`。
- `MQTT_DISCONNECT(handle)`：断开连接。
- `MQTT_CONNECTED(handle)`：返回是否已建立 MQTT 会话。
- `MQTT_PING(handle)`：主动发送 ping。
- `MQTT_SUBSCRIBE(handle, topic$[, qos])`：订阅完整 topic。
- `MQTT_UNSUBSCRIBE(handle, topic$)`：取消订阅。
- `MQTT_PUBLISH(handle, topic$, payload$[, qos, retain])`：向完整 topic 发布 payload；当前 STM32 支持 QoS 0/1 和 retain 标志。
- `MQTT_RECEIVE(handle[, timeout_ms])`：接收一条消息，返回 `DICT` 或 `NIL`；消息字段包含 `topic`、`payload`、`qos`、`retain`。
- `MQTT_LAST_ERROR([handle])`：读取最近一次 MQTT 错误。
- `DELAY(ms)` / `SLEEP(ms)` / `TICKS()`：脚本延时和系统 tick 辅助函数。

STM32 当前底层只有一个物理 MQTT 会话，但脚本层仍统一使用句柄模型；再次 `MQTT_CONNECT(...)` 会替换前一个物理会话并返回新的句柄。`endpoint$` 在 STM32 上使用 host/IP，不带 `mqtt://` scheme。

旧的板级单例入口 `MQTT_CONNECT()`、`MQTT_PUBLISH(topic$, payload$)`、`MQTT_RECV()` 不再兼容、不保留，也不提供别名；脚本必须显式保存并传递 MQTT 句柄。

## 配置 / 网络 函数

- `CONFIG_GET(key$[, default$])`：读取当前配置值，返回字符串。
- `CONFIG_SET(key$, value)`：修改当前配置草稿，`value` 可直接传字符串或数字。
- `CONFIG_APPLY()`：把当前草稿应用到运行态；固定 CH395Q 场景优先用 `NETWORK_CH395()` 或 `MQTT_SETUP_CH395(...)`。
- `CONFIG_SAVE()`：把当前配置写入 EEPROM。
- `CONFIG_RESET()`：恢复默认配置草稿。
- `NETWORK_AUTO([save])`：切回自动主备链路。
- `NETWORK_CH395([save])`：固定 MQTT 网络链路为 CH395Q/4 号串口/CN2，并用当前本机 IP 配置重刷 CH395。
- `NETWORK_4G([save])`：固定 MQTT 网络链路为 Air724UG/4 号串口。
- `NETWORK_USE(mode$[, save])`：直接切换 `auto|wired|4g`，可选 `save=1` 顺手落盘。
- `NETWORK_MODE()` / `NETWORK_LINK()` / `NETWORK_READY()`：读取当前配置模式、当前活动链路和链路就绪状态。
- `MQTT_USE_AUTO([save])` / `MQTT_USE_CH395([save])` / `MQTT_USE_4G([save])`：与 `NETWORK_*` 等价，但脚本语义更偏 MQTT 链路选择。
- `MQTT_SETUP_CH395(host$, port, user$, password$, local_ip$, gateway$, mask$[, save])`：设置 MQTT 与 CH395Q 本机网络参数，立即切到 CH395Q/4 号串口/CN2；`password$` 传 `"auto"` 会按平台规则自动生成密码。

其中 `wired` 对应 CH395Q/4 号串口/CN2，`4g` 对应 Air724UG/4 号串口。

## JSON 函数

- `JSON_PARSE(text$)`：把 JSON 文本解析成句柄，失败返回 `NIL`。
- `JSON_OBJECT()` / `JSON_ARRAY()` / `JSON_STRING(text$)` / `JSON_NUMBER(n)` / `JSON_BOOL(v)` / `JSON_NULL()`：创建 JSON 句柄。
- `JSON_VALID(text$)`：检查文本能否被 Parson 解析。
- `JSON_TYPE(json[, path$])`：返回 Parson 类型码，`-1/1/2/3/4/5/6` 对应 error/null/string/number/object/array/bool。
- `JSON_STRINGIFY(json[, path$])`：把句柄或对象字段序列化成字符串。
- `JSON_HAS(json, path$)` / `JSON_COUNT(json[, path$])` / `JSON_KEY(json, path$, index)`：对象访问辅助函数。
- `JSON_GET_STRING/NUMBER/INT/BOOL(json, path$[, default])`：按对象路径读取值。
- `JSON_AT_STRING/NUMBER/INT/BOOL(json, path$, index[, default])`：按数组字段名 + 下标读取值。
- `JSON_SET_STRING/NUMBER/BOOL/NULL/JSON(json, path$, ...)`：按对象路径写值，`path$` 为空时替换整个根值。
- `JSON_SET_AT_STRING/NUMBER/BOOL/NULL/JSON(json, path$, index, ...)`：按数组字段名 + 下标写值。
- `JSON_APPEND_STRING/NUMBER/BOOL/NULL/JSON(json, path$, ...)`：向数组追加元素。
- `JSON_REMOVE(json, path$)` / `JSON_REMOVE_AT(json, path$, index)` / `JSON_CLEAR(json[, path$])`：移除或清空对象/数组。

`JSON_GET` / `JSON_AT` 会返回一个独立句柄副本，适合继续读值或再组合；如果要改原对象，优先用 `JSON_SET_*` / `JSON_SET_AT_*` / `JSON_APPEND_*`。
对象路径使用 Parson 的点号写法，例如 `"device.id"`。数组用 `JSON_AT_*` / `JSON_SET_AT_*` / `JSON_APPEND_*` 通过数组字段名 + 下标访问。

## SERIAL / RS485 函数

- `SERIAL_OPEN(port, type$)`：按 CubeMX 生成的串口句柄打开端口，`port` 取 `1`/`2`/`4`/`5`，`type$` 取 `"SERIAL"`、`"RS485"` 或 `"RS232"`，返回可传递的句柄。
- `SERIAL_BAUD(port_handle)` / `SERIAL_SET_BAUD(port_handle, baud)`：读取或按 8N1 重新配置串口波特率。
- `SERIAL_WRITE(port_handle, text$, timeout_ms)` / `SERIAL_READ(port_handle, len, timeout_ms)`：文本收发；`SERIAL_READ` 单次最多返回 256 字节。
- `SERIAL_WRITE_BYTES(port_handle, buf, len, timeout_ms)` / `SERIAL_READ_BYTES(port_handle, buf, len, timeout_ms)`：一维数值数组收发原始字节，数组元素范围为 0-255，返回实际字节数。
- `SERIAL_FLUSH(port_handle)`：清掉对应串口的接收残留。

## MODBUS 函数

- `MODBUS_READ_COILS(port_handle, slave, reg, count, buf[, timeout_ms])`：`buf` 按 `count` 个 0/1 元素展开，返回位数。
- `MODBUS_READ_DISCRETE_INPUTS(port_handle, slave, reg, count, buf[, timeout_ms])`：`buf` 按 `count` 个 0/1 元素展开，返回位数。
- `MODBUS_READ_HOLD_REGS(port_handle, slave, reg, count, buf[, timeout_ms])`：`buf` 每个寄存器占 2 字节，返回寄存器个数。
- `MODBUS_READ_INPUT_REGS(port_handle, slave, reg, count, buf[, timeout_ms])`：`buf` 每个寄存器占 2 字节，返回寄存器个数。
- `MODBUS_WRITE_COIL(port_handle, slave, reg, value[, timeout_ms])`
- `MODBUS_WRITE_REG(port_handle, slave, reg, value[, timeout_ms])`
- `MODBUS_WRITE_COILS(port_handle, slave, reg, count, buf[, timeout_ms])`：`buf` 传 `count` 个 0/1 元素。
- `MODBUS_WRITE_REGS(port_handle, slave, reg, count, buf[, timeout_ms])`
- `MODBUS_ERROR()` / `MODBUS_LAST_ERROR()`：读取最近一次 Modbus 错误码。

`AIR724` 只是挂在 4 号串口上的外设名称，不是 BASIC 端口类型；如果脚本要通过它做串口收发，也还是先 `SERIAL_OPEN(4, "SERIAL")` 再使用句柄。
当前板级可用组合是 `1/RS485`、`2/SERIAL`、`4/SERIAL`、`5/RS232`。

## QB4.5 图形函数

Pandora 板载 240x240 RGB565 ST7789 LCD 会注册 QB4.5 风格图形入口。MY-BASIC 当前用函数式调用形式承载这些语义：

- `SCREEN([mode])`：选择显示目标；`SCREEN(12)` 初始化 ST7789 并让后续 `PRINT` 输出到 LCD，`SCREEN(0)` 恢复默认控制台/串口输出。成功返回 `1`。
- `CLS([color])`：用背景色或指定颜色清屏。
- `COLOR([fg][, bg])`：设置前景/背景色；支持 `COLOR(, bg)` 这种中间省略参数。
- `LOCATE([row][, col])`：设置文本光标位置；`SCREEN(12)` 后可配合 `PRINT` 显示 ASCII 文本。
- `PSET(x, y[, color])` / `PRESET(x, y[, color])`：画点；`PRESET` 默认使用背景色。
- `LINE(x1, y1, x2, y2[, color][, style$])`：画线；`style$` 可为 `"B"` 框或 `"BF"` 填充框；`LINE(x2, y2[, ...])` 会从上一个图形点继续。
- `CIRCLE(x, y, radius[, color][, fill])`：画圆，`fill<>0` 时填充；可用 `CIRCLE(x, y, r,, 1)` 保留当前前景色并填充。
- `PAINT(x, y[, fill_color][, border_color])`：已注册，但 Pandora SPI3 是 TX-only，当前无读像素/帧缓存，函数会返回 `0` 表示不支持泛洪填充。

颜色参数按 QB 属性色兼容：`0..15` 映射到 16 色调色板，`16..65535` 直接作为 RGB565 使用。

示例：

```basic
SCREEN(12)
COLOR(15, 0)
CLS()
LOCATE(1, 1)
PRINT "IOTEMBEDDED BASIC"
LOCATE(2, 1)
PRINT "ASCII LCD READY"
LINE(10, 10, 120, 80, &HF800)
LINE(20, 20, 100, 100,, "B")
CIRCLE(120, 120, 40, 9, 1)

MQTT_SETUP_CH395("192.168.137.110", 1883, "d0001", "auto", "192.168.137.201", "192.168.137.11", "255.255.255.0")

LET rs485 = SERIAL_OPEN(1, "RS485")
DIM regs(4)
IF MODBUS_READ_HOLD_REGS(rs485, 1, 0, 2, regs) THEN
  PRINT regs(0); ","; regs(1); ","; regs(2); ","; regs(3)
ENDIF

LET msg = JSON_OBJECT()
JSON_SET_STRING(msg, "type", "heartbeat")
JSON_SET_NUMBER(msg, "time", TICKS())
LET mqtt = MQTT_CONNECT("192.168.137.110", 1883, "basic-d0001", "d0001", "auto", 60)
IF mqtt <> 0 THEN
  MQTT_PUBLISH(mqtt, "/v1/devices/up/basic/d0001", JSON_STRINGIFY(msg), 0, 0)
  MQTT_DISCONNECT(mqtt)
ENDIF
```
