/**
 * @file sysobj_timer.c
 * @brief Hardware timer sysobj implementation.
 *
 * PSC / ARR computation:
 *   target_ticks = period_ns * tim_clock_hz / 1_000_000_000
 *   Pick smallest PSC (0..65535) such that ARR = target_ticks / (PSC+1) fits
 *   within the counter width (16-bit or 32-bit).
 *
 * Callback routing:
 *   A static instance table (up to SYSOBJ_TIMER_MAX_INSTANCES entries) maps
 *   TIM handles to sysobj_timer_t*. sysobj_timer_irq_handler() iterates the
 *   table and fires the matching callback.
 *
 * Limitation: all registered timers must have distinct htim->Instance values.
 */

#include "sysobj_timer.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Instance registry
 * -------------------------------------------------------------------------- */

static sysobj_timer_t *s_instances[SYSOBJ_TIMER_MAX_INSTANCES];
static uint32_t        s_instance_count = 0U;

static sysobj_timer_error_t register_instance(sysobj_timer_t *timer)
{
  for (uint32_t i = 0; i < s_instance_count; i++) {
    if (s_instances[i] == timer)
      return SYSOBJ_TIMER_OK; /* Already registered. */
  }
  if (s_instance_count >= SYSOBJ_TIMER_MAX_INSTANCES)
    return SYSOBJ_TIMER_ERR_TOO_MANY;
  s_instances[s_instance_count++] = timer;
  return SYSOBJ_TIMER_OK;
}

static void unregister_instance(sysobj_timer_t *timer)
{
  for (uint32_t i = 0; i < s_instance_count; i++) {
    if (s_instances[i] == timer) {
      s_instances[i] = s_instances[--s_instance_count];
      s_instances[s_instance_count] = NULL;
      return;
    }
  }
}

/* --------------------------------------------------------------------------
 * Period → nanoseconds conversion
 * -------------------------------------------------------------------------- */

static uint64_t period_to_ns(uint32_t period, sysobj_timer_unit_t unit)
{
  switch (unit) {
    case SYSOBJ_TIMER_UNIT_NSEC: return (uint64_t)period;
    case SYSOBJ_TIMER_UNIT_USEC: return (uint64_t)period * 1000ULL;
    case SYSOBJ_TIMER_UNIT_MSEC: return (uint64_t)period * 1000000ULL;
    case SYSOBJ_TIMER_UNIT_SEC:  return (uint64_t)period * 1000000000ULL;
    default:                     return (uint64_t)period;
  }
}

/* --------------------------------------------------------------------------
 * PSC / ARR computation
 * -------------------------------------------------------------------------- */

