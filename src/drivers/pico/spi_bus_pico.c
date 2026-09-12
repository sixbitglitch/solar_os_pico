/*
 * spi_bus_pico.c - RP2350 backend for the solar_os_buses' generic SPI path
 * (src/drivers/spi_bus.c, the original ESP-IDF-facing file, unmodified) and
 * the "spi"/"io" expansion shell commands built on it.
 *
 * SPI0 is shared (boards/manifests/picocalc.toml's "spi0" bus entry,
 * sharing = "shared"): the SD card (src/drivers/pico/sd_spi_pico.c) already
 * owns that physical peripheral directly via hardware_spi, using the exact
 * same chip-select pin (GPIO17) the manifest also lists for this generic
 * bus. This file implements ESP-IDF's spi_master API (spi_bus_initialize/
 * add_device/remove_device/polling_transmit/free - declared in the compat
 * driver/spi_master.h shim) against hardware_spi, and shares ONE mutex with
 * sd_spi_pico.c (via spi_bus_pico.h's solar_os_pico_spi0_lock()/unlock(),
 * which sd_spi_pico.c's cs_low()/cs_high() take around every SD transfer) so
 * a generic transaction from this file cannot land in the middle of an SD
 * card operation, or vice versa.
 *
 * spi_bus_initialize()/spi_bus_free() do NOT actually bring up or tear down
 * the physical peripheral: whichever of {this file, sd_spi_pico.c} runs
 * first does that (both are safe to call spi_init() from, since pico-sdk's
 * spi_init() is idempotent enough for this port's needs - it just programs
 * the block's control registers), and the bus must never be torn down while
 * the SD card still depends on it. Only SPI0 is implemented: SPI1 carries
 * the display, driven directly by tft_ili9488_picocalc.c, and is not part
 * of this generic path in this port.
 */

#include "driver/spi_master.h"

#include <string.h>

#include "FreeRTOS.h"
#include "esp_err.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "semphr.h"
#include "spi_bus_pico.h"

#define SPI_BUS_PICO_MAX_DEVICES 4U
#define SPI_BUS_PICO_DEFAULT_BAUDRATE 1000000U

struct spi_device_t {
    bool used;
    spi_inst_t *inst;
    int cs_pin;
    uint32_t clock_speed_hz;
    uint8_t mode;
};

static struct spi_device_t spi_devices[SPI_BUS_PICO_MAX_DEVICES];
static SemaphoreHandle_t spi0_mutex;
static bool spi0_hw_initialized;

