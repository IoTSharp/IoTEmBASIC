#include "Storage/Inc/app_eeprom.h"

#include "Storage/Inc/bsp_eeprom.h"
#include "Protocol/Modbus/Inc/modbus_core_crc.h"

#include <stddef.h>
#include <string.h>

#define EEPROM_TIMEOUT_MS 1000U
#define EEPROM_READY_TRIALS 100U
#define EEPROM_PAGE_SIZE 128U
#define EEPROM_BASIC_SCRIPT_MAGIC 0x42415331UL
#define EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V1 1U
#define EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V2 2U
#define EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V3 3U
#define EEPROM_BASIC_SCRIPT_BASE_ADDR 0x2000U
#define EEPROM_BASIC_SCRIPT_TOTAL_SIZE (EEPROM_BASIC_SCRIPT_SLOT_COUNT * EEPROM_BASIC_SCRIPT_SLOT_SIZE)
#define EEPROM_BASIC_LIFECYCLE_MAGIC 0x4241534CUL
#define EEPROM_BASIC_LIFECYCLE_VERSION 1U
#define EEPROM_BASIC_LIFECYCLE_BASE_ADDR (EEPROM_BASIC_SCRIPT_BASE_ADDR + EEPROM_BASIC_SCRIPT_TOTAL_SIZE)

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  char name[EEPROM_BASIC_SCRIPT_NAME_SIZE];
  uint32_t data_size;
  uint16_t crc;
  uint16_t reserved;
  eeprom_basic_script_metadata_t metadata;
  eeprom_basic_script_signature_t signature;
} eeprom_basic_script_header_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t active_slot;
  uint8_t has_candidate;
  uint8_t candidate_slot;
  uint8_t has_previous;
  uint8_t previous_slot;
  uint8_t last_attempt_slot;
  uint8_t last_success_slot;
  uint8_t last_failed_slot;
  uint8_t last_result;
  uint8_t reserved[3];
  uint32_t boot_count;
  uint32_t rollback_count;
  uint16_t crc;
  uint16_t reserved_tail;
} eeprom_basic_script_lifecycle_record_t;

static ErrorStatus eeprom_read_bytes(uint16_t memory_address, void *data, size_t data_size);
static ErrorStatus eeprom_write_bytes(uint16_t memory_address, const void *data, size_t data_size);
static bool eeprom_basic_script_slot_is_valid(eeprom_basic_script_slot_t slot);
static uint16_t eeprom_basic_script_slot_base(eeprom_basic_script_slot_t slot);
static ErrorStatus eeprom_read_basic_script_header(eeprom_basic_script_slot_t slot, eeprom_basic_script_header_t *header);
static void eeprom_fill_basic_script_header(eeprom_basic_script_header_t *header, const char *name, size_t script_size,
                                            uint16_t crc, const eeprom_basic_script_metadata_t *metadata,
                                            const eeprom_basic_script_signature_t *signature);
static void eeprom_fill_basic_script_metadata(eeprom_basic_script_metadata_t *target,
                                              const eeprom_basic_script_metadata_t *source,
                                              const char *fallback_entry_name);
static void eeprom_fill_basic_script_signature(eeprom_basic_script_signature_t *target,
                                               const eeprom_basic_script_signature_t *source);
static bool eeprom_basic_script_header_is_supported(const eeprom_basic_script_header_t *header);
static uint32_t eeprom_basic_script_payload_max_for_header(const eeprom_basic_script_header_t *header);
static void eeprom_basic_script_lifecycle_default(eeprom_basic_script_lifecycle_state_t *state);
static void eeprom_basic_script_lifecycle_normalize(eeprom_basic_script_lifecycle_state_t *state);
static bool eeprom_basic_script_startup_result_is_valid(eeprom_basic_script_startup_result_t result);
static uint16_t eeprom_basic_script_lifecycle_crc(const eeprom_basic_script_lifecycle_record_t *record);
static void eeprom_basic_script_lifecycle_record_to_state(const eeprom_basic_script_lifecycle_record_t *record,
                                                          eeprom_basic_script_lifecycle_state_t *state);
