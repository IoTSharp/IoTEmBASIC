#include "Application/Inc/app_rtos.h"

#include "Application/Inc/app.h"
#include "Board/Inc/bsp_watchdog.h"
#include "Config/Inc/config.h"
#include "Common/Inc/log.h"
#include "Interpreter/Inc/app_basic.h"
#include "Protocol/Modbus/Inc/modbus_core_master.h"
#include "Protocol/Mqtt/Inc/mqtt_client.h"

#include "Board/Inc/bsp_hal.h"

#if APP_ENABLE_CMSIS_RTOS
#include "cmsis_os2.h"
#endif

#define APP_RTOS_MAIN_THREAD_STACK 4096U
#define APP_RTOS_MQTT_RX_THREAD_STACK 3072U
#define APP_RTOS_BASIC_THREAD_STACK 4096U
#define APP_RTOS_DEBUG_THREAD_STACK 2048U
#define APP_RTOS_WATCHDOG_THREAD_STACK 768U
#define APP_RTOS_DEFAULT_WATCHDOG_TIMEOUT_MS 30000U
#define APP_RTOS_MIN_WATCHDOG_FEED_INTERVAL_MS 10000U
#define APP_RTOS_MIN_HEARTBEAT_TIMEOUT_MS 300000U

static volatile uint32_t app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_COUNT] = {0};
static volatile bool app_rtos_scheduler_started = false;
#if APP_ENABLE_CMSIS_RTOS
static osThreadId_t app_rtos_main_thread_id;
static osThreadId_t app_rtos_mqtt_rx_thread_id;
static osThreadId_t app_rtos_basic_thread_id;
static osThreadId_t app_rtos_debug_thread_id;
static osThreadId_t app_rtos_watchdog_thread_id;
#endif

static uint32_t app_rtos_normalize_interval(uint32_t interval_ms, uint32_t fallback_ms);

#if APP_ENABLE_CMSIS_RTOS
static void app_rtos_main_thread(void *argument);
static void app_rtos_mqtt_rx_thread(void *argument);
static void app_rtos_basic_thread(void *argument);
static void app_rtos_debug_thread(void *argument);
static void app_rtos_watchdog_thread(void *argument);
#endif

bool app_rtos_start(void) {
#if APP_ENABLE_CMSIS_RTOS
  if (osKernelInitialize() != osOK) {
    LOG_ERROR("RTOS kernel init failed");
    return false;
  }
  Modbus_MasterInit();

  /* 先迁移线程边界，不在业务层使用 FreeRTOS 私有 API，后续仍可替换 CMSIS-RTOS2 后端。 */
  const osThreadAttr_t main_attr = {
    .name = "pem_main",
    .priority = osPriorityNormal,
    .stack_size = APP_RTOS_MAIN_THREAD_STACK,
  };
  const osThreadAttr_t mqtt_rx_attr = {
    .name = "mqtt_rx",
    .priority = osPriorityAboveNormal,
    .stack_size = APP_RTOS_MQTT_RX_THREAD_STACK,
  };
  const osThreadAttr_t basic_attr = {
    .name = "basic",
    .priority = osPriorityBelowNormal,
    .stack_size = APP_RTOS_BASIC_THREAD_STACK,
  };
  const osThreadAttr_t debug_attr = {
    .name = "debug",
    .priority = osPriorityLow,
    .stack_size = APP_RTOS_DEBUG_THREAD_STACK,
  };
  const osThreadAttr_t watchdog_attr = {
    .name = "watchdog",
    .priority = osPriorityHigh,
    .stack_size = APP_RTOS_WATCHDOG_THREAD_STACK,
  };

  const uint32_t start_tick = HAL_GetTick();
  for (uint32_t i = 0U; i < APP_RTOS_HEARTBEAT_COUNT; i++) {
    app_rtos_heartbeat_ticks[i] = start_tick;
  }

  app_rtos_main_thread_id = osThreadNew(app_rtos_main_thread, NULL, &main_attr);
  app_rtos_mqtt_rx_thread_id = osThreadNew(app_rtos_mqtt_rx_thread, NULL, &mqtt_rx_attr);
  app_rtos_basic_thread_id = osThreadNew(app_rtos_basic_thread, NULL, &basic_attr);
  app_rtos_debug_thread_id = osThreadNew(app_rtos_debug_thread, NULL, &debug_attr);
  app_rtos_watchdog_thread_id = osThreadNew(app_rtos_watchdog_thread, NULL, &watchdog_attr);
  if (app_rtos_main_thread_id == NULL || app_rtos_mqtt_rx_thread_id == NULL || app_rtos_basic_thread_id == NULL ||
      app_rtos_debug_thread_id == NULL || app_rtos_watchdog_thread_id == NULL) {
    LOG_ERROR("RTOS thread create failed");
    return false;
  }

  LOG_INFO("RTOS scheduler starting");
  app_rtos_scheduler_started = true;
  if (osKernelStart() != osOK) {
    app_rtos_scheduler_started = false;
    LOG_ERROR("RTOS scheduler start failed");
    return false;
  }
  return true;
#else
  return false;
#endif
}

