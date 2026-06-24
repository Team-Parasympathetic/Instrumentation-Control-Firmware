#include "backplane_fpga_bus.h"

#include "backplane_card_bus.h"
#include "fpga_timing_regs.h"
#include "main.h"
#include "protocol.h"
#include "spi_frame.h"

#include <string.h>

#define FPGA_BUS_DISCOVERY_PASSES  6U
#define ACTION_HEADER_SIZE         4U
#define GPIO_WAVEFORM_PAYLOAD_SIZE 16U
#define GPIO_WAVEFORM_PHASE_STEP_OFFSET      4U
#define GPIO_WAVEFORM_DUTY_THRESHOLD_OFFSET  8U

typedef struct
{
  uint8_t active_slot;
  uint8_t active_slot_valid;
  uint8_t schedule_loaded;
  uint8_t offloaded_schedule_prepared;
  uint8_t offloaded_schedule_active;
  uint16_t queued_event_count;
  uint16_t queued_action_count;
  uint8_t last_status;
  uint8_t last_detail;
} FpgaBusContext;

typedef struct
{
  uint8_t module_type;
  uint8_t module_id;
  uint8_t action_type;
  uint8_t action_len;
  const uint8_t *payload;
} FpgaActionView;

static FpgaBusContext g_fpga_bus;

static void fpga_bus_set_error(uint8_t status, uint8_t detail)
{
  g_fpga_bus.last_status = status;
  g_fpga_bus.last_detail = detail;
}

static uint32_t fpga_bus_checksum_word(uint32_t checksum, uint32_t value)
{
  return ((checksum << 1) | (checksum >> 31)) ^ value;
}

static uint32_t fpga_bus_checksum_event(uint32_t checksum, uint64_t timestamp_us)
{
  checksum = fpga_bus_checksum_word(checksum, (uint32_t)(timestamp_us & 0xFFFFFFFFULL));
  checksum = fpga_bus_checksum_word(checksum, (uint32_t)((timestamp_us >> 32) & 0xFFFFFFFFULL));
  return checksum;
}

static uint32_t fpga_bus_checksum_action(uint32_t checksum,
                                         uint32_t event_index,
                                         uint32_t meta,
                                         uint32_t phase_step,
                                         uint32_t duty_threshold)
{
  checksum = fpga_bus_checksum_word(checksum, event_index);
  checksum = fpga_bus_checksum_word(checksum, meta);
  checksum = fpga_bus_checksum_word(checksum, phase_step);
  checksum = fpga_bus_checksum_word(checksum, duty_threshold);
  return checksum;
}

static bool fpga_bus_ensure_slot(void)
{
  uint8_t pass;

  g_fpga_bus.active_slot_valid = 0U;

  for (pass = 0U; pass < FPGA_BUS_DISCOVERY_PASSES; pass++)
  {
    card_bus_discover_all();
    g_fpga_bus.active_slot = card_bus_find_first_slot(CARD_TYPE_FPGA_GPIO_SYNC);
    if (g_fpga_bus.active_slot < CARD_BUS_SLOT_COUNT)
    {
      g_fpga_bus.active_slot_valid = 1U;
      fpga_bus_set_error(STATUS_OK, 0U);
      return true;
    }

    if ((pass + 1U) < FPGA_BUS_DISCOVERY_PASSES)
    {
      HAL_Delay(25U);
    }
  }

  fpga_bus_set_error(STATUS_HW_ERROR, 0U);
  return false;
}

static bool fpga_bus_read_action(const ScheduleEvent *event,
                                 uint16_t offset,
                                 uint8_t action_index,
                                 FpgaActionView *action_out)
{
  if ((event == 0) || (action_out == 0))
  {
    fpga_bus_set_error(STATUS_BAD_LEN, action_index);
    return false;
  }

  if ((uint16_t)(offset + ACTION_HEADER_SIZE) > event->action_bytes_len)
  {
    fpga_bus_set_error(STATUS_BAD_LEN, action_index);
    return false;
  }

  action_out->action_len = event->action_bytes[offset + 3U];
  if ((uint16_t)(offset + ACTION_HEADER_SIZE + action_out->action_len) > event->action_bytes_len)
  {
    fpga_bus_set_error(STATUS_BAD_LEN, action_index);
    return false;
  }

  action_out->module_type = event->action_bytes[offset + 0U];
  action_out->module_id = event->action_bytes[offset + 1U];
  action_out->action_type = event->action_bytes[offset + 2U];
  action_out->payload = &event->action_bytes[offset + ACTION_HEADER_SIZE];
  return true;
}