static void eeprom_basic_script_lifecycle_state_to_record(const eeprom_basic_script_lifecycle_state_t *state,
                                                          eeprom_basic_script_lifecycle_record_t *record);
static void eeprom_copy_text(char *target, size_t target_size, const char *source);
static void eeprom_copy_fixed_text(char *target, size_t target_size, const char *source, size_t source_size);
static void eeprom_copy_fixed_leaf_name(char *target, size_t target_size, const char *source, size_t source_size);
static const char *eeprom_basic_script_leaf_name(const char *name);
static bool eeprom_basic_script_name_equals(const char *left, const char *right);

typedef char eeprom_basic_script_header_size_check[(sizeof(eeprom_basic_script_header_t) == EEPROM_BASIC_SCRIPT_HEADER_SIZE)
                                                    ? 1
                                                    : -1];
typedef char eeprom_basic_script_payload_size_check
  [(EEPROM_BASIC_SCRIPT_MAX_SIZE == (EEPROM_BASIC_SCRIPT_SLOT_SIZE - EEPROM_BASIC_SCRIPT_HEADER_SIZE)) ? 1 : -1];
typedef char eeprom_basic_script_unsigned_payload_size_check
  [(EEPROM_BASIC_SCRIPT_UNSIGNED_MAX_SIZE == (EEPROM_BASIC_SCRIPT_SLOT_SIZE - EEPROM_BASIC_SCRIPT_METADATA_HEADER_SIZE))
     ? 1
     : -1];
typedef char eeprom_basic_script_legacy_payload_size_check
  [(EEPROM_BASIC_SCRIPT_LEGACY_MAX_SIZE == (EEPROM_BASIC_SCRIPT_SLOT_SIZE - EEPROM_BASIC_SCRIPT_LEGACY_HEADER_SIZE)) ? 1
                                                                                                                     : -1];
typedef char eeprom_basic_script_address_size_check
  [((EEPROM_BASIC_LIFECYCLE_BASE_ADDR + sizeof(eeprom_basic_script_lifecycle_record_t)) <= 0x10000UL) ? 1 : -1];
typedef char eeprom_basic_script_lifecycle_record_size_check
  [(sizeof(eeprom_basic_script_lifecycle_record_t) == 32U) ? 1 : -1];
typedef char eeprom_basic_script_lifecycle_crc_size_check
  [(offsetof(eeprom_basic_script_lifecycle_record_t, crc) <= UINT16_MAX) ? 1 : -1];

ErrorStatus eeprom_write_config_data(const void *data, size_t data_size) {
  return eeprom_write_bytes(EEPROM_CONFIG_BASE_ADDR, data, data_size);
}

ErrorStatus eeprom_read_config_data(void *data, size_t data_size) {
  return eeprom_read_bytes(EEPROM_CONFIG_BASE_ADDR, data, data_size);
}

ErrorStatus eeprom_write_basic_script(eeprom_basic_script_slot_t slot, const char *name, const char *script,
                                      size_t script_size) {
  return eeprom_write_basic_script_with_metadata(slot, name, script, script_size, NULL);
}

ErrorStatus eeprom_write_basic_script_with_metadata(eeprom_basic_script_slot_t slot, const char *name,
                                                    const char *script, size_t script_size,
                                                    const eeprom_basic_script_metadata_t *metadata) {
  return eeprom_write_basic_script_package(slot, name, script, script_size, metadata, NULL);
}

ErrorStatus eeprom_write_basic_script_package(eeprom_basic_script_slot_t slot, const char *name, const char *script,
                                              size_t script_size,
                                              const eeprom_basic_script_metadata_t *metadata,
                                              const eeprom_basic_script_signature_t *signature) {
  if (!eeprom_basic_script_slot_is_valid(slot) || name == NULL || script == NULL || script_size == 0U ||
      script_size > EEPROM_BASIC_SCRIPT_MAX_SIZE) {
    return ERROR;
  }

  uint16_t base = eeprom_basic_script_slot_base(slot);
  uint16_t crc = GetCRCData((uint8_t *)script, (uint16_t)script_size);
  eeprom_basic_script_header_t header;
  eeprom_fill_basic_script_header(&header, name, script_size, crc, metadata, signature);

  if (eeprom_write_bytes((uint16_t)(base + EEPROM_BASIC_SCRIPT_HEADER_SIZE), script, script_size) != SUCCESS) {
    return ERROR;
  }
  return eeprom_write_bytes(base, &header, sizeof(header));
}

