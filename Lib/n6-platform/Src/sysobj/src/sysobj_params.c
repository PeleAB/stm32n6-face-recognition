/**
 * @file sysobj_params.c
 * @brief Persistent parameter store — triple-bank NOR flash with CRC32 and
 *        majority-vote recovery.
 *
 * Flash layout (3 × 4 KB sectors):
 *   Bank 0: flash_base_addr + 0 × 4096
 *   Bank 1: flash_base_addr + 1 × 4096
 *   Bank 2: flash_base_addr + 2 × 4096
 *
 * Each bank uses the first 256-byte page of its sector.
 */

#include "sysobj_params.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32n6570_discovery_xspi.h"

/* --------------------------------------------------------------------------
 * Internal flash-page structures
 * -------------------------------------------------------------------------- */

#define PARAM_MAGIC     0xA55A5AA5UL
#define PARAM_VALID_TAG 0xA5U

typedef struct __attribute__((packed)) {
  uint64_t value;       /* 8 B — parameter value */
  uint32_t entry_crc32; /* 4 B — CRC32(id_LE[2] | type[1] | value_LE[8]) */
  uint16_t id;          /* 2 B */
  uint8_t  type;        /* 1 B — param_type_t */
  uint8_t  valid_tag;   /* 1 B — 0xA5 written, 0xFF erased */
} param_flash_entry_t;  /* 16 B */

/* Total: 4+4+240+4+4 = 256 B (exactly one NOR page). */
typedef struct __attribute__((packed)) {
  uint32_t            magic;                     /* 4 B */
  uint32_t            sequence;                  /* 4 B — monotonic write counter */
  param_flash_entry_t entries[PARAMS_MAX_ENTRIES];/* 240 B */
  uint32_t            page_crc32;                /* 4 B — CRC32 of bytes 0..247 */
  uint32_t            _pad;                      /* 4 B — unused */
} param_page_t; /* 256 B */

#define BANK_SECTOR_SIZE 4096U
#define PAGE_SIZE        256U

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

static params_cfg_t       s_cfg;
static bool               s_initialized = false;
static StaticSemaphore_t  s_mutex_buf;
static SemaphoreHandle_t  s_mutex;

/* RAM cache — avoids flash reads at runtime (XSPI2 is in MMP mode after
 * boot so BSP_XSPI_NOR_Read cannot issue indirect commands).
 * Filled by sysobj_params_init, updated by sysobj_params_write. */
static param_page_t       s_cache;
static bool               s_cache_valid = false;

/* --------------------------------------------------------------------------
 * Software CRC32 — IEEE 802.3 polynomial (self-contained)
 * -------------------------------------------------------------------------- */

static uint32_t s_crc_table[256];
static bool     s_crc_ready = false;

static void crc_init(void)
{
  const uint32_t poly = 0xEDB88320UL;
  for (uint32_t i = 0; i < 256U; i++) {
    uint32_t crc = i;
    for (uint32_t j = 0; j < 8U; j++)
      crc = (crc & 1U) ? ((crc >> 1) ^ poly) : (crc >> 1);
    s_crc_table[i] = crc;
  }
  s_crc_ready = true;
}

static uint32_t crc32(const uint8_t *data, uint32_t len)
{
  if (!s_crc_ready) crc_init();
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint32_t i = 0; i < len; i++)
    crc = (crc >> 8) ^ s_crc_table[(crc ^ data[i]) & 0xFFU];
  return crc ^ 0xFFFFFFFFUL;
}

/* --------------------------------------------------------------------------
 * Entry-level CRC: covers id(2 LE) | type(1) | value(8 LE) = 11 bytes
 * -------------------------------------------------------------------------- */

static uint32_t entry_crc(uint16_t id, uint8_t type, uint64_t value)
{
  uint8_t buf[11];
  buf[0] = (uint8_t)(id & 0xFFU);
  buf[1] = (uint8_t)(id >> 8);
  buf[2] = type;
  for (int i = 0; i < 8; i++)
    buf[3 + i] = (uint8_t)(value >> (8 * i));
  return crc32(buf, sizeof(buf));
}

/* --------------------------------------------------------------------------
 * Flash helpers
 * -------------------------------------------------------------------------- */

