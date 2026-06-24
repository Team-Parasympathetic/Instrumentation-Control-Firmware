#ifndef __BACKPLANE_PUMP_BUS_H__
#define __BACKPLANE_PUMP_BUS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "scheduler.h"

void pump_bus_init(void);
bool pump_bus_validate_schedule(const Schedule *schedule);
bool pump_bus_start_schedule(const Schedule *schedule);
bool pump_bus_preload_initial_dacs(void);
bool pump_bus_arm_schedule(void);
void pump_bus_stop_all(void);

uint8_t pump_bus_get_last_status(void);
uint8_t pump_bus_get_last_detail(void);

#ifdef __cplusplus
}
#endif

#endif /* __BACKPLANE_PUMP_BUS_H__ */