ErrorStatus eeprom_read_basic_script(eeprom_basic_script_slot_t slot, char *script, size_t script_size,
                                     size_t *actual_size) {
  if (!eeprom_basic_script_slot_is_valid(slot) || script == NULL || script_size == 0U) {
    return ERROR;
  }
  if (actual_size != NULL) {
    *actual_size = 0U;
  }

  eeprom_basic_script_header_t header;
  if (eeprom_read_basic_script_header(slot, &header) != SUCCESS) {
    return ERROR;
  }
  if (!eeprom_basic_script_header_is_supported(&header) || header.data_size == 0U ||
      header.data_size > eeprom_basic_script_payload_max_for_header(&header) || header.data_size >= script_size) {
    return ERROR;
  }

  uint16_t base = eeprom_basic_script_slot_base(slot);
  if (eeprom_read_bytes((uint16_t)(base + header.header_size), script, header.data_size) != SUCCESS) {
    return ERROR;
  }
  if (GetCRCData((uint8_t *)script, (uint16_t)header.data_size) != header.crc) {
    return ERROR;
  }

  script[header.data_size] = '\0';
  if (actual_size != NULL) {
    *actual_size = header.data_size;
  }
  return SUCCESS;
}

ErrorStatus eeprom_get_basic_script_info(eeprom_basic_script_slot_t slot, eeprom_basic_script_info_t *info) {
  if (!eeprom_basic_script_slot_is_valid(slot) || info == NULL) {
    return ERROR;
  }

  memset(info, 0, sizeof(*info));
  eeprom_basic_script_header_t header;
  if (eeprom_read_basic_script_header(slot, &header) != SUCCESS) {
    return ERROR;
  }

  info->format_version = header.version;
  info->header_size = header.header_size;
  memcpy(info->name, header.name, sizeof(info->name));
  info->name[EEPROM_BASIC_SCRIPT_NAME_SIZE - 1U] = '\0';
  info->data_size = header.data_size;
  info->crc = header.crc;
  if (header.version >= EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V2) {
    info->metadata = header.metadata;
    info->metadata.package_version[EEPROM_BASIC_SCRIPT_VERSION_SIZE - 1U] = '\0';
    info->metadata.entry_name[EEPROM_BASIC_SCRIPT_ENTRY_NAME_SIZE - 1U] = '\0';
    for (uint8_t i = 0U; i < EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT; i++) {
      info->metadata.dependencies[i][EEPROM_BASIC_SCRIPT_DEPENDENCY_NAME_SIZE - 1U] = '\0';
    }
    if (info->metadata.dependency_count > EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT) {
      info->metadata.dependency_count = EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT;
    }
  } else {
    eeprom_fill_basic_script_metadata(&info->metadata, NULL, info->name);
  }
  if (header.version >= EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V3) {
    info->signature = header.signature;
    info->signature.format[EEPROM_BASIC_SCRIPT_SIGNATURE_FORMAT_SIZE - 1U] = '\0';
    if (info->signature.data_size > EEPROM_BASIC_SCRIPT_SIGNATURE_DATA_SIZE) {
      info->signature.data_size = 0U;
      info->signature.flags = 0U;
      memset(info->signature.data, 0, sizeof(info->signature.data));
    }
  }
  info->valid = eeprom_basic_script_header_is_supported(&header) && header.data_size > 0U &&
                header.data_size <= eeprom_basic_script_payload_max_for_header(&header);

  return SUCCESS;
}

ErrorStatus eeprom_get_basic_script_lifecycle_state(eeprom_basic_script_lifecycle_state_t *state) {
  if (state == NULL) {
    return ERROR;
  }

  eeprom_basic_script_lifecycle_default(state);

  eeprom_basic_script_lifecycle_record_t record;
  memset(&record, 0, sizeof(record));
  if (eeprom_read_bytes((uint16_t)EEPROM_BASIC_LIFECYCLE_BASE_ADDR, &record, sizeof(record)) != SUCCESS) {
    return ERROR;
  }

  if (record.magic != EEPROM_BASIC_LIFECYCLE_MAGIC || record.version != EEPROM_BASIC_LIFECYCLE_VERSION ||
      record.size != sizeof(record)) {
    return SUCCESS;
  }
  if (record.crc != eeprom_basic_script_lifecycle_crc(&record)) {
    return SUCCESS;
  }

  eeprom_basic_script_lifecycle_record_to_state(&record, state);
  return SUCCESS;
}

