#include "scheduler.h"

#include "protocol.h"

#include <string.h>

#define EVENT_HEADER_SIZE            13U
#define ACTION_HEADER_SIZE           4U
#define PUMP_STATE_PAYLOAD_SIZE      8U
#define GPIO_WAVEFORM_PAYLOAD_SIZE   16U

typedef struct
{
  Schedule schedule;
  uint32_t last_event_id;
  uint8_t last_error;
  uint8_t state;
} SchedulerStore;

static SchedulerStore g_scheduler;

static uint8_t scheduler_validate_pump_action(uint8_t module_id,
                                              uint8_t action_type,
                                              const uint8_t *payload,
                                              uint8_t payload_len,
                                              uint8_t *detail)
{
  if (module_id >= MAX_PUMPS)
  {
    if (detail != 0)
    {
      *detail = module_id;
    }
    return ERR_BAD_MODULE;
  }

  if (action_type != PUMP_SET_STATE)
  {
    if (detail != 0)
    {
      *detail = action_type;
    }
    return ERR_BAD_ACTION;
  }

  if (payload_len != PUMP_STATE_PAYLOAD_SIZE)
  {
    if (detail != 0)
    {
      *detail = action_type;
    }
    return ERR_BAD_EVENT;
  }

  if ((payload[0] > 1U) || (payload[1] > 1U))
  {
    if (detail != 0)
    {
      *detail = action_type;
    }
    return ERR_BAD_ACTION;
  }

  return ACK_OK;
}

static uint8_t scheduler_validate_gpio_action(uint8_t module_id,
                                              uint8_t action_type,
                                              const uint8_t *payload,
                                              uint8_t payload_len,
                                              uint8_t *detail)
{
  if (module_id >= MAX_GPIO_OUTPUTS)
  {
    if (detail != 0)
    {
      *detail = module_id;
    }
    return ERR_BAD_MODULE;
  }

  switch (action_type)
  {
    case GPIO_SET_WAVEFORM:
      if (payload_len != GPIO_WAVEFORM_PAYLOAD_SIZE)
      {
        if (detail != 0)
        {
          *detail = action_type;
        }
        return ERR_BAD_EVENT;
      }

      if ((payload[0] > 1U) || (payload[1] > 1U))
      {
        if (detail != 0)
        {
          *detail = action_type;
        }
        return ERR_BAD_ACTION;
      }
      break;

    case GPIO_PULSE:
    case GPIO_STOP:
    case GPIO_MIRROR_SYNC:
      if (payload_len != 0U)
      {
        if (detail != 0)
        {
          *detail = action_type;
        }
        return ERR_BAD_EVENT;
      }
      break;

    default:
      if (detail != 0)
      {
        *detail = action_type;
      }
      return ERR_BAD_ACTION;
  }

  return ACK_OK;
}

static uint8_t scheduler_validate_action_record(const uint8_t *record,
                                                uint16_t remaining_len,
                                                uint8_t action_index,
                                                uint8_t *record_len,
                                                uint8_t *detail)
{
  uint8_t module_type;
  uint8_t module_id;
  uint8_t action_type;
  uint8_t action_len;
  uint8_t error_code;

  if (remaining_len < ACTION_HEADER_SIZE)
  {
    if (detail != 0)
    {
      *detail = action_index;
    }
    return ERR_BAD_EVENT;
  }

  module_type = record[0];
  module_id = record[1];
  action_type = record[2];
  action_len = record[3];

  if (remaining_len < (uint16_t)(ACTION_HEADER_SIZE + action_len))
  {
    if (detail != 0)
    {
      *detail = action_index;
    }
    return ERR_BAD_EVENT;
  }

  switch (module_type)
  {
    case MODULE_PUMP_PERISTALTIC:
      error_code = scheduler_validate_pump_action(module_id,
                                                  action_type,
                                                  &record[ACTION_HEADER_SIZE],
                                                  action_len,
                                                  detail);
      break;

    case MODULE_GPIO_FPGA:
      error_code = scheduler_validate_gpio_action(module_id,
                                                  action_type,
                                                  &record[ACTION_HEADER_SIZE],
                                                  action_len,
                                                  detail);
      break;

    default:
      if (detail != 0)
      {
        *detail = module_type;
      }
      return ERR_BAD_MODULE;
  }

  if (error_code != ACK_OK)
  {
    return error_code;
  }

  *record_len = (uint8_t)(ACTION_HEADER_SIZE + action_len);
  return ACK_OK;
}

void scheduler_init(void)
{
  memset(&g_scheduler, 0, sizeof(g_scheduler));
  g_scheduler.state = SCHED_IDLE;
}

uint8_t scheduler_clear(uint8_t *detail)
{
  if (detail != 0)
  {
    *detail = 0U;
  }

  if (g_scheduler.state == SCHED_RUNNING)
  {
    return ERR_BUSY_RUNNING;
  }

  g_scheduler.schedule.event_count = 0U;
  g_scheduler.last_event_id = 0U;
  g_scheduler.last_error = 0U;
  g_scheduler.state = SCHED_IDLE;
  return ACK_OK;
}

