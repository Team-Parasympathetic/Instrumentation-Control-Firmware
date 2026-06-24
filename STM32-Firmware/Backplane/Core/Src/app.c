#include "app.h"

#include "backplane_card_bus.h"
#include "backplane_fpga_bus.h"
#include "backplane_pump_bus.h"
#include "main.h"
#include "protocol.h"
#include "scheduler.h"
#include "usbd_cdc_if.h"

#include <string.h>

#define STATUS_PAYLOAD_SIZE  16U
#define PREPARE_STATUS_PAYLOAD_SIZE  8U
#define CARD_INVENTORY_ENTRY_SIZE    7U
#define CARD_INVENTORY_PAYLOAD_SIZE  (1U + (CARD_BUS_SLOT_COUNT * CARD_INVENTORY_ENTRY_SIZE))
#define APP_POST_PRELOAD_START_DELAY_MS 1000U

typedef enum
{
  APP_EVENT_TYPE_PACKET = 0,
  APP_EVENT_TYPE_PROTOCOL_ERROR
} AppEventType;

typedef struct
{
  uint8_t valid;
  AppEventType type;
  union
  {
    ProtocolPacket packet;
    struct
    {
      uint16_t seq;
      uint8_t error_code;
      uint8_t detail;
    } protocol_error;
  } data;
} AppEvent;

typedef struct
{
  ProtocolParser parser;
  uint8_t pending_tx_valid;
  uint16_t pending_tx_len;
  uint8_t pending_tx_frame[PROTOCOL_MAX_FRAME_SIZE];
  uint8_t start_pending;
  uint32_t start_ready_ms;
  uint8_t schedule_prepared;
  uint32_t prepare_ready_ms;
  /* One deferred event is enough because the host uses a request/response flow. */
  AppEvent deferred_event;
} AppContext;

static AppContext g_app;

static uint64_t app_get_time_us(void)
{
  return (uint64_t)HAL_GetTick() * 1000ULL;
}

static uint8_t app_prepared_schedule_ready(void)
{
  return ((g_app.schedule_prepared != 0U) &&
          ((int32_t)(HAL_GetTick() - g_app.prepare_ready_ms) >= 0)) ? 1U : 0U;
}

static uint32_t app_prepare_remaining_ms(void)
{
  if (app_prepared_schedule_ready() != 0U)
  {
    return 0U;
  }

  if (g_app.schedule_prepared == 0U)
  {
    return 0U;
  }

  return (uint32_t)(g_app.prepare_ready_ms - HAL_GetTick());
}

static void app_handle_scheduler_runtime(void)
{
  if (scheduler_get_state() != SCHED_RUNNING)
  {
    return;
  }

  if (fpga_bus_is_schedule_complete())
  {
    pump_bus_stop_all();
    fpga_bus_stop_all();
    scheduler_stop();
  }
}

static void app_cancel_pending_start(void)
{
  g_app.start_pending = 0U;
  g_app.start_ready_ms = 0U;
}

static void app_reset_prepare_state(void)
{
  app_cancel_pending_start();
  g_app.schedule_prepared = 0U;
  g_app.prepare_ready_ms = 0U;
}

static void app_clear_prepared_outputs(void)
{
  app_reset_prepare_state();
  pump_bus_stop_all();
  fpga_bus_stop_all();
}

static uint8_t app_arm_prepared_schedule(uint8_t *detail_out)
{
  uint8_t error_code;

  if (detail_out != 0)
  {
    *detail_out = 0U;
  }

  error_code = scheduler_start(detail_out);
  if (error_code == ACK_OK)
  {
    if (!pump_bus_arm_schedule())
    {
      if (detail_out != 0)
      {
        *detail_out = pump_bus_get_last_detail();
      }
      pump_bus_stop_all();
      fpga_bus_stop_all();
      scheduler_stop();
      return ERR_BAD_MODULE;
    }

    if (!fpga_bus_arm_schedule())
    {
      if (detail_out != 0)
      {
        *detail_out = fpga_bus_get_last_detail();
      }
      pump_bus_stop_all();
      fpga_bus_stop_all();
      scheduler_stop();
      return ERR_BAD_MODULE;
    }

  }
  else
  {
    pump_bus_stop_all();
    fpga_bus_stop_all();
  }

  return error_code;
}

