/*
 * solar_os_board_storage_sd_pico.c - board storage service on the PicoCalc.
 *
 * Implements src/board/solar_os_board_storage.h against the RP2350 SD-over-SPI
 * driver plus vendored FatFs, replacing src/board/solar_os_board_storage_sd.c
 * (which sits on ESP-IDF's sdspi + esp_vfs_fat).
 *
 * The service contract is generic - init/mount/unmount/format, block
 * enumeration by name, and a default mount point of "/sdcard" - so nothing
 * above this file changes. In particular the shell's on-disk conventions -
 * the .shell, .ssh and .reader directories - are relative paths inside the
 * mounted volume and stay exactly as they were; only the block and filesystem
 * plumbing underneath is different.
 */

#include "solar_os_board_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ff.h"
#include "sd_spi_pico.h"
#include "solar_os_board.h"

static const char *TAG = "storage-sd";

/* FatFs logical drive 0 is the card. */
#define SD_VOLUME_ID "0:"

/* Whole disk plus up to four MBR primary partitions. */
#define SD_MAX_BLOCKS 5

#ifndef SOLAR_OS_BOARD_PIN_SD_DETECT
#define SOLAR_OS_BOARD_PIN_SD_DETECT (-1)
#endif

#ifndef SOLAR_OS_BOARD_SD_TRANSFER_HZ
/*
 * 12.5 MHz. The vendor's reference LCD driver runs its SPI at 25 MHz, but that
 * is a different bus with a short, dedicated trace; the card slot's timing has
 * not been characterised here, and SD-over-SPI is routinely run at half the
 * card's rated speed. This is a conservative starting point, not a measured
 * ceiling - see doc/ports/picocalc.md.
 */
#define SOLAR_OS_BOARD_SD_TRANSFER_HZ 12500000U
#endif

static FATFS sd_fatfs;
static bool storage_initialised;
static bool storage_mounted;
static char storage_mount_point[SOLAR_OS_BOARD_STORAGE_MOUNT_POINT_MAX] =
    SOLAR_OS_BOARD_STORAGE_DEFAULT_MOUNT_POINT;
static solar_os_board_storage_block_t storage_blocks[SD_MAX_BLOCKS];
static size_t storage_block_count;

/*
 * FatFs timestamp hook, called from the diskio glue. SolarOS's time service
 * owns the wall clock; until it is wired in here, files are stamped with the
 * FAT epoch rather than a plausible-looking lie.
 *
 * Format: bits 31:25 year-1980, 24:21 month, 20:16 day, 15:11 hour,
 *         10:5 minute, 4:0 second/2.
 */
