/*
 * driver/spi_master.h - pico-sdk compatibility shim.
 *
 * SolarOS's SPI plumbing goes through src/services/solar_os_buses.c and the
 * board SPI driver, not through ESP-IDF's spi_master API directly; the shared
 * sources only need the host enum and an opaque device handle to name things.
 * The actual transfers on this target are done by
 * src/drivers/pico/spi_bus_pico.c against pico-sdk's hardware_spi.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RP2350 has two SPI blocks. Named with the ESP-IDF spellings so shared
 * sources that print or switch on a host identifier keep compiling. */
typedef enum {
    SPI1_HOST = 0,
    SPI2_HOST = 1,
    SPI3_HOST = 2,
    SPI_HOST_MAX = 3,
} spi_host_device_t;

typedef struct spi_device_t *spi_device_handle_t;

#define SPI_DMA_CH_AUTO 3
#define SPI_DMA_DISABLED 0

#define SPI_TRANS_USE_TXDATA (1 << 0)
#define SPI_TRANS_USE_RXDATA (1 << 1)
#define SPI_TRANS_CS_KEEP_ACTIVE (1 << 4)

#define SPI_DEVICE_HALFDUPLEX (1 << 2)
#define SPI_DEVICE_3WIRE (1 << 3)
#define SPI_DEVICE_NO_DUMMY (1 << 4)

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
    uint32_t flags;
    int intr_flags;
} spi_bus_config_t;

typedef struct spi_transaction_t spi_transaction_t;

typedef void (*transaction_cb_t)(spi_transaction_t *trans);

typedef struct {
    uint8_t command_bits;
    uint8_t address_bits;
    uint8_t dummy_bits;
    uint8_t mode;
    uint16_t duty_cycle_pos;
    uint16_t cs_ena_pretrans;
    uint8_t cs_ena_posttrans;
    int clock_speed_hz;
    int input_delay_ns;
    int spics_io_num;
    uint32_t flags;
    int queue_size;
    transaction_cb_t pre_cb;
    transaction_cb_t post_cb;
} spi_device_interface_config_t;

struct spi_transaction_t {
    uint32_t flags;
    uint16_t cmd;
    uint64_t addr;
    /* Bit counts, not byte counts - ESP-IDF's convention, preserved so the
     * shared sources need no arithmetic changes. */
    size_t length;
    size_t rxlength;
    void *user;
    union {
        const void *tx_buffer;
        uint8_t tx_data[4];
    };
    union {
        void *rx_buffer;
        uint8_t rx_data[4];
    };
};

esp_err_t spi_bus_initialize(spi_host_device_t host,
                             const spi_bus_config_t *bus_config,
                             int dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host);
esp_err_t spi_bus_add_device(spi_host_device_t host,
                             const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
                                      spi_transaction_t *transaction);
esp_err_t spi_device_transmit(spi_device_handle_t handle,
                              spi_transaction_t *transaction);
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, int wait);
void spi_device_release_bus(spi_device_handle_t device);

#ifdef __cplusplus
}
#endif
