#ifndef __CARD_REGISTERS_H__
#define __CARD_REGISTERS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define CARD_BUS_SLOT_COUNT              8U
#define CARD_ID_MAGIC                    0x43415244UL

#define CARD_TYPE_NONE                   0x00U
#define CARD_TYPE_PUMP_PERISTALTIC       0x01U
#define CARD_TYPE_FPGA_GPIO_SYNC         0x02U

#define CARD_CAP_PUMP_PERISTALTIC        0x0001U
#define CARD_CAP_FPGA_GPIO_3V3_16        0x0001U
#define CARD_CAP_FPGA_GPIO_5V_16         0x0002U
#define CARD_CAP_FPGA_SYNC_MASTER        0x0004U

#define PUMP_CARD_MAX_LOCAL_EVENTS       48U

#define CARD_REG_ID                      0x0000U
#define CARD_REG_TYPE                    0x0004U
#define CARD_REG_FW_VERSION              0x0008U
#define CARD_REG_STATUS                  0x000CU
#define CARD_REG_ERROR_FLAGS             0x0010U
#define CARD_REG_CONTROL                 0x0014U
#define CARD_REG_CAPABILITIES            0x0018U
#define CARD_REG_MAX_LOCAL_EVENTS        0x001CU

#define CARD_FW_VERSION(major, minor)    ((((uint32_t)(major)) << 16) | ((uint32_t)(minor)))

#define PUMP_CONTROL_ENABLE              0x00000001UL
#define PUMP_CONTROL_DIRECTION           0x00000002UL

#define PUMP_CARD_CONTROL_RUN            0x00000001UL
#define PUMP_CARD_CONTROL_CLEAR_QUEUE    0x00000002UL
#define PUMP_CARD_CONTROL_PRELOAD_DAC    0x00000004UL

#define PUMP_CARD_STATUS_RUNNING         0x00000001UL
#define PUMP_CARD_STATUS_DONE            0x00000002UL
#define PUMP_CARD_STATUS_QUEUE_READY     0x00000004UL

#define PUMP_QUEUE_REG_EVENT_INDEX       0x0200U
#define PUMP_QUEUE_REG_META              0x0204U
#define PUMP_QUEUE_REG_SPEED_MV          0x0208U
#define PUMP_QUEUE_REG_PUSH              0x020CU
#define PUMP_QUEUE_REG_COUNT             0x0210U
#define PUMP_QUEUE_REG_STATUS            0x0214U
#define PUMP_QUEUE_REG_LAST_EVENT_INDEX  0x0218U
#define PUMP_QUEUE_REG_CHECKSUM          0x021CU

#define PUMP_QUEUE_STATUS_FULL           0x00000001UL
#define PUMP_QUEUE_STATUS_PUSH_SEEN      0x00000002UL
#define PUMP_QUEUE_STATUS_RUNNING        0x00000004UL
#define PUMP_QUEUE_STATUS_DONE           0x00000008UL

#define PUMP_QUEUE_META(local_pump, control) \
  ((((uint32_t)(control) & 0xFFU) << 8) | ((uint32_t)(local_pump) & 0xFFU))
#define PUMP_QUEUE_META_LOCAL_PUMP(meta) ((uint8_t)((meta) & 0xFFU))
#define PUMP_QUEUE_META_CONTROL(meta)    ((uint8_t)(((meta) >> 8) & 0xFFU))

#ifdef __cplusplus
}
#endif

#endif /* __CARD_REGISTERS_H__ */
