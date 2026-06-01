#include "Interpreter/Inc/app_basic.h"

#include "Board/Inc/bsp_board.h"
#include "Common/Inc/log.h"
#include "Interpreter/Inc/app_basic_registry.h"
#include "Interpreter/Inc/basic.h"
#if BSP_HAS_DISPLAY
#include "Interpreter/Inc/basic_display.h"
#endif

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define APP_BASIC_SCRIPT_BUFFER_SIZE EEPROM_BASIC_SCRIPT_READ_BUFFER_SIZE
#define APP_BASIC_IMPORT_BUFFER_SIZE EEPROM_BASIC_SCRIPT_READ_BUFFER_SIZE
#define APP_BASIC_PRINT_BUFFER_SIZE  160U
#define APP_BASIC_DIRECT_STRING_FMT  "%s"
#define APP_BASIC_IMPORT_STACK_LIMIT  EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT

static struct mb_interpreter_t *app_basic_interpreter;
static bool app_basic_system_initialized;
static app_basic_print_target_t app_basic_print_target = APP_BASIC_PRINT_TARGET_DEFAULT;
static app_basic_signature_verifier_t app_basic_signature_verifier;
static app_basic_status_t app_basic_status = {
  .loaded_slot = APP_BASIC_SLOT_PRIMARY,
};
static char app_basic_script_buffer[APP_BASIC_SCRIPT_BUFFER_SIZE];
static char app_basic_import_buffer[APP_BASIC_IMPORT_BUFFER_SIZE];
static eeprom_basic_script_slot_t app_basic_import_stack[APP_BASIC_IMPORT_STACK_LIMIT];
static uint8_t app_basic_import_depth;

static ErrorStatus app_basic_load_from_slot(app_basic_slot_t slot);
static ErrorStatus app_basic_run_lifecycle_slot(eeprom_basic_script_slot_t slot,
                                                eeprom_basic_script_startup_result_t *result);
static ErrorStatus app_basic_rollback_candidate(eeprom_basic_script_lifecycle_state_t *state,
                                                eeprom_basic_script_slot_t failed_slot,
                                                eeprom_basic_script_slot_t previous_active,
                                                eeprom_basic_script_startup_result_t failed_result);
static int app_basic_import_handler(struct mb_interpreter_t *s, const char *name);
static int app_basic_printer(struct mb_interpreter_t *s, const char *fmt, ...);
#if BSP_HAS_DISPLAY
static int app_basic_write_to_display(const char *fmt, va_list args);
static int app_basic_write_formatted_to_display(const char *fmt, va_list args);
#endif
static void app_basic_close_current(void);
static void app_basic_set_status_from_info(app_basic_slot_t slot, const eeprom_basic_script_info_t *info);
static void app_basic_set_startup_status(eeprom_basic_script_slot_t slot,
                                         eeprom_basic_script_startup_result_t result, bool rollback_performed);
static void app_basic_set_import_status(app_basic_import_status_t status, const char *name,
                                        eeprom_basic_script_slot_t slot, size_t script_size);
static bool app_basic_import_stack_contains(eeprom_basic_script_slot_t slot);
static uint32_t app_basic_import_payload_max_for_info(const eeprom_basic_script_info_t *info);
static bool app_basic_eeprom_slot_is_valid(eeprom_basic_script_slot_t slot);
static eeprom_basic_script_slot_t app_basic_other_eeprom_slot(eeprom_basic_script_slot_t slot);
static app_basic_signature_status_t app_basic_verify_script(eeprom_basic_script_slot_t slot,
                                                            const eeprom_basic_script_info_t *info,
                                                            const char *script, size_t script_size);
static bool app_basic_signature_status_allows_load(app_basic_signature_status_t status);

void app_basic_init(void) {
  app_basic_close_current();
  if (!app_basic_system_initialized && mb_init() == MB_FUNC_OK) {
    app_basic_system_initialized = true;
  }
  memset(&app_basic_status, 0, sizeof(app_basic_status));
  app_basic_status.loaded_slot = APP_BASIC_SLOT_PRIMARY;
  app_basic_status.last_import_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
  app_basic_import_depth = 0U;
  app_basic_print_target = APP_BASIC_PRINT_TARGET_DEFAULT;
}

