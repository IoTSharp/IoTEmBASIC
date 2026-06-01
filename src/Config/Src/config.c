#include "Config/Inc/config.h"

#include "Storage/Inc/app_eeprom.h"
#include "Application/Inc/app_rtos.h"
#include "Interpreter/Inc/app_basic.h"
#include "Modem/Inc/bsp_air724.h"
#include "Network/Ch395/Inc/bsp_ch395.h"
#include "Bus/Rs485/Inc/bsp_rs485.h"
#include "Bus/Uart/Inc/bsp_uart.h"
#include "Network/Ch395/Inc/ch395_board.h"
#include "Network/Ch395/Inc/ch395_driver.h"
#include "Network/Ch395/Inc/ch395_probe.h"
#include "Devices/Inc/device_catalog.h"
#include "Devices/Inc/device_manager.h"
#include "Common/Inc/log.h"
#include "Protocol/Modbus/Inc/modbus_core_crc.h"
#include "Protocol/Modbus/Inc/modbus_core_master.h"
#include "Protocol/Mqtt/Inc/mqtt_client.h"
#include "Network/Ap6181/Inc/ap6181_socket.h"
#include "Network/Ap6181/Inc/bsp_ap6181.h"
#include "Network/Inc/network_manager.h"
#include "Network/Inc/network_socket.h"
#include "Protocol/Platform/Inc/platform_messages.h"
#include "Devices/Inc/temperature_humidity_sensor.h"
#include "Common/Inc/util.h"
#include "Board/Inc/bsp_board.h"
#include "Board/Inc/bsp_hal.h"
#if APP_ENABLE_CMSIS_RTOS
#include "FreeRTOS.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PORT_NUMBER 65535U
#define MAGIC_NUMBER_BYTE_1 0x78
#define MAGIC_NUMBER_BYTE_2 0x6A
#define MAGIC_NUMBER_BYTE_3 0x74
#define MAGIC_NUMBER_BYTE_4 0x01
#define CONFIG_SHELL_PROMPT "pem> "
#define CONFIG_LOGIN_PROMPT "login: "
#define CONFIG_PASSWORD_PROMPT "password: "
#define CONFIG_ANSI_CURSOR_SHOW "\x1B[?25h"
#define CONFIG_ANSI_CURSOR_BLINK "\x1B[?12h"
#define CONFIG_SHELL_HISTORY_DEPTH 4U
#define CONFIG_AUTH_IDLE_TIMEOUT_MS 60000UL
#define CONFIG_DEFAULT_ROOT_PASSWORD "kissme"
#define CONFIG_DEFAULT_USER_PASSWORD "user"
#define CONFIG_MAX_LINES_PER_PROCESS 8U
#define CONFIG_MODBUS_MAX_READ_REGS 16U
#define CONFIG_MODBUS_SCAN_MAX_SLAVES 64U
#define CONFIG_MIN_REPORT_DEVICES_DATA_INTERVAL_MS 10000UL
#define CONFIG_AIR_AT_RESPONSE_SIZE 384U
#define CONFIG_AIR_AT_SHORT_TIMEOUT_MS 1500U
#define CONFIG_AIR_AT_LONG_TIMEOUT_MS 3000U
/* CRC 覆盖 crc 字段之前的全部配置字节，避免结构体尾部填充影响校验长度。 */
#define CONFIG_CRC_DATA_LEN ((uint16_t)offsetof(config_t, crc))

#ifndef CONFIG_ENABLE_STORAGE_LOAD
#define CONFIG_ENABLE_STORAGE_LOAD 1
#endif

typedef enum {
  CONFIG_SHELL_MODE_HUMAN = 0,
  CONFIG_SHELL_MODE_MACHINE,
} config_shell_mode_t;

typedef enum {
  CONFIG_SHELL_ESC_NONE = 0,
  CONFIG_SHELL_ESC_STARTED,
  CONFIG_SHELL_ESC_CSI,
} config_shell_escape_state_t;

typedef enum {
  CONFIG_AUTH_USER_NONE = 0,
  CONFIG_AUTH_USER_ROOT,
  CONFIG_AUTH_USER_USER,
} config_auth_user_t;

typedef enum {
  CONFIG_AUTH_STAGE_USERNAME = 0,
  CONFIG_AUTH_STAGE_PASSWORD,
  CONFIG_AUTH_STAGE_AUTHENTICATED,
} config_auth_stage_t;

char config_cmd_recv_buf[CONFIG_CMD_RECV_BUF_SIZE];
__IO u16 config_cmd_recv_len = 0;
config_t active_config = {{0}};

static config_shell_mode_t shell_mode = CONFIG_SHELL_MODE_HUMAN;
static bool shell_prompt_enabled = true;
static bool shell_echo_enabled = true;
static bool shell_prompt_printed = false;
static bool shell_last_rx_was_cr = false;
static config_shell_escape_state_t shell_escape_state = CONFIG_SHELL_ESC_NONE;
static char shell_history[CONFIG_SHELL_HISTORY_DEPTH][CONFIG_CMD_RECV_BUF_SIZE];
static uint8_t shell_history_count = 0;
static config_auth_stage_t auth_stage = CONFIG_AUTH_STAGE_USERNAME;
static config_auth_user_t auth_user = CONFIG_AUTH_USER_NONE;
static uint32_t auth_last_interaction_ms = 0;
static char auth_pending_user[8] = {0};
static bool auth_banner_printed = false;

static const config_t default_config = {
  .magic_number = {MAGIC_NUMBER_BYTE_1, MAGIC_NUMBER_BYTE_2, MAGIC_NUMBER_BYTE_3, MAGIC_NUMBER_BYTE_4},
  .auth = {.root_password = CONFIG_DEFAULT_ROOT_PASSWORD, .user_password = CONFIG_DEFAULT_USER_PASSWORD},
  .mqtt = {.ip = "192.168.137.110",
           .port = 1883,
           .client_id = "",
           .keepalive = 60,
           .sub_qos = 0,
           .pub_qos = 0,
           .user_name = "d0001",
           .password = "",
           .local_ip = "192.168.137.201",
           .local_port = 49131,
           .gateway_ip = "192.168.137.11",
           .mask_ip = "255.255.255.0"},
  .ntp = {.ip = "192.168.137.110", .port = 123, .local_port = 123, .time_zone = 32},
  .loop = {.main_loop_interval = 2 * 1000,
           .keep_in_touch_with_server_interval = 15 * 1000,
           .sync_network_time_interval = 24 * 60 * 60 * 1000,
           .req_devices_reg_info_interval = 30 * 1000,
           .process_downlink_data_interval = 2 * 1000,
           .report_devices_data_interval = CONFIG_MIN_REPORT_DEVICES_DATA_INTERVAL_MS,
           .report_alarm_data_interval = 2 * 1000,
           .process_uart_cmd_interval = 2 * 1000,
           .feed_watch_dog_interval = 10 * 1000,
           .feed_watch_dog_max_retries = 30},
  .network_monitor = {.probe_host = "192.168.137.110", .probe_port = 1883, .probe_interval_ms = 5 * 60 * 1000},
  .network_mode =
#if BSP_HAS_AP6181
    NETWORK_MODE_WIFI,
#else
    NETWORK_MODE_AUTO,
#endif
  .wifi = {.ssid = "", .password = "", .ip_mode = NETWORK_WIFI_IP_MODE_DHCP, .local_ip = "", .gateway_ip = "",
           .mask_ip = "", .dns_ip = ""},
  .log = {.level = LOG_LEVEL_DEBUG, .print_prefix = true},
  .md5 = {0},
  .crc = 0,
};

/*
 * v0.0.5 的 EEPROM 记录已有 devices 字段但还没有 WiFi 凭据字段；升级后保留现场
 * MQTT/IP/探测/设备表配置，只把 WiFi 配置补成默认空值。
 */
typedef struct {
  char magic_number[MAGIC_NUMBER_SIZE];
  config_auth_t auth;
  mqtt_config_t mqtt;
  ntp_config_t ntp;
  loop_config_t loop;
  network_monitor_config_t network_monitor;
  network_mode_t network_mode;
  config_device_table_t devices;
  log_config_t log;
  char md5[MD5_STR_LEN + 1];
  u16 crc;
} config_v5_t;

/*
 * v0.0.4 之前的 EEPROM 记录没有 devices 字段，升级后先按旧结构读回并迁移，
 * 避免把现有 MQTT/IP/探测/网络模式配置一起清掉。迁移后会自动写回新结构。
 */
typedef struct {
  char magic_number[MAGIC_NUMBER_SIZE];
  config_auth_t auth;
  mqtt_config_t mqtt;
  ntp_config_t ntp;
  loop_config_t loop;
  network_monitor_config_t network_monitor;
  network_mode_t network_mode;
  log_config_t log;
  char md5[MD5_STR_LEN + 1];
  u16 crc;
} config_v4_t;

/*
 * 更早的旧 EEPROM 记录没有 network_mode 字段，升级后先按原结构读回并迁移，
 * 避免把现有 MQTT/IP/探测配置一起清掉。迁移后会自动写回新结构。
 */
typedef struct {
  char magic_number[MAGIC_NUMBER_SIZE];
  config_auth_t auth;
  mqtt_config_t mqtt;
  ntp_config_t ntp;
  loop_config_t loop;
  network_monitor_config_t network_monitor;
  log_config_t log;
  char md5[MD5_STR_LEN + 1];
  u16 crc;
} config_legacy_t;

/* CRC 仍然只覆盖 crc 字段之前的数据。旧结构用于兼容历史 EEPROM 记录。 */
#define CONFIG_V5_CRC_DATA_LEN ((uint16_t)offsetof(config_v5_t, crc))
#define CONFIG_V4_CRC_DATA_LEN ((uint16_t)offsetof(config_v4_t, crc))
#define CONFIG_LEGACY_CRC_DATA_LEN ((uint16_t)offsetof(config_legacy_t, crc))

static void config_clear_cmd_buf(void);
static bool config_consume_cmd_line(char *line, size_t line_size);
ErrorStatus config_get_value(const char *key, char *buf, size_t buf_size);
ErrorStatus config_set_value(const char *key, const char *value);
static ErrorStatus config_set_string(char *dst, size_t dst_size, const char *value);
static ErrorStatus config_set_mqtt_client_id(const char *value);
static ErrorStatus config_set_mqtt_password(const char *value);
static ErrorStatus config_set_u16(u16 *dst, const char *value);
static ErrorStatus config_set_u32(u32 *dst, const char *value);
static ErrorStatus config_set_u32_min(u32 *dst, const char *value, u32 min_value);
static ErrorStatus config_set_u8_max(u8 *dst, const char *value, u8 max_value);
static bool config_value_is_auto_token(const char *value);
static void config_format_device_uid(char *buf, size_t buf_size);
static void config_print_prompt(void);
static void config_shell_enable_cursor(void);
static bool config_shell_should_echo_input(void);
static void config_shell_write(const char *text);
static void config_shell_history_push(const char *line);
static void config_shell_load_history(uint8_t index);
static void config_auth_print_banner(void);
static void config_auth_print_prompt(void);
static void config_auth_logout(const char *reason);
static bool config_auth_is_logged_in(void);
static bool config_auth_is_root(void);
static void config_auth_handle_line(char *line);
static void config_auth_print_welcome(void);
static const char *config_auth_user_str(config_auth_user_t user);
static bool config_auth_password_matches(config_auth_user_t user, const char *password);
static bool config_require_root(void);
static void config_dispatch_cmd(char *line);
static bool config_load_from_storage(void);
static void config_print_help(void);
static void config_print_uname(void);
static void config_print_uptime(void);
static void config_print_free(void);
static void config_print_status(void);
static void config_print_rtos_status(void);
static void config_print_all(void);
static void config_print_eeprom_status(void);
static void config_handle_shell_cmd(char *args);
static void config_handle_passwd_cmd(char *args);
static void config_handle_getenv_cmd(char *args);
static void config_handle_setenv_cmd(char *args);
static void config_handle_log_level_cmd(char *args);
static void config_handle_config_cmd(char *args);
static void config_handle_ch395_cmd(char *args);
static const char *config_hal_status_str(HAL_StatusTypeDef status);
static void config_handle_network_cmd(char *args);
static void config_handle_netuse_cmd(char *args);
static void config_handle_nc_cmd(char *args);
static void config_handle_mqtt_cmd(char *args);
static void config_handle_air_cmd(char *args);
static bool config_air_cmd_is_readonly(const char *args);
static void config_air724_print_group(const char *group, const char *const *commands, const char *const *labels,
                                      size_t count, uint32_t timeout_ms);
static void config_air724_print_at(const char *label, const char *command, uint32_t timeout_ms);
static void config_air724_print_response_lines(const char *label, const char *response);
static void config_handle_time_cmd(char *args);
static void config_handle_modbus_cmd(char *args);
static void config_handle_rs485_cmd(char *args);
static void config_handle_device_cmd(char *args);
static void config_handle_device_rs485_cmd(char *args);
static bool config_parse_u32_range(const char *text, uint32_t min_value, uint32_t max_value, uint32_t *value);
static bool config_modbus_read_registers(const char *kind, uint8_t slave, uint16_t reg, uint16_t count, uint8_t *data,
                                         uint8_t *error_code);
static void config_modbus_print_registers(const char *prefix, const char *kind, uint8_t slave, uint16_t reg,
                                          uint16_t count, const uint8_t *data);
static const char *config_network_link_str(network_link_t link);
static const char *config_network_state_str(network_state_t state);
static const char *config_mqtt_state_str(mqtt_client_state_t state);
static const char *config_wifi_ip_mode_str(network_wifi_ip_mode_t mode);
static bool config_parse_wifi_ip_mode(const char *text, network_wifi_ip_mode_t *mode);
static bool config_is_valid_data(const config_t *config_data);
static bool config_v5_is_valid(const config_v5_t *config_data);
static bool config_v4_is_valid(const config_v4_t *config_data);
static bool config_legacy_is_valid(const config_legacy_t *config_data);
static bool config_device_table_is_valid(const config_device_table_t *devices);
static bool config_wifi_is_valid(const network_wifi_config_t *wifi);
static bool config_device_table_has_enabled_entries(void);
static bool config_device_table_allows(uint16_t service_id, uint16_t manufacture_model, uint8_t slave_addr);
static void config_migrate_v5(const config_v5_t *saved_config);
static void config_migrate_v4(const config_v4_t *saved_config);
static void config_migrate_legacy(const config_legacy_t *legacy_config);
ErrorStatus config_apply_runtime(void);
ErrorStatus config_apply_network_mode(network_mode_t mode, bool persist);
ErrorStatus config_apply_ch395q_network(bool persist);
static void config_device_table_clear(void);
static bool config_device_table_add(uint16_t service_id, uint16_t manufacture_model, uint8_t slave_addr);
static void config_print_device_config(void);