ErrorStatus eeprom_write_basic_script_lifecycle_state(const eeprom_basic_script_lifecycle_state_t *state) {
  if (state == NULL) {
    return ERROR;
  }

  eeprom_basic_script_lifecycle_state_t normalized = *state;
  eeprom_basic_script_lifecycle_normalize(&normalized);
  normalized.valid = true;

  eeprom_basic_script_lifecycle_record_t record;
  eeprom_basic_script_lifecycle_state_to_record(&normalized, &record);
  if (eeprom_write_bytes((uint16_t)EEPROM_BASIC_LIFECYCLE_BASE_ADDR, &record, sizeof(record)) != SUCCESS) {
    return ERROR;
  }

  eeprom_basic_script_lifecycle_record_t verify;
  memset(&verify, 0, sizeof(verify));
  if (eeprom_read_bytes((uint16_t)EEPROM_BASIC_LIFECYCLE_BASE_ADDR, &verify, sizeof(verify)) != SUCCESS) {
    return ERROR;
  }
  return memcmp(&record, &verify, sizeof(record)) == 0 ? SUCCESS : ERROR;
}

ErrorStatus eeprom_set_basic_script_active(eeprom_basic_script_slot_t slot) {
  if (!eeprom_basic_script_slot_is_valid(slot)) {
    return ERROR;
  }

  eeprom_basic_script_lifecycle_state_t state;
  if (eeprom_get_basic_script_lifecycle_state(&state) != SUCCESS) {
    eeprom_basic_script_lifecycle_default(&state);
  }

  eeprom_basic_script_slot_t old_active = state.active_slot;
  state.active_slot = slot;
  state.has_candidate = false;
  state.candidate_slot = old_active == EEPROM_BASIC_SCRIPT_SLOT_APP01 ? EEPROM_BASIC_SCRIPT_SLOT_APP02
                                                                      : EEPROM_BASIC_SCRIPT_SLOT_APP01;
  state.has_previous = old_active != slot;
  state.previous_slot = old_active;
  state.last_success_slot = slot;
  state.last_result = EEPROM_BASIC_SCRIPT_STARTUP_SUCCESS;
  return eeprom_write_basic_script_lifecycle_state(&state);
}

ErrorStatus eeprom_set_basic_script_candidate(eeprom_basic_script_slot_t slot) {
  if (!eeprom_basic_script_slot_is_valid(slot)) {
    return ERROR;
  }

  eeprom_basic_script_lifecycle_state_t state;
  if (eeprom_get_basic_script_lifecycle_state(&state) != SUCCESS) {
    eeprom_basic_script_lifecycle_default(&state);
  }

  state.has_candidate = true;
  state.candidate_slot = slot;
  state.has_previous = state.active_slot != slot;
  state.previous_slot = state.active_slot;
  state.last_attempt_slot = slot;
  state.last_failed_slot = state.active_slot;
  state.last_result = EEPROM_BASIC_SCRIPT_STARTUP_CANDIDATE_PENDING;
  return eeprom_write_basic_script_lifecycle_state(&state);
}