ErrorStatus app_basic_load(app_basic_slot_t preferred_slot) {
  if (app_basic_load_from_slot(preferred_slot) == SUCCESS) {
    return SUCCESS;
  }

  app_basic_slot_t fallback_slot =
    preferred_slot == APP_BASIC_SLOT_PRIMARY ? APP_BASIC_SLOT_BACKUP : APP_BASIC_SLOT_PRIMARY;
  if (app_basic_load_from_slot(fallback_slot) == SUCCESS) {
    LOG_WARNING("BASIC loaded fallback script %s", app_basic_status.loaded_name);
    return SUCCESS;
  }

  LOG_WARNING("BASIC script not loaded from EEPROM");
  return ERROR;
}

ErrorStatus app_basic_run_once(void) {
  if (app_basic_interpreter == NULL) {
    return ERROR;
  }

  int result = mb_run(app_basic_interpreter, true);
  if (result == MB_FUNC_OK || result == MB_FUNC_END) {
    return SUCCESS;
  }

  const char *file = NULL;
  int pos = 0;
  unsigned short row = 0U;
  unsigned short col = 0U;
  mb_error_e err = mb_get_last_error(app_basic_interpreter, &file, &pos, &row, &col);
  LOG_ERROR("BASIC run failed err=%d %s pos=%d row=%u col=%u", err, mb_get_error_desc(err), pos, row, col);
  return ERROR;
}

ErrorStatus app_basic_reload_and_run(app_basic_slot_t preferred_slot) {
  if (app_basic_load(preferred_slot) != SUCCESS) {
    return ERROR;
  }
  return app_basic_run_once();
}

ErrorStatus app_basic_reload_and_run_managed(void) {
  eeprom_basic_script_lifecycle_state_t state;
  if (eeprom_get_basic_script_lifecycle_state(&state) != SUCCESS) {
    memset(&state, 0, sizeof(state));
    state.active_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
    state.candidate_slot = EEPROM_BASIC_SCRIPT_SLOT_APP02;
    state.previous_slot = EEPROM_BASIC_SCRIPT_SLOT_APP02;
    state.last_attempt_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
    state.last_success_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
    state.last_failed_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
    state.last_result = EEPROM_BASIC_SCRIPT_STARTUP_NONE;
  }

  bool candidate_boot = state.has_candidate && app_basic_eeprom_slot_is_valid(state.candidate_slot);
  eeprom_basic_script_slot_t boot_slot = candidate_boot ? state.candidate_slot : state.active_slot;
  if (!app_basic_eeprom_slot_is_valid(boot_slot)) {
    boot_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
    state.active_slot = boot_slot;
  }

  eeprom_basic_script_slot_t previous_active = state.active_slot;
  if (candidate_boot && state.last_result == EEPROM_BASIC_SCRIPT_STARTUP_BOOTING &&
      state.last_attempt_slot == boot_slot) {
    state.boot_count++;
    LOG_WARNING("BASIC candidate %s did not complete previous startup", eeprom_basic_script_slot_name(boot_slot));
    return app_basic_rollback_candidate(&state, boot_slot, previous_active, EEPROM_BASIC_SCRIPT_STARTUP_RUN_FAILED);
  }

  state.last_attempt_slot = boot_slot;
  state.last_result = EEPROM_BASIC_SCRIPT_STARTUP_BOOTING;
  state.boot_count++;
  if (eeprom_write_basic_script_lifecycle_state(&state) != SUCCESS) {
    LOG_WARNING("BASIC lifecycle booting state write failed");
  }
  app_basic_set_startup_status(boot_slot, EEPROM_BASIC_SCRIPT_STARTUP_BOOTING, false);

  eeprom_basic_script_startup_result_t result = EEPROM_BASIC_SCRIPT_STARTUP_NONE;
  if (app_basic_run_lifecycle_slot(boot_slot, &result) == SUCCESS) {
    if (candidate_boot) {
      state.active_slot = boot_slot;
      state.has_candidate = false;
      state.candidate_slot = app_basic_other_eeprom_slot(boot_slot);
      state.has_previous = previous_active != boot_slot && app_basic_eeprom_slot_is_valid(previous_active);
      state.previous_slot = previous_active;
    }
    state.last_success_slot = boot_slot;
    state.last_result = EEPROM_BASIC_SCRIPT_STARTUP_SUCCESS;
    if (eeprom_write_basic_script_lifecycle_state(&state) != SUCCESS) {
      LOG_WARNING("BASIC lifecycle success state write failed");
    }
    app_basic_set_startup_status(boot_slot, state.last_result, false);
    return SUCCESS;
  }

  if (!candidate_boot) {
    state.last_failed_slot = boot_slot;
    state.last_result = result;
    if (eeprom_write_basic_script_lifecycle_state(&state) != SUCCESS) {
      LOG_WARNING("BASIC lifecycle failure state write failed");
    }
    app_basic_set_startup_status(boot_slot, state.last_result, false);
    return ERROR;
  }

  return app_basic_rollback_candidate(&state, boot_slot, previous_active, result);
}