DWORD solar_os_board_storage_fattime(void)
{
    /* 1980-01-01 00:00:00. */
    return ((DWORD)0 << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

static esp_err_t translate_fresult(FRESULT result)
{
    switch (result) {
    case FR_OK:                 return ESP_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:            return ESP_ERR_NOT_FOUND;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:  return ESP_ERR_INVALID_ARG;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:    return ESP_ERR_NOT_ALLOWED;
    case FR_NOT_READY:          return ESP_ERR_INVALID_STATE;
    case FR_NO_FILESYSTEM:      return ESP_ERR_NOT_FOUND;
    case FR_TIMEOUT:            return ESP_ERR_TIMEOUT;
    case FR_NOT_ENOUGH_CORE:    return ESP_ERR_NO_MEM;
    case FR_DISK_ERR:
    case FR_INT_ERR:
    default:                    return ESP_FAIL;
    }
}

static const char *fs_type_name(BYTE fs_type)
{
    switch (fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default:       return "";
    }
}

/*
 * Rebuild the block table.
 *
 * FatFs does not expose a partition list, so the MBR is read directly from
 * sector 0. A card with no MBR signature is treated as a whole-disk (superfloppy)
 * filesystem, which is how most factory-formatted SD cards below 32 GB are
 * actually laid out.
 */
static void rescan_blocks(void)
{
    memset(storage_blocks, 0, sizeof(storage_blocks));
    storage_block_count = 0;

    if (!sd_spi_ready()) {
        return;
    }

    sd_spi_status_t status;
    if (sd_spi_get_status(&status) != ESP_OK) {
        return;
    }

    /* Entry 0: the whole disk. */
    solar_os_board_storage_block_t *disk = &storage_blocks[storage_block_count++];
    snprintf(disk->name, sizeof(disk->name), "sd0");
    disk->type = SOLAR_OS_BOARD_STORAGE_BLOCK_DISK;
    disk->sector_size = SD_SPI_SECTOR_SIZE;
    disk->sector_count = status.sector_count;
    disk->size_bytes = (uint64_t)status.sector_count * SD_SPI_SECTOR_SIZE;
    disk->logical_volume = SOLAR_OS_BOARD_STORAGE_LOGICAL_VOLUME_INVALID;
    snprintf(disk->type_name,
             sizeof(disk->type_name),
             "%s",
             status.block_addressed ? "SDHC" : "SDSC");

    uint8_t sector[SD_SPI_SECTOR_SIZE];
    if (sd_spi_read_blocks(0, sector, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not read sector 0; reporting the disk only");
        return;
    }

    const bool has_mbr_signature = sector[510] == 0x55U && sector[511] == 0xAAU;
    if (!has_mbr_signature) {
        /* Superfloppy: the filesystem starts at sector 0. */
        disk->whole_disk_filesystem = true;
        disk->mountable = true;
        disk->mounted = storage_mounted;
        disk->logical_volume = 0;
        if (storage_mounted) {
            snprintf(disk->fs, sizeof(disk->fs), "%s", fs_type_name(sd_fatfs.fs_type));
            snprintf(disk->mount_point, sizeof(disk->mount_point), "%s", storage_mount_point);
        }
        return;
    }

    /* Four 16-byte primary partition entries at offset 0x1BE. */
    for (unsigned i = 0; i < 4U && storage_block_count < SD_MAX_BLOCKS; i++) {
        const uint8_t *entry = &sector[446U + i * 16U];
        const uint8_t mbr_type = entry[4];
        const uint32_t start = (uint32_t)entry[8] |
                               ((uint32_t)entry[9] << 8) |
                               ((uint32_t)entry[10] << 16) |
                               ((uint32_t)entry[11] << 24);
        const uint32_t count = (uint32_t)entry[12] |
                               ((uint32_t)entry[13] << 8) |
                               ((uint32_t)entry[14] << 16) |
                               ((uint32_t)entry[15] << 24);
        if (mbr_type == 0 || count == 0) {
            continue;
        }

        solar_os_board_storage_block_t *part = &storage_blocks[storage_block_count++];
        snprintf(part->name, sizeof(part->name), "sd0p%u", i + 1U);
        part->type = SOLAR_OS_BOARD_STORAGE_BLOCK_PARTITION;
        part->partition_number = (uint8_t)(i + 1U);
        part->mbr_type = mbr_type;
        part->bootable = (entry[0] & 0x80U) != 0;
        part->start_sector = start;
        part->sector_count = count;
        part->sector_size = SD_SPI_SECTOR_SIZE;
        part->size_bytes = (uint64_t)count * SD_SPI_SECTOR_SIZE;
        part->logical_volume = SOLAR_OS_BOARD_STORAGE_LOGICAL_VOLUME_INVALID;

        /* FAT12/16/32 and exFAT partition type bytes. */
        switch (mbr_type) {
        case 0x01: snprintf(part->type_name, sizeof(part->type_name), "FAT12"); break;
        case 0x04:
        case 0x06:
        case 0x0E: snprintf(part->type_name, sizeof(part->type_name), "FAT16"); break;
        case 0x0B:
        case 0x0C: snprintf(part->type_name, sizeof(part->type_name), "FAT32"); break;
        case 0x07: snprintf(part->type_name, sizeof(part->type_name), "exFAT"); break;
        default:   snprintf(part->type_name, sizeof(part->type_name), "0x%02x", mbr_type); break;
        }

        /*
         * FatFs mounts the first partition of logical drive 0 when
         * FF_MULTI_PARTITION is off, which it is. Only partition 1 is
         * therefore reachable; the rest are enumerated for visibility but
         * reported as not mountable rather than silently failing on mount.
         */
        part->mountable = (i == 0U);
        if (part->mountable) {
            part->logical_volume = 0;
            part->mounted = storage_mounted;
            if (storage_mounted) {
                snprintf(part->fs, sizeof(part->fs), "%s", fs_type_name(sd_fatfs.fs_type));
                snprintf(part->mount_point,
                         sizeof(part->mount_point),
                         "%s",
                         storage_mount_point);
            }
        }
    }
}

static esp_err_t bring_up_card(void)
{
    if (sd_spi_ready()) {
        return ESP_OK;
    }

    const sd_spi_config_t config = {
        .spi_index = SOLAR_OS_BOARD_SPI_HOST,
        .sck_pin = SOLAR_OS_BOARD_PIN_SPI_SCLK,
        .mosi_pin = SOLAR_OS_BOARD_PIN_SPI_MOSI,
        .miso_pin = SOLAR_OS_BOARD_PIN_SPI_MISO,
        .cs_pin = SOLAR_OS_BOARD_PIN_SD_CS,
        .detect_pin = SOLAR_OS_BOARD_PIN_SD_DETECT,
        .transfer_hz = SOLAR_OS_BOARD_SD_TRANSFER_HZ,
    };
    return sd_spi_init(&config);
}

static esp_err_t mount_volume(const char *mount_point)
{
    const esp_err_t err = bring_up_card();
    if (err != ESP_OK) {
        return err;
    }
    if (storage_mounted) {
        return ESP_OK;
    }

    /* opt = 1: mount now rather than lazily, so failures surface here. */
    const FRESULT result = f_mount(&sd_fatfs, SD_VOLUME_ID, 1);
    if (result != FR_OK) {
        ESP_LOGE(TAG, "f_mount failed: %d", (int)result);
        return translate_fresult(result);
    }

    storage_mounted = true;
    if (mount_point != NULL && mount_point[0] != '\0') {
        snprintf(storage_mount_point, sizeof(storage_mount_point), "%s", mount_point);
    }
    rescan_blocks();
    ESP_LOGI(TAG, "mounted %s at %s", fs_type_name(sd_fatfs.fs_type), storage_mount_point);
    return ESP_OK;
}

esp_err_t solar_os_board_storage_init(void)
{
    storage_initialised = true;
    const esp_err_t err = bring_up_card();
    if (err != ESP_OK) {
        /* No card is a normal state, not a boot failure. */
        ESP_LOGI(TAG, "no card available at init: %s", esp_err_to_name(err));
        return err;
    }
    rescan_blocks();
    return ESP_OK;
}

bool solar_os_board_storage_available(void)
{
    return storage_initialised && sd_spi_card_present();
}

esp_err_t solar_os_board_storage_mount(void)
{
    return mount_volume(SOLAR_OS_BOARD_STORAGE_DEFAULT_MOUNT_POINT);
}

esp_err_t solar_os_board_storage_mount_volume(const char *name, const char *mount_point)
{
    /*
     * One logical volume is reachable (see the note in rescan_blocks), so a
     * named mount is accepted only for a block that reports itself mountable.
     */
    if (name != NULL && name[0] != '\0') {
        bool known = false;
        for (size_t i = 0; i < storage_block_count; i++) {
            if (strcmp(storage_blocks[i].name, name) == 0) {
                if (!storage_blocks[i].mountable) {
                    return ESP_ERR_NOT_SUPPORTED;
                }
                known = true;
                break;
            }
        }
        if (!known) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    return mount_volume(mount_point);
}

esp_err_t solar_os_board_storage_unmount(void)
{
    if (!storage_mounted) {
        return ESP_OK;
    }
    /* Flush before dropping the mount so buffered writes reach the card. */
    (void)sd_spi_sync();
    const FRESULT result = f_mount(NULL, SD_VOLUME_ID, 0);
    storage_mounted = false;
    rescan_blocks();
    return translate_fresult(result);
}

esp_err_t solar_os_board_storage_unmount_volume(const char *target)
{
    (void)target;
    return solar_os_board_storage_unmount();
}

esp_err_t solar_os_board_storage_format(const char *name)
{
    (void)name;

    const esp_err_t err = bring_up_card();
    if (err != ESP_OK) {
        return err;
    }
    if (storage_mounted) {
        (void)solar_os_board_storage_unmount();
    }

    /*
     * f_mkfs needs a scratch buffer of at least FF_MAX_SS bytes. It is taken
     * from the stack deliberately: a static one would sit in .bss for the
     * lifetime of the firmware to serve an operation the user runs once.
     */
    static const MKFS_PARM options = {
        .fmt = FM_FAT32 | FM_FAT | FM_SFD,
        .n_fat = 1,
        .align = 0,
        .n_root = 0,
        .au_size = 0,
    };
    BYTE work[FF_MAX_SS];
    const FRESULT result = f_mkfs(SD_VOLUME_ID, &options, work, sizeof(work));
    if (result != FR_OK) {
        ESP_LOGE(TAG, "f_mkfs failed: %d", (int)result);
        return translate_fresult(result);
    }

    rescan_blocks();
    ESP_LOGI(TAG, "formatted");
    return ESP_OK;
}

bool solar_os_board_storage_is_mounted(void)
{
    return storage_mounted;
}

void solar_os_board_storage_get_status(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    if (!sd_spi_ready()) {
        snprintf(buffer, len, "no card");
        return;
    }

    sd_spi_status_t status;
    if (sd_spi_get_status(&status) != ESP_OK) {
        snprintf(buffer, len, "unavailable");
        return;
    }

    const unsigned long mib =
        (unsigned long)((uint64_t)status.sector_count * SD_SPI_SECTOR_SIZE / (1024U * 1024U));
    if (storage_mounted) {
        snprintf(buffer,
                 len,
                 "%s %lu MiB, %s at %s, %lu kHz",
                 status.block_addressed ? "SDHC" : "SDSC",
                 mib,
                 fs_type_name(sd_fatfs.fs_type),
                 storage_mount_point,
                 (unsigned long)(status.actual_hz / 1000U));
    } else {
        snprintf(buffer,
                 len,
                 "%s %lu MiB, not mounted",
                 status.block_addressed ? "SDHC" : "SDSC",
                 mib);
    }
}

const char *solar_os_board_storage_mount_point(void)
{
    return storage_mount_point;
}

esp_err_t solar_os_board_storage_rescan(void)
{
    const esp_err_t err = bring_up_card();
    if (err != ESP_OK) {
        return err;
    }
    rescan_blocks();
    return ESP_OK;
}

size_t solar_os_board_storage_block_count(void)
{
    return storage_block_count;
}

bool solar_os_board_storage_get_block(size_t index, solar_os_board_storage_block_t *block)
{
    if (block == NULL || index >= storage_block_count) {
        return false;
    }
    *block = storage_blocks[index];
    return true;
}
