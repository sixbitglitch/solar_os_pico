/*
 * sd_spi_pico.c - SD card over SPI on RP2350. See sd_spi_pico.h for the
 * protocol reference and the bring-up sequence.
 *
 * Written against the SD Physical Layer Simplified Specification (SPI mode).
 * No GPL-licensed SD library was consulted or copied.
 *
 * Never run on hardware. The logic follows the specification, but SD cards are
 * notoriously variable in how long they take to leave busy and how they
 * respond to marginal clocking, and none of those paths have been exercised.
 */

#include "sd_spi_pico.h"

#include <string.h>

#include "esp_log.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "spi_bus_pico.h"

static const char *TAG = "sd-spi";

/* --- Commands ------------------------------------------------------------ */
#define CMD0_GO_IDLE_STATE        0
#define CMD8_SEND_IF_COND         8
#define CMD9_SEND_CSD             9
#define CMD12_STOP_TRANSMISSION  12
#define CMD16_SET_BLOCKLEN       16
#define CMD17_READ_SINGLE_BLOCK  17
#define CMD18_READ_MULTIPLE      18
#define CMD24_WRITE_BLOCK        24
#define CMD25_WRITE_MULTIPLE     25
#define CMD55_APP_CMD            55
#define CMD58_READ_OCR           58
#define ACMD41_SD_SEND_OP_COND   41

/* --- R1 response bits ---------------------------------------------------- */
#define R1_IDLE_STATE      0x01U
#define R1_ILLEGAL_COMMAND 0x04U
#define R1_READY           0x00U
/* Any response with bit 7 clear is a valid R1; 0xFF means "no response". */
#define R1_VALID(r) (((r) & 0x80U) == 0U)

/* --- Data tokens --------------------------------------------------------- */
#define TOKEN_DATA_START       0xFEU /* single block read/write, multi read */
#define TOKEN_WRITE_MULTI      0xFCU /* multi-block write data */
#define TOKEN_STOP_TRAN        0xFDU /* multi-block write terminator */
#define DATA_RESPONSE_MASK     0x1FU
#define DATA_RESPONSE_ACCEPTED 0x05U

/*
 * Timeouts. The specification gives 100 ms for the read data token and 250 ms
 * for a write to complete, but real cards exceed the nominal figures, so these
 * are deliberately generous - a slow card should be slow, not absent.
 */
#define SD_INIT_TIMEOUT_MS      2000U
#define SD_TOKEN_TIMEOUT_MS      500U
#define SD_BUSY_TIMEOUT_MS      1000U

/* The specification mandates 100-400 kHz until initialisation completes. */
#define SD_INIT_CLOCK_HZ 400000U

typedef struct {
    spi_inst_t *spi;
    sd_spi_config_t config;
    sd_spi_status_t status;
} sd_spi_state_t;

static sd_spi_state_t sd;

/* --- Low-level SPI helpers ----------------------------------------------- */

/*
 * SPI0 is shared with the generic solar_os_buses "spi" path (see
 * spi_bus_pico.c's file header - same bus, same GPIO17 chip-select the board
 * manifest names for both). cs_low()/cs_high() bracket every SD transfer in
 * this file, so taking the shared lock here - and only here - covers every
 * real transaction without needing it at each call site individually.
 */
static void cs_low(void)
{
    solar_os_pico_spi0_lock();
    /*
     * Restore this card's own clock and format every time, not just at
     * init: a generic "spi" command running on the shared bus in between
     * two SD transfers reprograms the peripheral for its own transaction
     * (see spi_bus_pico.c) and has no way to know what to put back
     * afterward.
     */
    spi_set_format(sd.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    if (sd.config.transfer_hz != 0) {
        spi_set_baudrate(sd.spi, sd.config.transfer_hz);
    }
    gpio_put(sd.config.cs_pin, 0);
}

static void cs_high(void)
{
    gpio_put(sd.config.cs_pin, 1);
    /* One idle byte after deselect: the card needs a clock edge to release
     * DO, and without this a following transaction can see a stale bit. */
    uint8_t ff = 0xFF;
    spi_write_blocking(sd.spi, &ff, 1);
    solar_os_pico_spi0_unlock();
}

static uint8_t xfer(uint8_t value)
{
    uint8_t rx = 0xFF;
    spi_write_read_blocking(sd.spi, &value, &rx, 1);
    return rx;
}

static void skip_bytes(size_t count)
{
    for (size_t i = 0; i < count; i++) {
        (void)xfer(0xFF);
    }
}

/*
 * CRC7 for the command frame. Only CMD0 and CMD8 strictly require a correct
 * CRC (CRC checking is off by default in SPI mode after those), but computing
 * it always is cheap and avoids a class of bug where a card with CRC enabled
 * rejects everything.
 */
static uint8_t crc7(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if ((byte & 0x80U) ^ (crc & 0x80U)) {
                crc ^= 0x09U;
            }
            byte <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1U);
}

