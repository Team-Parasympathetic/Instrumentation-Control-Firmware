#include "pump_card_app.h"

#include "card_registers.h"
#include "pump_card_regs.h"
#include "spi.h"
#include "spi_frame.h"

#include <string.h>

static uint8_t g_rx_frame[SPI_FRAME_CMD_SIZE];
static uint8_t g_tx_frame[SPI_FRAME_RESP_SIZE];

static void pump_card_build_error(uint8_t status)
{
  (void)spi_frame_build_response(SPI_FRAME_ACK_ERR,
                                 status,
                                 0,
                                 0U,
                                 g_tx_frame,
                                 sizeof(g_tx_frame));
}

static void pump_card_build_identify(void)
{
  uint8_t payload[6];

  payload[0] = CARD_TYPE_PUMP_PERISTALTIC;
  payload[1] = PUMP_CARD_FW_MAJOR;
  payload[2] = PUMP_CARD_FW_MINOR;
  spi_frame_write_u16_le(&payload[3], CARD_CAP_PUMP_PERISTALTIC);
  payload[5] = PUMP_CARD_MAX_LOCAL_EVENTS;

  (void)spi_frame_build_response(SPI_FRAME_ACK_OK,
                                 STATUS_OK,
                                 payload,
                                 sizeof(payload),
                                 g_tx_frame,
                                 sizeof(g_tx_frame));
}

static void pump_card_build_read_response(uint32_t value)
{
  uint8_t payload[4];

  spi_frame_write_u32_le(payload, value);
  (void)spi_frame_build_response(SPI_FRAME_ACK_OK,
                                 STATUS_OK,
                                 payload,
                                 sizeof(payload),
                                 g_tx_frame,
                                 sizeof(g_tx_frame));
}

static void pump_card_build_ok(void)
{
  (void)spi_frame_build_response(SPI_FRAME_ACK_OK,
                                 STATUS_OK,
                                 0,
                                 0U,
                                 g_tx_frame,
                                 sizeof(g_tx_frame));
}

static void pump_card_handle_command(const SpiFrameCommand *command)
{
  uint16_t reg_addr;
  uint32_t reg_value;
  uint8_t status;

  switch (command->command_id)
  {
    case CMD_IDENTIFY:
      if (command->payload_len != 0U)
      {
        pump_card_build_error(STATUS_BAD_LEN);
        break;
      }

      pump_card_build_identify();
      break;

    case CMD_READ_REG:
      if (command->payload_len != 2U)
      {
        pump_card_build_error(STATUS_BAD_LEN);
        break;
      }

      reg_addr = spi_frame_read_u16_le(command->payload);
      if (!pump_card_regs_read(reg_addr, &reg_value, &status))
      {
        pump_card_build_error(status);
        break;
      }

      pump_card_build_read_response(reg_value);
      break;

    case CMD_WRITE_REG:
      if (command->payload_len != 6U)
      {
        pump_card_build_error(STATUS_BAD_LEN);
        break;
      }

      reg_addr = spi_frame_read_u16_le(command->payload);
      reg_value = spi_frame_read_u32_le(&command->payload[2]);
      if (!pump_card_regs_write(reg_addr, reg_value, &status))
      {
        pump_card_build_error(status);
        break;
      }

      pump_card_build_ok();
      break;

    default:
      pump_card_build_error(STATUS_BAD_CMD);
      break;
  }
}

void pump_card_app_init(void)
{
  memset(g_rx_frame, 0, sizeof(g_rx_frame));
  memset(g_tx_frame, 0, sizeof(g_tx_frame));
  pump_card_regs_init();
  pump_card_build_error(STATUS_BAD_CMD);
}

void pump_card_app_poll(void)
{
  SpiFrameCommand command;
  uint8_t parse_status;

  if (HAL_SPI_Receive(&hspi1, g_rx_frame, sizeof(g_rx_frame), HAL_MAX_DELAY) != HAL_OK)
  {
    return;
  }

  parse_status = spi_frame_parse_command(g_rx_frame, sizeof(g_rx_frame), &command);
  if (parse_status != STATUS_OK)
  {
    pump_card_build_error(parse_status);
  }
  else
  {
    pump_card_handle_command(&command);
  }

  (void)HAL_SPI_Transmit(&hspi1, g_tx_frame, sizeof(g_tx_frame), HAL_MAX_DELAY);
  pump_card_regs_service_post_response();
}

void pump_card_app_on_sync_edge(void)
{
  pump_card_regs_on_sync_edge();
}