void config_init(void) {
#if CONFIG_ENABLE_STORAGE_LOAD
  if (!config_load_from_storage()) {
    config_reset_to_default();
  }
#else
  config_reset_to_default();
#endif
#if BSP_HAS_AP6181
  ap6181_socket_configure(&active_config.wifi);
#endif
}

static bool config_load_from_storage(void) {
  config_t loaded_config = {{0}};
  if (eeprom_read_config_data(&loaded_config, sizeof(loaded_config)) == SUCCESS &&
      config_is_valid_data(&loaded_config)) {
    active_config = loaded_config;
#if BSP_HAS_AP6181
    if (active_config.network_mode != NETWORK_MODE_WIFI) {
      active_config.network_mode = NETWORK_MODE_WIFI;
      active_config.crc = 0U;
      (void)config_write_into_eeprom();
    }
#endif
    return true;
  }

  config_v5_t v5_config = {{0}};
  if (eeprom_read_config_data(&v5_config, sizeof(v5_config)) == SUCCESS && config_v5_is_valid(&v5_config)) {
    config_migrate_v5(&v5_config);
    return true;
  }

  config_v4_t v4_config = {{0}};
  if (eeprom_read_config_data(&v4_config, sizeof(v4_config)) == SUCCESS && config_v4_is_valid(&v4_config)) {
    config_migrate_v4(&v4_config);
    return true;
  }

  config_legacy_t legacy_config = {{0}};
  if (eeprom_read_config_data(&legacy_config, sizeof(legacy_config)) == SUCCESS &&
      config_legacy_is_valid(&legacy_config)) {
    config_migrate_legacy(&legacy_config);
    return true;
  }

  return false;
}

void config_reset_to_default(void) {
  memcpy(&active_config, &default_config, sizeof(active_config));
}

ErrorStatus config_apply_runtime(void) {
#if BSP_HAS_AP6181
  ap6181_socket_configure(&active_config.wifi);
#endif
  network_manager_set_mode(active_config.network_mode);
  network_manager_poll();
  return SUCCESS;
}

network_mode_t config_get_network_mode(void) {
  return active_config.network_mode;
}

const char *config_network_mode_name(network_mode_t mode) {
  switch (mode) {
  case NETWORK_MODE_AUTO:
    return "auto";
  case NETWORK_MODE_CH395Q:
    return "wired";
  case NETWORK_MODE_AIR724UG:
    return "4g";
  case NETWORK_MODE_WIFI:
    return "wifi";
  default:
#if BSP_HAS_AP6181
    return "wifi";
#else
    return "auto";
#endif
  }
}

ErrorStatus config_write_into_eeprom(void) {
#if !CONFIG_ENABLE_STORAGE_LOAD
  return ERROR;
#else
  active_config.crc = GetCRCData((uint8_t *)&active_config, CONFIG_CRC_DATA_LEN);
  if (eeprom_write_config_data(&active_config, sizeof(active_config)) != SUCCESS) {
    return ERROR;
  }

  config_t verify_config = {{0}};
  if (eeprom_read_config_data(&verify_config, sizeof(verify_config)) != SUCCESS) {
    return ERROR;
  }
  if (memcmp(&active_config, &verify_config, sizeof(active_config)) != 0) {
    /* 外部 AT24C 可能因 WP、地址脚或页写异常出现 ACK 但数据未落盘，保存后必须读回确认。 */
    LOG_ERROR("EEPROM config verify failed");
    return ERROR;
  }

  return SUCCESS;
#endif
}

void config_receive_cmd_byte(uint8_t byte) {
  auth_last_interaction_ms = HAL_GetTick();

  if (byte == '\n' && shell_last_rx_was_cr) {
    shell_last_rx_was_cr = false;
    return;
  }
  shell_last_rx_was_cr = byte == '\r';

  if (shell_escape_state == CONFIG_SHELL_ESC_STARTED) {
    shell_escape_state = byte == '[' ? CONFIG_SHELL_ESC_CSI : CONFIG_SHELL_ESC_NONE;
    return;
  }
  if (shell_escape_state == CONFIG_SHELL_ESC_CSI) {
    if (byte == 'A' && shell_history_count > 0U) {
      config_shell_load_history(0U);
    }
    shell_escape_state = CONFIG_SHELL_ESC_NONE;
    return;
  }

  if (byte == 0x1BU) {
    shell_escape_state = CONFIG_SHELL_ESC_STARTED;
    return;
  }
  if (byte == 0x03U) {
    /* Ctrl+C 只清理当前输入，不触碰正在运行的业务循环，避免现场误中断采集/上报状态机。 */
    config_clear_cmd_buf();
    shell_last_rx_was_cr = false;
    shell_prompt_printed = false;
    if (config_shell_should_echo_input()) {
      config_shell_write("^C\r\n");
    }
    return;
  }
  if (byte == '\b' || byte == 0x7FU) {
    shell_last_rx_was_cr = false;
    if (config_cmd_recv_len > 0U) {
      config_cmd_recv_buf[--config_cmd_recv_len] = '\0';
      if (config_shell_should_echo_input()) {
        config_shell_write("\b \b");
      }
    }
    return;
  }
  if (byte == '\t') {
    shell_last_rx_was_cr = false;
    if (config_cmd_recv_len == 0U) {
      (void)strncpy(config_cmd_recv_buf, "help", sizeof(config_cmd_recv_buf) - 1U);
      config_cmd_recv_len = 4U;
      if (config_shell_should_echo_input()) {
        config_shell_write("help");
      }
    }
    return;
  }
  if (config_cmd_recv_len < (CONFIG_CMD_RECV_BUF_SIZE - 1U)) {
    config_cmd_recv_buf[config_cmd_recv_len++] = (char)byte;
    config_cmd_recv_buf[config_cmd_recv_len] = '\0';
    if (shell_mode == CONFIG_SHELL_MODE_HUMAN && shell_echo_enabled && (byte == '\r' || byte == '\n')) {
      config_shell_write("\r\n");
    } else if (config_shell_should_echo_input()) {
      char echo_char[2] = {(char)byte, '\0'};
      config_shell_write(echo_char);
    }
  }
}

void config_process_cmd(void) {
  if (config_auth_is_logged_in() && (HAL_GetTick() - auth_last_interaction_ms) > CONFIG_AUTH_IDLE_TIMEOUT_MS) {
    config_auth_logout("idle timeout");
  }

  for (uint8_t i = 0U; i < CONFIG_MAX_LINES_PER_PROCESS; i++) {
    char line[CONFIG_CMD_RECV_BUF_SIZE] = {0};
    if (!config_consume_cmd_line(line, sizeof(line))) {
      config_print_prompt();
      return;
    }

    if (line[0] != '\0') {
      if (!config_auth_is_logged_in()) {
        config_auth_handle_line(line);
      } else {
        /* 空闲超时按“最后一条有效交互”计算；否则登录后 60 秒即使持续发命令也会被踢下线。 */
        auth_last_interaction_ms = HAL_GetTick();
        config_shell_history_push(line);
        config_dispatch_cmd(line);
      }
      shell_prompt_printed = false;
      config_print_prompt();
    } else {
      /* 空回车也要重打当前提示符：未登录显示 login/password，已登录显示 pem>。 */
      shell_prompt_printed = false;
      config_print_prompt();
    }
  }

  config_print_prompt();
}

static bool config_is_valid_data(const config_t *config_data) {
  if (config_data == NULL) {
    return false;
  }
  if (memcmp(config_data->magic_number, default_config.magic_number, sizeof(config_data->magic_number)) != 0) {
    return false;
  }
  if (config_data->network_mode > NETWORK_MODE_WIFI) {
    return false;
  }
  if (!config_device_table_is_valid(&config_data->devices)) {
    return false;
  }
  if (!config_wifi_is_valid(&config_data->wifi)) {
    return false;
  }
  uint16_t crc = GetCRCData((uint8_t *)config_data, CONFIG_CRC_DATA_LEN);
  return crc == config_data->crc || config_data->crc == 0U;
}

static bool config_v5_is_valid(const config_v5_t *config_data) {
  if (config_data == NULL) {
    return false;
  }
  if (memcmp(config_data->magic_number, default_config.magic_number, sizeof(config_data->magic_number)) != 0) {
    return false;
  }
  if (config_data->network_mode > NETWORK_MODE_WIFI) {
    return false;
  }
  if (!config_device_table_is_valid(&config_data->devices)) {
    return false;
  }
  uint16_t crc = GetCRCData((uint8_t *)config_data, CONFIG_V5_CRC_DATA_LEN);
  return crc == config_data->crc || config_data->crc == 0U;
}

static bool config_v4_is_valid(const config_v4_t *config_data) {
  if (config_data == NULL) {
    return false;
  }
  if (memcmp(config_data->magic_number, default_config.magic_number, sizeof(config_data->magic_number)) != 0) {
    return false;
  }
  if (config_data->network_mode > NETWORK_MODE_WIFI) {
    return false;
  }
  uint16_t crc = GetCRCData((uint8_t *)config_data, CONFIG_V4_CRC_DATA_LEN);
  return crc == config_data->crc || config_data->crc == 0U;
}

static bool config_legacy_is_valid(const config_legacy_t *config_data) {
  if (config_data == NULL) {
    return false;
  }
  if (memcmp(config_data->magic_number, default_config.magic_number, sizeof(config_data->magic_number)) != 0) {
    return false;
  }
  uint16_t crc = GetCRCData((uint8_t *)config_data, CONFIG_LEGACY_CRC_DATA_LEN);
  return crc == config_data->crc || config_data->crc == 0U;
}

static bool config_device_table_is_valid(const config_device_table_t *devices) {
  if (devices == NULL || devices->count > CONFIG_DEVICE_TABLE_MAX) {
    return false;
  }

  for (uint8_t index = 0U; index < devices->count; index++) {
    const config_device_entry_t *entry = &devices->entries[index];
    if (entry->enabled == 0U) {
      continue;
    }
    if (entry->slave_addr == 0U || entry->slave_addr > 247U || device_catalog_find(entry->service_id) == NULL) {
      return false;
    }
  }

  return true;
}

static bool config_wifi_is_valid(const network_wifi_config_t *wifi) {
  return wifi != NULL && wifi->ip_mode <= NETWORK_WIFI_IP_MODE_STATIC;
}

static bool config_device_table_has_enabled_entries(void) {
  for (uint8_t index = 0U; index < active_config.devices.count; index++) {
    const config_device_entry_t *entry = &active_config.devices.entries[index];
    if (entry->enabled != 0U && entry->slave_addr != 0U) {
      return true;
    }
  }
  return false;
}

static bool config_device_table_allows(uint16_t service_id, uint16_t manufacture_model, uint8_t slave_addr) {
  for (uint8_t index = 0U; index < active_config.devices.count; index++) {
    const config_device_entry_t *entry = &active_config.devices.entries[index];
    if (entry->enabled == 0U) {
      continue;
    }
    /*
     * 手工保存的现场设备表是最终接线白名单。任一侧型号为 0 表示暂不区分型号；
     * 两侧都非 0 时才要求一致，避免平台下发把未确认型号拉进轮询。
     */
    if (entry->service_id == service_id && entry->slave_addr == slave_addr &&
        (entry->manufacture_model == 0U || manufacture_model == 0U || entry->manufacture_model == manufacture_model)) {
      return true;
    }
  }
  return false;
}

bool config_saved_device_table_allows(uint16_t service_id, uint16_t manufacture_model, uint8_t slave_addr) {
  if (!config_device_table_has_enabled_entries()) {
    return true;
  }
  return config_device_table_allows(service_id, manufacture_model, slave_addr);
}

static void config_migrate_v5(const config_v5_t *saved_config) {
  if (saved_config == NULL) {
    config_reset_to_default();
    return;
  }

  memset(&active_config, 0, sizeof(active_config));
  memcpy(active_config.magic_number, saved_config->magic_number, sizeof(active_config.magic_number));
  memcpy(&active_config.auth, &saved_config->auth, sizeof(active_config.auth));
  memcpy(&active_config.mqtt, &saved_config->mqtt, sizeof(active_config.mqtt));
  memcpy(&active_config.ntp, &saved_config->ntp, sizeof(active_config.ntp));
  memcpy(&active_config.loop, &saved_config->loop, sizeof(active_config.loop));
  memcpy(&active_config.network_monitor, &saved_config->network_monitor, sizeof(active_config.network_monitor));
  active_config.network_mode =
#if BSP_HAS_AP6181
    NETWORK_MODE_WIFI;
#else
    saved_config->network_mode;
#endif
  memcpy(&active_config.wifi, &default_config.wifi, sizeof(active_config.wifi));
  memcpy(&active_config.devices, &saved_config->devices, sizeof(active_config.devices));
  memcpy(&active_config.log, &saved_config->log, sizeof(active_config.log));
  memcpy(active_config.md5, saved_config->md5, sizeof(active_config.md5));
  active_config.crc = 0U;
  LOG_INFO("EEPROM config migrated, WiFi config defaulted to empty");
  if (config_write_into_eeprom() != SUCCESS) {
    LOG_WARNING("EEPROM config migration write failed");
  }
}

static void config_migrate_v4(const config_v4_t *saved_config) {
  if (saved_config == NULL) {
    config_reset_to_default();
    return;
  }

  memset(&active_config, 0, sizeof(active_config));
  memcpy(active_config.magic_number, saved_config->magic_number, sizeof(active_config.magic_number));
  memcpy(&active_config.auth, &saved_config->auth, sizeof(active_config.auth));
  memcpy(&active_config.mqtt, &saved_config->mqtt, sizeof(active_config.mqtt));
  memcpy(&active_config.ntp, &saved_config->ntp, sizeof(active_config.ntp));
  memcpy(&active_config.loop, &saved_config->loop, sizeof(active_config.loop));
  memcpy(&active_config.network_monitor, &saved_config->network_monitor, sizeof(active_config.network_monitor));
  active_config.network_mode =
#if BSP_HAS_AP6181
    NETWORK_MODE_WIFI;
#else
    saved_config->network_mode;
#endif
  memcpy(&active_config.wifi, &default_config.wifi, sizeof(active_config.wifi));
  memset(&active_config.devices, 0, sizeof(active_config.devices));
  memcpy(&active_config.log, &saved_config->log, sizeof(active_config.log));
  memcpy(active_config.md5, saved_config->md5, sizeof(active_config.md5));
  active_config.crc = 0U;
  LOG_INFO("EEPROM config migrated, device table defaulted to empty");
  if (config_write_into_eeprom() != SUCCESS) {
    LOG_WARNING("EEPROM config migration write failed");
  }
}