app_basic_status_t app_basic_get_status(void) {
  return app_basic_status;
}

void app_basic_set_print_target(app_basic_print_target_t target) {
  app_basic_print_target = target;
}

app_basic_print_target_t app_basic_get_print_target(void) {
  return app_basic_print_target;
}

void app_basic_set_signature_verifier(app_basic_signature_verifier_t verifier) {
  app_basic_signature_verifier = verifier;
}

const char *app_basic_import_status_name(app_basic_import_status_t status) {
  switch (status) {
  case APP_BASIC_IMPORT_NONE:
    return "none";
  case APP_BASIC_IMPORT_OK:
    return "ok";
  case APP_BASIC_IMPORT_MISSING_DEPENDENCY:
    return "missing-dependency";
  case APP_BASIC_IMPORT_DEPENDENCY_CYCLE:
    return "dependency-cycle";
  case APP_BASIC_IMPORT_SCRIPT_SIZE_EXCEEDED:
    return "script-size-exceeded";
  case APP_BASIC_IMPORT_INVALID_HEADER:
    return "invalid-header";
  case APP_BASIC_IMPORT_READ_FAILED:
    return "read-failed";
  case APP_BASIC_IMPORT_SIGNATURE_REJECTED:
    return "signature-rejected";
  case APP_BASIC_IMPORT_PARSE_FAILED:
    return "parse-failed";
  case APP_BASIC_IMPORT_DEPTH_EXCEEDED:
    return "depth-exceeded";
  default:
    return "unknown";
  }
}

static ErrorStatus app_basic_load_from_slot(app_basic_slot_t slot) {
  eeprom_basic_script_slot_t eeprom_slot = (eeprom_basic_script_slot_t)slot;
  eeprom_basic_script_info_t script_info;
  app_basic_signature_status_t signature_status = APP_BASIC_SIGNATURE_UNCHECKED;
  size_t script_size = 0U;
  if (!app_basic_system_initialized) {
    app_basic_init();
  }
  if (!app_basic_system_initialized) {
    return ERROR;
  }
  app_basic_import_depth = 0U;
  app_basic_set_import_status(APP_BASIC_IMPORT_NONE, NULL, eeprom_slot, 0U);
  if (eeprom_get_basic_script_info(eeprom_slot, &script_info) != SUCCESS || !script_info.valid) {
    return ERROR;
  }
  if (eeprom_read_basic_script(eeprom_slot, app_basic_script_buffer, sizeof(app_basic_script_buffer), &script_size) !=
      SUCCESS) {
    return ERROR;
  }
  signature_status = app_basic_verify_script(eeprom_slot, &script_info, app_basic_script_buffer, script_size);
  if (!app_basic_signature_status_allows_load(signature_status)) {
    LOG_WARNING("BASIC signature rejected %s status=%u", script_info.name, (unsigned int)signature_status);
    return ERROR;
  }

  app_basic_close_current();
  app_basic_print_target = APP_BASIC_PRINT_TARGET_DEFAULT;
  if (mb_open(&app_basic_interpreter) != MB_FUNC_OK || app_basic_interpreter == NULL) {
    app_basic_close_current();
    return ERROR;
  }

  (void)mb_set_printer(app_basic_interpreter, app_basic_printer);
  (void)mb_set_import_handler(app_basic_interpreter, app_basic_import_handler);
  if (app_basic_register_profile(app_basic_interpreter) != SUCCESS) {
    app_basic_close_current();
    return ERROR;
  }

  app_basic_import_stack[app_basic_import_depth++] = eeprom_slot;
  if (mb_load_string(app_basic_interpreter, app_basic_script_buffer, true) != MB_FUNC_OK) {
    app_basic_import_depth = 0U;
    app_basic_close_current();
    return ERROR;
  }
  app_basic_import_depth = 0U;

  app_basic_import_status_t last_import_status = app_basic_status.last_import_status;
  char last_import_name[EEPROM_BASIC_SCRIPT_NAME_SIZE];
  (void)strncpy(last_import_name, app_basic_status.last_import_name, sizeof(last_import_name) - 1U);
  last_import_name[sizeof(last_import_name) - 1U] = '\0';
  eeprom_basic_script_slot_t last_import_slot = app_basic_status.last_import_slot;
  size_t last_import_size = app_basic_status.last_import_size;
  uint8_t last_import_depth = app_basic_status.last_import_depth;

  app_basic_set_status_from_info(slot, &script_info);
  app_basic_status.loaded_size = script_size;
  app_basic_status.signature_status = signature_status;
  app_basic_status.last_import_status = last_import_status;
  (void)strncpy(app_basic_status.last_import_name, last_import_name, sizeof(app_basic_status.last_import_name) - 1U);
  app_basic_status.last_import_slot = last_import_slot;
  app_basic_status.last_import_size = last_import_size;
  app_basic_status.last_import_depth = last_import_depth;
  LOG_INFO("BASIC loaded %s entry=%s package=%s build=%lu signature=%u size=%lu", app_basic_status.loaded_name,
           app_basic_status.entry_name, app_basic_status.package_version, (unsigned long)app_basic_status.build_number,
           (unsigned int)signature_status, (unsigned long)script_size);
  return SUCCESS;
}

