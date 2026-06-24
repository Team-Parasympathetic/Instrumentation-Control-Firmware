#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * Frame format:
 * [SOF][VERSION][MSG_TYPE][SEQ_L][SEQ_H][LEN_L][LEN_H][PAYLOAD...][CRC_L][CRC_H]
 *
 * CRC-16/CCITT-FALSE is calculated over VERSION through the end of PAYLOAD.
 */
#define PROTOCOL_SOF                 0xA5U
#define PROTOCOL_VERSION             0x01U
#define PROTOCOL_MAX_PAYLOAD         256U
#define PROTOCOL_FRAME_OVERHEAD      9U
#define PROTOCOL_MAX_FRAME_SIZE      (PROTOCOL_MAX_PAYLOAD + PROTOCOL_FRAME_OVERHEAD)

typedef enum
{
  MSG_PING = 0x01U,
  MSG_ACK = 0x02U,
  MSG_ERROR = 0x03U,
  MSG_CLEAR_SCHEDULE = 0x10U,
  MSG_UPLOAD_EVENT = 0x11U,
  MSG_START_SCHEDULE = 0x12U,
  MSG_STOP_SCHEDULE = 0x13U,
  MSG_GET_STATUS = 0x14U,
  MSG_GET_CARD_INVENTORY = 0x15U,
  MSG_PREPARE_SCHEDULE = 0x16U,
  MSG_GET_PREPARE_STATUS = 0x17U
} ProtocolMsgType;

typedef enum
{
  ACK_OK = 0x00U
} ProtocolAckStatus;

typedef enum
{
  ERR_BAD_CRC = 0x01U,
  ERR_BAD_VERSION = 0x02U,
  ERR_BAD_LENGTH = 0x03U,
  ERR_UNKNOWN_MSG = 0x04U,
  ERR_SCHEDULE_FULL = 0x05U,
  ERR_BAD_EVENT = 0x06U,
  ERR_BAD_MODULE = 0x07U,
  ERR_BAD_ACTION = 0x08U,
  ERR_BUSY_RUNNING = 0x09U
} ProtocolErrorCode;

typedef struct
{
  uint8_t version;
  uint8_t msg_type;
  uint16_t seq;
  uint16_t payload_len;
  uint8_t payload[PROTOCOL_MAX_PAYLOAD];
} ProtocolPacket;

typedef void (*ProtocolPacketHandler)(const ProtocolPacket *packet, void *context);
typedef void (*ProtocolParserErrorHandler)(uint16_t seq, uint8_t error_code, uint8_t detail, void *context);
typedef uint8_t (*ProtocolTransmitFn)(const uint8_t *data, uint16_t len, void *context);

typedef struct
{
  uint8_t state;
  uint16_t payload_index;
  uint16_t crc_calculated;
  uint16_t crc_received;
  ProtocolPacket current_packet;
  ProtocolPacketHandler packet_handler;
  ProtocolParserErrorHandler error_handler;
  void *context;
} ProtocolParser;

uint16_t protocol_crc16_ccitt_false(const uint8_t *data, uint16_t len);

uint16_t read_u16_le(const uint8_t *data);
uint32_t read_u32_le(const uint8_t *data);
uint64_t read_u64_le(const uint8_t *data);
int32_t read_i32_le(const uint8_t *data);

void write_u16_le(uint8_t *dst, uint16_t value);
void write_u32_le(uint8_t *dst, uint32_t value);
void write_u64_le(uint8_t *dst, uint64_t value);

void protocol_parser_init(ProtocolParser *parser,
                          ProtocolPacketHandler packet_handler,
                          ProtocolParserErrorHandler error_handler,
                          void *context);
void protocol_parser_process(ProtocolParser *parser, const uint8_t *data, uint32_t len);

uint16_t protocol_encode_frame(uint8_t msg_type,
                               uint16_t seq,
                               const uint8_t *payload,
                               uint16_t payload_len,
                               uint8_t *frame_out,
                               uint16_t frame_out_size);

uint8_t protocol_send_frame(ProtocolTransmitFn tx_fn,
                            void *tx_context,
                            uint8_t msg_type,
                            uint16_t seq,
                            const uint8_t *payload,
                            uint16_t payload_len);

uint8_t protocol_send_ack(ProtocolTransmitFn tx_fn,
                          void *tx_context,
                          uint16_t ack_seq,
                          uint8_t status);

uint8_t protocol_send_error(ProtocolTransmitFn tx_fn,
                            void *tx_context,
                            uint16_t failed_seq,
                            uint8_t error_code,
                            uint8_t detail);

#ifdef __cplusplus
}
#endif

#endif /* __PROTOCOL_H__ */