uint8_t scheduler_upload_event(const uint8_t *payload, uint16_t payload_len, uint8_t *detail)
{
  uint32_t event_id;
  uint64_t timestamp_us;
  uint8_t action_count;
  uint16_t payload_offset;
  uint8_t action_index;
  uint8_t action_record_len;
  uint16_t action_bytes_used;
  ScheduleEvent *event_slot;
  uint8_t error_code;

  if (detail != 0)
  {
    *detail = 0U;
  }

  if ((payload == 0) || (payload_len < EVENT_HEADER_SIZE))
  {
    return ERR_BAD_EVENT;
  }

  if (g_scheduler.state == SCHED_RUNNING)
  {
    return ERR_BUSY_RUNNING;
  }

  if (g_scheduler.schedule.event_count >= MAX_EVENTS)
  {
    return ERR_SCHEDULE_FULL;
  }

  event_id = read_u32_le(&payload[0]);
  timestamp_us = read_u64_le(&payload[4]);
  action_count = payload[12];

  if (g_scheduler.schedule.event_count != 0U)
  {
    const ScheduleEvent *last_event = &g_scheduler.schedule.events[g_scheduler.schedule.event_count - 1U];
    if ((timestamp_us <= last_event->timestamp_us) ||
        ((timestamp_us - last_event->timestamp_us) < SCHEDULE_MIN_EVENT_SPACING_US))
    {
      if (detail != 0)
      {
        *detail = (uint8_t)g_scheduler.schedule.event_count;
      }
      return ERR_BAD_EVENT;
    }
  }

  if (action_count == 0U)
  {
    return ERR_BAD_EVENT;
  }

  payload_offset = EVENT_HEADER_SIZE;
  action_bytes_used = 0U;

  for (action_index = 0U; action_index < action_count; action_index++)
  {
    error_code = scheduler_validate_action_record(&payload[payload_offset],
                                                  (uint16_t)(payload_len - payload_offset),
                                                  action_index,
                                                  &action_record_len,
                                                  detail);
    if (error_code != ACK_OK)
    {
      return error_code;
    }

    if ((uint16_t)(action_bytes_used + action_record_len) > MAX_EVENT_ACTION_BYTES)
    {
      if (detail != 0)
      {
        *detail = action_index;
      }
      return ERR_BAD_EVENT;
    }

    action_bytes_used = (uint16_t)(action_bytes_used + action_record_len);
    payload_offset = (uint16_t)(payload_offset + action_record_len);
  }

  if (payload_offset != payload_len)
  {
    return ERR_BAD_EVENT;
  }

  event_slot = &g_scheduler.schedule.events[g_scheduler.schedule.event_count];
  memset(event_slot, 0, sizeof(*event_slot));
  event_slot->event_id = event_id;
  event_slot->timestamp_us = timestamp_us;
  event_slot->action_count = action_count;
  event_slot->action_bytes_len = (uint8_t)action_bytes_used;
  memcpy(event_slot->action_bytes, &payload[EVENT_HEADER_SIZE], action_bytes_used);

  g_scheduler.schedule.event_count++;
  g_scheduler.last_event_id = event_id;
  g_scheduler.state = SCHED_LOADED;

  return ACK_OK;
}

uint8_t scheduler_start(uint8_t *detail)
{
  if (detail != 0)
  {
    *detail = 0U;
  }

  if (g_scheduler.state == SCHED_RUNNING)
  {
    return ERR_BUSY_RUNNING;
  }

  if (g_scheduler.schedule.event_count == 0U)
  {
    return ERR_BAD_EVENT;
  }

  g_scheduler.state = SCHED_RUNNING;
  return ACK_OK;
}

void scheduler_stop(void)
{
  if (g_scheduler.schedule.event_count == 0U)
  {
    g_scheduler.state = SCHED_IDLE;
  }
  else
  {
    g_scheduler.state = SCHED_STOPPED;
  }
}

void scheduler_get_status(SchedulerStatus *status, uint64_t current_time_us)
{
  if (status == 0)
  {
    return;
  }

  status->scheduler_state = g_scheduler.state;
  status->last_error = g_scheduler.last_error;
  status->event_count = g_scheduler.schedule.event_count;
  status->last_event_id = g_scheduler.last_event_id;
  status->current_time_us = current_time_us;
}

void scheduler_record_error(uint8_t error_code)
{
  g_scheduler.last_error = error_code;
}

void scheduler_set_error(uint8_t error_code)
{
  g_scheduler.last_error = error_code;
  g_scheduler.state = SCHED_ERROR;
}

uint8_t scheduler_get_state(void)
{
  return g_scheduler.state;
}

const Schedule *scheduler_get_schedule(void)
{
  return &g_scheduler.schedule;
}
