#include "pump_card_regs.h"

#include "DAC.h"
#include "card_registers.h"
#include "main.h"
#include "shiftreg.h"
#include "spi_frame.h"

#include <string.h>

#define PUMP_CARD_PUMPS_PER_CARD  8U
#define PUMP_CARD_DAC_AVDD_V      5.0f
#define PUMP_CARD_MAX_VOLTAGE_MV  5000U
#define PUMP_CARD_DAC_RAMP_MV_PER_S 10000U
#define PUMP_CARD_DAC_RAMP_PERIOD_MS 10U
#define PUMP_CARD_DAC_RAMP_STEP_MV \
  (((PUMP_CARD_DAC_RAMP_MV_PER_S * PUMP_CARD_DAC_RAMP_PERIOD_MS) + 999U) / 1000U)
#define PUMP_CARD_DIR_LATCH_COUNT 1U
#define PUMP_CARD_EN_LATCH_COUNT  1U
#define PUMP_QUEUE_CHECKSUM_INIT  0x811C9DC5UL
#define PUMP_QUEUE_CHECKSUM_PRIME 0x01000193UL

typedef struct
{
  uint32_t control;
  uint32_t speed_mV;
} PumpOutputState;

typedef struct
{
  uint32_t error_flags;
  uint32_t control;
  uint8_t dir_bits;
  uint32_t stage_event_index;
  uint32_t stage_meta;
  uint32_t stage_speed_mV;
  uint16_t queue_count;
  uint16_t queue_head;
  uint32_t current_event_index;
  uint32_t last_event_index;
  uint32_t queue_checksum;
  uint8_t queue_push_seen;
  uint8_t run_active;
  uint8_t run_done;
  uint32_t queue_event_index[PUMP_CARD_MAX_LOCAL_EVENTS];
  uint32_t queue_meta[PUMP_CARD_MAX_LOCAL_EVENTS];
  uint32_t queue_speed_mV[PUMP_CARD_MAX_LOCAL_EVENTS];
  PumpOutputState outputs[PUMP_CARD_PUMPS_PER_CARD];
  uint32_t dac_output_mV[PUMP_CARD_PUMPS_PER_CARD];
  uint32_t dac_target_mV[PUMP_CARD_PUMPS_PER_CARD];
  uint32_t dac_preload_target_mV[PUMP_CARD_PUMPS_PER_CARD];
  uint32_t dac_last_update_ms;
  uint8_t dac_ramp_active_mask;
  uint8_t dac_preload_mask;
  uint8_t dac_preload_start_pending;
  uint8_t dac_write_active;
} PumpCardRegsContext;

static PumpCardRegsContext g_pump_regs;

static uint32_t pump_card_checksum_word(uint32_t checksum, uint32_t value)
{
  uint8_t byte_index;

  for (byte_index = 0U; byte_index < 4U; byte_index++)
  {
    checksum ^= (uint8_t)(value & 0xFFU);
    checksum *= PUMP_QUEUE_CHECKSUM_PRIME;
    value >>= 8;
  }

  return checksum;
}

static uint32_t pump_card_checksum_entry(uint32_t checksum,
                                         uint32_t event_index,
                                         uint32_t meta,
                                         uint32_t speed_mV)
{
  checksum = pump_card_checksum_word(checksum, event_index);
  checksum = pump_card_checksum_word(checksum, meta);
  checksum = pump_card_checksum_word(checksum, speed_mV);
  return checksum;
}

static void pump_card_set_direction_bit(uint8_t local_pump_id, uint8_t direction)
{
  uint8_t mask = (uint8_t)(1U << local_pump_id);

  if (direction != 0U)
  {
    g_pump_regs.dir_bits |= mask;
  }
  else
  {
    g_pump_regs.dir_bits &= (uint8_t)(~mask);
  }
}

static uint8_t pump_card_en_bits_from_outputs(const PumpOutputState outputs[PUMP_CARD_PUMPS_PER_CARD])
{
  uint8_t pump_index;
  uint8_t en_bits = 0U;

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    if ((outputs[pump_index].control & PUMP_CONTROL_ENABLE) != 0U)
    {
      en_bits |= (uint8_t)(1U << pump_index);
    }
  }

  return en_bits;
}

static uint32_t pump_card_clamp_speed_mV(uint32_t speed_mV)
{
  if (speed_mV > PUMP_CARD_MAX_VOLTAGE_MV)
  {
    return PUMP_CARD_MAX_VOLTAGE_MV;
  }

  return speed_mV;
}

