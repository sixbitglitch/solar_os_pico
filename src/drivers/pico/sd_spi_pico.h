/*
 * sd_spi_pico.h - minimal SD/SDHC/SDXC block driver over SPI for RP2350.
 *
 * Written from the SD Physical Layer Simplified Specification's SPI mode
 * chapter, which is the public, well-documented protocol. Deliberately NOT
 * derived from any GPL-licensed Pico SD library.
 *
 * Card bring-up sequence implemented here:
 *   1. >= 74 dummy clocks with CS high      (card needs clocks to wake)
 *   2. CMD0  GO_IDLE_STATE                  (enter SPI mode, expect R1 = 0x01)
 *   3. CMD8  SEND_IF_COND                   (distinguishes v2.00+ from v1.x)
 *   4. ACMD41 SD_SEND_OP_COND, polled       (start init; HCS set for v2 cards)
 *   5. CMD58 READ_OCR                       (v2 only: CCS bit -> block vs byte
 *                                            addressing)
 *   6. CMD16 SET_BLOCKLEN 512               (byte-addressed cards only)
 *   Then raise the clock from the mandatory <= 400 kHz init rate to the
 *   configured transfer rate.
 *
 * Data transfer:
 *   CMD17 READ_SINGLE_BLOCK  / CMD24 WRITE_BLOCK
 *   CMD18 READ_MULTIPLE_BLOCK / CMD25 WRITE_MULTIPLE_BLOCK, terminated by
 *   CMD12 STOP_TRANSMISSION and a stop token respectively.
 *
 * All sizes are in 512-byte sectors, which is what both FatFs and SDHC/SDXC
 * block addressing use.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_SPI_SECTOR_SIZE 512U

typedef enum {
    SD_SPI_CARD_NONE = 0,
    SD_SPI_CARD_SDSC_V1,  /* byte-addressed, <= 2 GB */
    SD_SPI_CARD_SDSC_V2,  /* byte-addressed v2 */
    SD_SPI_CARD_SDHC,     /* block-addressed (SDHC/SDXC) */
} sd_spi_card_type_t;

typedef struct {
    /* 0 or 1: which RP2350 SPI block. The PicoCalc wires the card to spi0. */
    uint8_t spi_index;
    uint8_t sck_pin;
    uint8_t mosi_pin;
    uint8_t miso_pin;
    uint8_t cs_pin;
    /* Card-detect input, active low with a pull-up. -1 if unwired. */
    int detect_pin;
    /* Transfer clock after initialisation. Init always runs at <= 400 kHz
     * because the specification requires it. */
    uint32_t transfer_hz;
} sd_spi_config_t;

typedef struct {
    bool initialised;
    sd_spi_card_type_t type;
    /* True for SDHC/SDXC, where CMD17/18/24/25 take a sector index rather
     * than a byte offset. */
    bool block_addressed;
    uint32_t sector_count;
    uint32_t actual_hz;
    /* Raw 16-byte CSD, kept for capacity reporting and diagnostics. */
    uint8_t csd[16];
} sd_spi_status_t;

esp_err_t sd_spi_init(const sd_spi_config_t *config);
void sd_spi_deinit(void);
bool sd_spi_ready(void);

/* False when a card-detect pin is wired and reports no card. Returns true when
 * no detect pin is configured, since absence cannot then be distinguished. */
bool sd_spi_card_present(void);

esp_err_t sd_spi_get_status(sd_spi_status_t *status);
uint32_t sd_spi_sector_count(void);

esp_err_t sd_spi_read_blocks(uint32_t start_sector, uint8_t *buffer, uint32_t count);
esp_err_t sd_spi_write_blocks(uint32_t start_sector, const uint8_t *buffer, uint32_t count);

/* Blocks until the card leaves the busy state, so callers can be sure a write
 * has actually landed before cutting power. */
esp_err_t sd_spi_sync(void);

#ifdef __cplusplus
}
#endif