static ErrorStatus app_basic_run_lifecycle_slot(eeprom_basic_script_slot_t slot,
                                                eeprom_basic_script_startup_result_t *result) {
  if (result != NULL) {
    *result = EEPROM_BASIC_SCRIPT_STARTUP_LOAD_FAILED;
  }
  if (app_basic_load_from_slot((app_basic_slot_t)slot) != SUCCESS) {
    return ERROR;
  }

  if (result != NULL) {
    *result = EEPROM_BASIC_SCRIPT_STARTUP_RUN_FAILED;
  }
  if (app_basic_run_once() != SUCCESS) {
    return ERROR;
  }

  if (result != NULL) {
    *result = EEPROM_BASIC_SCRIPT_STARTUP_SUCCESS;
  }
  return SUCCESS;
}

static ErrorStatus app_basic_rollback_candidate(eeprom_basic_script_lifecycle_state_t *state,
                                                eeprom_basic_script_slot_t failed_slot,
                                                eeprom_basic_script_slot_t previous_active,
                                                eeprom_basic_script_startup_result_t failed_result) {
  state->last_failed_slot = failed_slot;
  state->rollback_count++;
  eeprom_basic_script_slot_t rollback_slot = state->has_previous ? state->previous_slot : previous_active;
  if (!app_basic_eeprom_slot_is_valid(rollback_slot) || rollback_slot == failed_slot) {
    state->has_candidate = false;
    state->candidate_slot = failed_slot;
    state->active_slot =
      app_basic_eeprom_slot_is_valid(previous_active) ? previous_active : EEPROM_BASIC_SCRIPT_SLOT_APP01;
    state->last_result = failed_result;
    if (eeprom_write_basic_script_lifecycle_state(state) != SUCCESS) {
      LOG_WARNING("BASIC lifecycle rollback unavailable state write failed");
    }
    app_basic_set_startup_status(failed_slot, state->last_result, true);
    LOG_WARNING("BASIC candidate %s failed and no previous slot is available",
                eeprom_basic_script_slot_name(failed_slot));
    return ERROR;
  }

  state->active_slot = rollback_slot;
  state->has_candidate = false;
  state->candidate_slot = failed_slot;
  state->has_previous = false;
  state->previous_slot = failed_slot;
  state->last_attempt_slot = rollback_slot;
  state->last_result = EEPROM_BASIC_SCRIPT_STARTUP_BOOTING;
  if (eeprom_write_basic_script_lifecycle_state(state) != SUCCESS) {
    LOG_WARNING("BASIC lifecycle rollback booting state write failed");
  }
  LOG_WARNING("BASIC candidate %s failed, rolling back to %s", eeprom_basic_script_slot_name(failed_slot),
              eeprom_basic_script_slot_name(rollback_slot));

  eeprom_basic_script_startup_result_t rollback_result = EEPROM_BASIC_SCRIPT_STARTUP_NONE;
  if (app_basic_run_lifecycle_slot(rollback_slot, &rollback_result) == SUCCESS) {
    state->last_success_slot = rollback_slot;
    state->last_result = EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_SUCCESS;
    if (eeprom_write_basic_script_lifecycle_state(state) != SUCCESS) {
      LOG_WARNING("BASIC lifecycle rollback success state write failed");
    }
    app_basic_set_startup_status(rollback_slot, state->last_result, true);
    return SUCCESS;
  }

  state->last_failed_slot = rollback_slot;
  state->last_result = rollback_result == EEPROM_BASIC_SCRIPT_STARTUP_LOAD_FAILED
                         ? EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_LOAD_FAILED
                         : EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_RUN_FAILED;
  if (eeprom_write_basic_script_lifecycle_state(state) != SUCCESS) {
    LOG_WARNING("BASIC lifecycle rollback failure state write failed");
  }
  app_basic_set_startup_status(rollback_slot, state->last_result, true);
  return ERROR;
}