static PumpOutputState pump_card_make_output_state(PumpOutputState current_state,
                                                   uint32_t control,
                                                   uint32_t speed_mV)
{
  PumpOutputState state = current_state;
  uint32_t target_control = (control & (PUMP_CONTROL_ENABLE | PUMP_CONTROL_DIRECTION));

  if ((target_control & PUMP_CONTROL_ENABLE) != 0U)
  {
    state.control = target_control;
    state.speed_mV = pump_card_clamp_speed_mV(speed_mV);
  }
  else
  {
    state.control &= PUMP_CONTROL_DIRECTION;
    if (speed_mV != 0U)
    {
      state.speed_mV = pump_card_clamp_speed_mV(speed_mV);
    }
  }

  return state;
}

static float pump_card_dac_voltage_from_speed(uint32_t speed_mV)
{
  return (float)speed_mV / 1000.0f;
}

static void pump_card_write_dac_mV(uint8_t local_pump_id, uint32_t speed_mV)
{
  g_pump_regs.dac_write_active = 1U;
  writeDAC(local_pump_id,
           PUMP_CARD_DAC_AVDD_V,
           pump_card_dac_voltage_from_speed(speed_mV),
           0U);
  g_pump_regs.dac_write_active = 0U;
}

static void pump_card_set_dac_target(uint8_t local_pump_id, uint32_t speed_mV)
{
  uint8_t mask = (uint8_t)(1U << local_pump_id);
  uint32_t clamped_speed_mV = pump_card_clamp_speed_mV(speed_mV);

  g_pump_regs.dac_target_mV[local_pump_id] = clamped_speed_mV;
  if (g_pump_regs.dac_output_mV[local_pump_id] == clamped_speed_mV)
  {
    g_pump_regs.dac_ramp_active_mask &= (uint8_t)(~mask);
  }
  else
  {
    g_pump_regs.dac_ramp_active_mask |= mask;
  }
}

static void pump_card_stage_preload_dac_if_needed(uint8_t local_pump_id,
                                                  uint32_t control,
                                                  uint32_t speed_mV)
{
  uint8_t mask = (uint8_t)(1U << local_pump_id);
  uint32_t clamped_speed_mV;

  if ((control & PUMP_CONTROL_ENABLE) == 0U)
  {
    return;
  }

  if ((g_pump_regs.dac_preload_mask & mask) != 0U)
  {
    return;
  }

  clamped_speed_mV = pump_card_clamp_speed_mV(speed_mV);
  g_pump_regs.dac_preload_target_mV[local_pump_id] = clamped_speed_mV;
  g_pump_regs.outputs[local_pump_id].speed_mV = clamped_speed_mV;
  g_pump_regs.dac_preload_mask |= mask;
}

static void pump_card_start_staged_preload_dacs(void)
{
  uint8_t pump_index;

  if (g_pump_regs.dac_preload_mask == 0U)
  {
    return;
  }

  g_pump_regs.dac_last_update_ms = HAL_GetTick();
  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    uint8_t mask = (uint8_t)(1U << pump_index);

    if ((g_pump_regs.dac_preload_mask & mask) == 0U)
    {
      continue;
    }

    pump_card_set_dac_target(pump_index, g_pump_regs.dac_preload_target_mV[pump_index]);
  }

  g_pump_regs.dac_preload_mask = 0U;
}

static void pump_card_latch_dir_bits(uint8_t latch_count)
{
  uint8_t latch_index;

  for (latch_index = 0U; latch_index < latch_count; latch_index++)
  {
    shiftByteDIR(g_pump_regs.dir_bits);
  }
}

static void pump_card_latch_en_outputs(const PumpOutputState outputs[PUMP_CARD_PUMPS_PER_CARD],
                                       uint8_t latch_count)
{
  uint8_t latch_index;
  uint8_t en_bits = pump_card_en_bits_from_outputs(outputs);

  for (latch_index = 0U; latch_index < latch_count; latch_index++)
  {
    shiftByteEN(en_bits);
  }
}

