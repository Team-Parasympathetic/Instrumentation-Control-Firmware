#ifndef __PUMP_CARD_REGS_H__
#define __PUMP_CARD_REGS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

void pump_card_regs_init(void);
bool pump_card_regs_read(uint16_t reg_addr, uint32_t *value_out, uint8_t *status_out);
bool pump_card_regs_write(uint16_t reg_addr, uint32_t value, uint8_t *status_out);
void pump_card_regs_service_post_response(void);
void pump_card_regs_service_deferred_outputs(void);
void pump_card_regs_on_sync_edge(void);

#ifdef __cplusplus
}
#endif

#endif /* __PUMP_CARD_REGS_H__ */