static void config_migrate_legacy(const config_legacy_t *legacy_config) {
  if (legacy_config == NULL) {
    config_reset_to_default();
    return;
  }

  memset(&active_config, 0, sizeof(active_config));
  memcpy(active_config.magic_number, legacy_config->magic_number, sizeof(active_config.magic_number));
  memcpy(&active_config.auth, &legacy_config->auth, sizeof(active_config.auth));
  memcpy(&active_config.mqtt, &legacy_config->mqtt, sizeof(active_config.mqtt));
  memcpy(&active_config.ntp, &legacy_config->ntp, sizeof(active_config.ntp));
  memcpy(&active_config.loop, &legacy_config->loop, sizeof(active_config.loop));
  memcpy(&active_config.network_monitor, &legacy_config->network_monitor, sizeof(active_config.network_monitor));
  active_config.network_mode =
#if BSP_HAS_AP6181
    NETWORK_MODE_WIFI;
#else
    NETWORK_MODE_AUTO;
#endif
  memcpy(&active_config.wifi, &default_config.wifi, sizeof(active_config.wifi));
  memset(&active_config.devices, 0, sizeof(active_config.devices));
  memcpy(&active_config.log, &legacy_config->log, sizeof(active_config.log));
  memcpy(active_config.md5, legacy_config->md5, sizeof(active_config.md5));
  active_config.crc = 0U;
  LOG_INFO("EEPROM legacy config migrated, network_mode=%s devices=empty",
           config_network_mode_name(active_config.network_mode));
  if (config_write_into_eeprom() != SUCCESS) {
    LOG_WARNING("EEPROM legacy config migration write failed");
  }
}

bool config_parse_network_mode(const char *text, network_mode_t *mode) {
  if (text == NULL || mode == NULL) {
    return false;
  }

  if (strcmp(text, "auto") == 0) {
    *mode =
#if BSP_HAS_AP6181
      NETWORK_MODE_WIFI;
#else
      NETWORK_MODE_AUTO;
#endif
    return true;
  }
  if (strcmp(text, "wifi") == 0 || strcmp(text, "ap6181") == 0) {
#if BSP_HAS_AP6181
    *mode = NETWORK_MODE_WIFI;
    return true;
#else
    return false;
#endif
  }
  if (strcmp(text, "wired") == 0 || strcmp(text, "eth") == 0 || strcmp(text, "ch395q") == 0) {
#if BSP_HAS_CH395Q
    *mode = NETWORK_MODE_CH395Q;
    return true;
#else
    return false;
#endif
  }
  if (strcmp(text, "4g") == 0 || strcmp(text, "air724ug") == 0 || strcmp(text, "modem") == 0) {
#if BSP_HAS_AIR724UG
    *mode = NETWORK_MODE_AIR724UG;
    return true;
#else
    return false;
#endif
  }

  return false;
}

ErrorStatus config_apply_network_mode(network_mode_t mode, bool persist) {
  if (mode > NETWORK_MODE_WIFI) {
    return ERROR;
  }
#if BSP_HAS_AP6181
  if (mode != NETWORK_MODE_WIFI) {
    return ERROR;
  }
#endif
  active_config.network_mode = mode;
  ErrorStatus status = config_apply_runtime();
  if (status != SUCCESS) {
    return status;
  }
  return persist ? config_write_into_eeprom() : SUCCESS;
}

ErrorStatus config_apply_ch395q_network(bool persist) {
  active_config.network_mode = NETWORK_MODE_CH395Q;
  if (!ch395_board_init_network(active_config.mqtt.local_ip, active_config.mqtt.gateway_ip,
                                active_config.mqtt.mask_ip)) {
    return ERROR;
  }
  network_manager_set_mode(active_config.network_mode);
  return persist ? config_write_into_eeprom() : SUCCESS;
}

static void config_device_table_clear(void) {
  memset(&active_config.devices, 0, sizeof(active_config.devices));
}

static bool config_device_table_add(uint16_t service_id, uint16_t manufacture_model, uint8_t slave_addr) {
  if (slave_addr == 0U || slave_addr > 247U || device_catalog_find(service_id) == NULL) {
    return false;
  }

  /*
   * 当前每类设备适配器只维护一个实例，因此同一个 serviceId 以后写覆盖前写。
   * 后续若支持同类多设备，需要先扩展设备层状态数组，再放开这里的唯一约束。
   */
  for (uint8_t index = 0U; index < active_config.devices.count; index++) {
    config_device_entry_t *entry = &active_config.devices.entries[index];
    if (entry->service_id == service_id) {
      entry->manufacture_model = manufacture_model;
      entry->slave_addr = slave_addr;
      entry->enabled = 1U;
      return true;
    }
  }

  if (active_config.devices.count >= CONFIG_DEVICE_TABLE_MAX) {
    return false;
  }

  config_device_entry_t *entry = &active_config.devices.entries[active_config.devices.count++];
  entry->service_id = service_id;
  entry->manufacture_model = manufacture_model;
  entry->slave_addr = slave_addr;
  entry->enabled = 1U;
  return true;
}

static void config_print_device_config(void) {
  LOG_CMD_RESP("device.config count=%u max=%u", active_config.devices.count, CONFIG_DEVICE_TABLE_MAX);
  for (uint8_t index = 0U; index < active_config.devices.count; index++) {
    const config_device_entry_t *entry = &active_config.devices.entries[index];
    LOG_CMD_RESP("device.config index=%u serviceId=%u addr=%u model=%u enabled=%u name=%s", index, entry->service_id,
                 entry->slave_addr, entry->manufacture_model, entry->enabled, device_catalog_name(entry->service_id));
  }
}

static void config_clear_cmd_buf(void) {
  memset(config_cmd_recv_buf, 0, sizeof(config_cmd_recv_buf));
  config_cmd_recv_len = 0;
}

static bool config_consume_cmd_line(char *line, size_t line_size) {
  if (line == NULL || line_size == 0U || config_cmd_recv_len == 0U) {
    return false;
  }

  char *line_end = strpbrk(config_cmd_recv_buf, "\r\n");
  if (line_end == NULL) {
    return false;
  }

  size_t line_len = (size_t)(line_end - config_cmd_recv_buf);
  if (line_len >= line_size) {
    line_len = line_size - 1U;
  }
  memcpy(line, config_cmd_recv_buf, line_len);
  line[line_len] = '\0';

  char *next_line = line_end + 1;
  while (*next_line == '\r' || *next_line == '\n') {
    next_line++;
  }

  // 上位机会在连接后连续发送登录和 shell 协商命令；只消费第一行，保留后续行继续处理。
  size_t remaining = strlen(next_line);
  memmove(config_cmd_recv_buf, next_line, remaining + 1U);
  config_cmd_recv_len = (u16)remaining;
  return true;
}

static void config_print_prompt(void) {
  if (!config_auth_is_logged_in()) {
    config_auth_print_prompt();
    return;
  }

  if (shell_mode == CONFIG_SHELL_MODE_HUMAN && shell_prompt_enabled && !shell_prompt_printed) {
    config_shell_enable_cursor();
    config_shell_write(CONFIG_SHELL_PROMPT);
    shell_prompt_printed = true;
  }
}

static void config_shell_enable_cursor(void) {
  /*
   * Xshell 等 ANSI 终端支持 DEC 私有模式：
   * ?25h 显示光标，?12h 请求闪烁光标。若终端全局禁用闪烁，会以终端设置为准。
   */
  config_shell_write(CONFIG_ANSI_CURSOR_SHOW);
  config_shell_write(CONFIG_ANSI_CURSOR_BLINK);
}

static bool config_shell_should_echo_input(void) {
  return shell_mode == CONFIG_SHELL_MODE_HUMAN && shell_echo_enabled &&
         !(auth_stage == CONFIG_AUTH_STAGE_PASSWORD && !config_auth_is_logged_in());
}

static void config_shell_write(const char *text) {
  if (text == NULL) {
    return;
  }
  printf("%s", text);
}

static void config_shell_history_push(const char *line) {
  if (line == NULL || line[0] == '\0') {
    return;
  }
  if (shell_history_count > 0U && strcmp(shell_history[0], line) == 0) {
    return;
  }
  for (uint8_t i = CONFIG_SHELL_HISTORY_DEPTH - 1U; i > 0U; i--) {
    memcpy(shell_history[i], shell_history[i - 1U], sizeof(shell_history[i]));
  }
  strncpy(shell_history[0], line, sizeof(shell_history[0]) - 1U);
  shell_history[0][sizeof(shell_history[0]) - 1U] = '\0';
  if (shell_history_count < CONFIG_SHELL_HISTORY_DEPTH) {
    shell_history_count++;
  }
}

static void config_shell_load_history(uint8_t index) {
  if (index >= shell_history_count) {
    return;
  }
  strncpy(config_cmd_recv_buf, shell_history[index], sizeof(config_cmd_recv_buf) - 2U);
  config_cmd_recv_len = (u16)strlen(config_cmd_recv_buf);
}

static void config_auth_print_banner(void) {
  if (auth_banner_printed) {
    return;
  }

  ch395_board_status_t ch395_status = ch395_board_get_status();
  LOG_CMD_RESP("  ____  _____ __  __");
  LOG_CMD_RESP(" |  _ \\| ____|  \\/  |");
  LOG_CMD_RESP(" | |_) |  _| | |\\/| |");
  LOG_CMD_RESP(" |  __/| |___| |  | |");
  LOG_CMD_RESP(" |_|   |_____|_|  |_|");
  LOG_CMD_RESP("firmware=%s version=%s mcu=%s sysclk=%lu", BSP_BOARD_NAME, CONFIG_VERSION, BSP_MCU_NAME,
               HAL_RCC_GetSysClockFreq());
  LOG_CMD_RESP("ip.local=%s gateway=%s mask=%s", active_config.mqtt.local_ip, active_config.mqtt.gateway_ip,
               active_config.mqtt.mask_ip);
  LOG_CMD_RESP("mqtt.server=%s:%u probe=%s:%u", active_config.mqtt.ip, active_config.mqtt.port,
               active_config.network_monitor.probe_host, active_config.network_monitor.probe_port);
  LOG_CMD_RESP("network.mode=%s active=%s state=%s ch395.present=%u", config_network_mode_name(config_get_network_mode()),
               config_network_link_str(network_manager_get_active_link()), config_network_state_str(network_manager_get_state()),
               ch395_status.present ? 1U : 0U);
  auth_banner_printed = true;
}

static void config_auth_print_prompt(void) {
  if (shell_mode != CONFIG_SHELL_MODE_HUMAN || !shell_prompt_enabled || shell_prompt_printed) {
    return;
  }
  config_auth_print_banner();
  config_shell_enable_cursor();
  config_shell_write(auth_stage == CONFIG_AUTH_STAGE_PASSWORD ? CONFIG_PASSWORD_PROMPT : CONFIG_LOGIN_PROMPT);
  shell_prompt_printed = true;
}

static void config_auth_logout(const char *reason) {
  auth_stage = CONFIG_AUTH_STAGE_USERNAME;
  auth_user = CONFIG_AUTH_USER_NONE;
  memset(auth_pending_user, 0, sizeof(auth_pending_user));
  auth_banner_printed = false;
  shell_prompt_printed = false;
  /* 退出登录后必须回到人工可见的 login 提示。
   * 上位机常把 shell 切到 machine/prompt off，若超时后沿用该状态，现场会看不到登录提示，
   * 后续业务命令会被当成用户名处理并反复 login incorrect。 */
  shell_mode = CONFIG_SHELL_MODE_HUMAN;
  shell_prompt_enabled = true;
  shell_echo_enabled = true;
  if (reason != NULL && reason[0] != '\0') {
    LOG_CMD_RESP("logout: %s", reason);
  }
}

static bool config_auth_is_logged_in(void) {
  return auth_stage == CONFIG_AUTH_STAGE_AUTHENTICATED && auth_user != CONFIG_AUTH_USER_NONE;
}

static bool config_auth_is_root(void) {
  return config_auth_is_logged_in() && auth_user == CONFIG_AUTH_USER_ROOT;
}

static void config_auth_handle_line(char *line) {
  if (auth_stage == CONFIG_AUTH_STAGE_USERNAME) {
    if (strcmp(line, "root") == 0 || strcmp(line, "user") == 0) {
      strncpy(auth_pending_user, line, sizeof(auth_pending_user) - 1U);
      auth_stage = CONFIG_AUTH_STAGE_PASSWORD;
    } else {
      LOG_CMD_RESP("login incorrect");
      auth_banner_printed = false;
    }
    return;
  }

  config_auth_user_t pending_user = strcmp(auth_pending_user, "root") == 0 ? CONFIG_AUTH_USER_ROOT : CONFIG_AUTH_USER_USER;
  if (config_auth_password_matches(pending_user, line)) {
    auth_user = pending_user;
    auth_stage = CONFIG_AUTH_STAGE_AUTHENTICATED;
    auth_last_interaction_ms = HAL_GetTick();
    config_auth_print_welcome();
  } else {
    LOG_CMD_RESP("login incorrect");
    auth_stage = CONFIG_AUTH_STAGE_USERNAME;
    auth_user = CONFIG_AUTH_USER_NONE;
    memset(auth_pending_user, 0, sizeof(auth_pending_user));
    auth_banner_printed = false;
  }
}

static void config_auth_print_welcome(void) {
  LOG_CMD_RESP("welcome %s", config_auth_user_str(auth_user));
  if (auth_user == CONFIG_AUTH_USER_ROOT) {
    LOG_CMD_RESP("tips: use help, printenv, setenv/saveenv, passwd, logout");
  } else {
    LOG_CMD_RESP("tips: user is read-only; use help, status, printenv, logout");
  }
}

static const char *config_auth_user_str(config_auth_user_t user) {
  return user == CONFIG_AUTH_USER_ROOT ? "root" : user == CONFIG_AUTH_USER_USER ? "user" : "none";
}

static bool config_auth_password_matches(config_auth_user_t user, const char *password) {
  const char *expected = user == CONFIG_AUTH_USER_ROOT ? active_config.auth.root_password : active_config.auth.user_password;
  return password != NULL && strcmp(password, expected) == 0;
}

static bool config_require_root(void) {
  if (config_auth_is_root()) {
    return true;
  }
  LOG_CMD_RESP("permission denied: root required");
  return false;
}