static void pump_card_apply_output_batch(const PumpOutputState target_outputs[PUMP_CARD_PUMPS_PER_CARD],
                                         const uint8_t touched_outputs[PUMP_CARD_PUMPS_PER_CARD])
{
  uint8_t pump_index;
  uint8_t touched_any = 0U;
  uint8_t en_off_latch_needed = 0U;
  uint8_t en_on_latch_needed = 0U;
  uint8_t dir_latch_needed = 0U;
  PumpOutputState en_latch_outputs[PUMP_CARD_PUMPS_PER_CARD];

  memcpy(en_latch_outputs, g_pump_regs.outputs, sizeof(en_latch_outputs));

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    uint8_t target_enable;

    if (touched_outputs[pump_index] == 0U)
    {
      continue;
    }

    touched_any = 1U;
    target_enable = (uint8_t)(target_outputs[pump_index].control & PUMP_CONTROL_ENABLE);
    if (target_enable == 0U)
    {
      en_latch_outputs[pump_index] = target_outputs[pump_index];
      en_off_latch_needed = 1U;
    }
  }

  if (touched_any == 0U)
  {
    return;
  }

  if (en_off_latch_needed != 0U)
  {
    pump_card_latch_en_outputs(en_latch_outputs, PUMP_CARD_EN_LATCH_COUNT);
  }

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    if (touched_outputs[pump_index] == 0U)
    {
      continue;
    }

    if (target_outputs[pump_index].speed_mV == g_pump_regs.outputs[pump_index].speed_mV)
    {
      continue;
    }

    pump_card_set_dac_target(pump_index, target_outputs[pump_index].speed_mV);
  }

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    uint8_t current_direction;
    uint8_t target_direction;

    if (touched_outputs[pump_index] == 0U)
    {
      continue;
    }

    current_direction = (uint8_t)(g_pump_regs.outputs[pump_index].control & PUMP_CONTROL_DIRECTION);
    target_direction = (uint8_t)(target_outputs[pump_index].control & PUMP_CONTROL_DIRECTION);
    if (current_direction != target_direction)
    {
      pump_card_set_direction_bit(pump_index, target_direction);
      dir_latch_needed = 1U;
    }
  }

  if (dir_latch_needed != 0U)
  {
    pump_card_latch_dir_bits(PUMP_CARD_DIR_LATCH_COUNT);
  }

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    uint8_t target_enable;

    if (touched_outputs[pump_index] == 0U)
    {
      continue;
    }

    target_enable = (uint8_t)(target_outputs[pump_index].control & PUMP_CONTROL_ENABLE);
    if (target_enable != 0U)
    {
      en_latch_outputs[pump_index] = target_outputs[pump_index];
      en_on_latch_needed = 1U;
    }
  }

  if (en_on_latch_needed != 0U)
  {
    pump_card_latch_en_outputs(en_latch_outputs, PUMP_CARD_EN_LATCH_COUNT);
  }

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    if (touched_outputs[pump_index] == 0U)
    {
      continue;
    }

    g_pump_regs.outputs[pump_index] = target_outputs[pump_index];
  }
}

static void pump_card_outputs_all_off(void)
{
  uint8_t pump_index;

  g_pump_regs.dir_bits = 0U;
  shiftByteEN(0U);
  shiftByteDIR(g_pump_regs.dir_bits);

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    pump_card_write_dac_mV(pump_index, 0U);
    g_pump_regs.dac_output_mV[pump_index] = 0U;
    g_pump_regs.dac_target_mV[pump_index] = 0U;
    g_pump_regs.dac_preload_target_mV[pump_index] = 0U;
    g_pump_regs.outputs[pump_index].control = 0U;
    g_pump_regs.outputs[pump_index].speed_mV = 0U;
  }

  g_pump_regs.dac_ramp_active_mask = 0U;
  g_pump_regs.dac_preload_mask = 0U;
  g_pump_regs.dac_preload_start_pending = 0U;
  g_pump_regs.dac_last_update_ms = HAL_GetTick();
}

static void pump_card_reset_queue_state(void)
{
  g_pump_regs.control = 0U;
  g_pump_regs.stage_event_index = 0U;
  g_pump_regs.stage_meta = 0U;
  g_pump_regs.stage_speed_mV = 0U;
  g_pump_regs.queue_count = 0U;
  g_pump_regs.queue_head = 0U;
  g_pump_regs.current_event_index = 0U;
  g_pump_regs.last_event_index = 0U;
  g_pump_regs.queue_checksum = PUMP_QUEUE_CHECKSUM_INIT;
  g_pump_regs.queue_push_seen = 0U;
  g_pump_regs.run_active = 0U;
  g_pump_regs.run_done = 0U;
  pump_card_outputs_all_off();
}

