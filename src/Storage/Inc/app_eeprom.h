#ifndef APP_EEPROM_H
#define APP_EEPROM_H

#include "Common/Inc/app_types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_MSG_BASE_ADDR 0x0000U
#define EEPROM_CONFIG_BASE_ADDR STORE_MSG_BASE_ADDR
#define EEPROM_BASIC_SCRIPT_SLOT_COUNT 2U
#define EEPROM_BASIC_SCRIPT_NAME_SIZE 16U
#define EEPROM_BASIC_SCRIPT_VERSION_SIZE 16U
#define EEPROM_BASIC_SCRIPT_ENTRY_NAME_SIZE EEPROM_BASIC_SCRIPT_NAME_SIZE
#define EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT 4U
#define EEPROM_BASIC_SCRIPT_DEPENDENCY_NAME_SIZE EEPROM_BASIC_SCRIPT_NAME_SIZE
#define EEPROM_BASIC_SCRIPT_SIGNATURE_FORMAT_SIZE 16U
#define EEPROM_BASIC_SCRIPT_SIGNATURE_DATA_SIZE 96U
#define EEPROM_BASIC_SCRIPT_SIGNATURE_FLAG_REQUIRED 0x01U
#define EEPROM_BASIC_SCRIPT_LEGACY_HEADER_SIZE 32U
#define EEPROM_BASIC_SCRIPT_METADATA_HEADER_SIZE 140U
#define EEPROM_BASIC_SCRIPT_SIGNED_HEADER_SIZE 256U
#define EEPROM_BASIC_SCRIPT_HEADER_SIZE EEPROM_BASIC_SCRIPT_SIGNED_HEADER_SIZE
#define EEPROM_BASIC_SCRIPT_SLOT_SIZE 4096U
#define EEPROM_BASIC_SCRIPT_LEGACY_MAX_SIZE (EEPROM_BASIC_SCRIPT_SLOT_SIZE - EEPROM_BASIC_SCRIPT_LEGACY_HEADER_SIZE)
#define EEPROM_BASIC_SCRIPT_UNSIGNED_MAX_SIZE (EEPROM_BASIC_SCRIPT_SLOT_SIZE - EEPROM_BASIC_SCRIPT_METADATA_HEADER_SIZE)
#define EEPROM_BASIC_SCRIPT_MAX_SIZE (EEPROM_BASIC_SCRIPT_SLOT_SIZE - EEPROM_BASIC_SCRIPT_HEADER_SIZE)
#define EEPROM_BASIC_SCRIPT_READ_BUFFER_SIZE (EEPROM_BASIC_SCRIPT_LEGACY_MAX_SIZE + 1U)

typedef enum {
  EEPROM_BASIC_SCRIPT_SLOT_APP01 = 0,
  EEPROM_BASIC_SCRIPT_SLOT_APP02 = 1,
} eeprom_basic_script_slot_t;

typedef enum {
  EEPROM_BASIC_SCRIPT_STARTUP_NONE = 0,
  EEPROM_BASIC_SCRIPT_STARTUP_CANDIDATE_PENDING,
  EEPROM_BASIC_SCRIPT_STARTUP_BOOTING,
  EEPROM_BASIC_SCRIPT_STARTUP_SUCCESS,
  EEPROM_BASIC_SCRIPT_STARTUP_LOAD_FAILED,
  EEPROM_BASIC_SCRIPT_STARTUP_RUN_FAILED,
  EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_SUCCESS,
  EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_LOAD_FAILED,
  EEPROM_BASIC_SCRIPT_STARTUP_ROLLBACK_RUN_FAILED,
} eeprom_basic_script_startup_result_t;

typedef struct {
  char package_version[EEPROM_BASIC_SCRIPT_VERSION_SIZE];
  uint32_t created_at_unix;
  uint32_t build_number;
  char entry_name[EEPROM_BASIC_SCRIPT_ENTRY_NAME_SIZE];
  uint8_t dependency_count;
  char dependencies[EEPROM_BASIC_SCRIPT_DEPENDENCY_COUNT][EEPROM_BASIC_SCRIPT_DEPENDENCY_NAME_SIZE];
} eeprom_basic_script_metadata_t;

typedef struct {
  char format[EEPROM_BASIC_SCRIPT_SIGNATURE_FORMAT_SIZE];
  uint16_t data_size;
  uint8_t flags;
  uint8_t reserved;
  uint8_t data[EEPROM_BASIC_SCRIPT_SIGNATURE_DATA_SIZE];
} eeprom_basic_script_signature_t;

typedef struct {
  uint16_t format_version;
  uint16_t header_size;
  char name[EEPROM_BASIC_SCRIPT_NAME_SIZE];
  uint32_t data_size;
  uint16_t crc;
  bool valid;
  eeprom_basic_script_metadata_t metadata;
  eeprom_basic_script_signature_t signature;
} eeprom_basic_script_info_t;

typedef struct {
  bool valid;
  eeprom_basic_script_slot_t active_slot;
  bool has_candidate;
  eeprom_basic_script_slot_t candidate_slot;
  bool has_previous;
  eeprom_basic_script_slot_t previous_slot;
  eeprom_basic_script_slot_t last_attempt_slot;
  eeprom_basic_script_slot_t last_success_slot;
  eeprom_basic_script_slot_t last_failed_slot;
  eeprom_basic_script_startup_result_t last_result;
  uint32_t boot_count;
  uint32_t rollback_count;
} eeprom_basic_script_lifecycle_state_t;

ErrorStatus eeprom_write_config_data(const void *data, size_t data_size);
ErrorStatus eeprom_read_config_data(void *data, size_t data_size);
ErrorStatus eeprom_write_basic_script(eeprom_basic_script_slot_t slot, const char *name, const char *script,
                                      size_t script_size);
ErrorStatus eeprom_write_basic_script_with_metadata(eeprom_basic_script_slot_t slot, const char *name,
                                                    const char *script, size_t script_size,
                                                    const eeprom_basic_script_metadata_t *metadata);
ErrorStatus eeprom_write_basic_script_package(eeprom_basic_script_slot_t slot, const char *name, const char *script,
                                              size_t script_size,
                                              const eeprom_basic_script_metadata_t *metadata,
                                              const eeprom_basic_script_signature_t *signature);
ErrorStatus eeprom_read_basic_script(eeprom_basic_script_slot_t slot, char *script, size_t script_size,
                                     size_t *actual_size);
ErrorStatus eeprom_get_basic_script_info(eeprom_basic_script_slot_t slot, eeprom_basic_script_info_t *info);
ErrorStatus eeprom_get_basic_script_lifecycle_state(eeprom_basic_script_lifecycle_state_t *state);
ErrorStatus eeprom_write_basic_script_lifecycle_state(const eeprom_basic_script_lifecycle_state_t *state);
ErrorStatus eeprom_set_basic_script_active(eeprom_basic_script_slot_t slot);
ErrorStatus eeprom_set_basic_script_candidate(eeprom_basic_script_slot_t slot);
bool eeprom_basic_script_slot_from_package_name(const char *name, eeprom_basic_script_slot_t *slot);
const char *eeprom_basic_script_slot_name(eeprom_basic_script_slot_t slot);
const char *eeprom_basic_script_startup_result_name(eeprom_basic_script_startup_result_t result);

#ifdef __cplusplus
}
#endif

#endif
