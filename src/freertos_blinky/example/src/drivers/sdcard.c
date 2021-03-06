#include <string.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include "crc.h"
#include "spi.h"
#include "logging.h"
#include "./sdcard.h"

#include "chip.h"

static xSemaphoreHandle xMutexSDCard;

#ifdef SDCARD_ERROR_LOGGING

typedef struct {
	int command; int response; int argument; int crc1; int crc2;
} SDCardError;

static SDCardError sdcard_errors[SDCARD_ERROR_LOG_SIZE] = {0};
static size_t sdcard_error_count = 0;
static void SDCardLogError(int command, int response, int argument, int crc1, int crc2) {
	if (sdcard_error_count < SDCARD_ERROR_LOG_SIZE) {
		sdcard_errors[sdcard_error_count].command = command;
		sdcard_errors[sdcard_error_count].response = response;
		sdcard_errors[sdcard_error_count].argument = argument;
		sdcard_errors[sdcard_error_count].crc1 = crc1;
		sdcard_errors[sdcard_error_count].crc2 = crc2;
	}
	sdcard_error_count += 1;
}
void SDCardDumpLogs(void) {
	if (sdcard_error_count > 0) {
		int i;
		LOG_ERROR("SDCARD: %d sdcard errors", sdcard_error_count);
		if (sdcard_error_count > SDCARD_ERROR_LOG_SIZE) {
			LOG_ERROR("SDCARD: overflowed by %d", sdcard_error_count - SDCARD_ERROR_LOG_SIZE);
		}

		for (i = 0; i < sdcard_error_count && i < SDCARD_ERROR_LOG_SIZE; i++) {
			LOG_ERROR("SDCARD: cmd %d -> %d (%d); crc %d vs %d", sdcard_errors[i].command, sdcard_errors[i].response, sdcard_errors[i].argument, sdcard_errors[i].crc1, sdcard_errors[i].crc2);
		}
		sdcard_error_count = 0;
	}
}

#endif

static void SDCardSlowMode() {
	spi_set_bit_rate(SDCARD_SPI_DEVICE, 100000);
}

static void SDCardFastMode() {
	spi_set_bit_rate(SDCARD_SPI_DEVICE, 10000000);
}

static inline void SDCardSetSS() {
	Chip_GPIO_SetPinState(LPC_GPIO, SDCARD_SPI_SLAVE_PORT, SDCARD_SPI_SLAVE_PIN, false);
}

static inline void SDCardClearSS() {
	Chip_GPIO_SetPinState(LPC_GPIO, SDCARD_SPI_SLAVE_PORT, SDCARD_SPI_SLAVE_PIN, true);
}

static uint8_t CommandBuffer[20];

static bool sdcard_initialized = false;
static bool sdcard_is_hc = false;
static int sdcard_version = -1;

void SDCardWaitIdle() {
	int i;
	retry:
	memset(CommandBuffer, 0xff, SDCARD_IDLE_WAIT_BYTES);
	spi_transceive(SDCARD_SPI_DEVICE, CommandBuffer, SDCARD_IDLE_WAIT_BYTES);

	for (i = 0; i < SDCARD_IDLE_WAIT_BYTES; i++) {
		if (CommandBuffer[i] != 0xff) {
			goto retry;
		}
	}
}