bool eeprom_basic_script_slot_from_package_name(const char *name, eeprom_basic_script_slot_t *slot) {
  if (name == NULL || slot == NULL) {
    return false;
  }
  name = eeprom_basic_script_leaf_name(name);

  for (uint8_t i = 0U; i < EEPROM_BASIC_SCRIPT_SLOT_COUNT; i++) {
    eeprom_basic_script_slot_t current_slot = (eeprom_basic_script_slot_t)i;
    eeprom_basic_script_info_t info;
    if (eeprom_get_basic_script_info(current_slot, &info) != SUCCESS) {
      continue;
    }
    if (eeprom_basic_script_name_equals(name, info.name) ||
        eeprom_basic_script_name_equals(name, info.metadata.entry_name)) {
      *slot = current_slot;
      return true;
    }
  }

  /* Compatibility only: legacy scripts may still import physical slot names. */
  if (eeprom_basic_script_name_equals(name, "app01.bas") || eeprom_basic_script_name_equals(name, "app01")) {
    *slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
    return true;
  }
  if (eeprom_basic_script_name_equals(name, "app02.bas") || eeprom_basic_script_name_equals(name, "app02")) {
    *slot = EEPROM_BASIC_SCRIPT_SLOT_APP02;
    return true;
  }
  return false;
}

const char *eeprom_basic_script_slot_name(eeprom_basic_script_slot_t slot) {
  switch (slot) {
  case EEPROM_BASIC_SCRIPT_SLOT_APP01:
    return "app01.bas";
  case EEPROM_BASIC_SCRIPT_SLOT_APP02:
    return "app02.bas";
  default:
    return "";
  }
}

const char *eeprom_basic_script_startup_result_name(eeprom_basic_script_startup_result_t result) {
  switch (result) {
  case EEPROM_BASIC_SCRIPT_STARTUP_NONE:
    return "none";
  case EEPROM_BASIC_SCRIPT_STARTUP_CANDIDATE_PENDING:
    return "candidate-pending";
  case EEPROM_BASIC_SCRIPT_STARTUP_BOOTING:
    return "booting";
  case EEPROM_BASIC_SCRIPT_STARTUP_SUCCESS:
    return "success";
  case EEPROM_BASIC_SCRIPT_STARTUP_LOAD_FAILED:
    return "load-failed";
  case EEPROM_BASIC_SCRIPT_STARTUP_RUN_FAILED:
    return "run-failed";
  case EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_SUCCESS:
    return "rollback-success";
  case EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_LOAD_FAILED:
    return "rollback-load-failed";
  case EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_RUN_FAILED:
    return "rollback-run-failed";
  default:
    return "unknown";
  }
}

static ErrorStatus eeprom_read_bytes(uint16_t memory_address, void *data, size_t data_size) {
  if (data == NULL || data_size == 0U || data_size > UINT16_MAX) {
    return ERROR;
  }
  if ((uint32_t)memory_address + data_size > 0x10000UL) {
    return ERROR;
  }

  if (bsp_eeprom_read(memory_address, (uint8_t *)data, (uint16_t)data_size, EEPROM_TIMEOUT_MS) != HAL_OK) {
    return ERROR;
  }

  return SUCCESS;
}

static ErrorStatus eeprom_write_bytes(uint16_t memory_address, const void *data, size_t data_size) {
  if (data == NULL || data_size == 0U || data_size > UINT16_MAX) {
    return ERROR;
  }
  if ((uint32_t)memory_address + data_size > 0x10000UL) {
    return ERROR;
  }

  const uint8_t *bytes = (const uint8_t *)data;
  uint16_t written = 0U;
  while (written < data_size) {
    uint16_t current_address = (uint16_t)(memory_address + written);
    uint16_t page_offset = (uint16_t)(current_address % EEPROM_PAGE_SIZE);
    uint16_t page_remaining = (uint16_t)(EEPROM_PAGE_SIZE - page_offset);
    uint16_t remaining = (uint16_t)(data_size - written);
    uint16_t chunk = remaining < page_remaining ? remaining : page_remaining;

    if (bsp_eeprom_write(current_address, bytes + written, chunk, EEPROM_TIMEOUT_MS) != HAL_OK) {
      return ERROR;
    }

    if (bsp_eeprom_is_ready(EEPROM_READY_TRIALS, EEPROM_TIMEOUT_MS) != HAL_OK) {
      return ERROR;
    }

    written = (uint16_t)(written + chunk);
  }

  return SUCCESS;
}

static bool eeprom_basic_script_slot_is_valid(eeprom_basic_script_slot_t slot) {
  return slot == EEPROM_BASIC_SCRIPT_SLOT_APP01 || slot == EEPROM_BASIC_SCRIPT_SLOT_APP02;
}