static int app_basic_import_handler(struct mb_interpreter_t *s, const char *name) {
  eeprom_basic_script_slot_t slot;
  eeprom_basic_script_info_t script_info;
  size_t script_size = 0U;
  int result = MB_FUNC_ERR;

  if (!eeprom_basic_script_slot_from_package_name(name, &slot)) {
    app_basic_set_import_status(APP_BASIC_IMPORT_MISSING_DEPENDENCY, name, EEPROM_BASIC_SCRIPT_SLOT_APP01, 0U);
    LOG_WARNING("BASIC import missing dependency %s", name == NULL ? "" : name);
    return MB_FUNC_ERR;
  }
  if (app_basic_import_stack_contains(slot)) {
    app_basic_set_import_status(APP_BASIC_IMPORT_DEPENDENCY_CYCLE, name, slot, 0U);
    LOG_WARNING("BASIC import cycle dependency %s slot=%s", name == NULL ? "" : name,
                eeprom_basic_script_slot_name(slot));
    return MB_FUNC_ERR;
  }
  if (app_basic_import_depth >= APP_BASIC_IMPORT_STACK_LIMIT) {
    app_basic_set_import_status(APP_BASIC_IMPORT_DEPTH_EXCEEDED, name, slot, 0U);
    LOG_WARNING("BASIC import depth exceeded %s slot=%s depth=%u", name == NULL ? "" : name,
                eeprom_basic_script_slot_name(slot), (unsigned int)app_basic_import_depth);
    return MB_FUNC_ERR;
  }
  if (eeprom_get_basic_script_info(slot, &script_info) != SUCCESS || !script_info.valid) {
    uint32_t payload_max = app_basic_import_payload_max_for_info(&script_info);
    app_basic_import_status_t status =
      ((payload_max > 0U && script_info.data_size > payload_max) || script_info.data_size >= sizeof(app_basic_import_buffer))
        ? APP_BASIC_IMPORT_SCRIPT_SIZE_EXCEEDED
        : APP_BASIC_IMPORT_INVALID_HEADER;
    app_basic_set_import_status(status, name, slot, script_info.data_size);
    LOG_WARNING("BASIC import %s %s slot=%s size=%lu max=%lu", app_basic_import_status_name(status),
                name == NULL ? "" : name, eeprom_basic_script_slot_name(slot),
                (unsigned long)script_info.data_size, (unsigned long)payload_max);
    goto exit;
  }
  if (script_info.data_size >= sizeof(app_basic_import_buffer)) {
    app_basic_set_import_status(APP_BASIC_IMPORT_SCRIPT_SIZE_EXCEEDED, name, slot, script_info.data_size);
    LOG_WARNING("BASIC import size exceeded %s slot=%s size=%lu max=%lu", name == NULL ? "" : name,
                eeprom_basic_script_slot_name(slot), (unsigned long)script_info.data_size,
                (unsigned long)(sizeof(app_basic_import_buffer) - 1U));
    goto exit;
  }
  if (eeprom_read_basic_script(slot, app_basic_import_buffer, sizeof(app_basic_import_buffer), &script_size) != SUCCESS) {
    app_basic_set_import_status(APP_BASIC_IMPORT_READ_FAILED, name, slot, script_info.data_size);
    LOG_WARNING("BASIC import read failed %s slot=%s size=%lu", name == NULL ? "" : name,
                eeprom_basic_script_slot_name(slot), (unsigned long)script_info.data_size);
    goto exit;
  }
  app_basic_signature_status_t signature_status =
    app_basic_verify_script(slot, &script_info, app_basic_import_buffer, script_size);
  if (!app_basic_signature_status_allows_load(signature_status)) {
    app_basic_set_import_status(APP_BASIC_IMPORT_SIGNATURE_REJECTED, name, slot, script_size);
    LOG_WARNING("BASIC import signature rejected %s status=%u", name == NULL ? "" : name,
                (unsigned int)signature_status);
    goto exit;
  }

  app_basic_import_stack[app_basic_import_depth++] = slot;
  result = mb_load_string(s, app_basic_import_buffer, true);
  app_basic_import_depth--;
  if (result == MB_FUNC_OK) {
    app_basic_set_import_status(APP_BASIC_IMPORT_OK, name, slot, script_size);
    LOG_INFO("BASIC imported %s slot=%s size=%lu", name == NULL ? "" : name, eeprom_basic_script_slot_name(slot),
             (unsigned long)script_size);
  } else {
    app_basic_set_import_status(APP_BASIC_IMPORT_PARSE_FAILED, name, slot, script_size);
    LOG_WARNING("BASIC import parse failed %s slot=%s size=%lu", name == NULL ? "" : name,
                eeprom_basic_script_slot_name(slot), (unsigned long)script_size);
  }

exit:
  return result;
}