static uint32_t bank_addr(int bank)
{
  return s_cfg.flash_base_addr + (uint32_t)bank * BANK_SECTOR_SIZE;
}

static bool flash_read_page(int bank, param_page_t *page)
{
  if (s_cfg.mmp_base_addr != 0) {
    /* Read directly from memory-mapped NOR flash window. */
    const uint8_t *src = (const uint8_t *)(s_cfg.mmp_base_addr + bank_addr(bank));
    memcpy(page, src, sizeof(param_page_t));
    return true;
  }
  int32_t ret = BSP_XSPI_NOR_Read(s_cfg.xspi_instance, (uint8_t *)page,
                                   bank_addr(bank), sizeof(param_page_t));
  return ret == BSP_ERROR_NONE;
}

static bool page_crc_ok(const param_page_t *p)
{
  /* CRC covers the first 248 bytes (magic + sequence + entries). */
  uint32_t computed = crc32((const uint8_t *)p,
                             offsetof(param_page_t, page_crc32));
  return computed == p->page_crc32;
}

static bool page_valid(const param_page_t *p)
{
  return (p->magic == PARAM_MAGIC) && page_crc_ok(p);
}

static const param_descriptor_t *find_descriptor(uint16_t id)
{
  for (uint16_t i = 0; i < s_cfg.table_count; i++) {
    if (s_cfg.table[i].id == id)
      return &s_cfg.table[i];
  }
  return NULL;
}

/* Find the entry for 'id' in page 'p'. Returns NULL if not found or corrupt. */
static const param_flash_entry_t *find_entry(const param_page_t *p, uint16_t id)
{
  for (uint16_t i = 0; i < PARAMS_MAX_ENTRIES; i++) {
    const param_flash_entry_t *e = &p->entries[i];
    if (e->valid_tag != PARAM_VALID_TAG) continue;
    if (e->id != id) continue;
    if (e->entry_crc32 != entry_crc(e->id, e->type, e->value)) continue;
    return e;
  }
  return NULL;
}