static uint16_t eeprom_basic_script_slot_base(eeprom_basic_script_slot_t slot) {
  return (uint16_t)(EEPROM_BASIC_SCRIPT_BASE_ADDR + ((uint16_t)slot * EEPROM_BASIC_SCRIPT_SLOT_SIZE));
}

static ErrorStatus eeprom_read_basic_script_header(eeprom_basic_script_slot_t slot, eeprom_basic_script_header_t *header) {
  if (header == NULL) {
    return ERROR;
  }
  memset(header, 0, sizeof(*header));
  return eeprom_read_bytes(eeprom_basic_script_slot_base(slot), header, sizeof(*header));
}

static void eeprom_fill_basic_script_header(eeprom_basic_script_header_t *header, const char *name, size_t script_size,
                                            uint16_t crc, const eeprom_basic_script_metadata_t *metadata,
                                            const eeprom_basic_script_signature_t *signature) {
  const char *leaf_name = eeprom_basic_script_leaf_name(name);
  memset(header, 0, sizeof(*header));
  header->magic = EEPROM_BASIC_SCRIPT_MAGIC;
  header->version = EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V3;
  header->header_size = EEPROM_BASIC_SCRIPT_HEADER_SIZE;
  eeprom_copy_text(header->name, sizeof(header->name), leaf_name);
  header->data_size = (uint32_t)script_size;
  header->crc = crc;
  eeprom_fill_basic_script_metadata(&header->metadata, metadata, leaf_name);
  eeprom_fill_basic_script_signature(&header->signature, signature);
}

static void eeprom_fill_basic_script_metadata(eeprom_basic_script_metadata_t *target,
                                              const eeprom_basic_script_metadata_t *source,
                                              const char *fallback_entry_name) {
  memset(target, 0, sizeof(*target));
  eeprom_copy_text(target->entry_name, sizeof(target->entry_name), fallback_entry_name);
  if (source == NULL) {
    return;
  }

  eeprom_copy_fixed_text(target->package_version, sizeof(target->package_version), source->package_version,
                         sizeof(source->package_version));
  target->created_at_unix = source->created_at_unix;
  target->build_number = source->build_number;

  if (source->entry_name[0] != '\0') {
    eeprom_copy_fixed_leaf_name(target->entry_name, sizeof(target->entry_name), source->entry_name,
                                sizeof(source->entry_name));
  }

  uint8_t source_count = source->dependency_count;
  if (source_count > EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT) {
    source_count = EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT;
  }

  for (uint8_t i = 0U; i < source_count; i++) {
    if (source->dependencies[i][0] == '\0') {
      continue;
    }
    uint8_t target_index = target->dependency_count;
    if (target_index >= EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT) {
      break;
    }
    eeprom_copy_fixed_leaf_name(target->dependencies[target_index], sizeof(target->dependencies[target_index]),
                                source->dependencies[i], sizeof(source->dependencies[i]));
    if (target->dependencies[target_index][0] != '\0') {
      target->dependency_count++;
    }
  }
}

static void eeprom_fill_basic_script_signature(eeprom_basic_script_signature_t *target,
                                               const eeprom_basic_script_signature_t *source) {
  memset(target, 0, sizeof(*target));
  if (source == NULL) {
    return;
  }

  uint16_t data_size = source->data_size;
  if (data_size > EEPROM_BASIC_SCRIPT_SIGNATURE_DATA_SIZE) {
    data_size = EEPROM_BASIC_SCRIPT_SIGNATURE_DATA_SIZE;
  }

  eeprom_copy_fixed_text(target->format, sizeof(target->format), source->format, sizeof(source->format));
  target->data_size = data_size;
  target->flags = source->flags;
  if (data_size > 0U) {
    memcpy(target->data, source->data, data_size);
  }
}

static bool eeprom_basic_script_header_is_supported(const eeprom_basic_script_header_t *header) {
  if (header->magic != EEPROM_BASIC_SCRIPT_MAGIC) {
    return false;
  }
  if (header->version == EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V1 &&
      header->header_size == EEPROM_BASIC_SCRIPT_LEGACY_HEADER_SIZE) {
    return true;
  }
  if (header->version == EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V2 &&
      header->header_size == EEPROM_BASIC_SCRIPT_METADATA_HEADER_SIZE) {
    return true;
  }
  return header->version == EEPROM_BASIC_SCRIPT_FORMAT_VERSION_V3 && header->header_size == EEPROM_BASIC_SCRIPT_HEADER_SIZE;
}