static bool pump_card_queue_write_allowed(uint8_t *status_out)
{
  if (g_pump_regs.run_active != 0U)
  {
    *status_out = STATUS_BAD_CMD;
    return false;
  }

  return true;
}

static bool pump_card_validate_staged_entry(uint8_t *status_out)
{
  uint8_t local_pump = PUMP_QUEUE_META_LOCAL_PUMP(g_pump_regs.stage_meta);
  uint32_t control = PUMP_QUEUE_META_CONTROL(g_pump_regs.stage_meta);

  if (local_pump >= PUMP_CARD_PUMPS_PER_CARD)
  {
    *status_out = STATUS_BAD_ADDR;
    return false;
  }

  if ((control & ~(PUMP_CONTROL_ENABLE | PUMP_CONTROL_DIRECTION)) != 0U)
  {
    *status_out = STATUS_BAD_CMD;
    return false;
  }

  return true;
}

void pump_card_regs_init(void)
{
  memset(&g_pump_regs, 0, sizeof(g_pump_regs));
  pump_card_reset_queue_state();
}

bool pump_card_regs_read(uint16_t reg_addr, uint32_t *value_out, uint8_t *status_out)
{
  if ((value_out == 0) || (status_out == 0))
  {
    return false;
  }

  *status_out = STATUS_OK;

  switch (reg_addr)
  {
    case CARD_REG_ID:
      *value_out = CARD_ID_MAGIC;
      return true;

    case CARD_REG_TYPE:
      *value_out = CARD_TYPE_PUMP_PERISTALTIC;
      return true;

    case CARD_REG_FW_VERSION:
      *value_out = CARD_FW_VERSION(PUMP_CARD_FW_MAJOR, PUMP_CARD_FW_MINOR);
      return true;

    case CARD_REG_STATUS:
      *value_out = 0U;
      if (g_pump_regs.run_active != 0U)
      {
        *value_out |= PUMP_CARD_STATUS_RUNNING;
      }
      if (g_pump_regs.run_done != 0U)
      {
        *value_out |= PUMP_CARD_STATUS_DONE;
      }
      if (g_pump_regs.queue_count != 0U)
      {
        *value_out |= PUMP_CARD_STATUS_QUEUE_READY;
      }
      return true;

    case CARD_REG_ERROR_FLAGS:
      *value_out = g_pump_regs.error_flags;
      return true;

    case CARD_REG_CONTROL:
      *value_out = g_pump_regs.control;
      return true;

    case CARD_REG_CAPABILITIES:
      *value_out = CARD_CAP_PUMP_PERISTALTIC;
      return true;

    case CARD_REG_MAX_LOCAL_EVENTS:
      *value_out = PUMP_CARD_MAX_LOCAL_EVENTS;
      return true;

    case PUMP_QUEUE_REG_EVENT_INDEX:
      *value_out = g_pump_regs.stage_event_index;
      return true;

    case PUMP_QUEUE_REG_META:
      *value_out = g_pump_regs.stage_meta;
      return true;

    case PUMP_QUEUE_REG_SPEED_MV:
      *value_out = g_pump_regs.stage_speed_mV;
      return true;

    case PUMP_QUEUE_REG_COUNT:
      *value_out = g_pump_regs.queue_count;
      return true;

    case PUMP_QUEUE_REG_STATUS:
      *value_out = 0U;
      if (g_pump_regs.queue_count >= PUMP_CARD_MAX_LOCAL_EVENTS)
      {
        *value_out |= PUMP_QUEUE_STATUS_FULL;
      }
      if (g_pump_regs.queue_push_seen != 0U)
      {
        *value_out |= PUMP_QUEUE_STATUS_PUSH_SEEN;
      }
      if (g_pump_regs.run_active != 0U)
      {
        *value_out |= PUMP_QUEUE_STATUS_RUNNING;
      }
      if (g_pump_regs.run_done != 0U)
      {
        *value_out |= PUMP_QUEUE_STATUS_DONE;
      }
      return true;

    case PUMP_QUEUE_REG_LAST_EVENT_INDEX:
      *value_out = g_pump_regs.last_event_index;
      return true;

    case PUMP_QUEUE_REG_CHECKSUM:
      *value_out = g_pump_regs.queue_checksum;
      return true;

    default:
      *status_out = STATUS_BAD_ADDR;
      return false;
  }
}

