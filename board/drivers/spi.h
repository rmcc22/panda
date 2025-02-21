#pragma once

#include "crc.h"

#define SPI_TIMEOUT_US 10000U

// got max rate from hitting a non-existent endpoint
// in a tight loop, plus some buffer
#define SPI_IRQ_RATE  16000U

#ifdef STM32H7
#define SPI_BUF_SIZE 2048U
__attribute__((section(".ram_d1"))) uint8_t spi_buf_rx[SPI_BUF_SIZE];
__attribute__((section(".ram_d2"))) uint8_t spi_buf_tx[SPI_BUF_SIZE];
#else
#define SPI_BUF_SIZE 1024U
uint8_t spi_buf_rx[SPI_BUF_SIZE];
uint8_t spi_buf_tx[SPI_BUF_SIZE];
#endif

#define SPI_CHECKSUM_START 0xABU
#define SPI_SYNC_BYTE 0x5AU
#define SPI_HACK 0x79U
#define SPI_DACK 0x85U
#define SPI_NACK 0x1FU

// SPI states
enum {
  SPI_STATE_HEADER,
  SPI_STATE_HEADER_ACK,
  SPI_STATE_HEADER_NACK,
  SPI_STATE_DATA_RX,
  SPI_STATE_DATA_RX_ACK,
  SPI_STATE_DATA_TX
};

bool spi_tx_dma_done = false;
uint8_t spi_state = SPI_STATE_HEADER;
uint8_t spi_endpoint;
uint16_t spi_data_len_mosi;
uint16_t spi_data_len_miso;
uint16_t spi_checksum_error_count = 0;
bool spi_can_tx_ready = false;

#define SPI_HEADER_SIZE 7U

// low level SPI prototypes
void llspi_init(void);
void llspi_mosi_dma(uint8_t *addr, int len);
void llspi_miso_dma(uint8_t *addr, int len);

void can_tx_comms_resume_spi(void) {
  spi_can_tx_ready = true;
}

uint16_t spi_version_packet(uint8_t *out) {
  // this protocol version request is a stable portion of
  // the panda's SPI protocol. its contents match that of the
  // panda USB descriptors and are sufficent to list/enumerate
  // a panda, determine panda type, and bootstub status.

  // the response is:
  // VERSION + 2 byte data length + data + CRC8

  // echo "VERSION"
  (void)memcpy(out, "VERSION", 7);

  // write response
  uint16_t data_len = 0;
  uint16_t data_pos = 7U + 2U;

  // write serial
  #ifdef UID_BASE
  (void)memcpy(&out[data_pos], ((uint8_t *)UID_BASE), 12);
  data_len += 12U;
  #endif

  // HW type
  out[data_pos + data_len] = hw_type;
  data_len += 1U;

  // bootstub
  out[data_pos + data_len] = USB_PID & 0xFFU;
  data_len += 1U;

  // SPI protocol version
  out[data_pos + data_len] = 0x1;
  data_len += 1U;

  // data length
  out[7] = data_len & 0xFFU;
  out[8] = (data_len >> 8) & 0xFFU;

  // CRC8
  uint16_t resp_len = data_pos + data_len;
  out[resp_len] = crc_checksum(out, resp_len, 0xD5U);
  resp_len += 1U;

  return resp_len;
}

void spi_init(void) {
  // platform init
  llspi_init();

  // Start the first packet!
  spi_state = SPI_STATE_HEADER;
  llspi_mosi_dma(spi_buf_rx, SPI_HEADER_SIZE);
}

bool check_checksum(uint8_t *data, uint16_t len) {
  // TODO: can speed this up by casting the bulk to uint32_t and xor-ing the bytes afterwards
  uint8_t checksum = SPI_CHECKSUM_START;
  for(uint16_t i = 0U; i < len; i++){
    checksum ^= data[i];
  }
  return checksum == 0U;
}