int SDCardSendCommand(uint8_t command, uint32_t param, uint8_t crc, void* buffer, size_t recvSize) {
	Chip_GPIO_SetPinState(LPC_GPIO, 0, 2, !Chip_GPIO_GetPinState(LPC_GPIO, 0, 2));
	xSemaphoreTake(xMutexSDCard, portMAX_DELAY);
	int result = SDCARD_ERROR_GENERIC;
	int wait = SDCARD_SPI_MAX_WAIT;
	int i;
	uint8_t read = 0;
	uint8_t* data = (uint8_t*) buffer;
	command += 0x40;
	SDCardSetSS();
	while (spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff) != 0xff);

	result = SDCARD_ERROR_TRANSMIT_INTERRUPTED;

	CommandBuffer[0] = command;
	CommandBuffer[1] = param >> 24;
	CommandBuffer[2] = param >> 16;
	CommandBuffer[3] = param >> 8;
	CommandBuffer[4] = param;
	CommandBuffer[5] = (crc_crc7(CommandBuffer, 5) << 1) | 1;

	spi_transceive(SDCARD_SPI_DEVICE, CommandBuffer, 6);

	for (i = 0; i < 6; i++) {
		if (CommandBuffer[i] != 0xff) {
//			MSS_GPIO_set_output(MSS_GPIO_27, 0);
			__asm volatile ("nop");
//			MSS_GPIO_set_output(MSS_GPIO_27, 1);
			goto fail;
		}
	}

	for (;;) {
		read = spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff);
		if (read != 0xff) {
			result = read;
			break;
		}
		if (wait == 0) {
			goto fail;
		}
		-- wait;
	}

	// Read block instructions
	if (command == 0x40 + 17 || command == 0x40 + 18) {
		if (read != 0) {
			goto fail;
		}

		// Wait for Data token
		for (;;) {
			read = spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff);
			if (read != 0xff) {
				result = read;
				break;
			}
		}

		if (result != 0xfe) { // Data token
			result <<= 8;
			goto fail;
		}
		result = 0;
	}

	// Clear array first
	if (recvSize > 0) {
		memset(data, 0xff, recvSize);
		spi_transceive(SDCARD_SPI_DEVICE, data, recvSize);
	}

	if (command == 0x40 + 17 || command == 0x40 + 18) {
		uint16_t crc = 0;
		// Skip crc = 2 bytes
		crc = spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff) << 8;
		crc |= spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff);

		uint16_t checkCRC = crc_crc16(data, recvSize);

		if (crc != checkCRC) {
			result = SDCARD_ERROR_CRC_FAILED;
			goto fail;
		}
	}
	goto finish;
 fail:
	if (result == 0) {
		result = SDCARD_ERROR_GENERIC;
	}
 finish:
	SDCardClearSS();
	xSemaphoreGive(xMutexSDCard);
	return result;

}

int SDCardSendACommand(uint8_t acommand, uint32_t param, uint8_t crc, void* buffer, size_t recvSize) {
	int first_result = SDCardSendCommand(55, 0, 0, NULL, 0);
	if (first_result < 0 || (first_result & SDCARD_R1_ILLEGAL_CMD)) return first_result;
	return SDCardSendCommand(acommand, param, crc, buffer, recvSize);
}

void SDCardInit() {
	SDCardSlowMode();
	Chip_GPIO_SetPinDIROutput(LPC_GPIO, SDCARD_SPI_SLAVE_PORT, SDCARD_SPI_SLAVE_PIN);
	SDCardClearSS();
	xMutexSDCard = xSemaphoreCreateMutex();
}

int SDCardStartup() {
	int result = SDCARD_ERROR_GENERIC;
	int i;
	int counter;
	int sd_version = 2;  // 0: MMC; 1: SD; 2: SDv2
	bool is_sdhc = false;

	SDCardClearSS();
	// Generate 80 pulses on SCLK
	LOG_DEBUG("Sending 80 pulses");
	for (i = 0; i < 10; i++) {
		// Send 80 clock pulses
		spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff);
	}

	// Reset device
	LOG_DEBUG("Resetting SDCard");
	counter = SDCARD_RESET_ATTEMPTS;
	for (;;) {
		int response = SDCardSendCommand(0, 0, 0x95, NULL, 0);
		if (response == 1) {
			break;
		}
		if (response < 0) {
			result = response;
			if (response == SDCARD_ERROR_TRANSMIT_INTERRUPTED) {
				counter --;
				if (counter > 0) {
					continue;
				}
			}
			goto fail;
		}
	}

	LOG_DEBUG("Checking voltage range CMD8, and support for SDHC/SDCv2");
	// Read ?
	{
		uint8_t buffer[4];
		int response;
		response = SDCardSendCommand(8, 0x000001AA, 0xff, &buffer, 4);
		if (response & SDCARD_R1_ILLEGAL_CMD) {
			sd_version = 1;
		} else {
			if (!(buffer[2] == 0x01 && buffer[3] == 0xAA)) {
				LOG_ERROR("SDCard does not support 2.7-3.6V voltage range based on CMD8");
				result = SDCARD_ERROR_INVALID_SDCARD;
				goto fail;
			}
		}
	}

	LOG_DEBUG("Waiting for SDCARD to start");
	// Wait for start. Tested startup time for SDHC is 200ms
	for (i = 0; i < 10000; i++) {
		int response;
		if (sd_version == 2) {
			response = SDCardSendACommand(41, 1<<30, 0xff, NULL, 0);
			if (response & SDCARD_R1_ILLEGAL_CMD) {
				sd_version = 0;
				LOG_DEBUG("CMD41 failed. Assuming MMC.");
			}
		} else if (sd_version == 1) {
			response = SDCardSendACommand(41, 0, 0xff, NULL, 0);
			if (response & SDCARD_R1_ILLEGAL_CMD) {
				sd_version = 0;
				LOG_DEBUG("CMD41 failed. Assuming MMC.");
			}
		} else {
			response = SDCardSendCommand(1, 0, 0xff, NULL, 0);
		}
		if (response == 0) {
			break;
		}
		if (response < 0) {
			result = response;
			goto fail;
		}
	}

	if (i >= 10000) {
		result = SDCARD_ERROR_IDLE_WAIT_TIMEOUT;
		goto fail;
	}

	LOG_DEBUG("Checking voltage range CMD58");
	// Read OCR
	{
		uint8_t buffer[4];
		int response;
		if (((response = SDCardSendCommand(58, 0, 0xff, &buffer, 4)) & SDCARD_R1_ERROR_MASK) != 0) {
			goto fail;
		}
		if ((buffer[1] & 0xC0) == 0) {
			result = SDCARD_ERROR_VOLTAGE_NOT_SUPPORTED;
			goto fail;
		}

		if (sd_version >= 1) {
			LOG_DEBUG("Checking SDHC support");
			if (buffer[0] & 0x40) {
				is_sdhc = true;
				LOG_INFO("Detected SDHC card");
			} else {
				LOG_INFO("Detected regular card");
			}
		}
	}

	// Set block size to 512
	if ((result = SDCardSendCommand(16, 512, 0xff, NULL, 0)) != 0) {
		goto fail;
	}

	// Enable CRC checking
	if ((result = SDCardSendCommand(59, 1, 0xff, NULL, 0)) != 0) {
		goto fail;
	}

	SDCardFastMode();
	result = SDCARD_ERROR_OK;
	sdcard_initialized = true;
	sdcard_is_hc = is_sdhc;
	sdcard_version = sd_version;
	goto finish;
 fail:
	if (result == 0) {
		result = SDCARD_ERROR_GENERIC;
	}
 finish:
	return result;
}