static uint8_t app_prepare_schedule(uint8_t *detail_out)
{
  const Schedule *schedule = scheduler_get_schedule();

  if (detail_out != 0)
  {
    *detail_out = 0U;
  }

  if ((scheduler_get_state() == SCHED_RUNNING) || (g_app.start_pending != 0U))
  {
    return ERR_BUSY_RUNNING;
  }

  if ((schedule == 0) || (schedule->event_count == 0U))
  {
    return ERR_BAD_EVENT;
  }

  if (g_app.schedule_prepared != 0U)
  {
    return ACK_OK;
  }

  if (!pump_bus_validate_schedule(schedule))
  {
    if (detail_out != 0)
    {
      *detail_out = pump_bus_get_last_detail();
    }
    return ERR_BAD_MODULE;
  }

  if (!fpga_bus_validate_schedule(schedule))
  {
    if (detail_out != 0)
    {
      *detail_out = fpga_bus_get_last_detail();
    }
    return ERR_BAD_MODULE;
  }

  if (!pump_bus_start_schedule(schedule))
  {
    if (detail_out != 0)
    {
      *detail_out = pump_bus_get_last_detail();
    }
    pump_bus_stop_all();
    return ERR_BAD_MODULE;
  }

  if (!fpga_bus_start_schedule(schedule))
  {
    if (detail_out != 0)
    {
      *detail_out = fpga_bus_get_last_detail();
    }
    pump_bus_stop_all();
    fpga_bus_stop_all();
    return ERR_BAD_MODULE;
  }

  if (!pump_bus_preload_initial_dacs())
  {
    if (detail_out != 0)
    {
      *detail_out = pump_bus_get_last_detail();
    }
    pump_bus_stop_all();
    fpga_bus_stop_all();
    return ERR_BAD_MODULE;
  }

  g_app.schedule_prepared = 1U;
  g_app.prepare_ready_ms = HAL_GetTick() + APP_POST_PRELOAD_START_DELAY_MS;
  return ACK_OK;
}

static uint8_t app_start_schedule(uint8_t *detail_out)
{
  uint8_t error_code;

  if (detail_out != 0)
  {
    *detail_out = 0U;
  }

  if ((scheduler_get_state() == SCHED_RUNNING) || (g_app.start_pending != 0U))
  {
    return ERR_BUSY_RUNNING;
  }

  if (g_app.schedule_prepared == 0U)
  {
    error_code = app_prepare_schedule(detail_out);
    if (error_code != ACK_OK)
    {
      return error_code;
    }

    g_app.start_pending = 1U;
    g_app.start_ready_ms = g_app.prepare_ready_ms;
    return ACK_OK;
  }

  if (app_prepared_schedule_ready() == 0U)
  {
    return ERR_BUSY_RUNNING;
  }

  error_code = app_arm_prepared_schedule(detail_out);
  app_reset_prepare_state();
  return error_code;
}

static void app_handle_pending_start(void)
{
  uint8_t error_code;
  uint8_t detail = 0U;

  if (g_app.start_pending == 0U)
  {
    return;
  }

  if ((int32_t)(HAL_GetTick() - g_app.start_ready_ms) < 0)
  {
    return;
  }

  app_cancel_pending_start();
  error_code = app_arm_prepared_schedule(&detail);
  app_reset_prepare_state();
  if (error_code != ACK_OK)
  {
    scheduler_set_error(error_code);
  }
}

static uint8_t app_try_tx(const uint8_t *data, uint16_t len)
{
  return CDC_Transmit_FS((uint8_t *)data, len);
}