static bool fpga_bus_build_gpio_registers(uint8_t channel_id,
                                          uint8_t action_type,
                                          const uint8_t *payload,
                                          uint8_t payload_len,
                                          uint32_t *control_out,
                                          uint32_t *phase_step_out,
                                          uint32_t *duty_threshold_out)
{
  uint32_t control = 0U;
  uint32_t phase_step = 0U;
  uint32_t duty_threshold = 0U;

  if ((control_out == 0) || (phase_step_out == 0) || (duty_threshold_out == 0))
  {
    fpga_bus_set_error(STATUS_BAD_LEN, channel_id);
    return false;
  }

  if (channel_id >= 32U)
  {
    fpga_bus_set_error(STATUS_BAD_ADDR, channel_id);
    return false;
  }

  switch (action_type)
  {
    case GPIO_SET_WAVEFORM:
      if (payload_len != GPIO_WAVEFORM_PAYLOAD_SIZE)
      {
        fpga_bus_set_error(STATUS_BAD_LEN, channel_id);
        return false;
      }

      phase_step = read_u32_le(&payload[GPIO_WAVEFORM_PHASE_STEP_OFFSET]);
      duty_threshold = read_u32_le(&payload[GPIO_WAVEFORM_DUTY_THRESHOLD_OFFSET]);
      control = FPGA_CHANNEL_MODE_PWM;
      if (payload[0] != 0U)
      {
        control |= FPGA_CHANNEL_FLAG_POLARITY_INV;
      }
      if (payload[1] != 0U)
      {
        control |= FPGA_CHANNEL_FLAG_IDLE_HIGH;
      }
      break;

    case GPIO_PULSE:
      if (payload_len != 0U)
      {
        fpga_bus_set_error(STATUS_BAD_LEN, channel_id);
        return false;
      }
      control = FPGA_CHANNEL_MODE_FORCE_HIGH;
      break;

    case GPIO_STOP:
      if (payload_len != 0U)
      {
        fpga_bus_set_error(STATUS_BAD_LEN, channel_id);
        return false;
      }
      control = FPGA_CHANNEL_MODE_STOP;
      break;

    case GPIO_MIRROR_SYNC:
      if (payload_len != 0U)
      {
        fpga_bus_set_error(STATUS_BAD_LEN, channel_id);
        return false;
      }
      control = FPGA_CHANNEL_MODE_MIRROR_SYNC;
      break;

    default:
      fpga_bus_set_error(STATUS_BAD_CMD, action_type);
      return false;
  }

  *control_out = control;
  *phase_step_out = phase_step;
  *duty_threshold_out = duty_threshold;
  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool fpga_bus_validate_actions(const Schedule *schedule,
                                      uint16_t *gpio_action_count_out)
{
  uint16_t event_index;
  uint16_t gpio_action_count = 0U;

  if ((schedule == 0) || (gpio_action_count_out == 0))
  {
    fpga_bus_set_error(STATUS_BAD_LEN, 0U);
    return false;
  }

  for (event_index = 0U; event_index < schedule->event_count; event_index++)
  {
    const ScheduleEvent *event = &schedule->events[event_index];
    uint16_t offset = 0U;
    uint8_t action_index;

    for (action_index = 0U; action_index < event->action_count; action_index++)
    {
      FpgaActionView action;
      uint32_t control;
      uint32_t phase_step;
      uint32_t duty_threshold;

      if (!fpga_bus_read_action(event, offset, action_index, &action))
      {
        return false;
      }

      if (action.module_type == MODULE_GPIO_FPGA)
      {
        gpio_action_count++;
        if (gpio_action_count > FPGA_CARD_MAX_LOCAL_EVENTS)
        {
          fpga_bus_set_error(STATUS_BAD_LEN, action.module_id);
          return false;
        }

        if (!fpga_bus_build_gpio_registers(action.module_id,
                                           action.action_type,
                                           action.payload,
                                           action.action_len,
                                           &control,
                                           &phase_step,
                                           &duty_threshold))
        {
          return false;
        }
      }

      offset = (uint16_t)(offset + ACTION_HEADER_SIZE + action.action_len);
    }
  }

  *gpio_action_count_out = gpio_action_count;
  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool fpga_bus_write_control(uint32_t value)
{
  if (!card_bus_write_reg(g_fpga_bus.active_slot, CARD_REG_CONTROL, value))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 0U);
    return false;
  }

  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool fpga_bus_clear_remote_queue(void)
{
  if (!fpga_bus_write_control(FPGA_CARD_CONTROL_CLEAR_QUEUE))
  {
    return false;
  }

  return fpga_bus_write_control(0U);
}

static bool fpga_bus_write_event_entry(uint64_t timestamp_us)
{
  if (!card_bus_write_reg(g_fpga_bus.active_slot,
                          FPGA_EVENT_REG_TIME_LO,
                          (uint32_t)(timestamp_us & 0xFFFFFFFFULL)))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 0U);
    return false;
  }

  if (!card_bus_write_reg(g_fpga_bus.active_slot,
                          FPGA_EVENT_REG_TIME_HI,
                          (uint32_t)((timestamp_us >> 32) & 0xFFFFFFFFULL)))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 0U);
    return false;
  }

  if (!card_bus_write_reg(g_fpga_bus.active_slot, FPGA_EVENT_REG_PUSH, 1U))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 0U);
    return false;
  }

  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool fpga_bus_write_action_entry(uint16_t event_index,
                                        uint8_t channel_id,
                                        uint32_t control,
                                        uint32_t phase_step,
                                        uint32_t duty_threshold)
{
  if (!card_bus_write_reg(g_fpga_bus.active_slot,
                          FPGA_ACTION_REG_EVENT_INDEX,
                          (uint32_t)event_index))
  {
    fpga_bus_set_error(card_bus_get_last_status(), channel_id);
    return false;
  }

  if (!card_bus_write_reg(g_fpga_bus.active_slot,
                          FPGA_ACTION_REG_META,
                          FPGA_ACTION_META(channel_id, control)))
  {
    fpga_bus_set_error(card_bus_get_last_status(), channel_id);
    return false;
  }

  if (!card_bus_write_reg(g_fpga_bus.active_slot, FPGA_ACTION_REG_PHASE_STEP, phase_step))
  {
    fpga_bus_set_error(card_bus_get_last_status(), channel_id);
    return false;
  }

  if (!card_bus_write_reg(g_fpga_bus.active_slot, FPGA_ACTION_REG_DUTY_THRESHOLD, duty_threshold))
  {
    fpga_bus_set_error(card_bus_get_last_status(), channel_id);
    return false;
  }

  if (!card_bus_write_reg(g_fpga_bus.active_slot, FPGA_ACTION_REG_PUSH, 1U))
  {
    fpga_bus_set_error(card_bus_get_last_status(), channel_id);
    return false;
  }

  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool fpga_bus_verify_remote_schedule(uint16_t expected_event_count,
                                            uint16_t expected_action_count,
                                            uint64_t expected_last_event_time,
                                            uint32_t expected_last_action_event,
                                            uint32_t expected_event_checksum,
                                            uint32_t expected_action_checksum)
{
  uint32_t actual_event_count;
  uint32_t actual_action_count;
  uint32_t actual_last_event_lo;
  uint32_t actual_last_event_hi;
  uint32_t actual_last_action_event;
  uint32_t actual_event_checksum;
  uint32_t actual_action_checksum;

  if (!card_bus_read_reg(g_fpga_bus.active_slot, FPGA_EVENT_REG_COUNT, &actual_event_count))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 0U);
    return false;
  }

  if (!card_bus_read_reg(g_fpga_bus.active_slot, FPGA_ACTION_REG_COUNT, &actual_action_count))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 1U);
    return false;
  }

  if (!card_bus_read_reg(g_fpga_bus.active_slot, FPGA_EVENT_REG_LAST_TIME_LO, &actual_last_event_lo))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 2U);
    return false;
  }

  if (!card_bus_read_reg(g_fpga_bus.active_slot, FPGA_EVENT_REG_LAST_TIME_HI, &actual_last_event_hi))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 3U);
    return false;
  }

  if (!card_bus_read_reg(g_fpga_bus.active_slot,
                         FPGA_ACTION_REG_LAST_EVENT_INDEX,
                         &actual_last_action_event))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 4U);
    return false;
  }

  if (!card_bus_read_reg(g_fpga_bus.active_slot, FPGA_EVENT_REG_CHECKSUM, &actual_event_checksum))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 5U);
    return false;
  }

  if (!card_bus_read_reg(g_fpga_bus.active_slot, FPGA_ACTION_REG_CHECKSUM, &actual_action_checksum))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 6U);
    return false;
  }

  if ((actual_event_count != expected_event_count) ||
      (actual_action_count != expected_action_count) ||
      (actual_last_event_lo != (uint32_t)(expected_last_event_time & 0xFFFFFFFFULL)) ||
      (actual_last_event_hi != (uint32_t)((expected_last_event_time >> 32) & 0xFFFFFFFFULL)) ||
      ((expected_action_count != 0U) && (actual_last_action_event != expected_last_action_event)) ||
      (actual_event_checksum != expected_event_checksum) ||
      (actual_action_checksum != expected_action_checksum))
  {
    fpga_bus_set_error(STATUS_HW_ERROR, 0U);
    return false;
  }

  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