void app_rtos_mark_heartbeat(app_rtos_heartbeat_t heartbeat, uint32_t now_ms) {
  if (heartbeat >= APP_RTOS_HEARTBEAT_COUNT) {
    return;
  }
  app_rtos_heartbeat_ticks[heartbeat] = now_ms;
}

bool app_rtos_all_heartbeats_alive(uint32_t now_ms, uint32_t timeout_ms) {
  timeout_ms = app_rtos_normalize_interval(timeout_ms, app_rtos_get_heartbeat_timeout_ms());
  for (uint32_t i = 0U; i < APP_RTOS_HEARTBEAT_COUNT; i++) {
    if (app_rtos_heartbeat_ticks[i] == 0U || (now_ms - app_rtos_heartbeat_ticks[i]) > timeout_ms) {
      return false;
    }
  }
  return true;
}

uint32_t app_rtos_get_heartbeat_timeout_ms(void) {
  uint32_t feed_interval_ms =
    app_rtos_normalize_interval(active_config.loop.feed_watch_dog_interval, APP_RTOS_MIN_WATCHDOG_FEED_INTERVAL_MS);
  uint32_t max_retries = app_rtos_normalize_interval(active_config.loop.feed_watch_dog_max_retries, 30U);
  uint32_t timeout_ms = feed_interval_ms * max_retries;

  /* 旧 EEPROM 里可能仍保存 max_retries=3。MQTT/TCP 在 Broker 未启动时会阻塞数十秒，
   * 心跳窗口过短会把“服务端不可达”误判成线程死亡，所以正式运行至少保留 5 分钟诊断窗口。 */
  if (timeout_ms < APP_RTOS_MIN_HEARTBEAT_TIMEOUT_MS) {
    timeout_ms = APP_RTOS_MIN_HEARTBEAT_TIMEOUT_MS;
  }
  return timeout_ms;
}

app_rtos_status_t app_rtos_get_status(uint32_t now_ms, uint32_t timeout_ms) {
  timeout_ms = app_rtos_normalize_interval(timeout_ms, app_rtos_get_heartbeat_timeout_ms());
  app_rtos_status_t status = {
#if APP_ENABLE_CMSIS_RTOS
    .enabled = true,
#else
    .enabled = false,
#endif
    .started = app_rtos_scheduler_started,
    .main_stack_size = APP_RTOS_MAIN_THREAD_STACK,
    .mqtt_rx_stack_size = APP_RTOS_MQTT_RX_THREAD_STACK,
    .basic_stack_size = APP_RTOS_BASIC_THREAD_STACK,
    .debug_stack_size = APP_RTOS_DEBUG_THREAD_STACK,
    .watchdog_stack_size = APP_RTOS_WATCHDOG_THREAD_STACK,
#if APP_ENABLE_CMSIS_RTOS
    .main_stack_free = osThreadGetStackSpace(app_rtos_main_thread_id),
    .mqtt_rx_stack_free = osThreadGetStackSpace(app_rtos_mqtt_rx_thread_id),
    .basic_stack_free = osThreadGetStackSpace(app_rtos_basic_thread_id),
    .debug_stack_free = osThreadGetStackSpace(app_rtos_debug_thread_id),
    .watchdog_stack_free = osThreadGetStackSpace(app_rtos_watchdog_thread_id),
#else
    .main_stack_free = 0U,
    .mqtt_rx_stack_free = 0U,
    .basic_stack_free = 0U,
    .debug_stack_free = 0U,
    .watchdog_stack_free = 0U,
#endif
    .main_last_heartbeat_ms = app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_MAIN],
    .mqtt_rx_last_heartbeat_ms = app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_MQTT_RX],
    .basic_last_heartbeat_ms = app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_BASIC],
    .debug_last_heartbeat_ms = app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_DEBUG],
    .heartbeat_timeout_ms = timeout_ms,
    .heartbeats_alive = app_rtos_all_heartbeats_alive(now_ms, timeout_ms),
  };
  return status;
}