static uint8_t app_protocol_tx(const uint8_t *data, uint16_t len, void *context)
{
  uint8_t tx_result;

  (void)context;

  if ((data == 0) || (len == 0U) || (len > PROTOCOL_MAX_FRAME_SIZE))
  {
    return USBD_FAIL;
  }

  if (g_app.pending_tx_valid != 0U)
  {
    /* Keep response ordering deterministic until the pending frame is flushed. */
    return USBD_BUSY;
  }

  tx_result = app_try_tx(data, len);
  if (tx_result == USBD_OK)
  {
    return USBD_OK;
  }

  if ((tx_result == USBD_BUSY) && (g_app.pending_tx_valid == 0U))
  {
    memcpy(g_app.pending_tx_frame, data, len);
    g_app.pending_tx_len = len;
    g_app.pending_tx_valid = 1U;
    return USBD_OK;
  }

  /* The host protocol is request/response, so one pending frame is intentional. */
  return tx_result;
}

static void app_send_status(uint16_t seq)
{
  SchedulerStatus status;
  uint8_t payload[STATUS_PAYLOAD_SIZE];

  scheduler_get_status(&status, app_get_time_us());

  payload[0] = status.scheduler_state;
  payload[1] = status.last_error;
  write_u16_le(&payload[2], status.event_count);
  write_u32_le(&payload[4], status.last_event_id);
  write_u64_le(&payload[8], status.current_time_us);

  (void)protocol_send_frame(app_protocol_tx, 0, MSG_GET_STATUS, seq, payload, sizeof(payload));
}

static void app_send_prepare_status(uint16_t seq)
{
  uint8_t payload[PREPARE_STATUS_PAYLOAD_SIZE];

  memset(payload, 0, sizeof(payload));
  payload[0] = g_app.schedule_prepared;
  payload[1] = app_prepared_schedule_ready();
  payload[2] = g_app.start_pending;
  write_u32_le(&payload[4], app_prepare_remaining_ms());

  (void)protocol_send_frame(app_protocol_tx,
                            0,
                            MSG_GET_PREPARE_STATUS,
                            seq,
                            payload,
                            sizeof(payload));
}

static void app_send_card_inventory(uint16_t seq)
{
  const CardSlotInfo *slots;
  uint8_t payload[CARD_INVENTORY_PAYLOAD_SIZE];
  uint8_t slot;
  uint16_t offset = 1U;

  memset(payload, 0, sizeof(payload));
  payload[0] = CARD_BUS_SLOT_COUNT;
  slots = card_bus_get_slots();

  for (slot = 0U; slot < CARD_BUS_SLOT_COUNT; slot++)
  {
    payload[offset + 0U] = slots[slot].present;
    payload[offset + 1U] = slots[slot].card_type;
    payload[offset + 2U] = slots[slot].firmware_major;
    payload[offset + 3U] = slots[slot].firmware_minor;
    write_u16_le(&payload[offset + 4U], slots[slot].capabilities);
    payload[offset + 6U] = slots[slot].max_local_events;
    offset = (uint16_t)(offset + CARD_INVENTORY_ENTRY_SIZE);
  }

  (void)protocol_send_frame(app_protocol_tx,
                            0,
                            MSG_GET_CARD_INVENTORY,
                            seq,
                            payload,
                            sizeof(payload));
}

static uint8_t app_store_deferred_event(const AppEvent *event)
{
  if ((event == 0) || (g_app.deferred_event.valid != 0U))
  {
    return 0U;
  }

  g_app.deferred_event = *event;
  g_app.deferred_event.valid = 1U;
  return 1U;
}

static uint8_t app_take_deferred_event(AppEvent *event_out)
{
  if ((event_out == 0) || (g_app.deferred_event.valid == 0U))
  {
    return 0U;
  }

  *event_out = g_app.deferred_event;
  g_app.deferred_event.valid = 0U;
  return 1U;
}

static void app_enqueue_protocol_error(uint16_t seq, uint8_t error_code, uint8_t detail, void *context)
{
  AppEvent event;

  (void)context;
  memset(&event, 0, sizeof(event));
  event.type = APP_EVENT_TYPE_PROTOCOL_ERROR;
  event.data.protocol_error.seq = seq;
  event.data.protocol_error.error_code = error_code;
  event.data.protocol_error.detail = detail;
  (void)app_store_deferred_event(&event);
}

