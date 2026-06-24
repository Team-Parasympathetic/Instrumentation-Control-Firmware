#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/*
 * The protocol can express up to 256-byte payloads, but the STM32F103C8T6 SRAM
 * cannot hold 256 events with 192 raw action bytes each. These defaults keep
 * the prototype within MCU RAM and can be raised on larger targets.
 */
#define MAX_EVENTS                    48U
#define MAX_EVENT_ACTION_BYTES        192U
#define MAX_PUMPS                     64U
#define MAX_GPIO_OUTPUTS              32U
#define SCHEDULE_MIN_EVENT_SPACING_US 10000ULL

/*
 * Action record layout inside each event:
 * [MODULE_TYPE][MODULE_ID][ACTION_TYPE][ACTION_LEN][ACTION_PAYLOAD...]
 */
#define MODULE_PUMP_PERISTALTIC       0x01U
#define MODULE_GPIO_FPGA              0x02U

#define PUMP_SET_STATE                0x01U

#define GPIO_SET_WAVEFORM             0x01U
#define GPIO_PULSE                    0x02U
#define GPIO_STOP                     0x03U
#define GPIO_MIRROR_SYNC              0x04U

typedef enum
{
  SCHED_IDLE = 0U,
  SCHED_LOADED = 1U,
  SCHED_RUNNING = 2U,
  SCHED_STOPPED = 3U,
  SCHED_ERROR = 4U
} SchedulerState;

typedef struct
{
  uint64_t timestamp_us;
  uint32_t event_id;
  uint8_t action_count;
  uint8_t action_bytes_len;
  uint8_t reserved[2];
  uint8_t action_bytes[MAX_EVENT_ACTION_BYTES];
} ScheduleEvent;

typedef struct
{
  ScheduleEvent events[MAX_EVENTS];
  uint16_t event_count;
} Schedule;

typedef struct
{
  uint8_t scheduler_state;
  uint8_t last_error;
  uint16_t event_count;
  uint32_t last_event_id;
  uint64_t current_time_us;
} SchedulerStatus;

void scheduler_init(void);
uint8_t scheduler_clear(uint8_t *detail);
uint8_t scheduler_upload_event(const uint8_t *payload, uint16_t payload_len, uint8_t *detail);
uint8_t scheduler_start(uint8_t *detail);
void scheduler_stop(void);
void scheduler_get_status(SchedulerStatus *status, uint64_t current_time_us);
void scheduler_record_error(uint8_t error_code);
void scheduler_set_error(uint8_t error_code);
uint8_t scheduler_get_state(void);
const Schedule *scheduler_get_schedule(void);

#ifdef __cplusplus
}
#endif

#endif /* __SCHEDULER_H__ */