static int app_basic_printer(struct mb_interpreter_t *s, const char *fmt, ...) {
  (void)s;
  if (fmt == NULL) {
    return 0;
  }

#if BSP_HAS_DISPLAY
  if (app_basic_print_target == APP_BASIC_PRINT_TARGET_DISPLAY) {
    va_list args;
    va_start(args, fmt);
    int result = app_basic_write_to_display(fmt, args);
    va_end(args);
    return result;
  }
#endif

  va_list args;
  va_start(args, fmt);
  int result = vprintf(fmt, args);
  va_end(args);
  return result;
}

#if BSP_HAS_DISPLAY
static int app_basic_write_to_display(const char *fmt, va_list args) {
  if (strcmp(fmt, APP_BASIC_DIRECT_STRING_FMT) == 0) {
    const char *text = va_arg(args, const char *);
    if (text == NULL) {
      text = "";
    }
    return basic_display_write_text(text) == SUCCESS ? (int)strlen(text) : -1;
  }
  return app_basic_write_formatted_to_display(fmt, args);
}

static int app_basic_write_formatted_to_display(const char *fmt, va_list args) {
  char buffer[APP_BASIC_PRINT_BUFFER_SIZE];
  int length = vsnprintf(buffer, sizeof(buffer), fmt, args);
  if (length < 0) {
    return length;
  }

  buffer[sizeof(buffer) - 1U] = '\0';
  if (basic_display_write_text(buffer) != SUCCESS) {
    return -1;
  }
  return length;
}
#endif

static void app_basic_close_current(void) {
  if (app_basic_interpreter != NULL) {
    (void)mb_close(&app_basic_interpreter);
    app_basic_interpreter = NULL;
  }
}

static void app_basic_set_status_from_info(app_basic_slot_t slot, const eeprom_basic_script_info_t *info) {
  memset(&app_basic_status, 0, sizeof(app_basic_status));
  app_basic_status.loaded_slot = slot;
  app_basic_status.format_version = info->format_version;
  app_basic_status.build_number = info->metadata.build_number;
  app_basic_status.created_at_unix = info->metadata.created_at_unix;
  app_basic_status.dependency_count = info->metadata.dependency_count;
  app_basic_status.signature = info->signature;
  (void)strncpy(app_basic_status.loaded_name, info->name, sizeof(app_basic_status.loaded_name) - 1U);
  (void)strncpy(app_basic_status.package_version, info->metadata.package_version,
                sizeof(app_basic_status.package_version) - 1U);
  (void)strncpy(app_basic_status.entry_name, info->metadata.entry_name, sizeof(app_basic_status.entry_name) - 1U);

  if (app_basic_status.entry_name[0] == '\0') {
    (void)strncpy(app_basic_status.entry_name, app_basic_status.loaded_name, sizeof(app_basic_status.entry_name) - 1U);
  }
  if (app_basic_status.dependency_count > EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT) {
    app_basic_status.dependency_count = EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT;
  }
  for (uint8_t i = 0U; i < app_basic_status.dependency_count; i++) {
    (void)strncpy(app_basic_status.dependencies[i], info->metadata.dependencies[i],
                  sizeof(app_basic_status.dependencies[i]) - 1U);
  }
}

