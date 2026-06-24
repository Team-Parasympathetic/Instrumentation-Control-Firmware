#include "backplane_pump_bus.h"

#include "backplane_card_bus.h"
#include "card_registers.h"
#include "main.h"
#include "protocol.h"
#include "spi_frame.h"

#include <string.h>

#define PUMP_BUS_DISCOVERY_PASSES  6U
#define ACTION_HEADER_SIZE         4U
#define PUMP_PAYLOAD_SIZE          8U
#define PUMP_QUEUE_CHECKSUM_INIT   0x811C9DC5UL
#define PUMP_QUEUE_CHECKSUM_PRIME  0x01000193UL
#define PUMP_BUS_INITIAL_DAC_PRELOAD_WAIT_MS 1000U

typedef struct
{
  uint8_t required_slot_mask;
  uint8_t schedule_uses_pumps;
  uint8_t offloaded_schedule_prepared;
  uint8_t offloaded_schedule_active;
  uint8_t initial_preload_slot_mask;
  uint16_t queued_action_count;
  uint8_t last_status;
  uint8_t last_detail;
} PumpBusContext;

typedef struct
{
  uint8_t module_type;
  uint8_t module_id;
  uint8_t action_type;
  uint8_t action_len;
  const uint8_t *payload;
} PumpActionView;

static PumpBusContext g_pump_bus;

static void pump_bus_set_error(uint8_t status, uint8_t detail)
{
  g_pump_bus.last_status = status;
  g_pump_bus.last_detail = detail;
}

static uint32_t pump_bus_checksum_word(uint32_t checksum, uint32_t value)
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

static uint32_t pump_bus_checksum_entry(uint32_t checksum,
                                        uint32_t event_index,
                                        uint32_t meta,
                                        uint32_t speed_mV)
{
  checksum = pump_bus_checksum_word(checksum, event_index);
  checksum = pump_bus_checksum_word(checksum, meta);
  checksum = pump_bus_checksum_word(checksum, speed_mV);
  return checksum;
}

static bool pump_bus_read_action(const ScheduleEvent *event,
                                 uint16_t offset,
                                 uint8_t action_index,
                                 PumpActionView *action_out)
{
  if ((event == 0) || (action_out == 0))
  {
    pump_bus_set_error(STATUS_BAD_LEN, action_index);
    return false;
  }

  if ((uint16_t)(offset + ACTION_HEADER_SIZE) > event->action_bytes_len)
  {
    pump_bus_set_error(STATUS_BAD_LEN, action_index);
    return false;
  }

  action_out->action_len = event->action_bytes[offset + 3U];
  if ((uint16_t)(offset + ACTION_HEADER_SIZE + action_out->action_len) > event->action_bytes_len)
  {
    pump_bus_set_error(STATUS_BAD_LEN, action_index);
    return false;
  }

  action_out->module_type = event->action_bytes[offset + 0U];
  action_out->module_id = event->action_bytes[offset + 1U];
  action_out->action_type = event->action_bytes[offset + 2U];
  action_out->payload = &event->action_bytes[offset + ACTION_HEADER_SIZE];
  return true;
}

static uint32_t pump_bus_flow_nl_to_speed_mV(uint32_t flow_nl_min)
{
  uint64_t speed_mV = ((uint64_t)flow_nl_min + 100ULL) / 200ULL;

  if (speed_mV > 5000ULL)
  {
    speed_mV = 5000ULL;
  }

  return (uint32_t)speed_mV;
}