bool SDCardInitialized() {
	return sdcard_initialized;
}

static inline uint32_t sector_address_to_sd_address(uint32_t sector) {
	if (sdcard_is_hc) {
		return sector;
	} else {
		return sector * 512;
	}
}

int SDCardReadSector(uint8_t* buffer, uint32_t sector) {
	int result;
	result = SDCardSendCommand(17, sector_address_to_sd_address(sector), 0, buffer, 512);
	if (result != 0) {
		SDCardLogError(17, result, sector, 0, 0);
	}
	return result;
}

int SDCardDiskRead(uint8_t* buffer, uint32_t sector, size_t count) {
	// Do single sector reads
	int result = 0;
	while (count > 0) {
		result = SDCardReadSector(buffer, sector);
		if (result != 0) return result;
		buffer += 512;
		sector ++;
		count --;
	}
	return result;
}

static int SDCardWriteSectorInternal(const uint8_t* buffer, uint32_t sector) {
	int result;
	result = SDCardSendCommand(24, sector_address_to_sd_address(sector), 0, NULL, 0);
	if (result != 0) {
		SDCardLogError(24, result, sector, 0, 0);
		return result;
	}

	uint16_t sendCRC = crc_crc16(buffer, 512);
	// Send actual data blocks now
	SDCardSetSS();

	spi_transceive_byte(SDCARD_SPI_DEVICE, 0xfe);
	spi_send(SDCARD_SPI_DEVICE, buffer, 512);
	spi_transceive_byte(SDCARD_SPI_DEVICE, sendCRC >> 8);
	spi_transceive_byte(SDCARD_SPI_DEVICE, sendCRC & 0xff);

	result = spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff) & 0x1f;

	while (spi_transceive_byte(SDCARD_SPI_DEVICE, 0xff) == 0);

	if (result == 5) return 0;

	SDCardLogError(240, result, sector, sendCRC, 0);
	return result;
}

int SDCardWriteSector(const uint8_t* buffer, uint32_t sector) {
	int retry_limit = SDCARD_WRITE_CRC_FAIL_RETRY;
	int result;
	while (retry_limit > 0) {
		result = SDCardWriteSectorInternal(buffer, sector);
		if (result == 11) { // CRC error
			continue;
		}
		retry_limit --;
		return result;
	}
	return result;
}

int SDCardDiskWrite(const uint8_t* buffer, uint32_t sector, size_t count) {
	int result = 0;
	while (count > 0) {
		result = SDCardWriteSector(buffer, sector);
		if (result != 0) return result;
		buffer += 512;
		sector ++;
		count --;
	}
	return result;
}