static void app_basic_set_startup_status(eeprom_basic_script_slot_t slot,
                                         eeprom_basic_script_startup_result_t result, bool rollback_performed) {
  app_basic_status.loaded_slot = (app_basic_slot_t)slot;
  app_basic_status.startup_result = result;
  app_basic_status.rollback_performed = rollback_performed;
}

static void app_basic_set_import_status(app_basic_import_status_t status, const char *name,
                                        eeprom_basic_script_slot_t slot, size_t script_size) {
  app_basic_status.last_import_status = status;
  app_basic_status.last_import_slot = slot;
  app_basic_status.last_import_size = script_size;
  app_basic_status.last_import_depth = app_basic_import_depth;
  app_basic_status.last_import_name[0] = '\0';
  if (name != NULL) {
    (void)strncpy(app_basic_status.last_import_name, name, sizeof(app_basic_status.last_import_name) - 1U);
  }
}

static bool app_basic_import_stack_contains(eeprom_basic_script_slot_t slot) {
  for (uint8_t i = 0U; i < app_basic_import_depth; i++) {
    if (app_basic_import_stack[i] == slot) {
      return true;
    }
  }
  return false;
}

static uint32_t app_basic_import_payload_max_for_info(const eeprom_basic_script_info_t *info) {
  if (info == NULL) {
    return 0U;
  }
  if (info->header_size == EEPROM_BASIC_SCRIPT_LEGACY_HEADER_SIZE) {
    return EEPROM_BASIC_SCRIPT_LEGACY_MAX_SIZE;
  }
  if (info->header_size == EEPROM_BASIC_SCRIPT_METADATA_HEADER_SIZE) {
    return EEPROM_BASIC_SCRIPT_UNSIGNED_MAX_SIZE;
  }
  if (info->header_size == EEPROM_BASIC_SCRIPT_HEADER_SIZE) {
    return EEPROM_BASIC_SCRIPT_MAX_SIZE;
  }
  return 0U;
}

static bool app_basic_eeprom_slot_is_valid(eeprom_basic_script_slot_t slot) {
  return slot == EEPROM_BASIC_SCRIPT_SLOT_APP01 || slot == EEPROM_BASIC_SCRIPT_SLOT_APP02;
}

static eeprom_basic_script_slot_t app_basic_other_eeprom_slot(eeprom_basic_script_slot_t slot) {
  return slot == EEPROM_BASIC_SCRIPT_SLOT_APP01 ? EEPROM_BASIC_SCRIPT_SLOT_APP02 : EEPROM_BASIC_SCRIPT_SLOT_APP01;
}

static app_basic_signature_status_t app_basic_verify_script(eeprom_basic_script_slot_t slot,
                                                            const eeprom_basic_script_info_t *info,
                                                            const char *script, size_t script_size) {
  if (info == NULL || script == NULL || script_size == 0U) {
    return APP_BASIC_SIGNATURE_INVALID;
  }

  bool has_signature = info->signature.data_size > 0U && info->signature.format[0] != '\0';
  bool signature_required = (info->signature.flags & EEPROM_BASIC_SCRIPT_SIGNATURE_FLAG_REQUIRED) != 0U;
  if (!has_signature) {
    return signature_required ? APP_BASIC_SIGNATURE_REQUIRED_MISSING : APP_BASIC_SIGNATURE_NOT_PRESENT;
  }
  if (app_basic_signature_verifier == NULL) {
    return signature_required ? APP_BASIC_SIGNATURE_UNSUPPORTED_FORMAT : APP_BASIC_SIGNATURE_SKIPPED;
  }

  app_basic_signature_context_t context = {
    .slot = slot,
    .info = info,
    .script = script,
    .script_size = script_size,
  };
  return app_basic_signature_verifier(&context);
}

static bool app_basic_signature_status_allows_load(app_basic_signature_status_t status) {
  return status == APP_BASIC_SIGNATURE_NOT_PRESENT || status == APP_BASIC_SIGNATURE_SKIPPED ||
         status == APP_BASIC_SIGNATURE_VALID;
}