static void config_dispatch_cmd(char *line) {
  if (line == NULL || line[0] == '\0') {
    return;
  }

  if (strcmp(line, "logout") == 0) {
    config_auth_logout("user request");
  } else if (strncmp(line, "passwd", 6) == 0) {
    if (config_require_root()) {
      config_handle_passwd_cmd(line + 6);
    }
  } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
    config_print_help();
  } else if (strcmp(line, "uname") == 0 || strcmp(line, "uname -a") == 0) {
    config_print_uname();
  } else if (strcmp(line, "uptime") == 0) {
    config_print_uptime();
  } else if (strcmp(line, "free") == 0) {
    config_print_free();
  } else if (strcmp(line, "dmesg") == 0) {
    LOG_CMD_RESP("dmesg: ring buffer not enabled; use live %s log output", BSP_DEBUG_UART_NAME);
  } else if (strcmp(line, "reboot") == 0) {
    if (config_require_root()) {
      LOG_CMD_RESP("rebooting");
      HAL_NVIC_SystemReset();
    }
  } else if (strcmp(line, "status") == 0) {
    config_print_status();
  } else if (strcmp(line, "rtos status") == 0) {
    config_print_rtos_status();
  } else if (strcmp(line, "printenv") == 0) {
    config_print_all();
  } else if (strncmp(line, "getenv ", 7) == 0) {
    config_handle_getenv_cmd(line + 7);
  } else if (strncmp(line, "setenv ", 7) == 0) {
    if (config_require_root()) {
      config_handle_setenv_cmd(line + 7);
    }
  } else if (strcmp(line, "saveenv") == 0 || strcmp(line, "save") == 0 || strcmp(line, "config save") == 0) {
    if (config_require_root()) {
      LOG_CMD_RESP("saveenv %s", config_write_into_eeprom() == SUCCESS ? "ok" : "failed");
    }
  } else if (strcmp(line, "env default") == 0 || strcmp(line, "reset") == 0 || strcmp(line, "config reset") == 0) {
    if (config_require_root()) {
      config_reset_to_default();
      LOG_CMD_RESP("env default ok");
    }
  } else if (strncmp(line, "loglevel", 8) == 0) {
    if (line[8] == '\0' || config_require_root()) {
      config_handle_log_level_cmd(line + 8);
    }
  } else if (strcmp(line, "ifconfig") == 0 || strcmp(line, "ip addr") == 0 || strcmp(line, "route") == 0) {
    config_handle_network_cmd("status");
  } else if (strncmp(line, "ping ", 5) == 0) {
    LOG_CMD_RESP("ping: ICMP not implemented; use nc -vz <host> <port>");
  } else if (strncmp(line, "nc ", 3) == 0) {
    if (config_require_root()) {
      config_handle_nc_cmd(line + 3);
    }
  } else if (strncmp(line, "netuse ", 7) == 0) {
    if (config_require_root()) {
      config_handle_netuse_cmd(line + 7);
    }
  } else if (strncmp(line, "eth ", 4) == 0) {
    if (strcmp(line + 4, "status") == 0 || config_require_root()) {
      config_handle_ch395_cmd(line + 4);
    }
  } else if (strncmp(line, "modem ", 6) == 0) {
    if (config_air_cmd_is_readonly(line + 6) || config_require_root()) {
      config_handle_air_cmd(line + 6);
    }
  } else if (strcmp(line, "date") == 0 || strncmp(line, "ntpdate", 7) == 0) {
    config_handle_time_cmd(line);
  } else if (strncmp(line, "shell ", 6) == 0) {
    config_handle_shell_cmd(line + 6);
  } else if (strncmp(line, "config ", 7) == 0) {
    bool readonly = strcmp(line + 7, "show") == 0 || strncmp(line + 7, "get ", 4) == 0;
    if (readonly || config_require_root()) {
      config_handle_config_cmd(line + 7);
    }
  } else if (strncmp(line, "network ", 8) == 0) {
    if (strcmp(line + 8, "status") == 0 || config_require_root()) {
      config_handle_network_cmd(line + 8);
    }
  } else if (strncmp(line, "mqtt ", 5) == 0) {
    config_handle_mqtt_cmd(line + 5);
  } else if (strncmp(line, "ch395 ", 6) == 0) {
    if (strcmp(line + 6, "status") == 0 || config_require_root()) {
      config_handle_ch395_cmd(line + 6);
    }
  } else if (strncmp(line, "air ", 4) == 0) {
    if (config_air_cmd_is_readonly(line + 4) || config_require_root()) {
      config_handle_air_cmd(line + 4);
    }
  } else if (strncmp(line, "time ", 5) == 0) {
    if (strcmp(line + 5, "status") == 0 || config_require_root()) {
      config_handle_time_cmd(line + 5);
    }
  } else if (strncmp(line, "modbus ", 7) == 0) {
    if (config_require_root()) {
      config_handle_modbus_cmd(line + 7);
    }
  } else if (strncmp(line, "rs485 ", 6) == 0) {
    if (strcmp(line + 6, "status") == 0 || config_require_root()) {
      config_handle_rs485_cmd(line + 6);
    }
  } else if (strncmp(line, "device ", 7) == 0) {
    if (strcmp(line + 7, "status") == 0 || config_require_root()) {
      config_handle_device_cmd(line + 7);
    }
  } else if (strncmp(line, "rtos ", 5) == 0) {
    LOG_CMD_RESP("usage: rtos status");
  } else if (strncmp(line, "get ", 4) == 0 || strncmp(line, "set ", 4) == 0) {
    if (strncmp(line, "get ", 4) == 0 || config_require_root()) {
      config_handle_config_cmd(line);
    }
  } else {
    LOG_CMD_RESP("unknown cmd: %s", line);
    LOG_CMD_RESP("type 'help' to show command flow");
  }
}

ErrorStatus config_get_value(const char *key, char *buf, size_t buf_size) {
  if (strcmp(key, "version") == 0) {
    snprintf(buf, buf_size, "%s", CONFIG_VERSION);
  } else if (strcmp(key, "log_level") == 0) {
    snprintf(buf, buf_size, "%s", log_get_current_level_str_value());
  } else if (strcmp(key, "mqtt_ip") == 0) {
    snprintf(buf, buf_size, "%s", active_config.mqtt.ip);
  } else if (strcmp(key, "mqtt_port") == 0) {
    snprintf(buf, buf_size, "%u", active_config.mqtt.port);
  } else if (strcmp(key, "mqtt_local_port") == 0) {
    snprintf(buf, buf_size, "%u", active_config.mqtt.local_port);
  } else if (strcmp(key, "mqtt_user") == 0 || strcmp(key, "mqtt_user_name") == 0) {
    snprintf(buf, buf_size, "%s", active_config.mqtt.user_name);
  } else if (strcmp(key, "mqtt_client_id") == 0) {
    snprintf(buf, buf_size, "%s", active_config.mqtt.client_id);
  } else if (strcmp(key, "mqtt_password_set") == 0) {
    snprintf(buf, buf_size, "%u", active_config.mqtt.password[0] == '\0' ? 0U : 1U);
  } else if (strcmp(key, "mqtt_keepalive") == 0) {
    snprintf(buf, buf_size, "%u", active_config.mqtt.keepalive);
  } else if (strcmp(key, "mqtt_sub_qos") == 0) {
    snprintf(buf, buf_size, "%u", active_config.mqtt.sub_qos);
  } else if (strcmp(key, "mqtt_pub_qos") == 0) {
    snprintf(buf, buf_size, "%u", active_config.mqtt.pub_qos);
  } else if (strcmp(key, "device_uid") == 0) {
    config_format_device_uid(buf, buf_size);
  } else if (strcmp(key, "local_ip") == 0) {
    snprintf(buf, buf_size, "%s", active_config.mqtt.local_ip);
  } else if (strcmp(key, "gateway") == 0) {
    snprintf(buf, buf_size, "%s", active_config.mqtt.gateway_ip);
  } else if (strcmp(key, "mask") == 0) {
    snprintf(buf, buf_size, "%s", active_config.mqtt.mask_ip);
  } else if (strcmp(key, "probe_host") == 0) {
    snprintf(buf, buf_size, "%s", active_config.network_monitor.probe_host);
  } else if (strcmp(key, "probe_port") == 0) {
    snprintf(buf, buf_size, "%u", active_config.network_monitor.probe_port);
  } else if (strcmp(key, "probe_interval_ms") == 0) {
    snprintf(buf, buf_size, "%lu", active_config.network_monitor.probe_interval_ms);
  } else if (strcmp(key, "network_mode") == 0) {
    snprintf(buf, buf_size, "%s", config_network_mode_name(active_config.network_mode));
  } else if (strcmp(key, "wifi_ssid") == 0) {
    snprintf(buf, buf_size, "%s", active_config.wifi.ssid);
  } else if (strcmp(key, "wifi_password_set") == 0) {
    snprintf(buf, buf_size, "%u", active_config.wifi.password[0] == '\0' ? 0U : 1U);
  } else if (strcmp(key, "wifi_ip_mode") == 0) {
    snprintf(buf, buf_size, "%s", config_wifi_ip_mode_str(active_config.wifi.ip_mode));
  } else if (strcmp(key, "wifi_local_ip") == 0) {
    snprintf(buf, buf_size, "%s", active_config.wifi.local_ip);
  } else if (strcmp(key, "wifi_gateway") == 0) {
    snprintf(buf, buf_size, "%s", active_config.wifi.gateway_ip);
  } else if (strcmp(key, "wifi_mask") == 0) {
    snprintf(buf, buf_size, "%s", active_config.wifi.mask_ip);
  } else if (strcmp(key, "wifi_dns") == 0) {
    snprintf(buf, buf_size, "%s", active_config.wifi.dns_ip);
  } else if (strcmp(key, "main_loop_interval") == 0) {
    snprintf(buf, buf_size, "%lu", active_config.loop.main_loop_interval);
  } else if (strcmp(key, "report_devices_data_interval") == 0) {
    snprintf(buf, buf_size, "%lu", active_config.loop.report_devices_data_interval);
  } else if (strcmp(key, "feed_watch_dog_interval") == 0) {
    snprintf(buf, buf_size, "%u", active_config.loop.feed_watch_dog_interval);
  } else if (strcmp(key, "feed_watch_dog_max_retries") == 0) {
    snprintf(buf, buf_size, "%u", active_config.loop.feed_watch_dog_max_retries);
  } else if (strcmp(key, "auth_timeout_ms") == 0) {
    snprintf(buf, buf_size, "%lu", CONFIG_AUTH_IDLE_TIMEOUT_MS);
  } else {
    return ERROR;
  }
  return SUCCESS;
}

ErrorStatus config_set_value(const char *key, const char *value) {
  if (strcmp(key, "log_level") == 0) {
    log_level_t level;
    if (!log_level_get_enum_value_by_str_value(value, &level)) {
      return ERROR;
    }
    active_config.log.level = level;
    return SUCCESS;
  } else if (strcmp(key, "mqtt_ip") == 0) {
    return config_set_string(active_config.mqtt.ip, sizeof(active_config.mqtt.ip), value);
  } else if (strcmp(key, "mqtt_port") == 0) {
    return config_set_u16(&active_config.mqtt.port, value);
  } else if (strcmp(key, "mqtt_local_port") == 0) {
    return config_set_u16(&active_config.mqtt.local_port, value);
  } else if (strcmp(key, "mqtt_user") == 0 || strcmp(key, "mqtt_user_name") == 0) {
    ErrorStatus status = config_set_string(active_config.mqtt.user_name, sizeof(active_config.mqtt.user_name), value);
    if (status == SUCCESS) {
      /* 用户名就是平台 collectorId/设备ID；改名后默认同步派生 clientId 和平台 MD5 密码。 */
      (void)config_set_mqtt_client_id("auto");
      (void)config_set_mqtt_password("auto");
    }
    return status;
  } else if (strcmp(key, "mqtt_client_id") == 0) {
    return config_set_mqtt_client_id(value);
  } else if (strcmp(key, "mqtt_password") == 0) {
    return config_set_mqtt_password(value);
  } else if (strcmp(key, "mqtt_keepalive") == 0) {
    return config_set_u16(&active_config.mqtt.keepalive, value);
  } else if (strcmp(key, "mqtt_sub_qos") == 0) {
    return config_set_u8_max(&active_config.mqtt.sub_qos, value, 1U);
  } else if (strcmp(key, "mqtt_pub_qos") == 0) {
    return config_set_u8_max(&active_config.mqtt.pub_qos, value, 1U);
  } else if (strcmp(key, "local_ip") == 0) {
    return config_set_string(active_config.mqtt.local_ip, sizeof(active_config.mqtt.local_ip), value);
  } else if (strcmp(key, "gateway") == 0) {
    return config_set_string(active_config.mqtt.gateway_ip, sizeof(active_config.mqtt.gateway_ip), value);
  } else if (strcmp(key, "mask") == 0) {
    return config_set_string(active_config.mqtt.mask_ip, sizeof(active_config.mqtt.mask_ip), value);
  } else if (strcmp(key, "probe_host") == 0) {
    return config_set_string(active_config.network_monitor.probe_host, sizeof(active_config.network_monitor.probe_host), value);
  } else if (strcmp(key, "probe_port") == 0) {
    return config_set_u16(&active_config.network_monitor.probe_port, value);
  } else if (strcmp(key, "probe_interval_ms") == 0) {
    return config_set_u32(&active_config.network_monitor.probe_interval_ms, value);
  } else if (strcmp(key, "network_mode") == 0) {
    network_mode_t mode = NETWORK_MODE_AUTO;
    if (!config_parse_network_mode(value, &mode)) {
      return ERROR;
    }
    active_config.network_mode = mode;
    return SUCCESS;
  } else if (strcmp(key, "wifi_ssid") == 0) {
    return config_set_string(active_config.wifi.ssid, sizeof(active_config.wifi.ssid), value);
  } else if (strcmp(key, "wifi_password") == 0) {
    return config_set_string(active_config.wifi.password, sizeof(active_config.wifi.password), value);
  } else if (strcmp(key, "wifi_ip_mode") == 0) {
    network_wifi_ip_mode_t mode = NETWORK_WIFI_IP_MODE_DHCP;
    if (!config_parse_wifi_ip_mode(value, &mode)) {
      return ERROR;
    }
    active_config.wifi.ip_mode = mode;
    return SUCCESS;
  } else if (strcmp(key, "wifi_local_ip") == 0) {
    return config_set_string(active_config.wifi.local_ip, sizeof(active_config.wifi.local_ip), value);
  } else if (strcmp(key, "wifi_gateway") == 0) {
    return config_set_string(active_config.wifi.gateway_ip, sizeof(active_config.wifi.gateway_ip), value);
  } else if (strcmp(key, "wifi_mask") == 0) {
    return config_set_string(active_config.wifi.mask_ip, sizeof(active_config.wifi.mask_ip), value);
  } else if (strcmp(key, "wifi_dns") == 0) {
    return config_set_string(active_config.wifi.dns_ip, sizeof(active_config.wifi.dns_ip), value);
  } else if (strcmp(key, "main_loop_interval") == 0) {
    return config_set_u32(&active_config.loop.main_loop_interval, value);
  } else if (strcmp(key, "report_devices_data_interval") == 0) {
    return config_set_u32_min(&active_config.loop.report_devices_data_interval, value,
                              CONFIG_MIN_REPORT_DEVICES_DATA_INTERVAL_MS);
  } else if (strcmp(key, "feed_watch_dog_interval") == 0) {
    return config_set_u16(&active_config.loop.feed_watch_dog_interval, value);
  } else if (strcmp(key, "feed_watch_dog_max_retries") == 0) {
    return config_set_u16(&active_config.loop.feed_watch_dog_max_retries, value);
  }
  return ERROR;
}

static ErrorStatus config_set_string(char *dst, size_t dst_size, const char *value) {
  if (value == NULL || util_strnlen(value, dst_size) >= dst_size) {
    return ERROR;
  }
  snprintf(dst, dst_size, "%s", value);
  return SUCCESS;
}