static void app_enqueue_packet(const ProtocolPacket *packet, void *context)
{
  AppEvent event;

  (void)context;

  if (packet == 0)
  {
    return;
  }

  memset(&event, 0, sizeof(event));
  event.type = APP_EVENT_TYPE_PACKET;
  event.data.packet = *packet;
  (void)app_store_deferred_event(&event);
}

static void app_process_protocol_error(uint16_t seq, uint8_t error_code, uint8_t detail)
{
  scheduler_record_error(error_code);
  (void)protocol_send_error(app_protocol_tx, 0, seq, error_code, detail);
}

static void app_handle_packet(const ProtocolPacket *packet)
{
  uint8_t error_code = ACK_OK;
  uint8_t detail = 0U;

  switch (packet->msg_type)
  {
    case MSG_PING:
      break;

    case MSG_CLEAR_SCHEDULE:
      error_code = scheduler_clear(&detail);
      if (error_code == ACK_OK)
      {
        app_clear_prepared_outputs();
      }
      break;

    case MSG_UPLOAD_EVENT:
      if (g_app.start_pending != 0U)
      {
        error_code = ERR_BUSY_RUNNING;
      }
      else
      {
        if (g_app.schedule_prepared != 0U)
        {
          app_clear_prepared_outputs();
        }
        error_code = scheduler_upload_event(packet->payload, packet->payload_len, &detail);
      }
      break;

    case MSG_PREPARE_SCHEDULE:
      error_code = app_prepare_schedule(&detail);
      break;

    case MSG_START_SCHEDULE:
      error_code = app_start_schedule(&detail);
      break;

    case MSG_STOP_SCHEDULE:
      app_reset_prepare_state();
      pump_bus_stop_all();
      fpga_bus_stop_all();
      scheduler_stop();
      break;

    case MSG_GET_STATUS:
      break;

    case MSG_GET_PREPARE_STATUS:
      break;

    case MSG_GET_CARD_INVENTORY:
      if (scheduler_get_state() != SCHED_RUNNING)
      {
        card_bus_discover_all();
      }
      break;

    default:
      error_code = ERR_UNKNOWN_MSG;
      detail = packet->msg_type;
      break;
  }

  if (error_code != ACK_OK)
  {
    scheduler_record_error(error_code);
    (void)protocol_send_error(app_protocol_tx, 0, packet->seq, error_code, detail);
    return;
  }

  (void)protocol_send_ack(app_protocol_tx, 0, packet->seq, ACK_OK);

  if (packet->msg_type == MSG_GET_STATUS)
  {
    app_send_status(packet->seq);
  }
  else if (packet->msg_type == MSG_GET_PREPARE_STATUS)
  {
    app_send_prepare_status(packet->seq);
  }
  else if (packet->msg_type == MSG_GET_CARD_INVENTORY)
  {
    app_send_card_inventory(packet->seq);
  }
}

static void app_process_events(void)
{
  AppEvent event;

  if (g_app.pending_tx_valid != 0U)
  {
    return;
  }

  if (!app_take_deferred_event(&event))
  {
    return;
  }

  if (event.type == APP_EVENT_TYPE_PROTOCOL_ERROR)
  {
    app_process_protocol_error(event.data.protocol_error.seq,
                               event.data.protocol_error.error_code,
                               event.data.protocol_error.detail);
  }
  else
  {
    app_handle_packet(&event.data.packet);
  }
}

void app_init(void)
{
  memset(&g_app, 0, sizeof(g_app));
  scheduler_init();
  card_bus_init();
  pump_bus_init();
  fpga_bus_init();
  card_bus_discover_all();
  protocol_parser_init(&g_app.parser, app_enqueue_packet, app_enqueue_protocol_error, 0);
}

void app_poll(void)
{
  if (g_app.pending_tx_valid != 0U)
  {
    if (app_try_tx(g_app.pending_tx_frame, g_app.pending_tx_len) == USBD_OK)
    {
      g_app.pending_tx_valid = 0U;
      g_app.pending_tx_len = 0U;
    }
  }

  app_process_events();
  app_handle_pending_start();
  app_handle_scheduler_runtime();
}

void app_on_cdc_rx(uint8_t *buf, uint32_t len)
{
  protocol_parser_process(&g_app.parser, buf, len);
}