static esp_err_t spi_bus_pico_ensure_mutex(void)
{
    if (spi0_mutex == NULL) {
        /* Recursive: spi_device_acquire_bus()+spi_device_polling_transmit()
         * is a legal ESP-IDF call sequence (hold the bus across several
         * transactions, each of which also locks internally) and must not
         * deadlock a caller that does both from the same task. */
        spi0_mutex = xSemaphoreCreateRecursiveMutex();
        if (spi0_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void solar_os_pico_spi0_lock(void)
{
    if (spi_bus_pico_ensure_mutex() == ESP_OK) {
        xSemaphoreTakeRecursive(spi0_mutex, portMAX_DELAY);
    }
}

void solar_os_pico_spi0_unlock(void)
{
    if (spi0_mutex != NULL) {
        xSemaphoreGiveRecursive(spi0_mutex);
    }
}

static spi_inst_t *spi_inst_for_host(spi_host_device_t host)
{
    /* Board manifest naming: SOLAR_OS_BOARD_SPI_HOST is a plain integer (0
     * or 1), and the compat spi_host_device_t enum numbers SPI1_HOST=0,
     * SPI2_HOST=1 to match it - see that header's comment. This is RP2350's
     * spi0/spi1, not ESP32's numbering the enumerator names borrow. */
    return host == 0 ? spi0 : spi1;
}

esp_err_t spi_bus_initialize(spi_host_device_t host,
                             const spi_bus_config_t *bus_config,
                             int dma_chan)
{
    (void)dma_chan;
    if (bus_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = spi_bus_pico_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_pico_spi0_lock();
    if (!spi0_hw_initialized) {
        spi_inst_t *inst = spi_inst_for_host(host);
        spi_init(inst, SPI_BUS_PICO_DEFAULT_BAUDRATE);
        gpio_set_function((uint)bus_config->sclk_io_num, GPIO_FUNC_SPI);
        gpio_set_function((uint)bus_config->mosi_io_num, GPIO_FUNC_SPI);
        gpio_set_function((uint)bus_config->miso_io_num, GPIO_FUNC_SPI);
        spi0_hw_initialized = true;
    }
    solar_os_pico_spi0_unlock();
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host)
{
    (void)host;
    /* Deliberately does not call spi_deinit(): see the file header comment -
     * sd_spi_pico.c may still depend on this peripheral staying up. */
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host,
                             const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle)
{
    if (dev_config == NULL || handle == NULL || dev_config->spics_io_num < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct spi_device_t *device = NULL;
    for (size_t i = 0; i < SPI_BUS_PICO_MAX_DEVICES; i++) {
        if (!spi_devices[i].used) {
            device = &spi_devices[i];
            break;
        }
    }
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    device->used = true;
    device->inst = spi_inst_for_host(host);
    device->cs_pin = dev_config->spics_io_num;
    device->clock_speed_hz = dev_config->clock_speed_hz > 0 ?
        (uint32_t)dev_config->clock_speed_hz : SPI_BUS_PICO_DEFAULT_BAUDRATE;
    device->mode = dev_config->mode;

    gpio_init((uint)device->cs_pin);
    gpio_set_dir((uint)device->cs_pin, GPIO_OUT);
    gpio_put((uint)device->cs_pin, 1); /* deselected */

    *handle = device;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    if (handle == NULL || !handle->used) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->used = false;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
                                      spi_transaction_t *transaction)
{
    if (handle == NULL || !handle->used || transaction == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* ESP-IDF's length/rxlength are bit counts; this compat layer only
     * moves whole bytes. */
    if ((transaction->length % 8U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t byte_len = transaction->length / 8U;

    const void *tx = (transaction->flags & SPI_TRANS_USE_TXDATA) ?
        transaction->tx_data : transaction->tx_buffer;
    void *rx = (transaction->flags & SPI_TRANS_USE_RXDATA) ?
        transaction->rx_data : transaction->rx_buffer;

    solar_os_pico_spi0_lock();

    /* CPOL/CPHA from the ESP-IDF "mode" 0-3 encoding, the standard mapping:
     * mode = (CPOL << 1) | CPHA. */
    const spi_cpol_t cpol = (handle->mode & 0x2U) ? SPI_CPOL_1 : SPI_CPOL_0;
    const spi_cpha_t cpha = (handle->mode & 0x1U) ? SPI_CPHA_1 : SPI_CPHA_0;
    spi_set_baudrate(handle->inst, handle->clock_speed_hz);
    spi_set_format(handle->inst, 8, cpol, cpha, SPI_MSB_FIRST);

    gpio_put((uint)handle->cs_pin, 0);

    if (byte_len > 0) {
        if (tx != NULL && rx != NULL) {
            spi_write_read_blocking(handle->inst, tx, rx, byte_len);
        } else if (tx != NULL) {
            spi_write_blocking(handle->inst, tx, byte_len);
        } else if (rx != NULL) {
            spi_read_blocking(handle->inst, 0xFF, rx, byte_len);
        }
    }

    if (!(transaction->flags & SPI_TRANS_CS_KEEP_ACTIVE)) {
        gpio_put((uint)handle->cs_pin, 1);
    }

    solar_os_pico_spi0_unlock();
    return ESP_OK;
}

esp_err_t spi_device_transmit(spi_device_handle_t handle,
                              spi_transaction_t *transaction)
{
    /* No interrupt/DMA-queued path on this target: polling is exact. */
    return spi_device_polling_transmit(handle, transaction);
}

esp_err_t spi_device_acquire_bus(spi_device_handle_t device, int wait)
{
    (void)wait;
    if (device == NULL || !device->used) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_pico_spi0_lock();
    return ESP_OK;
}

void spi_device_release_bus(spi_device_handle_t device)
{
    (void)device;
    solar_os_pico_spi0_unlock();
}