/* Wait for the card to stop holding DO low (busy). */
static esp_err_t wait_not_busy(uint32_t timeout_ms)
{
    const absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    do {
        if (xfer(0xFF) == 0xFF) {
            return ESP_OK;
        }
    } while (!time_reached(deadline));
    return ESP_ERR_TIMEOUT;
}

/* Issue a command and return its R1 response. CS must already be asserted. */
static uint8_t send_command(uint8_t command, uint32_t argument)
{
    /* A command must not be sent while the card is busy. */
    if (command != CMD12_STOP_TRANSMISSION) {
        (void)wait_not_busy(SD_BUSY_TIMEOUT_MS);
    }

    uint8_t frame[6];
    frame[0] = (uint8_t)(0x40U | command);
    frame[1] = (uint8_t)(argument >> 24);
    frame[2] = (uint8_t)(argument >> 16);
    frame[3] = (uint8_t)(argument >> 8);
    frame[4] = (uint8_t)argument;
    frame[5] = crc7(frame, 5);

    for (size_t i = 0; i < sizeof(frame); i++) {
        (void)xfer(frame[i]);
    }

    /* CMD12's first response byte is a stuff byte that must be discarded. */
    if (command == CMD12_STOP_TRANSMISSION) {
        (void)xfer(0xFF);
    }

    /* R1 arrives within 8 bytes (NCR). */
    for (int i = 0; i < 10; i++) {
        const uint8_t response = xfer(0xFF);
        if (R1_VALID(response)) {
            return response;
        }
    }
    return 0xFF;
}

/* ACMD<n> is CMD55 followed by CMD<n>. */
static uint8_t send_app_command(uint8_t command, uint32_t argument)
{
    const uint8_t r1 = send_command(CMD55_APP_CMD, 0);
    if (!R1_VALID(r1)) {
        return r1;
    }
    return send_command(command, argument);
}