static sysobj_timer_error_t compute_psc_arr(uint32_t period, sysobj_timer_unit_t unit,
                                            uint32_t clock_hz, bool is_32bit,
                                            uint32_t *out_psc, uint32_t *out_arr)
{
  if (period == 0) return SYSOBJ_TIMER_ERR_PERIOD;

  uint64_t period_ns = period_to_ns(period, unit);
  if (period_ns == 0) return SYSOBJ_TIMER_ERR_PERIOD;

  /* target_ticks = period_ns * clock_hz / 1e9.
   * Use 64-bit intermediate to avoid overflow for large periods. */
  uint64_t target_ticks = (period_ns / 1000U) * ((uint64_t)clock_hz / 1000U);
  /* Above is (period_ns / 1000) * (clock_hz / 1000) = period_us * (clock_hz/1000).
   * More accurately: target_ticks = period_ns * clock_hz / 1_000_000_000.
   * Rearrange to avoid 64-bit overflow: ticks = (period_ns / 1000) * (clock_hz / 1000). */

  if (target_ticks == 0) return SYSOBJ_TIMER_ERR_PERIOD;

  const uint64_t max_arr = is_32bit ? 0xFFFFFFFFULL : 0xFFFFULL;
  const uint64_t max_psc = 0xFFFFULL; /* PSC is always 16-bit */

  /* Find smallest PSC such that arr = target_ticks / (psc+1) ≤ max_arr. */
  uint64_t psc = 0;
  uint64_t arr = target_ticks - 1U; /* ARR = ticks - 1 for correct period */

  if (arr > max_arr) {
    /* Need prescaler. */
    psc = (target_ticks / (max_arr + 1U));
    if (psc > max_psc) return SYSOBJ_TIMER_ERR_OVERFLOW;
    arr = (target_ticks / (psc + 1U)) - 1U;
  }

  if (arr == 0) arr = 1; /* Minimum useful ARR. */
  if (arr > max_arr) return SYSOBJ_TIMER_ERR_OVERFLOW;

  *out_psc = (uint32_t)psc;
  *out_arr = (uint32_t)arr;
  return SYSOBJ_TIMER_OK;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

sysobj_timer_error_t sysobj_timer_init(sysobj_timer_t *timer,
                                       const sysobj_timer_cfg_t *cfg)
{
  if (!timer || !cfg || !cfg->htim) return SYSOBJ_TIMER_ERR_NULL;
  if (cfg->period == 0)             return SYSOBJ_TIMER_ERR_PERIOD;

  memset(timer, 0, sizeof(*timer));
  timer->cfg = *cfg;

  sysobj_timer_error_t err = compute_psc_arr(cfg->period, cfg->unit,
                                              cfg->tim_clock_hz, cfg->is_32bit,
                                              &timer->_psc, &timer->_arr);
  if (err != SYSOBJ_TIMER_OK) return err;

  err = register_instance(timer);
  if (err != SYSOBJ_TIMER_OK) return err;

  timer->_initialized = true;
  return SYSOBJ_TIMER_OK;
}

sysobj_timer_error_t sysobj_timer_start(sysobj_timer_t *timer)
{
  if (!timer || !timer->_initialized) return SYSOBJ_TIMER_ERR_NOT_INIT;
  if (!timer->cfg.htim)               return SYSOBJ_TIMER_ERR_NULL;

  /* Set up the repeat counter. */
  timer->remaining = (timer->cfg.repeat == SYSOBJ_TIMER_REPEAT_INFINITE)
                     ? SYSOBJ_TIMER_REPEAT_INFINITE
                     : timer->cfg.repeat;
  timer->running = true;

  /* Program prescaler and auto-reload. */
  __HAL_TIM_SET_PRESCALER(timer->cfg.htim, timer->_psc);
  __HAL_TIM_SET_AUTORELOAD(timer->cfg.htim, timer->_arr);

  /* Reset the counter. */
  __HAL_TIM_SET_COUNTER(timer->cfg.htim, 0U);

  /* Generate an update event to shadow-load PSC and ARR immediately. */
  HAL_TIM_GenerateEvent(timer->cfg.htim, TIM_EVENTSOURCE_UPDATE);

  /* Clear the update flag that the generate event just set. */
  __HAL_TIM_CLEAR_FLAG(timer->cfg.htim, TIM_FLAG_UPDATE);

  /* Start interrupt-driven base timer. */
  if (HAL_TIM_Base_Start_IT(timer->cfg.htim) != HAL_OK) {
    timer->running = false;
    return SYSOBJ_TIMER_ERR_HAL;
  }
  return SYSOBJ_TIMER_OK;
}

void sysobj_timer_stop(sysobj_timer_t *timer)
{
  if (!timer || !timer->_initialized) return;
  timer->running = false;
  HAL_TIM_Base_Stop_IT(timer->cfg.htim);
}

void sysobj_timer_deinit(sysobj_timer_t *timer)
{
  if (!timer || !timer->_initialized) return;
  sysobj_timer_stop(timer);
  unregister_instance(timer);
  timer->_initialized = false;
}

bool sysobj_timer_is_running(const sysobj_timer_t *timer)
{
  return timer && timer->running;
}

int32_t sysobj_timer_get_remaining(const sysobj_timer_t *timer)
{
  if (!timer) return 0;
  return timer->remaining;
}

void sysobj_timer_irq_handler(TIM_HandleTypeDef *htim)
{
  for (uint32_t i = 0; i < s_instance_count; i++) {
    sysobj_timer_t *t = s_instances[i];
    if (!t || !t->running) continue;
    if (t->cfg.htim->Instance != htim->Instance) continue;

    /* Fire callback. */
    if (t->cfg.callback) t->cfg.callback(t->cfg.user_data);

    /* Manage repetition counter. */
    if (t->remaining == SYSOBJ_TIMER_REPEAT_INFINITE) {
      /* Keep running forever. */
    } else {
      t->remaining--;
      if (t->remaining <= 0) {
        t->running = false;
        HAL_TIM_Base_Stop_IT(t->cfg.htim);
      }
    }
    return; /* Each htim matches at most one instance. */
  }
}