bool pump_card_regs_write(uint16_t reg_addr, uint32_t value, uint8_t *status_out)
{
  if (status_out == 0)
  {
    return false;
  }

  *status_out = STATUS_OK;

  switch (reg_addr)
  {
    case CARD_REG_ERROR_FLAGS:
      g_pump_regs.error_flags = value;
      return true;

    case CARD_REG_CONTROL:
      if ((value & PUMP_CARD_CONTROL_CLEAR_QUEUE) != 0U)
      {
        pump_card_reset_queue_state();
      }

      if ((value & PUMP_CARD_CONTROL_PRELOAD_DAC) != 0U)
      {
        if (g_pump_regs.run_active != 0U)
        {
          *status_out = STATUS_BAD_CMD;
          return false;
        }

        g_pump_regs.dac_preload_start_pending =
          (g_pump_regs.dac_preload_mask != 0U) ? 1U : 0U;
      }

      if ((value & PUMP_CARD_CONTROL_RUN) != 0U)
      {
        g_pump_regs.control = PUMP_CARD_CONTROL_RUN;
        g_pump_regs.queue_head = 0U;
        g_pump_regs.current_event_index = 0U;
        g_pump_regs.run_active = (g_pump_regs.queue_count != 0U) ? 1U : 0U;
        g_pump_regs.run_done = (g_pump_regs.queue_count == 0U) ? 1U : 0U;
      }
      else if ((value & PUMP_CARD_CONTROL_CLEAR_QUEUE) == 0U)
      {
        g_pump_regs.control = 0U;
        g_pump_regs.run_active = 0U;
      }
      return true;

    case PUMP_QUEUE_REG_EVENT_INDEX:
      if (!pump_card_queue_write_allowed(status_out))
      {
        return false;
      }

      g_pump_regs.stage_event_index = value;
      return true;

    case PUMP_QUEUE_REG_META:
      if (!pump_card_queue_write_allowed(status_out))
      {
        return false;
      }

      g_pump_regs.stage_meta = value;
      return true;

    case PUMP_QUEUE_REG_SPEED_MV:
      if (!pump_card_queue_write_allowed(status_out))
      {
        return false;
      }

      g_pump_regs.stage_speed_mV = value;
      return true;

    case PUMP_QUEUE_REG_PUSH:
      if (!pump_card_queue_write_allowed(status_out))
      {
        return false;
      }

      if (g_pump_regs.queue_count >= PUMP_CARD_MAX_LOCAL_EVENTS)
      {
        *status_out = STATUS_BAD_LEN;
        return false;
      }

      if (!pump_card_validate_staged_entry(status_out))
      {
        return false;
      }

      pump_card_stage_preload_dac_if_needed(PUMP_QUEUE_META_LOCAL_PUMP(g_pump_regs.stage_meta),
                                            PUMP_QUEUE_META_CONTROL(g_pump_regs.stage_meta),
                                            g_pump_regs.stage_speed_mV);

      g_pump_regs.queue_event_index[g_pump_regs.queue_count] = g_pump_regs.stage_event_index;
      g_pump_regs.queue_meta[g_pump_regs.queue_count] = g_pump_regs.stage_meta;
      g_pump_regs.queue_speed_mV[g_pump_regs.queue_count] = g_pump_regs.stage_speed_mV;
      g_pump_regs.queue_count++;
      g_pump_regs.last_event_index = g_pump_regs.stage_event_index;
      g_pump_regs.queue_checksum = pump_card_checksum_entry(g_pump_regs.queue_checksum,
                                                            g_pump_regs.stage_event_index,
                                                            g_pump_regs.stage_meta,
                                                            g_pump_regs.stage_speed_mV);
      g_pump_regs.queue_push_seen = 1U;
      g_pump_regs.run_done = 0U;
      return true;

    default:
      *status_out = STATUS_BAD_ADDR;
      return false;
  }
}