/* Wait for a data-start token (0xFE) or a read-error token. */
static esp_err_t wait_data_token(uint8_t expected)
{
    const absolute_time_t deadline = make_timeout_time_ms(SD_TOKEN_TIMEOUT_MS);
    do {
        const uint8_t token = xfer(0xFF);
        if (token == expected) {
            return ESP_OK;
        }
        /* 0x00-0x0F with bit 7..4 clear is a data-error token. */
        if (token != 0xFF && (token & 0xF0U) == 0x00U) {
            ESP_LOGE(TAG, "data error token 0x%02x", token);
            return ESP_FAIL;
        }
    } while (!time_reached(deadline));
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_data_block(uint8_t *buffer, size_t length)
{
    const esp_err_t err = wait_data_token(TOKEN_DATA_START);
    if (err != ESP_OK) {
        return err;
    }
    /* spi_read_blocking drives 0xFF on MOSI while clocking data in. */
    spi_read_blocking(sd.spi, 0xFF, buffer, length);
    /* Discard the 16-bit CRC; CRC checking is disabled in SPI mode. */
    skip_bytes(2);
    return ESP_OK;
}

static esp_err_t write_data_block(const uint8_t *buffer, size_t length, uint8_t token)
{
    (void)xfer(token);
    spi_write_blocking(sd.spi, buffer, length);
    /* Dummy CRC. */
    (void)xfer(0xFF);
    (void)xfer(0xFF);

    const uint8_t response = xfer(0xFF);
    if ((response & DATA_RESPONSE_MASK) != DATA_RESPONSE_ACCEPTED) {
        ESP_LOGE(TAG, "write rejected, data response 0x%02x", response);
        return ESP_FAIL;
    }
    /* The card now goes busy while it programs the block. */
    return wait_not_busy(SD_BUSY_TIMEOUT_MS);
}

/* --- Capacity ------------------------------------------------------------ */

/*
 * Decode the sector count from the CSD.
 *
 * CSD v2 (SDHC/SDXC) stores C_SIZE in bits 69:48, and capacity is
 * (C_SIZE + 1) * 512 KB, i.e. (C_SIZE + 1) * 1024 sectors.
 *
 * CSD v1 (SDSC) stores C_SIZE (bits 73:62), C_SIZE_MULT (bits 49:47) and
 * READ_BL_LEN (bits 83:80), and capacity is
 * (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN bytes.
 */
static uint32_t sector_count_from_csd(const uint8_t *csd)
{
    const uint8_t version = (uint8_t)(csd[0] >> 6);

    if (version == 1) {
        const uint32_t c_size =
            ((uint32_t)(csd[7] & 0x3FU) << 16) |
            ((uint32_t)csd[8] << 8) |
            (uint32_t)csd[9];
        return (c_size + 1U) * 1024U;
    }

    const uint32_t c_size =
        ((uint32_t)(csd[6] & 0x03U) << 10) |
        ((uint32_t)csd[7] << 2) |
        ((uint32_t)(csd[8] & 0xC0U) >> 6);
    const uint8_t c_size_mult = (uint8_t)(((csd[9] & 0x03U) << 1) | (csd[10] >> 7));
    const uint8_t read_bl_len = (uint8_t)(csd[5] & 0x0FU);

    const uint64_t bytes =
        (uint64_t)(c_size + 1U) * (1ULL << (c_size_mult + 2U)) * (1ULL << read_bl_len);
    return (uint32_t)(bytes / SD_SPI_SECTOR_SIZE);
}

static esp_err_t read_csd(uint8_t *csd)
{
    cs_low();
    const uint8_t r1 = send_command(CMD9_SEND_CSD, 0);
    if (r1 != R1_READY) {
        cs_high();
        return ESP_FAIL;
    }
    const esp_err_t err = read_data_block(csd, 16);
    cs_high();
    return err;
}

/* --- Initialisation ------------------------------------------------------ */

static esp_err_t sd_spi_bring_up_card(void)
{
    /*
     * Step 1: at least 74 clocks with CS high and MOSI high so the card can
     * wake and synchronise. 10 bytes = 80 clocks.
     */
    gpio_put(sd.config.cs_pin, 1);
    skip_bytes(10);

    /* Step 2: CMD0 -> idle state. Retried: some cards miss the first attempt. */
    bool idle = false;
    for (int attempt = 0; attempt < 10 && !idle; attempt++) {
        cs_low();
        const uint8_t r1 = send_command(CMD0_GO_IDLE_STATE, 0);
        cs_high();
        if (r1 == R1_IDLE_STATE) {
            idle = true;
        } else {
            sleep_ms(10);
        }
    }
    if (!idle) {
        ESP_LOGE(TAG, "CMD0 failed: no card, or wiring/clock problem");
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Step 3: CMD8 with the check pattern 0x1AA and voltage range 2.7-3.6 V.
     * An ILLEGAL_COMMAND response means a v1.x card; a valid R7 echoing the
     * pattern means v2.00+.
     */
    bool version2 = false;
    cs_low();
    uint8_t r1 = send_command(CMD8_SEND_IF_COND, 0x000001AAU);
    if (r1 == R1_IDLE_STATE) {
        uint8_t r7[4];
        for (size_t i = 0; i < sizeof(r7); i++) {
            r7[i] = xfer(0xFF);
        }
        cs_high();
        if (r7[2] != 0x01U || r7[3] != 0xAAU) {
            ESP_LOGE(TAG, "CMD8 check pattern mismatch (%02x %02x)", r7[2], r7[3]);
            return ESP_ERR_NOT_SUPPORTED;
        }
        version2 = true;
    } else {
        cs_high();
        if ((r1 & R1_ILLEGAL_COMMAND) == 0) {
            ESP_LOGE(TAG, "CMD8 failed: R1 0x%02x", r1);
            return ESP_FAIL;
        }
        /* v1.x card: fall through with version2 false. */
    }

    /*
     * Step 4: poll ACMD41 until the card leaves idle. HCS (bit 30) is set only
     * for v2 cards; setting it on a v1 card is a protocol violation.
     */
    const uint32_t acmd41_arg = version2 ? (1UL << 30) : 0UL;
    const absolute_time_t deadline = make_timeout_time_ms(SD_INIT_TIMEOUT_MS);
    bool ready = false;
    do {
        cs_low();
        r1 = send_app_command(ACMD41_SD_SEND_OP_COND, acmd41_arg);
        cs_high();
        if (r1 == R1_READY) {
            ready = true;
            break;
        }
        sleep_ms(10);
    } while (!time_reached(deadline));

    if (!ready) {
        ESP_LOGE(TAG, "ACMD41 timed out; card never left idle");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * Step 5: for v2 cards, CMD58's OCR carries CCS (bit 30). CCS set means
     * block addressing (SDHC/SDXC); clear means byte addressing.
     */
    sd.status.block_addressed = false;
    if (version2) {
        cs_low();
        r1 = send_command(CMD58_READ_OCR, 0);
        if (r1 != R1_READY) {
            cs_high();
            ESP_LOGE(TAG, "CMD58 failed: R1 0x%02x", r1);
            return ESP_FAIL;
        }
        uint8_t ocr[4];
        for (size_t i = 0; i < sizeof(ocr); i++) {
            ocr[i] = xfer(0xFF);
        }
        cs_high();
        sd.status.block_addressed = (ocr[0] & 0x40U) != 0;
        sd.status.type = sd.status.block_addressed ? SD_SPI_CARD_SDHC
                                                   : SD_SPI_CARD_SDSC_V2;
    } else {
        sd.status.type = SD_SPI_CARD_SDSC_V1;
    }

    /*
     * Step 6: byte-addressed cards need an explicit 512-byte block length.
     * Block-addressed cards have a fixed 512-byte block and reject CMD16.
     */
    if (!sd.status.block_addressed) {
        cs_low();
        r1 = send_command(CMD16_SET_BLOCKLEN, SD_SPI_SECTOR_SIZE);
        cs_high();
        if (r1 != R1_READY) {
            ESP_LOGE(TAG, "CMD16 failed: R1 0x%02x", r1);
            return ESP_FAIL;
        }
    }

    /* Initialisation done: the mandatory slow clock no longer applies. */
    sd.status.actual_hz = spi_set_baudrate(sd.spi, sd.config.transfer_hz);

    if (read_csd(sd.status.csd) != ESP_OK) {
        ESP_LOGE(TAG, "CMD9 (SEND_CSD) failed");
        return ESP_FAIL;
    }
    sd.status.sector_count = sector_count_from_csd(sd.status.csd);

    return ESP_OK;
}

esp_err_t sd_spi_init(const sd_spi_config_t *config)
{
    if (config == NULL || config->spi_index > 1) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&sd, 0, sizeof(sd));
    sd.config = *config;
    sd.spi = (config->spi_index == 0) ? spi0 : spi1;
    if (sd.config.transfer_hz == 0) {
        sd.config.transfer_hz = 12500000U;
    }

    /* Card detect first: probing an empty slot just wastes 2 s of timeouts. */
    if (sd.config.detect_pin >= 0) {
        gpio_init((uint)sd.config.detect_pin);
        gpio_set_dir((uint)sd.config.detect_pin, GPIO_IN);
        gpio_pull_up((uint)sd.config.detect_pin);
        /* Let the pull-up settle before the first read. */
        sleep_ms(1);
        if (!sd_spi_card_present()) {
            ESP_LOGI(TAG, "no card in slot");
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Init must run at <= 400 kHz per the specification. */
    spi_init(sd.spi, SD_INIT_CLOCK_HZ);
    spi_set_format(sd.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(sd.config.sck_pin, GPIO_FUNC_SPI);
    gpio_set_function(sd.config.mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(sd.config.miso_pin, GPIO_FUNC_SPI);

    /* CS is driven by software, not the SPI block's own chip select, because
     * a multi-byte SD transaction must hold CS across several transfers. */
    gpio_init(sd.config.cs_pin);
    gpio_set_dir(sd.config.cs_pin, GPIO_OUT);
    gpio_put(sd.config.cs_pin, 1);

    const esp_err_t err = sd_spi_bring_up_card();
    if (err != ESP_OK) {
        spi_deinit(sd.spi);
        return err;
    }

    sd.status.initialised = true;
    ESP_LOGI(TAG,
             "card ready: %s, %lu sectors (%lu MiB), %lu Hz",
             sd.status.block_addressed ? "SDHC/SDXC" : "SDSC",
             (unsigned long)sd.status.sector_count,
             (unsigned long)((uint64_t)sd.status.sector_count * SD_SPI_SECTOR_SIZE /
                             (1024U * 1024U)),
             (unsigned long)sd.status.actual_hz);
    return ESP_OK;
}

void sd_spi_deinit(void)
{
    if (!sd.status.initialised) {
        return;
    }
    spi_deinit(sd.spi);
    memset(&sd.status, 0, sizeof(sd.status));
}

bool sd_spi_ready(void)
{
    return sd.status.initialised;
}

bool sd_spi_card_present(void)
{
    if (sd.config.detect_pin < 0) {
        /* No detect line wired: cannot tell, so do not claim absence. */
        return true;
    }
    /* Active low with a pull-up: low means a card is seated. */
    return !gpio_get((uint)sd.config.detect_pin);
}

esp_err_t sd_spi_get_status(sd_spi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = sd.status;
    return ESP_OK;
}

uint32_t sd_spi_sector_count(void)
{
    return sd.status.sector_count;
}

/* Cards addressed by byte need the sector index scaled. */
static uint32_t address_for(uint32_t sector)
{
    return sd.status.block_addressed ? sector : sector * SD_SPI_SECTOR_SIZE;
}

esp_err_t sd_spi_read_blocks(uint32_t start_sector, uint8_t *buffer, uint32_t count)
{
    if (buffer == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd.status.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sd.status.sector_count != 0 &&
        (start_sector > sd.status.sector_count ||
         count > sd.status.sector_count - start_sector)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ESP_OK;
    cs_low();

    if (count == 1) {
        if (send_command(CMD17_READ_SINGLE_BLOCK, address_for(start_sector)) != R1_READY) {
            err = ESP_FAIL;
        } else {
            err = read_data_block(buffer, SD_SPI_SECTOR_SIZE);
        }
    } else {
        if (send_command(CMD18_READ_MULTIPLE, address_for(start_sector)) != R1_READY) {
            err = ESP_FAIL;
        } else {
            for (uint32_t i = 0; i < count; i++) {
                err = read_data_block(buffer + (size_t)i * SD_SPI_SECTOR_SIZE,
                                      SD_SPI_SECTOR_SIZE);
                if (err != ESP_OK) {
                    break;
                }
            }
            /* CMD12 must be sent even when a block read failed, otherwise the
             * card stays in the multi-block streaming state. */
            (void)send_command(CMD12_STOP_TRANSMISSION, 0);
        }
    }

    cs_high();
    return err;
}

esp_err_t sd_spi_write_blocks(uint32_t start_sector, const uint8_t *buffer, uint32_t count)
{
    if (buffer == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd.status.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sd.status.sector_count != 0 &&
        (start_sector > sd.status.sector_count ||
         count > sd.status.sector_count - start_sector)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ESP_OK;
    cs_low();

    if (count == 1) {
        if (send_command(CMD24_WRITE_BLOCK, address_for(start_sector)) != R1_READY) {
            err = ESP_FAIL;
        } else {
            err = write_data_block(buffer, SD_SPI_SECTOR_SIZE, TOKEN_DATA_START);
        }
    } else {
        if (send_command(CMD25_WRITE_MULTIPLE, address_for(start_sector)) != R1_READY) {
            err = ESP_FAIL;
        } else {
            for (uint32_t i = 0; i < count; i++) {
                err = write_data_block(buffer + (size_t)i * SD_SPI_SECTOR_SIZE,
                                       SD_SPI_SECTOR_SIZE,
                                       TOKEN_WRITE_MULTI);
                if (err != ESP_OK) {
                    break;
                }
            }
            /* Stop token closes the multi-block write; the card then goes busy
             * one final time. */
            (void)xfer(TOKEN_STOP_TRAN);
            const esp_err_t busy = wait_not_busy(SD_BUSY_TIMEOUT_MS);
            if (err == ESP_OK) {
                err = busy;
            }
        }
    }

    cs_high();
    return err;
}

esp_err_t sd_spi_sync(void)
{
    if (!sd.status.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    cs_low();
    const esp_err_t err = wait_not_busy(SD_BUSY_TIMEOUT_MS);
    cs_high();
    return err;
}