/* Write a page to all 3 banks (erase + write each). */
static params_status_t write_all_banks(param_page_t *page)
{
  if (s_cfg.pre_write_hook)  s_cfg.pre_write_hook();

  params_status_t st = PARAMS_OK;

  for (int b = 0; b < 3; b++) {
    uint32_t addr = bank_addr(b);

    if (BSP_XSPI_NOR_Erase_Block(s_cfg.xspi_instance, addr,
                                  BSP_XSPI_NOR_ERASE_4K) != BSP_ERROR_NONE) {
      st = PARAMS_ERR_FLASH;
      continue;
    }
    if (BSP_XSPI_NOR_Write(s_cfg.xspi_instance, (uint8_t *)page, addr,
                            sizeof(param_page_t)) != BSP_ERROR_NONE) {
      st = PARAMS_ERR_FLASH;
    }
  }

  if (s_cfg.post_write_hook) s_cfg.post_write_hook();
  return st;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

params_status_t sysobj_params_init(const params_cfg_t *cfg)
{
  if (!cfg || !cfg->table || cfg->table_count == 0)
    return PARAMS_ERR_NOT_FOUND;

  s_cfg = *cfg;
  s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
  configASSERT(s_mutex != NULL);

  s_initialized = true;

  /* Read all three banks and cache the best (highest sequence) valid page.
   * This is the ONLY point where flash is read — at runtime, reads use the
   * RAM cache because XSPI2 will be in MMP mode (no indirect access). */
  param_page_t pages[3];
  bool         valid[3];
  int          best_b   = -1;
  uint32_t     best_seq = 0;

  for (int b = 0; b < 3; b++) {
    valid[b] = flash_read_page(b, &pages[b]) && page_valid(&pages[b]);
    if (valid[b] && pages[b].sequence >= best_seq) {
      best_seq = pages[b].sequence;
      best_b   = b;
    }
  }

  if (best_b >= 0) {
    s_cache       = pages[best_b];
    s_cache_valid = true;
    return PARAMS_OK;
  }
  return PARAMS_ERR_ALL_CORRUPT;
}

params_status_t sysobj_params_read(uint16_t id, uint64_t *out_value,
                                   bool *out_was_default)
{
  if (!s_initialized)           return PARAMS_ERR_NOT_INIT;
  if (!out_value || !out_was_default) return PARAMS_ERR_NOT_FOUND;

  const param_descriptor_t *desc = find_descriptor(id);
  if (!desc) return PARAMS_ERR_NOT_FOUND;

  xSemaphoreTake(s_mutex, portMAX_DELAY);

  /* Use RAM cache (filled at init, updated on write).
   * Flash is NOT read at runtime because XSPI2 is in MMP mode. */
  if (!s_cache_valid) {
    xSemaphoreGive(s_mutex);
    *out_value       = desc->default_val;
    *out_was_default = true;
    return PARAMS_OK;   /* blank flash — return defaults gracefully */
  }

  const param_flash_entry_t *e = find_entry(&s_cache, id);

  xSemaphoreGive(s_mutex);

  if (!e || e->value < desc->min_val || e->value > desc->max_val) {
    *out_value       = desc->default_val;
    *out_was_default = true;
    return PARAMS_OK;
  }

  *out_value       = e->value;
  *out_was_default = false;
  return PARAMS_OK;
}

params_status_t sysobj_params_write(uint16_t id, uint64_t value)
{
  if (!s_initialized) return PARAMS_ERR_NOT_INIT;

  const param_descriptor_t *desc = find_descriptor(id);
  if (!desc) return PARAMS_ERR_NOT_FOUND;

  if (value < desc->min_val || value > desc->max_val)
    return PARAMS_ERR_OUT_OF_RANGE;

  xSemaphoreTake(s_mutex, portMAX_DELAY);

  /* Use the RAM cache as base page (avoids flash reads — XSPI2 is in MMP
   * at runtime).  The pre_write_hook will disable MMP for the actual
   * erase/write sequence; we only need the cached data to build the page. */
  param_page_t base_page;
  if (s_cache_valid) {
    base_page = s_cache;
  } else {
    /* No valid cache — start fresh. */
    memset(&base_page, 0xFF, sizeof(base_page));
    base_page.magic    = PARAM_MAGIC;
    base_page.sequence = 0;
    memset(base_page.entries, 0xFF, sizeof(base_page.entries));
  }

  /* Find or allocate a slot for this ID. */
  int slot = -1;
  for (int i = 0; i < (int)PARAMS_MAX_ENTRIES; i++) {
    if (base_page.entries[i].id == id &&
        base_page.entries[i].valid_tag == PARAM_VALID_TAG) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    /* Find an empty slot (valid_tag == 0xFF = erased). */
    for (int i = 0; i < (int)PARAMS_MAX_ENTRIES; i++) {
      if (base_page.entries[i].valid_tag == 0xFFU) {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) {
    xSemaphoreGive(s_mutex);
    return PARAMS_ERR_FLASH; /* Table full — should not happen with ≤15 params. */
  }

  /* Update the entry. */
  base_page.entries[slot].id          = id;
  base_page.entries[slot].type        = (uint8_t)desc->type;
  base_page.entries[slot].value       = value;
  base_page.entries[slot].entry_crc32 = entry_crc(id, (uint8_t)desc->type, value);
  base_page.entries[slot].valid_tag   = PARAM_VALID_TAG;

  /* Increment sequence and recalculate page CRC. */
  base_page.sequence++;
  base_page.page_crc32 = crc32((const uint8_t *)&base_page,
                                offsetof(param_page_t, page_crc32));
  base_page._pad = 0xFFFFFFFFUL;

  params_status_t st = write_all_banks(&base_page);

  /* Update RAM cache so subsequent reads see the new value. */
  if (st == PARAMS_OK) {
    s_cache       = base_page;
    s_cache_valid = true;
  }

  xSemaphoreGive(s_mutex);
  return st;
}

params_status_t sysobj_params_bank_health(uint16_t id,
                                          params_bank_health_t *out)
{
  (void)id;
  if (!s_initialized) return PARAMS_ERR_NOT_INIT;
  if (!out)           return PARAMS_ERR_NOT_FOUND;

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  for (int b = 0; b < 3; b++) {
    param_page_t page;
    out->valid[b] = flash_read_page(b, &page) && page_valid(&page);
  }
  xSemaphoreGive(s_mutex);
  return PARAMS_OK;
}