static bool pump_bus_build_registers(uint8_t pump_id,
                                     uint8_t action_type,
                                     const uint8_t *payload,
                                     uint8_t payload_len,
                                     uint8_t *slot_out,
                                     uint8_t *local_pump_out,
                                     uint32_t *control_out,
                                     uint32_t *speed_mV_out)
{
  uint8_t slot;
  uint8_t local_pump;
  uint32_t control;
  uint32_t speed_mV;

  if ((slot_out == 0) || (local_pump_out == 0) || (control_out == 0) || (speed_mV_out == 0))
  {
    pump_bus_set_error(STATUS_BAD_LEN, pump_id);
    return false;
  }

  if (action_type != PUMP_SET_STATE)
  {
    pump_bus_set_error(STATUS_BAD_CMD, action_type);
    return false;
  }

  if ((payload == 0) || (payload_len != PUMP_PAYLOAD_SIZE))
  {
    pump_bus_set_error(STATUS_BAD_LEN, pump_id);
    return false;
  }

  slot = (uint8_t)(pump_id / 8U);
  local_pump = (uint8_t)(pump_id % 8U);
  if (slot >= CARD_BUS_SLOT_COUNT)
  {
    pump_bus_set_error(STATUS_BAD_ADDR, pump_id);
    return false;
  }

  control = (payload[1] != 0U) ? PUMP_CONTROL_DIRECTION : 0U;
  speed_mV = 0U;
  if (payload[0] != 0U)
  {
    control |= PUMP_CONTROL_ENABLE;
    speed_mV = pump_bus_flow_nl_to_speed_mV(read_u32_le(&payload[4]));
  }

  *slot_out = slot;
  *local_pump_out = local_pump;
  *control_out = control;
  *speed_mV_out = speed_mV;
  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool pump_bus_find_next_enabled_speed(const Schedule *schedule,
                                             uint16_t start_event_index,
                                             uint8_t pump_id,
                                             uint32_t *speed_mV_out)
{
  uint16_t event_index;

  if ((schedule == 0) || (speed_mV_out == 0))
  {
    return false;
  }

  for (event_index = start_event_index; event_index < schedule->event_count; event_index++)
  {
    const ScheduleEvent *event = &schedule->events[event_index];
    uint16_t offset = 0U;
    uint8_t action_index;

    for (action_index = 0U; action_index < event->action_count; action_index++)
    {
      PumpActionView action;

      if (!pump_bus_read_action(event, offset, action_index, &action))
      {
        return false;
      }

      if ((action.module_type == MODULE_PUMP_PERISTALTIC) &&
          (action.module_id == pump_id) &&
          (action.action_type == PUMP_SET_STATE) &&
          (action.action_len == PUMP_PAYLOAD_SIZE) &&
          (action.payload[0] != 0U))
      {
        *speed_mV_out = pump_bus_flow_nl_to_speed_mV(read_u32_le(&action.payload[4]));
        return true;
      }

      offset = (uint16_t)(offset + ACTION_HEADER_SIZE + action.action_len);
    }
  }

  return false;
}

static uint8_t pump_bus_required_slots_mask(const Schedule *schedule,
                                            uint8_t *mask_out,
                                            uint16_t *action_count_out,
                                            uint8_t *uses_pumps_out)
{
  uint16_t event_index;
  uint16_t per_slot_counts[CARD_BUS_SLOT_COUNT];
  uint16_t total_action_count = 0U;
  uint8_t mask = 0U;
  uint8_t uses_pumps = 0U;

  if ((schedule == 0) || (mask_out == 0) || (action_count_out == 0) || (uses_pumps_out == 0))
  {
    return STATUS_BAD_LEN;
  }

  memset(per_slot_counts, 0, sizeof(per_slot_counts));

  for (event_index = 0U; event_index < schedule->event_count; event_index++)
  {
    const ScheduleEvent *event = &schedule->events[event_index];
    uint16_t offset = 0U;
    uint8_t action_index;

    for (action_index = 0U; action_index < event->action_count; action_index++)
    {
      PumpActionView action;

      if (!pump_bus_read_action(event, offset, action_index, &action))
      {
        return g_pump_bus.last_status;
      }

      if (action.module_type == MODULE_PUMP_PERISTALTIC)
      {
        uint8_t slot;
        uint8_t local_pump;
        uint32_t control;
        uint32_t speed_mV;

        uses_pumps = 1U;
        if (!pump_bus_build_registers(action.module_id,
                                      action.action_type,
                                      action.payload,
                                      action.action_len,
                                      &slot,
                                      &local_pump,
                                      &control,
                                      &speed_mV))
        {
          return g_pump_bus.last_status;
        }

        per_slot_counts[slot]++;
        if (per_slot_counts[slot] > PUMP_CARD_MAX_LOCAL_EVENTS)
        {
          g_pump_bus.last_detail = action.module_id;
          return STATUS_BAD_LEN;
        }

        total_action_count++;
        mask |= (uint8_t)(1U << slot);
      }

      offset = (uint16_t)(offset + ACTION_HEADER_SIZE + action.action_len);
    }
  }

  *mask_out = mask;
  *action_count_out = total_action_count;
  *uses_pumps_out = uses_pumps;
  return STATUS_OK;
}

static bool pump_bus_write_control(uint8_t slot, uint32_t value)
{
  if (!card_bus_write_reg(slot, CARD_REG_CONTROL, value))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool pump_bus_clear_remote_queue(uint8_t slot)
{
  if (!pump_bus_write_control(slot, PUMP_CARD_CONTROL_CLEAR_QUEUE))
  {
    return false;
  }

  return pump_bus_write_control(slot, 0U);
}

static bool pump_bus_write_queue_entry(uint8_t slot,
                                       uint16_t event_index,
                                       uint8_t local_pump,
                                       uint32_t control,
                                       uint32_t speed_mV)
{
  if (!card_bus_write_reg(slot, PUMP_QUEUE_REG_EVENT_INDEX, (uint32_t)event_index))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  if (!card_bus_write_reg(slot,
                          PUMP_QUEUE_REG_META,
                          PUMP_QUEUE_META(local_pump, control)))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  if (!card_bus_write_reg(slot, PUMP_QUEUE_REG_SPEED_MV, speed_mV))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  if (!card_bus_write_reg(slot, PUMP_QUEUE_REG_PUSH, 1U))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool pump_bus_verify_remote_queue(uint8_t slot,
                                         uint16_t expected_count,
                                         uint32_t expected_last_event,
                                         uint32_t expected_checksum)
{
  uint32_t actual_count;
  uint32_t actual_last_event;
  uint32_t actual_checksum;

  if (!card_bus_read_reg(slot, PUMP_QUEUE_REG_COUNT, &actual_count))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  if (!card_bus_read_reg(slot, PUMP_QUEUE_REG_LAST_EVENT_INDEX, &actual_last_event))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  if (!card_bus_read_reg(slot, PUMP_QUEUE_REG_CHECKSUM, &actual_checksum))
  {
    pump_bus_set_error(card_bus_get_last_status(), slot);
    return false;
  }

  if ((actual_count != (uint32_t)expected_count) ||
      (actual_last_event != expected_last_event) ||
      (actual_checksum != expected_checksum))
  {
    pump_bus_set_error(STATUS_HW_ERROR, slot);
    return false;
  }

  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool pump_bus_upload_schedule(const Schedule *schedule)
{
  uint8_t slot;
  uint16_t event_index;
  uint16_t expected_count[CARD_BUS_SLOT_COUNT];
  uint32_t expected_last_event[CARD_BUS_SLOT_COUNT];
  uint32_t expected_checksum[CARD_BUS_SLOT_COUNT];

  if (schedule == 0)
  {
    pump_bus_set_error(STATUS_BAD_LEN, 0U);
    return false;
  }

  g_pump_bus.initial_preload_slot_mask = 0U;
  memset(expected_count, 0, sizeof(expected_count));
  memset(expected_last_event, 0, sizeof(expected_last_event));
  for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
  {
    expected_checksum[slot] = PUMP_QUEUE_CHECKSUM_INIT;
  }

  for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
  {
    if ((g_pump_bus.required_slot_mask & (uint8_t)(1U << slot)) == 0U)
    {
      continue;
    }

    if (!pump_bus_clear_remote_queue(slot))
    {
      return false;
    }
  }

  for (event_index = 0U; event_index < schedule->event_count; event_index++)
  {
    const ScheduleEvent *event = &schedule->events[event_index];
    uint16_t offset = 0U;
    uint8_t action_index;

    for (action_index = 0U; action_index < event->action_count; action_index++)
    {
      PumpActionView action;

      if (!pump_bus_read_action(event, offset, action_index, &action))
      {
        return false;
      }

      if (action.module_type == MODULE_PUMP_PERISTALTIC)
      {
        uint8_t target_slot;
        uint8_t local_pump;
        uint32_t control;
        uint32_t speed_mV;
        uint32_t next_speed_mV;

        if (!pump_bus_build_registers(action.module_id,
                                      action.action_type,
                                      action.payload,
                                      action.action_len,
                                      &target_slot,
                                      &local_pump,
                                      &control,
                                      &speed_mV))
        {
          return false;
        }

        if ((control & PUMP_CONTROL_ENABLE) != 0U)
        {
          g_pump_bus.initial_preload_slot_mask |= (uint8_t)(1U << target_slot);
        }

        if (((control & PUMP_CONTROL_ENABLE) == 0U) &&
            pump_bus_find_next_enabled_speed(schedule,
                                             (uint16_t)(event_index + 1U),
                                             action.module_id,
                                             &next_speed_mV))
        {
          speed_mV = next_speed_mV;
        }

        if (!pump_bus_write_queue_entry(target_slot,
                                        event_index,
                                        local_pump,
                                        control,
                                        speed_mV))
        {
          return false;
        }

        expected_count[target_slot]++;
        expected_last_event[target_slot] = (uint32_t)event_index;
        expected_checksum[target_slot] =
          pump_bus_checksum_entry(expected_checksum[target_slot],
                                  (uint32_t)event_index,
                                  PUMP_QUEUE_META(local_pump, control),
                                  speed_mV);
      }

      offset = (uint16_t)(offset + ACTION_HEADER_SIZE + action.action_len);
    }
  }

  for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
  {
    if ((g_pump_bus.required_slot_mask & (uint8_t)(1U << slot)) == 0U)
    {
      continue;
    }

    if (!pump_bus_verify_remote_queue(slot,
                                      expected_count[slot],
                                      expected_last_event[slot],
                                      expected_checksum[slot]))
    {
      return false;
    }
  }

  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

void pump_bus_init(void)
{
  memset(&g_pump_bus, 0, sizeof(g_pump_bus));
}

bool pump_bus_validate_schedule(const Schedule *schedule)
{
  uint16_t action_count;
  uint8_t uses_pumps;
  uint8_t required_mask;
  uint8_t status;

  g_pump_bus.required_slot_mask = 0U;
  g_pump_bus.schedule_uses_pumps = 0U;
  g_pump_bus.offloaded_schedule_prepared = 0U;
  g_pump_bus.offloaded_schedule_active = 0U;
  g_pump_bus.initial_preload_slot_mask = 0U;
  g_pump_bus.queued_action_count = 0U;

  status = pump_bus_required_slots_mask(schedule, &required_mask, &action_count, &uses_pumps);
  if (status != STATUS_OK)
  {
    pump_bus_set_error(status, g_pump_bus.last_detail);
    return false;
  }

  g_pump_bus.required_slot_mask = required_mask;
  g_pump_bus.schedule_uses_pumps = uses_pumps;
  g_pump_bus.queued_action_count = action_count;
  if (uses_pumps == 0U)
  {
    pump_bus_set_error(STATUS_OK, 0U);
    return true;
  }

  if (!card_bus_discover_type_slots(g_pump_bus.required_slot_mask,
                                    CARD_TYPE_PUMP_PERISTALTIC,
                                    PUMP_BUS_DISCOVERY_PASSES))
  {
    pump_bus_set_error(card_bus_get_last_status(),
                       (uint8_t)(card_bus_get_last_detail() * 8U));
    return false;
  }

  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

bool pump_bus_start_schedule(const Schedule *schedule)
{
  if (g_pump_bus.schedule_uses_pumps == 0U)
  {
    g_pump_bus.offloaded_schedule_prepared = 0U;
    g_pump_bus.offloaded_schedule_active = 0U;
    g_pump_bus.initial_preload_slot_mask = 0U;
    pump_bus_set_error(STATUS_OK, 0U);
    return true;
  }

  if (!pump_bus_upload_schedule(schedule))
  {
    uint8_t slot;

    for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
    {
      if ((g_pump_bus.required_slot_mask & (uint8_t)(1U << slot)) != 0U)
      {
        (void)pump_bus_clear_remote_queue(slot);
      }
    }

    g_pump_bus.offloaded_schedule_prepared = 0U;
    g_pump_bus.offloaded_schedule_active = 0U;
    g_pump_bus.initial_preload_slot_mask = 0U;
    return false;
  }

  g_pump_bus.offloaded_schedule_prepared = 1U;
  g_pump_bus.offloaded_schedule_active = 0U;
  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

bool pump_bus_preload_initial_dacs(void)
{
  uint8_t slot;

  if ((g_pump_bus.schedule_uses_pumps == 0U) ||
      (g_pump_bus.initial_preload_slot_mask == 0U))
  {
    pump_bus_set_error(STATUS_OK, 0U);
    return true;
  }

  if (g_pump_bus.offloaded_schedule_prepared == 0U)
  {
    pump_bus_set_error(STATUS_BAD_CMD, 0U);
    return false;
  }

  for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
  {
    if ((g_pump_bus.initial_preload_slot_mask & (uint8_t)(1U << slot)) == 0U)
    {
      continue;
    }

    if (!pump_bus_write_control(slot, PUMP_CARD_CONTROL_PRELOAD_DAC))
    {
      return false;
    }
  }

  HAL_Delay(PUMP_BUS_INITIAL_DAC_PRELOAD_WAIT_MS);
  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

bool pump_bus_arm_schedule(void)
{
  uint8_t slot;

  if (g_pump_bus.schedule_uses_pumps == 0U)
  {
    pump_bus_set_error(STATUS_OK, 0U);
    return true;
  }

  if (g_pump_bus.offloaded_schedule_prepared == 0U)
  {
    pump_bus_set_error(STATUS_BAD_LEN, 0U);
    return false;
  }

  for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
  {
    if ((g_pump_bus.required_slot_mask & (uint8_t)(1U << slot)) == 0U)
    {
      continue;
    }

    if (!pump_bus_write_control(slot, PUMP_CARD_CONTROL_RUN))
    {
      return false;
    }
  }

  g_pump_bus.offloaded_schedule_active = 1U;
  pump_bus_set_error(STATUS_OK, 0U);
  return true;
}

void pump_bus_stop_all(void)
{
  uint8_t slot;

  g_pump_bus.offloaded_schedule_prepared = 0U;
  g_pump_bus.offloaded_schedule_active = 0U;
  g_pump_bus.schedule_uses_pumps = 0U;
  g_pump_bus.queued_action_count = 0U;
  g_pump_bus.required_slot_mask = 0U;
  g_pump_bus.initial_preload_slot_mask = 0U;

  for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
  {
    if (!card_bus_is_slot_present(slot, CARD_TYPE_PUMP_PERISTALTIC))
    {
      continue;
    }

    (void)pump_bus_write_control(slot, 0U);
    (void)pump_bus_clear_remote_queue(slot);
  }

  pump_bus_set_error(STATUS_OK, 0U);
}

uint8_t pump_bus_get_last_status(void)
{
  return g_pump_bus.last_status;
}

uint8_t pump_bus_get_last_detail(void)
{
  return g_pump_bus.last_detail;
}