static uint32_t eeprom_basic_script_payload_max_for_header(const eeprom_basic_script_header_t *header) {
  if (header->header_size == EEPROM_BASIC_SCRIPT_LEGACY_HEADER_SIZE) {
    return EEPROM_BASIC_SCRIPT_LEGACY_MAX_SIZE;
  }
  if (header->header_size == EEPROM_BASIC_SCRIPT_METADATA_HEADER_SIZE) {
    return EEPROM_BASIC_SCRIPT_UNSIGNED_MAX_SIZE;
  }
  return EEPROM_BASIC_SCRIPT_MAX_SIZE;
}

static void eeprom_basic_script_lifecycle_default(eeprom_basic_script_lifecycle_state_t *state) {
  memset(state, 0, sizeof(*state));
  state->active_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
  state->candidate_slot = EEPROM_BASIC_SCRIPT_SLOT_APP02;
  state->previous_slot = EEPROM_BASIC_SCRIPT_SLOT_APP02;
  state->last_attempt_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
  state->last_success_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
  state->last_failed_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
  state->last_result = EEPROM_BASIC_SCRIPT_STARTUP_NONE;
}

static void eeprom_basic_script_lifecycle_normalize(eeprom_basic_script_lifecycle_state_t *state) {
  if (!eeprom_basic_script_slot_is_valid(state->active_slot)) {
    state->active_slot = EEPROM_BASIC_SCRIPT_SLOT_APP01;
  }
  if (!state->has_candidate || !eeprom_basic_script_slot_is_valid(state->candidate_slot)) {
    state->has_candidate = false;
    state->candidate_slot = state->active_slot == EEPROM_BASIC_SCRIPT_SLOT_APP01 ? EEPROM_BASIC_SCRIPT_SLOT_APP02
                                                                                 : EEPROM_BASIC_SCRIPT_SLOT_APP01;
  }
  if (!state->has_previous || !eeprom_basic_script_slot_is_valid(state->previous_slot)) {
    state->has_previous = false;
    state->previous_slot = state->active_slot == EEPROM_BASIC_SCRIPT_SLOT_APP01 ? EEPROM_BASIC_SCRIPT_SLOT_APP02
                                                                               : EEPROM_BASIC_SCRIPT_SLOT_APP01;
  }
  if (!eeprom_basic_script_slot_is_valid(state->last_attempt_slot)) {
    state->last_attempt_slot = state->active_slot;
  }
  if (!eeprom_basic_script_slot_is_valid(state->last_success_slot)) {
    state->last_success_slot = state->active_slot;
  }
  if (!eeprom_basic_script_slot_is_valid(state->last_failed_slot)) {
    state->last_failed_slot = state->active_slot;
  }
  if (!eeprom_basic_script_startup_result_is_valid(state->last_result)) {
    state->last_result = EEPROM_BASIC_SCRIPT_STARTUP_NONE;
  }
}

static bool eeprom_basic_script_startup_result_is_valid(eeprom_basic_script_startup_result_t result) {
  return result == EEPROM_BASIC_SCRIPT_STARTUP_NONE ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_CANDIDATE_PENDING ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_BOOTING ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_SUCCESS ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_LOAD_FAILED ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_RUN_FAILED ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_SUCCESS ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_LOAD_FAILED ||
         result == EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_RUN_FAILED;
}

static uint16_t eeprom_basic_script_lifecycle_crc(const eeprom_basic_script_lifecycle_record_t *record) {
  return GetCRCData((uint8_t *)record, (uint16_t)offsetof(eeprom_basic_script_lifecycle_record_t, crc));
}

