/*
 * diskio_sd_spi_pico.c - FatFs diskio glue for the RP2350 SD-over-SPI driver.
 *
 * FatFs calls these five functions; everything below is a direct translation
 * onto src/drivers/pico/sd_spi_pico.c. Only physical drive 0 exists (the
 * PicoCalc has one card slot).
 *
 * This replaces ESP-IDF's fatfs component diskio, which routes through
 * sdmmc_host / esp_vfs_fat. Using the same upstream FatFs as ESP-IDF does
 * means the on-disk layout and semantics match, so a card formatted by the
 * ESP32 firmware reads here and vice versa - which matters because the shell's
 * storage conventions (/.shell/*, /.ssh/*, /.reader/positions) are relative
 * paths inside the mounted volume and must stay valid.
 */

/* ff.h must come first: diskio.h uses FatFs's integer typedefs. */
#include "ff.h"
#include "diskio.h"

#include "sd_spi_pico.h"

#define SD_PHYSICAL_DRIVE 0

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != SD_PHYSICAL_DRIVE) {
        return STA_NOINIT;
    }
    if (!sd_spi_ready()) {
        return STA_NOINIT;
    }
    if (!sd_spi_card_present()) {
        return STA_NODISK;
    }
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != SD_PHYSICAL_DRIVE) {
        return STA_NOINIT;
    }
    /*
     * The card is brought up by the board storage layer, which owns the pin
     * configuration from the board profile. FatFs calling disk_initialize is
     * therefore a status query, not a request to re-run the init sequence -
     * re-running it here would drop the clock back to 400 kHz mid-mount.
     */
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != SD_PHYSICAL_DRIVE || buff == NULL) {
        return RES_PARERR;
    }
    if (!sd_spi_ready()) {
        return RES_NOTRDY;
    }
    if (sd_spi_read_blocks((uint32_t)sector, buff, (uint32_t)count) != ESP_OK) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != SD_PHYSICAL_DRIVE || buff == NULL) {
        return RES_PARERR;
    }
    if (!sd_spi_ready()) {
        return RES_NOTRDY;
    }
    if (sd_spi_write_blocks((uint32_t)sector, buff, (uint32_t)count) != ESP_OK) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != SD_PHYSICAL_DRIVE) {
        return RES_PARERR;
    }
    if (!sd_spi_ready()) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        return sd_spi_sync() == ESP_OK ? RES_OK : RES_ERROR;

    case GET_SECTOR_COUNT:
        if (buff == NULL) {
            return RES_PARERR;
        }
        *(LBA_t *)buff = (LBA_t)sd_spi_sector_count();
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff == NULL) {
            return RES_PARERR;
        }
        *(WORD *)buff = (WORD)SD_SPI_SECTOR_SIZE;
        return RES_OK;

    case GET_BLOCK_SIZE:
        /*
         * Erase-block size in sectors, used by f_mkfs to align structures.
         * The real value lives in the SD status register (AU_SIZE), which this
         * driver does not read; 1 means "unknown/no alignment preference",
         * which is safe but gives a slightly less optimal format.
         */
        if (buff == NULL) {
            return RES_PARERR;
        }
        *(DWORD *)buff = 1;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

/*
 * FatFs asks for a timestamp for file modification times. Returning 0 would
 * make every file dateless, so this is wired to the board clock in the storage
 * layer instead; see solar_os_board_storage_sd_pico.c, which installs the
 * value this reads.
 */
DWORD get_fattime(void)
{
    extern DWORD solar_os_board_storage_fattime(void);
    return solar_os_board_storage_fattime();
}