static bool fpga_bus_upload_schedule(const Schedule *schedule)
{
  uint16_t event_index;
  uint16_t uploaded_action_count = 0U;
  uint32_t expected_event_checksum = 0U;
  uint32_t expected_action_checksum = 0U;
  uint32_t expected_last_action_event = 0U;

  if (schedule == 0)
  {
    fpga_bus_set_error(STATUS_BAD_LEN, 0U);
    return false;
  }

  if (!fpga_bus_clear_remote_queue())
  {
    return false;
  }

  for (event_index = 0U; event_index < schedule->event_count; event_index++)
  {
    if (!fpga_bus_write_event_entry(schedule->events[event_index].timestamp_us))
    {
      return false;
    }

    expected_event_checksum =
      fpga_bus_checksum_event(expected_event_checksum,
                              schedule->events[event_index].timestamp_us);
  }

  for (event_index = 0U; event_index < schedule->event_count; event_index++)
  {
    const ScheduleEvent *event = &schedule->events[event_index];
    uint16_t offset = 0U;
    uint8_t action_index;

    for (action_index = 0U; action_index < event->action_count; action_index++)
    {
      FpgaActionView action;
      uint32_t control;
      uint32_t phase_step;
      uint32_t duty_threshold;

      if (!fpga_bus_read_action(event, offset, action_index, &action))
      {
        return false;
      }

      if (action.module_type == MODULE_GPIO_FPGA)
      {
        uint32_t meta;

        if (!fpga_bus_build_gpio_registers(action.module_id,
                                           action.action_type,
                                           action.payload,
                                           action.action_len,
                                           &control,
                                           &phase_step,
                                           &duty_threshold))
        {
          return false;
        }

        if (!fpga_bus_write_action_entry(event_index,
                                         action.module_id,
                                         control,
                                         phase_step,
                                         duty_threshold))
        {
          return false;
        }

        meta = FPGA_ACTION_META(action.module_id, control);
        expected_action_checksum =
          fpga_bus_checksum_action(expected_action_checksum,
                                   (uint32_t)event_index,
                                   meta,
                                   phase_step,
                                   duty_threshold);
        expected_last_action_event = (uint32_t)event_index;
        uploaded_action_count++;
      }

      offset = (uint16_t)(offset + ACTION_HEADER_SIZE + action.action_len);
    }
  }

  if (!fpga_bus_verify_remote_schedule(
        schedule->event_count,
        uploaded_action_count,
        schedule->events[schedule->event_count - 1U].timestamp_us,
        expected_last_action_event,
        expected_event_checksum,
        expected_action_checksum))
  {
    return false;
  }

  g_fpga_bus.queued_event_count = schedule->event_count;
  g_fpga_bus.queued_action_count = uploaded_action_count;
  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

void fpga_bus_init(void)
{
  memset(&g_fpga_bus, 0, sizeof(g_fpga_bus));
}

bool fpga_bus_validate_schedule(const Schedule *schedule)
{
  uint16_t gpio_action_count;

  g_fpga_bus.schedule_loaded = 0U;
  g_fpga_bus.offloaded_schedule_prepared = 0U;
  g_fpga_bus.offloaded_schedule_active = 0U;
  g_fpga_bus.queued_event_count = 0U;
  g_fpga_bus.queued_action_count = 0U;

  if (schedule == 0)
  {
    fpga_bus_set_error(STATUS_BAD_LEN, 0U);
    return false;
  }

  if (!fpga_bus_validate_actions(schedule, &gpio_action_count))
  {
    return false;
  }

  g_fpga_bus.schedule_loaded = (schedule->event_count != 0U) ? 1U : 0U;
  g_fpga_bus.queued_action_count = gpio_action_count;

  if (schedule->event_count == 0U)
  {
    g_fpga_bus.active_slot_valid = 0U;
    fpga_bus_set_error(STATUS_OK, 0U);
    return true;
  }

  return fpga_bus_ensure_slot();
}

bool fpga_bus_start_schedule(const Schedule *schedule)
{
  if (schedule == 0)
  {
    fpga_bus_set_error(STATUS_BAD_LEN, 0U);
    return false;
  }

  if (schedule->event_count == 0U)
  {
    g_fpga_bus.offloaded_schedule_prepared = 0U;
    g_fpga_bus.offloaded_schedule_active = 0U;
    fpga_bus_set_error(STATUS_OK, 0U);
    return true;
  }

  if (!g_fpga_bus.active_slot_valid && !fpga_bus_ensure_slot())
  {
    return false;
  }

  if (!fpga_bus_upload_schedule(schedule))
  {
    (void)fpga_bus_clear_remote_queue();
    g_fpga_bus.offloaded_schedule_prepared = 0U;
    g_fpga_bus.offloaded_schedule_active = 0U;
    return false;
  }

  g_fpga_bus.offloaded_schedule_prepared = 1U;
  g_fpga_bus.offloaded_schedule_active = 0U;
  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

bool fpga_bus_arm_schedule(void)
{
  if (g_fpga_bus.schedule_loaded == 0U)
  {
    fpga_bus_set_error(STATUS_OK, 0U);
    return true;
  }

  if (!g_fpga_bus.active_slot_valid && !fpga_bus_ensure_slot())
  {
    return false;
  }

  if (g_fpga_bus.offloaded_schedule_prepared == 0U)
  {
    fpga_bus_set_error(STATUS_BAD_LEN, 0U);
    return false;
  }

  if (!fpga_bus_write_control(FPGA_CARD_CONTROL_RUN))
  {
    return false;
  }

  g_fpga_bus.offloaded_schedule_active = 1U;
  fpga_bus_set_error(STATUS_OK, 0U);
  return true;
}

bool fpga_bus_is_schedule_complete(void)
{
  uint32_t status_value;

  if ((g_fpga_bus.schedule_loaded == 0U) || (g_fpga_bus.offloaded_schedule_active == 0U))
  {
    fpga_bus_set_error(STATUS_OK, 0U);
    return false;
  }

  if (g_fpga_bus.active_slot_valid == 0U)
  {
    fpga_bus_set_error(STATUS_BAD_ADDR, 0U);
    return false;
  }

  if (!card_bus_read_reg(g_fpga_bus.active_slot, CARD_REG_STATUS, &status_value))
  {
    fpga_bus_set_error(card_bus_get_last_status(), 0U);
    return false;
  }

  fpga_bus_set_error(STATUS_OK, 0U);
  return ((status_value & FPGA_CARD_STATUS_DONE) != 0U);
}

void fpga_bus_stop_all(void)
{
  g_fpga_bus.offloaded_schedule_prepared = 0U;
  g_fpga_bus.offloaded_schedule_active = 0U;
  g_fpga_bus.schedule_loaded = 0U;
  g_fpga_bus.queued_event_count = 0U;
  g_fpga_bus.queued_action_count = 0U;

  if (g_fpga_bus.active_slot_valid == 0U)
  {
    fpga_bus_set_error(STATUS_OK, 0U);
    return;
  }

  (void)fpga_bus_write_control(0U);
  (void)fpga_bus_clear_remote_queue();

  fpga_bus_set_error(STATUS_OK, 0U);
}

uint8_t fpga_bus_get_last_status(void)
{
  return g_fpga_bus.last_status;
}

uint8_t fpga_bus_get_last_detail(void)
{
  return g_fpga_bus.last_detail;
}