static void pump_card_regs_service_dac_ramp(void)
{
  uint32_t now_ms;
  uint32_t elapsed_ms;
  uint32_t periods;
  uint32_t max_delta_mV;
  uint8_t pump_index;

  if ((g_pump_regs.dac_ramp_active_mask == 0U) || (g_pump_regs.dac_write_active != 0U))
  {
    g_pump_regs.dac_last_update_ms = HAL_GetTick();
    return;
  }

  now_ms = HAL_GetTick();
  elapsed_ms = now_ms - g_pump_regs.dac_last_update_ms;
  if (elapsed_ms < PUMP_CARD_DAC_RAMP_PERIOD_MS)
  {
    return;
  }

  periods = elapsed_ms / PUMP_CARD_DAC_RAMP_PERIOD_MS;
  g_pump_regs.dac_last_update_ms += periods * PUMP_CARD_DAC_RAMP_PERIOD_MS;
  max_delta_mV = periods * PUMP_CARD_DAC_RAMP_STEP_MV;
  if (max_delta_mV > PUMP_CARD_MAX_VOLTAGE_MV)
  {
    max_delta_mV = PUMP_CARD_MAX_VOLTAGE_MV;
  }

  for (pump_index = 0U; pump_index < PUMP_CARD_PUMPS_PER_CARD; pump_index++)
  {
    uint8_t mask = (uint8_t)(1U << pump_index);
    uint32_t current_mV;
    uint32_t target_mV;
    uint32_t next_mV;

    if ((g_pump_regs.dac_ramp_active_mask & mask) == 0U)
    {
      continue;
    }

    current_mV = g_pump_regs.dac_output_mV[pump_index];
    target_mV = g_pump_regs.dac_target_mV[pump_index];

    if (current_mV < target_mV)
    {
      if ((target_mV - current_mV) <= max_delta_mV)
      {
        next_mV = target_mV;
      }
      else
      {
        next_mV = current_mV + max_delta_mV;
      }
    }
    else if (current_mV > target_mV)
    {
      if ((current_mV - target_mV) <= max_delta_mV)
      {
        next_mV = target_mV;
      }
      else
      {
        next_mV = current_mV - max_delta_mV;
      }
    }
    else
    {
      g_pump_regs.dac_ramp_active_mask &= (uint8_t)(~mask);
      continue;
    }

    pump_card_write_dac_mV(pump_index, next_mV);
    g_pump_regs.dac_output_mV[pump_index] = next_mV;
    if (next_mV == g_pump_regs.dac_target_mV[pump_index])
    {
      g_pump_regs.dac_ramp_active_mask &= (uint8_t)(~mask);
    }
    else
    {
      g_pump_regs.dac_ramp_active_mask |= mask;
    }
  }
}

void pump_card_regs_service_deferred_outputs(void)
{
  pump_card_regs_service_dac_ramp();
}

void pump_card_regs_service_post_response(void)
{
  if (g_pump_regs.dac_preload_start_pending == 0U)
  {
    return;
  }

  g_pump_regs.dac_preload_start_pending = 0U;
  g_pump_regs.dac_write_active = 1U;
  pump_card_start_staged_preload_dacs();
  g_pump_regs.dac_write_active = 0U;
}

void pump_card_regs_on_sync_edge(void)
{
  PumpOutputState target_outputs[PUMP_CARD_PUMPS_PER_CARD];
  uint8_t touched_outputs[PUMP_CARD_PUMPS_PER_CARD];

  memcpy(target_outputs, g_pump_regs.outputs, sizeof(target_outputs));
  memset(touched_outputs, 0, sizeof(touched_outputs));

  while ((g_pump_regs.run_active != 0U) &&
         (g_pump_regs.queue_head < g_pump_regs.queue_count) &&
         (g_pump_regs.queue_event_index[g_pump_regs.queue_head] == g_pump_regs.current_event_index))
  {
    uint8_t local_pump = PUMP_QUEUE_META_LOCAL_PUMP(g_pump_regs.queue_meta[g_pump_regs.queue_head]);
    uint32_t control = PUMP_QUEUE_META_CONTROL(g_pump_regs.queue_meta[g_pump_regs.queue_head]);
    uint32_t speed_mV = g_pump_regs.queue_speed_mV[g_pump_regs.queue_head];

    if (local_pump < PUMP_CARD_PUMPS_PER_CARD)
    {
      target_outputs[local_pump] = pump_card_make_output_state(target_outputs[local_pump],
                                                               control,
                                                               speed_mV);
      touched_outputs[local_pump] = 1U;
    }

    g_pump_regs.queue_head++;
  }

  pump_card_apply_output_batch(target_outputs, touched_outputs);

  if (g_pump_regs.run_active != 0U)
  {
    g_pump_regs.current_event_index++;
    if (g_pump_regs.queue_head >= g_pump_regs.queue_count)
    {
      g_pump_regs.control = 0U;
      g_pump_regs.run_active = 0U;
      g_pump_regs.run_done = 1U;
    }
  }
}