static ErrorStatus config_set_mqtt_client_id(const char *value) {
  if (!config_value_is_auto_token(value)) {
    return config_set_string(active_config.mqtt.client_id, sizeof(active_config.mqtt.client_id), value);
  }

  if (active_config.mqtt.user_name[0] == '\0') {
    return ERROR;
  }

  /* 旧平台约定 clientId 固定为 "c" + collectorId，上位机可用 auto 恢复这条规则。 */
  int written = snprintf(active_config.mqtt.client_id, sizeof(active_config.mqtt.client_id), "c%s",
                         active_config.mqtt.user_name);
  return written > 0 && (size_t)written < sizeof(active_config.mqtt.client_id) ? SUCCESS : ERROR;
}

static ErrorStatus config_set_mqtt_password(const char *value) {
  if (!config_value_is_auto_token(value)) {
    return config_set_string(active_config.mqtt.password, sizeof(active_config.mqtt.password), value);
  }

  if (active_config.mqtt.user_name[0] == '\0') {
    return ERROR;
  }

  /* 平台密码沿用旧工程规则：MD5("identify:" + collectorId)，避免现场手算密码。 */
  char plain[MQTT_USER_NAME_BUF_SIZE + 10U] = {0};
  (void)snprintf(plain, sizeof(plain), "identify:%s", active_config.mqtt.user_name);
  Md5GenerateStr(plain, (unsigned int)strlen(plain), active_config.mqtt.password);
  return SUCCESS;
}

static ErrorStatus config_set_u16(u16 *dst, const char *value) {
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed > MAX_PORT_NUMBER) {
    return ERROR;
  }
  *dst = (u16)parsed;
  return SUCCESS;
}

static ErrorStatus config_set_u32(u32 *dst, const char *value) {
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0') {
    return ERROR;
  }
  *dst = (u32)parsed;
  return SUCCESS;
}

static ErrorStatus config_set_u8_max(u8 *dst, const char *value, u8 max_value) {
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed > max_value) {
    return ERROR;
  }
  *dst = (u8)parsed;
  return SUCCESS;
}

static ErrorStatus config_set_u32_min(u32 *dst, const char *value, u32 min_value) {
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed < min_value) {
    return ERROR;
  }
  *dst = (u32)parsed;
  return SUCCESS;
}

static bool config_value_is_auto_token(const char *value) {
  return value != NULL &&
         (strcmp(value, "auto") == 0 || strcmp(value, "AUTO") == 0 || strcmp(value, "-") == 0);
}