static void eeprom_basic_script_lifecycle_record_to_state(const eeprom_basic_script_lifecycle_record_t *record,
                                                          eeprom_basic_script_lifecycle_state_t *state) {
  eeprom_basic_script_lifecycle_default(state);
  state->valid = true;
  state->active_slot = (eeprom_basic_script_slot_t)record->active_slot;
  state->has_candidate = record->has_candidate != 0U;
  state->candidate_slot = (eeprom_basic_script_slot_t)record->candidate_slot;
  state->has_previous = record->has_previous != 0U;
  state->previous_slot = (eeprom_basic_script_slot_t)record->previous_slot;
  state->last_attempt_slot = (eeprom_basic_script_slot_t)record->last_attempt_slot;
  state->last_success_slot = (eeprom_basic_script_slot_t)record->last_success_slot;
  state->last_failed_slot = (eeprom_basic_script_slot_t)record->last_failed_slot;
  state->last_result = (eeprom_basic_script_startup_result_t)record->last_result;
  state->boot_count = record->boot_count;
  state->rollback_count = record->rollback_count;
  eeprom_basic_script_lifecycle_normalize(state);
}

static void eeprom_basic_script_lifecycle_state_to_record(const eeprom_basic_script_lifecycle_state_t *state,
                                                          eeprom_basic_script_lifecycle_record_t *record) {
  memset(record, 0, sizeof(*record));
  record->magic = EEPROM_BASIC_LIFECYCLE_MAGIC;
  record->version = EEPROM_BASIC_LIFECYCLE_VERSION;
  record->size = sizeof(*record);
  record->active_slot = (uint8_t)state->active_slot;
  record->has_candidate = state->has_candidate ? 1U : 0U;
  record->candidate_slot = (uint8_t)state->candidate_slot;
  record->has_previous = state->has_previous ? 1U : 0U;
  record->previous_slot = (uint8_t)state->previous_slot;
  record->last_attempt_slot = (uint8_t)state->last_attempt_slot;
  record->last_success_slot = (uint8_t)state->last_success_slot;
  record->last_failed_slot = (uint8_t)state->last_failed_slot;
  record->last_result = (uint8_t)state->last_result;
  record->boot_count = state->boot_count;
  record->rollback_count = state->rollback_count;
  record->crc = eeprom_basic_script_lifecycle_crc(record);
}

static void eeprom_copy_text(char *target, size_t target_size, const char *source) {
  if (target == NULL || target_size == 0U) {
    return;
  }

  target[0] = '\0';
  if (source == NULL) {
    return;
  }

  size_t i = 0U;
  while (i + 1U < target_size && source[i] != '\0') {
    target[i] = source[i];
    i++;
  }
  target[i] = '\0';
}

static void eeprom_copy_fixed_text(char *target, size_t target_size, const char *source, size_t source_size) {
  if (target == NULL || target_size == 0U) {
    return;
  }

  target[0] = '\0';
  if (source == NULL || source_size == 0U) {
    return;
  }

  size_t i = 0U;
  while (i + 1U < target_size && i < source_size && source[i] != '\0') {
    target[i] = source[i];
    i++;
  }
  target[i] = '\0';
}

static void eeprom_copy_fixed_leaf_name(char *target, size_t target_size, const char *source, size_t source_size) {
  if (target == NULL || target_size == 0U) {
    return;
  }

  target[0] = '\0';
  if (source == NULL || source_size == 0U) {
    return;
  }

  size_t leaf_index = 0U;
  for (size_t i = 0U; i < source_size && source[i] != '\0'; i++) {
    if (source[i] == '/' || source[i] == '\\') {
      leaf_index = i + 1U;
    }
  }

  eeprom_copy_fixed_text(target, target_size, source + leaf_index, source_size - leaf_index);
}

static const char *eeprom_basic_script_leaf_name(const char *name) {
  const char *leaf = name;
  while (*name != '\0') {
    if (*name == '/' || *name == '\\') {
      leaf = name + 1;
    }
    name++;
  }
  return leaf;
}

static bool eeprom_basic_script_name_equals(const char *left, const char *right) {
  while (*left != '\0' && *right != '\0') {
    char l = *left;
    char r = *right;
    if (l >= 'A' && l <= 'Z') {
      l = (char)(l - 'A' + 'a');
    }
    if (r >= 'A' && r <= 'Z') {
      r = (char)(r - 'A' + 'a');
    }
    if (l != r) {
      return false;
    }
    left++;
    right++;
  }
  return *left == '\0' && *right == '\0';
}