static uint32_t app_rtos_normalize_interval(uint32_t interval_ms, uint32_t fallback_ms) {
  return interval_ms == 0U ? fallback_ms : interval_ms;
}

#if APP_ENABLE_CMSIS_RTOS
static void app_rtos_main_thread(void *argument) {
  (void)argument;
  const uint32_t interval_ms = app_rtos_normalize_interval(active_config.loop.main_loop_interval, 1000U);

  for (;;) {
    const uint32_t now_ms = osKernelGetTickCount();
    app_process_once(now_ms);
    app_rtos_mark_heartbeat(APP_RTOS_HEARTBEAT_MAIN, now_ms);
    osDelay(interval_ms);
  }
}

static void app_rtos_mqtt_rx_thread(void *argument) {
  (void)argument;

  for (;;) {
    const uint32_t now_ms = osKernelGetTickCount();
    mqtt_client_poll();
    app_rtos_mark_heartbeat(APP_RTOS_HEARTBEAT_MQTT_RX, now_ms);
    osDelay(10U);
  }
}

static void app_rtos_basic_thread(void *argument) {
  (void)argument;

  if (app_basic_reload_and_run_managed() == SUCCESS) {
    app_basic_status_t status = app_basic_get_status();
    LOG_INFO("BASIC script thread executed %s entry=%s package=%s build=%lu deps=%u size=%lu startup=%s rollback=%u import=%s",
             status.loaded_name,
             status.entry_name, status.package_version, (unsigned long)status.build_number,
             (unsigned int)status.dependency_count, (unsigned long)status.loaded_size,
             eeprom_basic_script_startup_result_name(status.startup_result), status.rollback_performed ? 1U : 0U,
             app_basic_import_status_name(status.last_import_status));
  } else {
    app_basic_status_t status = app_basic_get_status();
    LOG_WARNING("BASIC script thread idle: no runnable script startup=%s rollback=%u import=%s name=%s",
                eeprom_basic_script_startup_result_name(status.startup_result),
                status.rollback_performed ? 1U : 0U, app_basic_import_status_name(status.last_import_status),
                status.last_import_name);
  }

  for (;;) {
    app_rtos_mark_heartbeat(APP_RTOS_HEARTBEAT_BASIC, osKernelGetTickCount());
    osDelay(1000U);
  }
}

static void app_rtos_debug_thread(void *argument) {
  (void)argument;

  for (;;) {
    const uint32_t now_ms = osKernelGetTickCount();
    config_process_cmd();
    app_rtos_mark_heartbeat(APP_RTOS_HEARTBEAT_DEBUG, now_ms);
    osDelay(20U);
  }
}

static void app_rtos_watchdog_thread(void *argument) {
  (void)argument;
  const uint32_t feed_interval_ms =
    app_rtos_normalize_interval(active_config.loop.feed_watch_dog_interval, APP_RTOS_MIN_WATCHDOG_FEED_INTERVAL_MS);
  const uint32_t heartbeat_timeout_ms = app_rtos_get_heartbeat_timeout_ms();

  /* IWDG 在 main() 早于 RTOS 启动，watchdog 线程起来后先刷新一次，避免首个周期等待叠加启动耗时。 */
  (void)bsp_watchdog_refresh();
  for (;;) {
    osDelay(feed_interval_ms);
    uint32_t now_ms = osKernelGetTickCount();
    if (!app_rtos_all_heartbeats_alive(now_ms, heartbeat_timeout_ms)) {
      /* 任一关键线程不再更新心跳时停止喂狗，让 IWDG 复位；不要在这里强行重启单线程。 */
      uint32_t main_age = now_ms - app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_MAIN];
      uint32_t mqtt_rx_age = now_ms - app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_MQTT_RX];
      uint32_t basic_age = now_ms - app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_BASIC];
      uint32_t debug_age = now_ms - app_rtos_heartbeat_ticks[APP_RTOS_HEARTBEAT_DEBUG];
      LOG_FATAL("stop feed watchdog: RTOS heartbeat timeout main=%lu mqtt_rx=%lu basic=%lu debug=%lu timeout=%lu",
                main_age, mqtt_rx_age, basic_age, debug_age, heartbeat_timeout_ms);
      return;
    }
    (void)bsp_watchdog_refresh();
  }
}
#endif