static void config_format_device_uid(char *buf, size_t buf_size) {
  if (buf == NULL || buf_size == 0U) {
    return;
  }

  /* STM32F103 内置 96-bit UID，HAL 从 UID_BASE(0x1FFFF7E8) 读取，适合作为设备唯一编号来源。 */
  snprintf(buf, buf_size, "%08lX%08lX%08lX", (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(),
           (unsigned long)HAL_GetUIDw2());
}

static void config_print_help(void) {
  LOG_CMD_RESP("shell: help uname uptime free dmesg loglevel [level] reboot");
  LOG_CMD_RESP("auth: logout | passwd root|user <new-password>");
  LOG_CMD_RESP("env: printenv | getenv <key> | setenv <key> <value> | saveenv | env default | config apply|eeprom");
  LOG_CMD_RESP("net: ifconfig | ip addr | route | nc -vz <host> <port> | netuse auto|wifi|wired|4g");
  LOG_CMD_RESP("dev: eth status|init [local gw mask]|reset");
  LOG_CMD_RESP("dev: modem status|sim|radio|cell|ip|reset|at <AT>");
  LOG_CMD_RESP("mqtt: mqtt status|maintain|connect|sub|ping|reg|hb|last");
  LOG_CMD_RESP("rtos: rtos status");
  LOG_CMD_RESP("modbus: modbus read hold|input <slave> <reg> <count>");
  LOG_CMD_RESP("modbus: modbus scan hold|input <start> <end> <reg> <count>");
  LOG_CMD_RESP("rs485: rs485 status|baud <rate>");
  LOG_CMD_RESP("device: device status|poll|payload|report|clear|defaults|add <serviceId> <addr> [model]");
  LOG_CMD_RESP("device: device rs485 get <addr>|set addr <old> <new>|set baud <addr> <rate>");
  LOG_CMD_RESP("device: device switch <on|off> <1-8>|switch all-on|all-off");
  LOG_CMD_RESP("device: device ac cool|heat|off|diy <1-20>|learn-cool|learn-heat|learn-off|learn-diy <1-20>");
  LOG_CMD_RESP("shell: shell mode human|machine | shell prompt on|off | shell echo on|off");
  LOG_CMD_RESP("compat: config/network/ch395/air/time commands are kept as transition aliases");
  LOG_CMD_RESP("keys: version log_level device_uid mqtt_ip mqtt_port mqtt_local_port mqtt_user mqtt_client_id");
  LOG_CMD_RESP("keys: mqtt_password mqtt_password_set mqtt_keepalive mqtt_sub_qos mqtt_pub_qos local_ip gateway mask");
  LOG_CMD_RESP("keys: probe_host probe_port probe_interval_ms network_mode main_loop_interval report_devices_data_interval");
  LOG_CMD_RESP("keys: wifi_ssid wifi_password wifi_password_set wifi_ip_mode wifi_local_ip wifi_gateway wifi_mask wifi_dns");
  LOG_CMD_RESP("keys: feed_watch_dog_interval feed_watch_dog_max_retries");
  LOG_CMD_RESP("auth: root can modify config/control hardware; user can only view status/config");
}

static void config_print_uname(void) {
  char uid[25] = {0};
  config_format_device_uid(uid, sizeof(uid));
  LOG_CMD_RESP("firmware=%s version=%s mcu=%s sysclk=%lu", BSP_BOARD_NAME, CONFIG_VERSION, BSP_MCU_NAME,
               HAL_RCC_GetSysClockFreq());
  LOG_CMD_RESP("device.uid=%s", uid);
  LOG_CMD_RESP("primary=%s debug=%s/115200", BSP_PRIMARY_NETWORK_NAME, BSP_DEBUG_UART_NAME);
}

static void config_print_uptime(void) {
  uint32_t seconds = HAL_GetTick() / 1000U;
  LOG_CMD_RESP("uptime=%lu days=%lu %02lu:%02lu:%02lu", seconds, seconds / 86400UL, (seconds / 3600UL) % 24UL,
               (seconds / 60UL) % 60UL, seconds % 60UL);
}

static void config_print_free(void) {
#if APP_ENABLE_CMSIS_RTOS
  LOG_CMD_RESP("heap.free=%lu", (uint32_t)xPortGetFreeHeapSize());
  LOG_CMD_RESP("heap.min_free=%lu", (uint32_t)xPortGetMinimumEverFreeHeapSize());
#else
  LOG_CMD_RESP("heap.free=unknown");
#endif
}

static void config_print_status(void) {
  char uid[25] = {0};
  config_format_device_uid(uid, sizeof(uid));
  LOG_CMD_RESP("version=%s", CONFIG_VERSION);
  LOG_CMD_RESP("device.uid=%s", uid);
  LOG_CMD_RESP("log_level=%s", log_get_current_level_str_value());
  LOG_CMD_RESP("network.mode=%s", config_network_mode_name(config_get_network_mode()));
  LOG_CMD_RESP("network.active=%s", config_network_link_str(network_manager_get_active_link()));
  LOG_CMD_RESP("network.state=%s", config_network_state_str(network_manager_get_state()));
  LOG_CMD_RESP("mqtt.state=%s", config_mqtt_state_str(mqtt_client_get_state()));
  LOG_CMD_RESP("mqtt.registered=%u", platform_messages_is_registered() ? 1U : 0U);
  device_manager_print_status();
  LOG_CMD_RESP("network.probe=%s:%u every %lu ms", active_config.network_monitor.probe_host,
               active_config.network_monitor.probe_port, active_config.network_monitor.probe_interval_ms);
  LOG_CMD_RESP("mqtt=%s:%u local=%s:%u", active_config.mqtt.ip, active_config.mqtt.port, active_config.mqtt.local_ip,
               active_config.mqtt.local_port);
#if BSP_HAS_AP6181
  LOG_CMD_RESP("wifi.ssid=%s password_set=%u ip_mode=%s", active_config.wifi.ssid,
               active_config.wifi.password[0] == '\0' ? 0U : 1U, config_wifi_ip_mode_str(active_config.wifi.ip_mode));
  LOG_CMD_RESP("wifi.status=%s detail=%s backend=%s",
               ap6181_socket_status_name(ap6181_socket_get_status()), ap6181_socket_status_detail(),
               ap6181_socket_backend_name());
#endif
  LOG_CMD_RESP("time.ntp=disabled");
  ch395_board_status_t ch395_status = ch395_board_get_status();
  LOG_CMD_RESP("ch395.present=%u ver=0x%02X phy=0x%02X init=0x%02X", ch395_status.present ? 1U : 0U,
               ch395_status.version, ch395_status.phy_status, ch395_status.init_status);
  app_basic_status_t basic_status = app_basic_get_status();
  LOG_CMD_RESP("basic.loaded=%s size=%lu startup=%s import=%s import_name=%s import_slot=%s import_size=%lu",
               basic_status.loaded_name, (unsigned long)basic_status.loaded_size,
               eeprom_basic_script_startup_result_name(basic_status.startup_result),
               app_basic_import_status_name(basic_status.last_import_status), basic_status.last_import_name,
               eeprom_basic_script_slot_name(basic_status.last_import_slot),
               (unsigned long)basic_status.last_import_size);
  config_print_rtos_status();
}

static void config_print_rtos_status(void) {
  app_rtos_status_t rtos = app_rtos_get_status(HAL_GetTick(), 0U);

  /* 心跳由关键业务线程主动上报，现场可区分“调度未起”和“某线程卡死”。 */
  LOG_CMD_RESP("rtos.enabled=%u started=%u alive=%u", rtos.enabled ? 1U : 0U, rtos.started ? 1U : 0U,
               rtos.heartbeats_alive ? 1U : 0U);
  LOG_CMD_RESP("rtos.stack.main=%lu mqtt_rx=%lu basic=%lu debug=%lu watchdog=%lu", rtos.main_stack_size,
               rtos.mqtt_rx_stack_size, rtos.basic_stack_size, rtos.debug_stack_size, rtos.watchdog_stack_size);
  LOG_CMD_RESP("rtos.stack_free.main=%lu mqtt_rx=%lu basic=%lu debug=%lu watchdog=%lu", rtos.main_stack_free,
               rtos.mqtt_rx_stack_free, rtos.basic_stack_free, rtos.debug_stack_free, rtos.watchdog_stack_free);
  LOG_CMD_RESP("rtos.heartbeat.main=%lu mqtt_rx=%lu basic=%lu debug=%lu timeout=%lu", rtos.main_last_heartbeat_ms,
               rtos.mqtt_rx_last_heartbeat_ms, rtos.basic_last_heartbeat_ms, rtos.debug_last_heartbeat_ms,
               rtos.heartbeat_timeout_ms);
}

static void config_print_all(void) {
  LOG_CMD_RESP("version=%s", CONFIG_VERSION);
  LOG_CMD_RESP("log_level=%s", log_get_current_level_str_value());
  char uid[25] = {0};
  config_format_device_uid(uid, sizeof(uid));
  LOG_CMD_RESP("device_uid=%s", uid);
  LOG_CMD_RESP("mqtt_ip=%s", active_config.mqtt.ip);
  LOG_CMD_RESP("mqtt_port=%u", active_config.mqtt.port);
  LOG_CMD_RESP("mqtt_local_port=%u", active_config.mqtt.local_port);
  LOG_CMD_RESP("mqtt_user=%s", active_config.mqtt.user_name);
  LOG_CMD_RESP("mqtt_client_id=%s", active_config.mqtt.client_id);
  LOG_CMD_RESP("mqtt_password_set=%u", active_config.mqtt.password[0] == '\0' ? 0U : 1U);
  LOG_CMD_RESP("mqtt_keepalive=%u", active_config.mqtt.keepalive);
  LOG_CMD_RESP("mqtt_sub_qos=%u", active_config.mqtt.sub_qos);
  LOG_CMD_RESP("mqtt_pub_qos=%u", active_config.mqtt.pub_qos);
  LOG_CMD_RESP("local_ip=%s", active_config.mqtt.local_ip);
  LOG_CMD_RESP("gateway=%s", active_config.mqtt.gateway_ip);
  LOG_CMD_RESP("mask=%s", active_config.mqtt.mask_ip);
  LOG_CMD_RESP("probe_host=%s", active_config.network_monitor.probe_host);
  LOG_CMD_RESP("probe_port=%u", active_config.network_monitor.probe_port);
  LOG_CMD_RESP("probe_interval_ms=%lu", active_config.network_monitor.probe_interval_ms);
  LOG_CMD_RESP("network_mode=%s", config_network_mode_name(active_config.network_mode));
  LOG_CMD_RESP("wifi_ssid=%s", active_config.wifi.ssid);
  LOG_CMD_RESP("wifi_password_set=%u", active_config.wifi.password[0] == '\0' ? 0U : 1U);
  LOG_CMD_RESP("wifi_ip_mode=%s", config_wifi_ip_mode_str(active_config.wifi.ip_mode));
  LOG_CMD_RESP("wifi_local_ip=%s", active_config.wifi.local_ip);
  LOG_CMD_RESP("wifi_gateway=%s", active_config.wifi.gateway_ip);
  LOG_CMD_RESP("wifi_mask=%s", active_config.wifi.mask_ip);
  LOG_CMD_RESP("wifi_dns=%s", active_config.wifi.dns_ip);
  LOG_CMD_RESP("device_count=%u", active_config.devices.count);
  LOG_CMD_RESP("main_loop_interval=%lu", active_config.loop.main_loop_interval);
  LOG_CMD_RESP("report_devices_data_interval=%lu", active_config.loop.report_devices_data_interval);
  LOG_CMD_RESP("feed_watch_dog_interval=%u", active_config.loop.feed_watch_dog_interval);
  LOG_CMD_RESP("feed_watch_dog_max_retries=%u", active_config.loop.feed_watch_dog_max_retries);
  config_print_device_config();
}

static void config_handle_config_cmd(char *args) {
  char response[128] = {0};

  if (strcmp(args, "show") == 0) {
    config_print_all();
  } else if (strcmp(args, "apply") == 0) {
    LOG_CMD_RESP("config apply %s", config_apply_runtime() == SUCCESS ? "ok" : "failed");
  } else if (strcmp(args, "eeprom") == 0) {
    config_print_eeprom_status();
  } else if (strcmp(args, "save") == 0) {
    LOG_CMD_RESP("config save %s", config_write_into_eeprom() == SUCCESS ? "ok" : "failed");
  } else if (strcmp(args, "reset") == 0) {
    config_reset_to_default();
    LOG_CMD_RESP("config reset default ok");
  } else if (strncmp(args, "get ", 4) == 0) {
    char *key = args + 4;
    if (config_get_value(key, response, sizeof(response)) == SUCCESS) {
      LOG_CMD_RESP("%s=%s", key, response);
    } else {
      LOG_CMD_RESP("unknown key: %s", key);
    }
  } else if (strncmp(args, "set ", 4) == 0) {
    char *key = args + 4;
    char *value = strchr(key, ' ');
    if (value == NULL) {
      LOG_CMD_RESP("usage: config set <key> <value>");
    } else {
      *value++ = '\0';
      ErrorStatus status = config_set_value(key, value);
      LOG_CMD_RESP("config set %s %s", key, status == SUCCESS ? "ok" : "failed");
    }
  } else {
    LOG_CMD_RESP("usage: config show|get|set|apply|save|reset|eeprom");
  }
}

static void config_print_eeprom_status(void) {
  config_t saved_config = {{0}};
  ErrorStatus read_status = eeprom_read_config_data(&saved_config, sizeof(saved_config));
  if (read_status != SUCCESS) {
    LOG_CMD_RESP("eeprom.read=failed");
    return;
  }

  uint16_t crc = GetCRCData((uint8_t *)&saved_config, CONFIG_CRC_DATA_LEN);
  bool magic_ok = memcmp(saved_config.magic_number, default_config.magic_number, sizeof(saved_config.magic_number)) == 0;
  bool valid = magic_ok && saved_config.network_mode <= NETWORK_MODE_WIFI &&
               config_device_table_is_valid(&saved_config.devices) && config_wifi_is_valid(&saved_config.wifi) &&
               (crc == saved_config.crc || saved_config.crc == 0U);

  /* 只打印安全的诊断字段，不输出账号密码等敏感配置。 */
  LOG_CMD_RESP("eeprom.read=ok");
  LOG_CMD_RESP("eeprom.magic=%u crc=0x%04X calc=0x%04X valid=%u", magic_ok ? 1U : 0U, saved_config.crc, crc,
               valid ? 1U : 0U);
  LOG_CMD_RESP("eeprom.probe_interval_ms=%lu", saved_config.network_monitor.probe_interval_ms);
  LOG_CMD_RESP("eeprom.network_mode=%s", config_network_mode_name(saved_config.network_mode));
  LOG_CMD_RESP("eeprom.device_count=%u", saved_config.devices.count);

  eeprom_basic_script_lifecycle_state_t lifecycle;
  if (eeprom_get_basic_script_lifecycle_state(&lifecycle) != SUCCESS) {
    LOG_CMD_RESP("eeprom.basic.lifecycle=read-failed");
    return;
  }

  LOG_CMD_RESP("eeprom.basic.lifecycle.valid=%u", lifecycle.valid ? 1U : 0U);
  LOG_CMD_RESP("eeprom.basic.lifecycle.active=%s", eeprom_basic_script_slot_name(lifecycle.active_slot));
  LOG_CMD_RESP("eeprom.basic.lifecycle.candidate=%s",
               lifecycle.has_candidate ? eeprom_basic_script_slot_name(lifecycle.candidate_slot) : "none");
  LOG_CMD_RESP("eeprom.basic.lifecycle.previous=%s",
               lifecycle.has_previous ? eeprom_basic_script_slot_name(lifecycle.previous_slot) : "none");
  LOG_CMD_RESP("eeprom.basic.lifecycle.result=%s",
               eeprom_basic_script_startup_result_name(lifecycle.last_result));
  LOG_CMD_RESP("eeprom.basic.lifecycle.boot_count=%lu rollback_count=%lu",
               (unsigned long)lifecycle.boot_count, (unsigned long)lifecycle.rollback_count);
  LOG_CMD_RESP("eeprom.basic.lifecycle.last_attempt=%s last_success=%s last_failed=%s",
               eeprom_basic_script_slot_name(lifecycle.last_attempt_slot),
               eeprom_basic_script_slot_name(lifecycle.last_success_slot),
               eeprom_basic_script_slot_name(lifecycle.last_failed_slot));
}

static void config_handle_shell_cmd(char *args) {
  if (strcmp(args, "mode human") == 0) {
    shell_mode = CONFIG_SHELL_MODE_HUMAN;
    shell_prompt_enabled = true;
    shell_echo_enabled = true;
    shell_prompt_printed = false;
    LOG_CMD_RESP("OK cmd=shell.mode mode=human");
  } else if (strcmp(args, "mode machine") == 0) {
    shell_mode = CONFIG_SHELL_MODE_MACHINE;
    LOG_CMD_RESP("OK cmd=shell.mode mode=machine");
  } else if (strcmp(args, "prompt on") == 0) {
    shell_prompt_enabled = true;
    LOG_CMD_RESP("OK cmd=shell.prompt enabled=1");
  } else if (strcmp(args, "prompt off") == 0) {
    shell_prompt_enabled = false;
    LOG_CMD_RESP("OK cmd=shell.prompt enabled=0");
  } else if (strcmp(args, "echo on") == 0) {
    shell_echo_enabled = true;
    LOG_CMD_RESP("OK cmd=shell.echo enabled=1");
  } else if (strcmp(args, "echo off") == 0) {
    shell_echo_enabled = false;
    LOG_CMD_RESP("OK cmd=shell.echo enabled=0");
  } else {
    LOG_CMD_RESP("usage: shell mode human|machine | shell prompt on|off | shell echo on|off");
  }
}

static void config_handle_passwd_cmd(char *args) {
  while (args != NULL && *args == ' ') {
    args++;
  }
  if (args == NULL || args[0] == '\0') {
    LOG_CMD_RESP("usage: passwd root|user <new-password>");
    return;
  }

  char *password = strchr(args, ' ');
  if (password == NULL) {
    LOG_CMD_RESP("usage: passwd root|user <new-password>");
    return;
  }
  *password++ = '\0';
  if (password[0] == '\0') {
    LOG_CMD_RESP("passwd failed: empty password");
    return;
  }

  ErrorStatus status = ERROR;
  if (strcmp(args, "root") == 0) {
    status = config_set_string(active_config.auth.root_password, sizeof(active_config.auth.root_password), password);
  } else if (strcmp(args, "user") == 0) {
    status = config_set_string(active_config.auth.user_password, sizeof(active_config.auth.user_password), password);
  }

  if (status == SUCCESS) {
    LOG_CMD_RESP("passwd %s ok", args);
    LOG_CMD_RESP("run saveenv to persist password changes");
  } else {
    LOG_CMD_RESP("passwd %s failed", args);
  }
}

static void config_handle_getenv_cmd(char *args) {
  char response[128] = {0};
  if (args == NULL || args[0] == '\0') {
    LOG_CMD_RESP("usage: getenv <key>");
    return;
  }
  if (config_get_value(args, response, sizeof(response)) == SUCCESS) {
    LOG_CMD_RESP("%s=%s", args, response);
  } else {
    LOG_CMD_RESP("unknown key: %s", args);
  }
}

static void config_handle_setenv_cmd(char *args) {
  if (args == NULL || args[0] == '\0') {
    LOG_CMD_RESP("usage: setenv <key> <value>");
    return;
  }
  char *value = strchr(args, ' ');
  if (value == NULL) {
    LOG_CMD_RESP("usage: setenv <key> <value>");
    return;
  }
  *value++ = '\0';
  ErrorStatus status = config_set_value(args, value);
  if (strcmp(args, "mqtt_password") == 0) {
    LOG_CMD_RESP("setenv mqtt_password %s", status == SUCCESS ? "ok" : "failed");
    return;
  }
  LOG_CMD_RESP("setenv %s %s", args, status == SUCCESS ? "ok" : "failed");
}

static void config_handle_log_level_cmd(char *args) {
  while (args != NULL && *args == ' ') {
    args++;
  }
  if (args == NULL || args[0] == '\0') {
    LOG_CMD_RESP("loglevel=%s", log_get_current_level_str_value());
    return;
  }
  LOG_CMD_RESP("loglevel %s", config_set_value("log_level", args) == SUCCESS ? "ok" : "failed");
}

static void config_handle_ch395_cmd(char *args) {
  if (strcmp(args, "status") == 0) {
    uint8_t check_result = ch395_cmd_check_exist(CH395_CHECK_TEST_DATA);
    ch395_board_status_t status = ch395_board_get_status();
    uint8_t version = check_result == CH395_CHECK_EXPECTED ? ch395_cmd_get_ver() : status.version;
    uint8_t phy_status = check_result == CH395_CHECK_EXPECTED ? ch395_cmd_get_phy_status() : status.phy_status;
    LOG_CMD_RESP("ch395.check=0x%02X expected=0x%02X boot_check=0x%02X", check_result, CH395_CHECK_EXPECTED,
                 status.check_result);
    LOG_CMD_RESP("ch395.present=%u", check_result == CH395_CHECK_EXPECTED ? 1U : (status.present ? 1U : 0U));
    LOG_CMD_RESP("ch395.version=0x%02X", version);
    LOG_CMD_RESP("ch395.phy=0x%02X", phy_status);
    LOG_CMD_RESP("ch395.init=0x%02X", status.init_status);
  } else if (strncmp(args, "init", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
    const char *local_ip = active_config.mqtt.local_ip;
    const char *gateway_ip = active_config.mqtt.gateway_ip;
    const char *mask_ip = active_config.mqtt.mask_ip;
    char *p = args + 4;
    while (*p == ' ') {
      p++;
    }
    if (*p != '\0') {
      /* EEPROM 里若残留未终止的旧字符串，普通 eth init 会被脏配置挡住。
       * 允许现场显式传入三段 IPv4，先验证 CH395Q 硬件链路和 socket 层，再回头修配置落盘。 */
      char *local_arg = p;
      char *gateway_arg = strchr(local_arg, ' ');
      if (gateway_arg == NULL) {
        LOG_CMD_RESP("usage: ch395 init [local_ip gateway mask]");
        return;
      }
      *gateway_arg++ = '\0';
      while (*gateway_arg == ' ') {
        gateway_arg++;
      }
      char *mask_arg = strchr(gateway_arg, ' ');
      if (mask_arg == NULL) {
        LOG_CMD_RESP("usage: ch395 init [local_ip gateway mask]");
        return;
      }
      *mask_arg++ = '\0';
      while (*mask_arg == ' ') {
        mask_arg++;
      }
      if (*mask_arg == '\0' || strchr(mask_arg, ' ') != NULL) {
        LOG_CMD_RESP("usage: ch395 init [local_ip gateway mask]");
        return;
      }
      local_ip = local_arg;
      gateway_ip = gateway_arg;
      mask_ip = mask_arg;
      LOG_CMD_RESP("ch395.init.config local=%s gateway=%s mask=%s", local_ip, gateway_ip, mask_ip);
    }
    bool ok = ch395_board_init_network(local_ip, gateway_ip, mask_ip);
    LOG_CMD_RESP("ch395 init %s", ok ? "ok" : "failed");
    ch395_board_status_t status = ch395_board_get_status();
    LOG_CMD_RESP("ch395.version=0x%02X phy=0x%02X init=0x%02X", status.version, status.phy_status, status.init_status);
  } else if (strcmp(args, "reset") == 0) {
    ch395_board_init_default();
    LOG_CMD_RESP("ch395 reset ok");
    ch395_board_status_t status = ch395_board_get_status();
    LOG_CMD_RESP("ch395.present=%u version=0x%02X", status.present ? 1U : 0U, status.version);
  } else {
    LOG_CMD_RESP("usage: ch395 status|init [local gateway mask]|reset");
  }
}

static const char *config_hal_status_str(HAL_StatusTypeDef status) {
  switch (status) {
  case HAL_OK:
    return "ok";
  case HAL_ERROR:
    return "error";
  case HAL_BUSY:
    return "busy";
  case HAL_TIMEOUT:
    return "timeout";
  default:
    return "unknown";
  }
}

static void config_handle_network_cmd(char *args) {
  if (strcmp(args, "status") == 0) {
    LOG_CMD_RESP("network.mode=%s", config_network_mode_name(config_get_network_mode()));
    LOG_CMD_RESP("network.active=%s", config_network_link_str(network_manager_get_active_link()));
    LOG_CMD_RESP("network.state=%s", config_network_state_str(network_manager_get_state()));
    LOG_CMD_RESP("network.socket_ready=%u", network_socket_active_link_ready() ? 1U : 0U);
    LOG_CMD_RESP("network.probe=%s:%u", active_config.network_monitor.probe_host, active_config.network_monitor.probe_port);
#if BSP_HAS_AP6181
    LOG_CMD_RESP("network.wifi=%s enabled=%u irq=%u", bsp_ap6181_pin_map(), bsp_ap6181_is_enabled() ? 1U : 0U,
                 bsp_ap6181_read_irq() == GPIO_PIN_SET ? 1U : 0U);
    LOG_CMD_RESP("network.wifi_cfg ssid=%s password_set=%u ip_mode=%s", active_config.wifi.ssid,
                 active_config.wifi.password[0] == '\0' ? 0U : 1U,
                 config_wifi_ip_mode_str(active_config.wifi.ip_mode));
    LOG_CMD_RESP("network.wifi_status=%s detail=%s backend=%s",
                 ap6181_socket_status_name(ap6181_socket_get_status()), ap6181_socket_status_detail(),
                 ap6181_socket_backend_name());
#endif
    LOG_CMD_RESP("network.isolation ch395_rsti_low=%u air_rst_low=%u",
                 bsp_ch395_is_reset_asserted() ? 1U : 0U, bsp_air724_is_reset_asserted() ? 1U : 0U);
  } else if (strcmp(args, "probe") == 0) {
    bool ok = network_manager_ch395q_probe_now();
    LOG_CMD_RESP("network probe CH395Q %s", ok ? "ok" : "failed");
    network_manager_force_probe();
  } else if (strncmp(args, "use ", 4) == 0) {
    network_mode_t mode = NETWORK_MODE_AUTO;
    if (!config_parse_network_mode(args + 4, &mode)) {
      LOG_CMD_RESP("usage: network status|probe|use auto|wifi|wired|4g");
      return;
    }
    ErrorStatus status = config_apply_network_mode(mode, true);
    LOG_CMD_RESP("network.mode=%s save=%s", config_network_mode_name(mode), status == SUCCESS ? "ok" : "failed");
  } else {
    LOG_CMD_RESP("usage: network status|probe|use auto|wifi|wired|4g");
  }
}

static void config_handle_netuse_cmd(char *args) {
  network_mode_t mode = NETWORK_MODE_AUTO;
  if (!config_parse_network_mode(args, &mode)) {
    LOG_CMD_RESP("usage: netuse auto|wifi|wired|4g");
    return;
  }

  ErrorStatus status = config_apply_network_mode(mode, true);
  LOG_CMD_RESP("netuse %s %s", config_network_mode_name(mode), status == SUCCESS ? "ok" : "failed");
}

static void config_handle_nc_cmd(char *args) {
  if (strncmp(args, "-vz ", 4) != 0) {
    LOG_CMD_RESP("usage: nc -vz <host> <port>");
    return;
  }

  char *host = args + 4;
  char *port_text = strchr(host, ' ');
  if (port_text == NULL) {
    LOG_CMD_RESP("usage: nc -vz <host> <port>");
    return;
  }
  *port_text++ = '\0';
  uint32_t port = strtoul(port_text, NULL, 10);
  if (port == 0U || port > MAX_PORT_NUMBER) {
    LOG_CMD_RESP("nc: invalid port");
    return;
  }

  bool ok = network_socket_probe_tcp_port(host, (uint16_t)port);
  LOG_CMD_RESP("nc %s via %s %s:%lu", ok ? "ok" : "failed", network_socket_active_link_name(), host, port);
}

static void config_handle_mqtt_cmd(char *args) {
  if (strcmp(args, "status") == 0) {
    LOG_CMD_RESP("mqtt.server=%s:%u", active_config.mqtt.ip, active_config.mqtt.port);
    LOG_CMD_RESP("mqtt.local=%s:%u", active_config.mqtt.local_ip, active_config.mqtt.local_port);
    LOG_CMD_RESP("mqtt.client_id=%s", active_config.mqtt.client_id);
    LOG_CMD_RESP("mqtt.user=%s", active_config.mqtt.user_name);
    LOG_CMD_RESP("mqtt.password_set=%u", active_config.mqtt.password[0] == '\0' ? 0U : 1U);
    LOG_CMD_RESP("mqtt.keepalive=%u", active_config.mqtt.keepalive);
    LOG_CMD_RESP("mqtt.qos.sub=%u pub=%u", active_config.mqtt.sub_qos, active_config.mqtt.pub_qos);
    LOG_CMD_RESP("mqtt.state=%s", config_mqtt_state_str(mqtt_client_get_state()));
    LOG_CMD_RESP("mqtt.flags=0x%04X", mqtt_client_get_packet_flags());
    LOG_CMD_RESP("mqtt.last_puback_id=%u", mqtt_client_get_last_puback_id());
    LOG_CMD_RESP("platform.registered=%u", platform_messages_is_registered() ? 1U : 0U);
  } else if (strcmp(args, "connect") == 0) {
    bool ok = mqtt_client_connect();
    LOG_CMD_RESP("mqtt connect %s", ok ? "ok" : "failed");
  } else if (strcmp(args, "maintain") == 0 || strcmp(args, "auto") == 0) {
    bool ok = mqtt_client_maintain(HAL_GetTick(), active_config.loop.keep_in_touch_with_server_interval);
    LOG_CMD_RESP("mqtt maintain %s", ok ? "ok" : "failed");
  } else if (strcmp(args, "sub") == 0 || strcmp(args, "subscribe") == 0) {
    bool ok = mqtt_client_subscribe_platform_topics();
    LOG_CMD_RESP("mqtt subscribe %s", ok ? "ok" : "failed");
  } else if (strcmp(args, "ping") == 0) {
    bool ok = mqtt_client_ping();
    LOG_CMD_RESP("mqtt ping %s", ok ? "ok" : "failed");
  } else if (strcmp(args, "reg") == 0 || strcmp(args, "register") == 0) {
    bool ok = platform_messages_request_register_info();
    LOG_CMD_RESP("mqtt register request %s", ok ? "ok" : "failed");
  } else if (strcmp(args, "hb") == 0 || strcmp(args, "heartbeat") == 0) {
    bool ok = platform_messages_publish_heartbeat(HAL_GetTick() / 1000U);
    LOG_CMD_RESP("mqtt heartbeat %s", ok ? "ok" : "failed");
  } else if (strcmp(args, "last") == 0) {
    LOG_CMD_RESP("platform.last_register=%s", platform_messages_last_register_info());
    LOG_CMD_RESP("platform.last_update=%s", platform_messages_last_update_response());
    LOG_CMD_RESP("platform.last_command=%s", platform_messages_last_command());
  } else {
    LOG_CMD_RESP("usage: mqtt status|maintain|connect|sub|ping|reg|hb|last");
  }
}

static void config_handle_air_cmd(char *args) {
  static const char *const status_commands[] = {
    "AT",       "AT+CPIN?",  "AT+CSQ",    "AT+CREG?",  "AT+CGREG?",
    "AT+CEREG?", "AT+CGATT?", "AT+COPS?", "AT+CGACT?", "AT+CGPADDR",
  };
  static const char *const status_labels[] = {
    "alive", "cpin", "csq", "creg", "cgreg", "cereg", "cgatt", "cops", "cgact", "cgpaddr",
  };
  static const char *const sim_commands[] = {
    "AT+CPIN?",
    "AT+CCID",
    "AT+CIMI",
    "AT+CGSN",
  };
  static const char *const sim_labels[] = {
    "cpin",
    "ccid",
    "imsi",
    "imei",
  };
  static const char *const radio_commands[] = {
    "AT+CSQ",
    "AT+COPS?",
    "AT+CREG?",
    "AT+CGREG?",
    "AT+CEREG?",
  };
  static const char *const radio_labels[] = {
    "csq",
    "cops",
    "creg",
    "cgreg",
    "cereg",
  };
  static const char *const cell_commands[] = {
    "AT+CREG=2",
    "AT+CREG?",
    "AT+CGREG=2",
    "AT+CGREG?",
    "AT+CEREG=2",
    "AT+CEREG?",
    "AT+CENG?",
  };
  static const char *const cell_labels[] = {
    "creg_mode",
    "creg",
    "cgreg_mode",
    "cgreg",
    "cereg_mode",
    "cereg",
    "ceng",
  };
  static const char *const ip_commands[] = {
    "AT+CGATT?",
    "AT+CGACT?",
    "AT+CGPADDR",
  };
  static const char *const ip_labels[] = {
    "cgatt",
    "cgact",
    "cgpaddr",
  };

  if (strcmp(args, "state") == 0) {
    LOG_CMD_RESP("modem.netstate=%u", bsp_air724_read_netstate() == GPIO_PIN_SET ? 1U : 0U);
  } else if (strcmp(args, "status") == 0 || strcmp(args, "info") == 0) {
    LOG_CMD_RESP("modem.netstate=%u", bsp_air724_read_netstate() == GPIO_PIN_SET ? 1U : 0U);
    config_air724_print_group("status", status_commands, status_labels,
                              sizeof(status_commands) / sizeof(status_commands[0]), CONFIG_AIR_AT_SHORT_TIMEOUT_MS);
  } else if (strcmp(args, "sim") == 0) {
    config_air724_print_group("sim", sim_commands, sim_labels, sizeof(sim_commands) / sizeof(sim_commands[0]),
                              CONFIG_AIR_AT_SHORT_TIMEOUT_MS);
  } else if (strcmp(args, "radio") == 0 || strcmp(args, "signal") == 0) {
    config_air724_print_group("radio", radio_commands, radio_labels, sizeof(radio_commands) / sizeof(radio_commands[0]),
                              CONFIG_AIR_AT_SHORT_TIMEOUT_MS);
  } else if (strcmp(args, "cell") == 0) {
    LOG_CMD_RESP("modem.cell.note=CREG/CGREG/CEREG mode 2 prints LAC/TAC and Cell ID when module supports it");
    /* CREG/CGREG/CEREG=2 只调整注册状态上报格式，用于现场读取 LAC/TAC/CI，不会发起拨号或改变 PDP。 */
    config_air724_print_group("cell", cell_commands, cell_labels, sizeof(cell_commands) / sizeof(cell_commands[0]),
                              CONFIG_AIR_AT_LONG_TIMEOUT_MS);
  } else if (strcmp(args, "ip") == 0 || strcmp(args, "net") == 0) {
    config_air724_print_group("ip", ip_commands, ip_labels, sizeof(ip_commands) / sizeof(ip_commands[0]),
                              CONFIG_AIR_AT_SHORT_TIMEOUT_MS);
  } else if (strcmp(args, "reset") == 0) {
    bsp_air724_reset();
    LOG_CMD_RESP("modem reset ok");
  } else if (strncmp(args, "at ", 3) == 0) {
    config_air724_print_at("at", args + 3, CONFIG_AIR_AT_LONG_TIMEOUT_MS);
  } else {
    LOG_CMD_RESP("usage: modem state|status|sim|radio|cell|ip|reset|at <AT command>");
  }
}

static bool config_air_cmd_is_readonly(const char *args) {
  return args != NULL &&
         (strcmp(args, "state") == 0 || strcmp(args, "status") == 0 || strcmp(args, "info") == 0 ||
          strcmp(args, "radio") == 0 || strcmp(args, "signal") == 0 || strcmp(args, "ip") == 0 ||
          strcmp(args, "net") == 0);
}

static void config_air724_print_group(const char *group, const char *const *commands, const char *const *labels,
                                      size_t count, uint32_t timeout_ms) {
  LOG_CMD_RESP("modem.%s.count=%u", group, (unsigned int)count);
  for (size_t index = 0U; index < count; index++) {
    config_air724_print_at(labels[index], commands[index], timeout_ms);
  }
}

static void config_air724_print_at(const char *label, const char *command, uint32_t timeout_ms) {
  char response[CONFIG_AIR_AT_RESPONSE_SIZE] = {0};
  HAL_StatusTypeDef status = bsp_air724_at_command(command, response, sizeof(response), timeout_ms);
  LOG_CMD_RESP("modem.%s.cmd=%s status=%s", label, command, config_hal_status_str(status));
  if (response[0] != '\0') {
    config_air724_print_response_lines(label, response);
  }
}

static void config_air724_print_response_lines(const char *label, const char *response) {
  char line[128] = {0};
  size_t length = 0U;

  for (const char *cursor = response; cursor != NULL && *cursor != '\0'; cursor++) {
    char ch = *cursor;
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (length > 0U) {
        line[length] = '\0';
        LOG_CMD_RESP("modem.%s.line=%s", label, line);
        length = 0U;
      }
      continue;
    }
    if (length < sizeof(line) - 1U) {
      line[length++] = ch;
    }
  }

  if (length > 0U) {
    line[length] = '\0';
    LOG_CMD_RESP("modem.%s.line=%s", label, line);
  }
}

static void config_handle_time_cmd(char *args) {
  (void)args;
  /* 本机闭环只保留 MQTT；NTP/UDP 在当前无真实网络场景没有业务意义，串口入口也明确返回禁用。 */
  LOG_CMD_RESP("time.ntp=disabled");
  LOG_CMD_RESP("time.clock=rtc-local");
}

static void config_handle_modbus_cmd(char *args) {
  char *action = strtok(args, " ");
  if (action == NULL) {
    LOG_CMD_RESP("usage: modbus read hold|input <slave> <reg> <count>");
    LOG_CMD_RESP("usage: modbus scan hold|input <start> <end> <reg> <count>");
    return;
  }

  if (strcmp(action, "read") == 0) {
    char *kind = strtok(NULL, " ");
    char *slave_text = strtok(NULL, " ");
    char *reg_text = strtok(NULL, " ");
    char *count_text = strtok(NULL, " ");
    uint32_t slave = 0U;
    uint32_t reg = 0U;
    uint32_t count = 0U;
    if (kind == NULL || !config_parse_u32_range(slave_text, 1U, 247U, &slave) ||
        !config_parse_u32_range(reg_text, 0U, 0xFFFFU, &reg) ||
        !config_parse_u32_range(count_text, 1U, CONFIG_MODBUS_MAX_READ_REGS, &count)) {
      LOG_CMD_RESP("usage: modbus read hold|input <slave 1-247> <reg> <count 1-%u>", CONFIG_MODBUS_MAX_READ_REGS);
      return;
    }

    uint8_t data[CONFIG_MODBUS_MAX_READ_REGS * 2U] = {0};
    uint8_t error_code = 0U;
    bool ok = config_modbus_read_registers(kind, (uint8_t)slave, (uint16_t)reg, (uint16_t)count, data, &error_code);
    if (ok) {
      config_modbus_print_registers("modbus read ok", kind, (uint8_t)slave, (uint16_t)reg, (uint16_t)count, data);
    } else {
      LOG_CMD_RESP("modbus read failed type=%s slave=%lu reg=0x%04lX count=%lu err=0x%02X", kind, slave, reg, count,
                   error_code);
    }
  } else if (strcmp(action, "scan") == 0) {
    char *kind = strtok(NULL, " ");
    char *start_text = strtok(NULL, " ");
    char *end_text = strtok(NULL, " ");
    char *reg_text = strtok(NULL, " ");
    char *count_text = strtok(NULL, " ");
    uint32_t start = 0U;
    uint32_t end = 0U;
    uint32_t reg = 0U;
    uint32_t count = 0U;
    if (kind == NULL || !config_parse_u32_range(start_text, 1U, 247U, &start) ||
        !config_parse_u32_range(end_text, 1U, 247U, &end) || end < start ||
        (end - start + 1U) > CONFIG_MODBUS_SCAN_MAX_SLAVES ||
        !config_parse_u32_range(reg_text, 0U, 0xFFFFU, &reg) ||
        !config_parse_u32_range(count_text, 1U, CONFIG_MODBUS_MAX_READ_REGS, &count)) {
      LOG_CMD_RESP("usage: modbus scan hold|input <start> <end> <reg> <count>");
      LOG_CMD_RESP("limit: slaves<=%u count<=%u", CONFIG_MODBUS_SCAN_MAX_SLAVES, CONFIG_MODBUS_MAX_READ_REGS);
      return;
    }

    uint32_t hits = 0U;
    for (uint32_t slave = start; slave <= end; slave++) {
      uint8_t data[CONFIG_MODBUS_MAX_READ_REGS * 2U] = {0};
      uint8_t error_code = 0U;
      if (config_modbus_read_registers(kind, (uint8_t)slave, (uint16_t)reg, (uint16_t)count, data, &error_code)) {
        hits++;
        config_modbus_print_registers("modbus scan hit", kind, (uint8_t)slave, (uint16_t)reg, (uint16_t)count, data);
      }
    }
    LOG_CMD_RESP("modbus scan done type=%s start=%lu end=%lu reg=0x%04lX count=%lu hits=%lu", kind, start, end, reg,
                 count, hits);
  } else {
    LOG_CMD_RESP("usage: modbus read hold|input <slave> <reg> <count>");
    LOG_CMD_RESP("usage: modbus scan hold|input <start> <end> <reg> <count>");
  }
}

static bool config_parse_u32_range(const char *text, uint32_t min_value, uint32_t max_value, uint32_t *value) {
  if (text == NULL || value == NULL) {
    return false;
  }
  char *end = NULL;
  unsigned long parsed = strtoul(text, &end, 0);
  if (end == text || *end != '\0' || parsed < min_value || parsed > max_value) {
    return false;
  }
  *value = (uint32_t)parsed;
  return true;
}

static bool config_modbus_read_registers(const char *kind, uint8_t slave, uint16_t reg, uint16_t count, uint8_t *data,
                                         uint8_t *error_code) {
  if (kind == NULL || data == NULL || count == 0U || count > CONFIG_MODBUS_MAX_READ_REGS) {
    return false;
  }

  /*
   * 这里只提供只读诊断，避免现场为了探测设备误写寄存器。
   * hold/input 分别对应 Modbus 03/04，寄存器地址支持十进制或 0x 前缀。
   */
  if (strcmp(kind, "hold") == 0 || strcmp(kind, "holding") == 0 || strcmp(kind, "hr") == 0) {
    return Master_ReadHoldRegisters(slave, reg, count, data, error_code);
  }
  if (strcmp(kind, "input") == 0 || strcmp(kind, "ir") == 0) {
    return Master_ReadInputRegisters(slave, reg, count, data, error_code);
  }
  return false;
}

static void config_modbus_print_registers(const char *prefix, const char *kind, uint8_t slave, uint16_t reg,
                                          uint16_t count, const uint8_t *data) {
  char values[128] = {0};
  size_t offset = 0U;
  for (uint16_t index = 0U; index < count; index++) {
    uint16_t value = ((uint16_t)data[index * 2U] << 8) | data[index * 2U + 1U];
    int written = snprintf(values + offset, sizeof(values) - offset, "%s0x%04X", index == 0U ? "" : ",", value);
    if (written < 0 || (size_t)written >= (sizeof(values) - offset)) {
      break;
    }
    offset += (size_t)written;
  }
  LOG_CMD_RESP("%s type=%s slave=%u reg=0x%04X count=%u data=%s", prefix, kind, slave, reg, count, values);
}

static void config_handle_rs485_cmd(char *args) {
  if (strcmp(args, "status") == 0) {
    LOG_CMD_RESP("rs485.host.port=%s %s", BSP_RS485_UART_NAME, BSP_RS485_PIN_DESC);
    LOG_CMD_RESP("rs485.host.baud=%lu data_bits=8 parity=none stop_bits=1", bsp_rs485_get_baud_rate());
  } else if (strncmp(args, "baud ", 5) == 0) {
    uint32_t baud_rate = 0U;
    if (!config_parse_u32_range(args + 5, 1200U, 115200U, &baud_rate)) {
      LOG_CMD_RESP("usage: rs485 baud <1200|2400|4800|9600|19200|38400|57600|115200>");
      return;
    }
    uint16_t baud_code = 0U;
    if (!temp_humidity_sensor_rate_to_baud_code(baud_rate, &baud_code)) {
      LOG_CMD_RESP("rs485 baud failed unsupported=%lu", baud_rate);
      return;
    }
    HAL_StatusTypeDef status = bsp_rs485_set_baud_rate(baud_rate);
    LOG_CMD_RESP("rs485 baud %s host_baud=%lu", status == HAL_OK ? "ok" : "failed", bsp_rs485_get_baud_rate());
  } else {
    LOG_CMD_RESP("usage: rs485 status|baud <rate>");
  }
}

static void config_handle_device_cmd(char *args) {
  if (strcmp(args, "status") == 0) {
    device_manager_print_status();
  } else if (strcmp(args, "poll") == 0) {
    bool ok = device_manager_poll_all();
    LOG_CMD_RESP("device poll all %s", ok ? "ok" : "failed");
    device_manager_print_status();
  } else if (strcmp(args, "report") == 0) {
    bool ok = device_manager_publish_all();
    LOG_CMD_RESP("device report all %s", ok ? "ok" : "failed");
  } else if (strcmp(args, "payload") == 0) {
    char payload[MQTT_PAYLOAD_MAX_LEN] = {0};
    bool ok = device_manager_build_data_payload(payload, sizeof(payload));
    if (ok) {
      LOG_CMD_RESP("device payload=%s", payload);
    } else {
      LOG_CMD_RESP("device payload failed");
    }
  } else if (strcmp(args, "config") == 0) {
    config_print_device_config();
  } else if (strcmp(args, "save") == 0) {
    LOG_CMD_RESP("device save %s", config_write_into_eeprom() == SUCCESS ? "ok" : "failed");
  } else if (strcmp(args, "load") == 0 || strcmp(args, "defaults") == 0) {
    bool ok = config_load_from_storage();
    if (ok) {
      device_manager_apply_current_defaults();
    }
    LOG_CMD_RESP("device load %s", ok ? "ok" : "failed");
    config_print_device_config();
    device_manager_print_status();
  } else if (strcmp(args, "clear") == 0) {
    device_manager_clear_registered_devices();
    config_device_table_clear();
    LOG_CMD_RESP("device clear ok staged=1");
  } else if (strncmp(args, "add ", 4) == 0) {
    char *service_text = strtok(args + 4, " ");
    char *addr_text = strtok(NULL, " ");
    char *model_text = strtok(NULL, " ");
    uint32_t service_id = 0U;
    uint32_t slave_addr = 0U;
    uint32_t manufacture_model = 0U;
    if (!config_parse_u32_range(service_text, 1U, 65535U, &service_id) ||
        !config_parse_u32_range(addr_text, 1U, 247U, &slave_addr) ||
        (model_text != NULL && !config_parse_u32_range(model_text, 0U, 65535U, &manufacture_model))) {
      LOG_CMD_RESP("usage: device add <serviceId> <addr 1-247> [model]");
      return;
    }
    if (model_text == NULL) {
      manufacture_model = device_catalog_default_model((uint16_t)service_id);
    }
    /*
     * 本地 add 复用平台 registerInfo 的配置函数，并同步写入内存中的现场设备表。
     * 只有执行 device save/saveenv 后才落 EEPROM；平台 registerInfo 分支仍是运行态注册。
     */
    bool ok = device_manager_configure_registered_device((uint16_t)service_id, (uint16_t)manufacture_model,
                                                         (uint8_t)slave_addr);
    if (ok) {
      ok = config_device_table_add((uint16_t)service_id, (uint16_t)manufacture_model, (uint8_t)slave_addr);
    }
    LOG_CMD_RESP("device add %s serviceId=%lu addr=%lu model=%lu", ok ? "ok" : "failed", service_id, slave_addr,
                 manufacture_model);
  } else if (strncmp(args, "rs485 ", 6) == 0) {
    config_handle_device_rs485_cmd(args + 6);
  } else if (strncmp(args, "switch ", 7) == 0) {
    char *action = strtok(args + 7, " ");
    char *channel = strtok(NULL, " ");
    bool ok = false;
    if (action != NULL && strcmp(action, "all-on") == 0) {
      ok = device_manager_execute_command(140U, 0U, 3U, 0U);
    } else if (action != NULL && strcmp(action, "all-off") == 0) {
      ok = device_manager_execute_command(140U, 0U, 4U, 0U);
    } else if (action != NULL && channel != NULL) {
      uint16_t command = strcmp(action, "on") == 0 ? 1U : strcmp(action, "off") == 0 ? 2U : 0U;
      uint16_t index = (uint16_t)strtoul(channel, NULL, 10);
      ok = command != 0U && device_manager_execute_command(140U, 0U, command, index);
    }
    LOG_CMD_RESP("device switch %s", ok ? "ok" : "failed");
  } else if (strncmp(args, "ac ", 3) == 0) {
    char *action = strtok(args + 3, " ");
    char *param_text = strtok(NULL, " ");
    uint16_t command = 0U;
    uint16_t param = param_text == NULL ? 0U : (uint16_t)strtoul(param_text, NULL, 10);
    if (action != NULL && strcmp(action, "cool") == 0) {
      command = 1U;
    } else if (action != NULL && strcmp(action, "heat") == 0) {
      command = 2U;
    } else if (action != NULL && strcmp(action, "off") == 0) {
      command = 3U;
    } else if (action != NULL && strcmp(action, "diy") == 0) {
      command = 4U;
    } else if (action != NULL && strcmp(action, "learn-cool") == 0) {
      command = 11U;
    } else if (action != NULL && strcmp(action, "learn-heat") == 0) {
      command = 12U;
    } else if (action != NULL && strcmp(action, "learn-off") == 0) {
      command = 13U;
    } else if (action != NULL && strcmp(action, "learn-diy") == 0) {
      command = 14U;
    }
    // 空调控制命令号在设备层统一收口；串口命令只做文本到旧协议语义的薄转换。
    bool ok = command != 0U && device_manager_execute_command(150U, 0U, command, param);
    LOG_CMD_RESP("device ac %s", ok ? "ok" : "failed");
  } else {
    LOG_CMD_RESP("usage: device status|poll|payload|report|config|clear|add <serviceId> <addr> [model]|save|load");
    LOG_CMD_RESP("usage: device rs485 get <addr>|set addr <old> <new>|set baud <addr> <rate>");
    LOG_CMD_RESP("usage: device switch <on|off> <1-8>|switch all-on|all-off|ac <cmd>");
  }
}

static void config_handle_device_rs485_cmd(char *args) {
  char *action = strtok(args, " ");
  if (action == NULL) {
    LOG_CMD_RESP("usage: device rs485 get <addr>|set addr <old> <new>|set baud <addr> <rate>");
    return;
  }

  if (strcmp(action, "get") == 0) {
    char *addr_text = strtok(NULL, " ");
    uint32_t slave_addr = 0U;
    if (!config_parse_u32_range(addr_text, 1U, 247U, &slave_addr)) {
      LOG_CMD_RESP("usage: device rs485 get <addr 1-247>");
      return;
    }
    temp_humidity_rs485_params_t params = {0};
    uint8_t error_code = 0U;
    bool ok = temp_humidity_sensor_read_rs485_params((uint8_t)slave_addr, &params, &error_code);
    if (ok) {
      LOG_CMD_RESP("device.rs485 addr=%lu reported=%u baud_code=%u baud=%lu host_baud=%lu", slave_addr,
                   params.reported_slave_addr, params.baud_code, params.baud_rate, bsp_rs485_get_baud_rate());
    } else {
      LOG_CMD_RESP("device.rs485 get failed addr=%lu err=0x%02X host_baud=%lu", slave_addr, error_code,
                   bsp_rs485_get_baud_rate());
    }
    return;
  }

  if (strcmp(action, "set") == 0) {
    char *field = strtok(NULL, " ");
    if (field != NULL && strcmp(field, "addr") == 0) {
      char *old_text = strtok(NULL, " ");
      char *new_text = strtok(NULL, " ");
      uint32_t old_addr = 0U;
      uint32_t new_addr = 0U;
      if (!config_parse_u32_range(old_text, 1U, 247U, &old_addr) ||
          !config_parse_u32_range(new_text, 1U, 247U, &new_addr)) {
        LOG_CMD_RESP("usage: device rs485 set addr <old 1-247> <new 1-247>");
        return;
      }
      uint8_t error_code = 0U;
      bool ok = temp_humidity_sensor_write_slave_addr_at((uint8_t)old_addr, (uint8_t)new_addr, &error_code);
      if (ok) {
        (void)device_manager_configure_registered_device(DEVICE_SERVICE_TEMP_HUMIDITY,
                                                         device_catalog_default_model(DEVICE_SERVICE_TEMP_HUMIDITY),
                                                         (uint8_t)new_addr);
        (void)config_device_table_add(DEVICE_SERVICE_TEMP_HUMIDITY,
                                      device_catalog_default_model(DEVICE_SERVICE_TEMP_HUMIDITY),
                                      (uint8_t)new_addr);
      }
      LOG_CMD_RESP("device.rs485 set addr %s old=%lu new=%lu err=0x%02X", ok ? "ok" : "failed", old_addr, new_addr,
                   error_code);
      return;
    }

    if (field != NULL && strcmp(field, "baud") == 0) {
      char *addr_text = strtok(NULL, " ");
      char *baud_text = strtok(NULL, " ");
      uint32_t slave_addr = 0U;
      uint32_t baud_rate = 0U;
      if (!config_parse_u32_range(addr_text, 1U, 247U, &slave_addr) ||
          !config_parse_u32_range(baud_text, 1200U, 115200U, &baud_rate)) {
        LOG_CMD_RESP("usage: device rs485 set baud <addr 1-247> <rate>");
        return;
      }
      uint8_t error_code = 0U;
      bool ok = temp_humidity_sensor_write_baud_at((uint8_t)slave_addr, baud_rate, &error_code);
      if (ok) {
        (void)bsp_rs485_set_baud_rate(baud_rate);
      }
      LOG_CMD_RESP("device.rs485 set baud %s addr=%lu baud=%lu host_baud=%lu err=0x%02X", ok ? "ok" : "failed",
                   slave_addr, baud_rate, bsp_rs485_get_baud_rate(), error_code);
      return;
    }
  }

  LOG_CMD_RESP("usage: device rs485 get <addr>|set addr <old> <new>|set baud <addr> <rate>");
}

static const char *config_network_link_str(network_link_t link) {
  switch (link) {
  case NETWORK_LINK_NONE:
    return "none";
  case NETWORK_LINK_CH395Q:
    return "CH395Q/UART4/CN2";
  case NETWORK_LINK_AIR724UG:
    return "Air724UG/UART4";
  case NETWORK_LINK_AP6181:
    return "AP6181 WiFi/SDMMC1";
  default:
    return "unknown";
  }
}

static const char *config_network_state_str(network_state_t state) {
  switch (state) {
  case NETWORK_STATE_UNKNOWN:
    return "unknown";
  case NETWORK_STATE_UNAVAILABLE:
    return "unavailable";
  case NETWORK_STATE_CH395Q_ACTIVE:
    return "ch395q_active";
  case NETWORK_STATE_AIR724UG_ACTIVE:
    return "air724ug_active";
  case NETWORK_STATE_AP6181_ACTIVE:
    return "ap6181_active";
  default:
    return "unknown";
  }
}

static const char *config_mqtt_state_str(mqtt_client_state_t state) {
  switch (state) {
  case MQTT_CLIENT_STATE_DISCONNECTED:
    return "disconnected";
  case MQTT_CLIENT_STATE_TCP_CONNECTED:
    return "tcp_connected";
  case MQTT_CLIENT_STATE_SESSION_CONNECTED:
    return "session_connected";
  case MQTT_CLIENT_STATE_SUBSCRIBED:
    return "subscribed";
  default:
    return "unknown";
  }
}

static const char *config_wifi_ip_mode_str(network_wifi_ip_mode_t mode) {
  switch (mode) {
  case NETWORK_WIFI_IP_MODE_DHCP:
    return "dhcp";
  case NETWORK_WIFI_IP_MODE_STATIC:
    return "static";
  default:
    return "unknown";
  }
}

static bool config_parse_wifi_ip_mode(const char *text, network_wifi_ip_mode_t *mode) {
  if (text == NULL || mode == NULL) {
    return false;
  }
  if (strcmp(text, "dhcp") == 0 || strcmp(text, "auto") == 0) {
    *mode = NETWORK_WIFI_IP_MODE_DHCP;
    return true;
  }
  if (strcmp(text, "static") == 0 || strcmp(text, "manual") == 0) {
    *mode = NETWORK_WIFI_IP_MODE_STATIC;
    return true;
  }
  return false;
}