void spi_rx_done(void) {
  uint16_t response_len = 0U;
  uint8_t next_rx_state = SPI_STATE_HEADER_NACK;
  bool checksum_valid = false;

  // parse header
  spi_endpoint = spi_buf_rx[1];
  spi_data_len_mosi = (spi_buf_rx[3] << 8) | spi_buf_rx[2];
  spi_data_len_miso = (spi_buf_rx[5] << 8) | spi_buf_rx[4];

  if (memcmp(spi_buf_rx, "VERSION", 7) == 0) {
    response_len = spi_version_packet(spi_buf_tx);
    next_rx_state = SPI_STATE_HEADER_NACK;;
  } else if (spi_state == SPI_STATE_HEADER) {
    checksum_valid = check_checksum(spi_buf_rx, SPI_HEADER_SIZE);
    if ((spi_buf_rx[0] == SPI_SYNC_BYTE) && checksum_valid) {
      // response: ACK and start receiving data portion
      spi_buf_tx[0] = SPI_HACK;
      next_rx_state = SPI_STATE_HEADER_ACK;
      response_len = 1U;
    } else {
      // response: NACK and reset state machine
      print("- incorrect header sync or checksum "); hexdump(spi_buf_rx, SPI_HEADER_SIZE);
      spi_buf_tx[0] = SPI_NACK;
      next_rx_state = SPI_STATE_HEADER_NACK;
      response_len = 1U;
    }
  } else if (spi_state == SPI_STATE_DATA_RX) {
    // We got everything! Based on the endpoint specified, call the appropriate handler
    bool response_ack = false;
    checksum_valid = check_checksum(&(spi_buf_rx[SPI_HEADER_SIZE]), spi_data_len_mosi + 1U);
    if (checksum_valid) {
      if (spi_endpoint == 0U) {
        if (spi_data_len_mosi >= sizeof(ControlPacket_t)) {
          ControlPacket_t ctrl;
          (void)memcpy(&ctrl, &spi_buf_rx[SPI_HEADER_SIZE], sizeof(ControlPacket_t));
          response_len = comms_control_handler(&ctrl, &spi_buf_tx[3]);
          response_ack = true;
        } else {
          print("SPI: insufficient data for control handler\n");
        }
      } else if ((spi_endpoint == 1U) || (spi_endpoint == 0x81U)) {
        if (spi_data_len_mosi == 0U) {
          response_len = comms_can_read(&(spi_buf_tx[3]), spi_data_len_miso);
          response_ack = true;
        } else {
          print("SPI: did not expect data for can_read\n");
        }
      } else if (spi_endpoint == 2U) {
        comms_endpoint2_write(&spi_buf_rx[SPI_HEADER_SIZE], spi_data_len_mosi);
        response_ack = true;
      } else if (spi_endpoint == 3U) {
        if (spi_data_len_mosi > 0U) {
          if (spi_can_tx_ready) {
            spi_can_tx_ready = false;
            comms_can_write(&spi_buf_rx[SPI_HEADER_SIZE], spi_data_len_mosi);
            response_ack = true;
          } else {
            response_ack = false;
            print("SPI: CAN NACK\n");
          }
        } else {
          print("SPI: did expect data for can_write\n");
        }
      } else {
        print("SPI: unexpected endpoint"); puth(spi_endpoint); print("\n");
      }
    } else {
      // Checksum was incorrect
      response_ack = false;
      print("- incorrect data checksum ");
      puth4(spi_data_len_mosi);
      print("\n");
      hexdump(spi_buf_rx, SPI_HEADER_SIZE);
      hexdump(&(spi_buf_rx[SPI_HEADER_SIZE]), MIN(spi_data_len_mosi, 64));
      print("\n");
    }

    if (!response_ack) {
      spi_buf_tx[0] = SPI_NACK;
      next_rx_state = SPI_STATE_HEADER_NACK;
      response_len = 1U;
    } else {
      // Setup response header
      spi_buf_tx[0] = SPI_DACK;
      spi_buf_tx[1] = response_len & 0xFFU;
      spi_buf_tx[2] = (response_len >> 8) & 0xFFU;

      // Add checksum
      uint8_t checksum = SPI_CHECKSUM_START;
      for(uint16_t i = 0U; i < (response_len + 3U); i++) {
        checksum ^= spi_buf_tx[i];
      }
      spi_buf_tx[response_len + 3U] = checksum;
      response_len += 4U;

      next_rx_state = SPI_STATE_DATA_TX;
    }
  } else {
    print("SPI: RX unexpected state: "); puth(spi_state); print("\n");
  }

  // send out response
  if (response_len == 0U) {
    print("SPI: no response\n");
    spi_buf_tx[0] = SPI_NACK;
    spi_state = SPI_STATE_HEADER_NACK;
    response_len = 1U;
  }
  llspi_miso_dma(spi_buf_tx, response_len);

  spi_state = next_rx_state;
  if (!checksum_valid && (spi_checksum_error_count < __UINT16_MAX__)) {
    spi_checksum_error_count += 1U;
  }
}

void spi_tx_done(bool reset) {
  if ((spi_state == SPI_STATE_HEADER_NACK) || reset) {
    // Reset state
    spi_state = SPI_STATE_HEADER;
    llspi_mosi_dma(spi_buf_rx, SPI_HEADER_SIZE);
  } else if (spi_state == SPI_STATE_HEADER_ACK) {
    // ACK was sent, queue up the RX buf for the data + checksum
    spi_state = SPI_STATE_DATA_RX;
    llspi_mosi_dma(&spi_buf_rx[SPI_HEADER_SIZE], spi_data_len_mosi + 1U);
  } else if (spi_state == SPI_STATE_DATA_TX) {
    // Reset state
    spi_state = SPI_STATE_HEADER;
    llspi_mosi_dma(spi_buf_rx, SPI_HEADER_SIZE);
  } else {
    spi_state = SPI_STATE_HEADER;
    llspi_mosi_dma(spi_buf_rx, SPI_HEADER_SIZE);
    print("SPI: TX unexpected state: "); puth(spi_state); print("\n");
  }
}
