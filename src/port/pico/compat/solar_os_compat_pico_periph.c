/*
 * solar_os_compat_pico_periph.c - RP2350 backing for the ESP-IDF peripheral
 * and flash-layout shims.
 *
 * Split from solar_os_compat_pico.c to keep the core runtime shims (errors,
 * logging, heap, NVS) separate from the peripheral ones.
 */

#include <string.h>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

static const char *TAG = "compat-periph";

/* ------------------------------------------------------------------ */
/* I2C: ESP-IDF bus/device handles over hardware_i2c                  */
/* ------------------------------------------------------------------ */

/*
 * ESP-IDF models "a bus you add addressed devices to". RP2350's hardware_i2c
 * takes the address per transfer. These tables bridge the two.
 *
 * Limit: per-device scl_speed_hz is recorded but not applied - hardware_i2c
 * sets the rate per peripheral, not per device. With more than one device at
 * different speeds on a bus, all of them run at whatever the last
 * i2c_new_master_bus set. Nothing in the picocalc-core flavour does that; the
 * warning below fires if something starts to.
 */
#define COMPAT_I2C_MAX_BUSES   2U
#define COMPAT_I2C_MAX_DEVICES 8U

struct i2c_master_bus_t {
    bool used;
    i2c_inst_t *inst;
    uint8_t port;
    uint32_t speed_hz;
};

struct i2c_master_dev_t {
    bool used;
    struct i2c_master_bus_t *bus;
    uint16_t address;
    uint32_t speed_hz;
};

static struct i2c_master_bus_t compat_i2c_buses[COMPAT_I2C_MAX_BUSES];
static struct i2c_master_dev_t compat_i2c_devices[COMPAT_I2C_MAX_DEVICES];

/* ESP-IDF timeouts are milliseconds and -1 means "wait forever"; pico-sdk
 * wants microseconds. A generous ceiling stands in for "forever" so a wedged
 * bus cannot hang a task permanently. */
static uint32_t compat_i2c_timeout_us(int timeout_ms)
{
    if (timeout_ms < 0) {
        return 1000000U;
    }
    return (uint32_t)timeout_ms * 1000U;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->i2c_port < 0 || config->i2c_port >= (int)COMPAT_I2C_MAX_BUSES) {
        return ESP_ERR_INVALID_ARG;
    }

    struct i2c_master_bus_t *bus = &compat_i2c_buses[config->i2c_port];
    if (bus->used) {
        /* Already up: hand back the same bus, as ESP-IDF's
         * i2c_master_get_bus_handle would. */
        *out_handle = bus;
        return ESP_OK;
    }

    bus->inst = (config->i2c_port == 0) ? i2c0 : i2c1;
    bus->port = (uint8_t)config->i2c_port;
    /* ESP-IDF carries the rate on the device, not the bus. 100 kHz is the
     * standard-mode default and is what a device config overrides. */
    bus->speed_hz = 100000U;

    i2c_init(bus->inst, bus->speed_hz);
    gpio_set_function((uint)config->sda_io_num, GPIO_FUNC_I2C);
    gpio_set_function((uint)config->scl_io_num, GPIO_FUNC_I2C);
    if (config->flags.enable_internal_pullup) {
        gpio_pull_up((uint)config->sda_io_num);
        gpio_pull_up((uint)config->scl_io_num);
    }

    bus->used = true;
    *out_handle = bus;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle)
{
    if (handle == NULL || !handle->used) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < COMPAT_I2C_MAX_DEVICES; i++) {
        if (compat_i2c_devices[i].used && compat_i2c_devices[i].bus == handle) {
            compat_i2c_devices[i].used = false;
        }
    }
    i2c_deinit(handle->inst);
    handle->used = false;
    return ESP_OK;
}

esp_err_t i2c_master_get_bus_handle(i2c_port_t port, i2c_master_bus_handle_t *out_handle)
{
    if (out_handle == NULL || port < 0 || port >= (int)COMPAT_I2C_MAX_BUSES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!compat_i2c_buses[port].used) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_handle = &compat_i2c_buses[port];
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out_handle)
{
    if (bus == NULL || !bus->used || config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->dev_addr_length != I2C_ADDR_BIT_LEN_7) {
        /* hardware_i2c supports 10-bit addressing, but nothing in this tree
         * uses it and it has not been exercised, so refuse rather than
         * silently truncate the address. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (size_t i = 0; i < COMPAT_I2C_MAX_DEVICES; i++) {
        struct i2c_master_dev_t *device = &compat_i2c_devices[i];
        if (device->used) {
            continue;
        }
        device->used = true;
        device->bus = bus;
        device->address = config->device_address;
        device->speed_hz = config->scl_speed_hz;

        if (config->scl_speed_hz != 0 && config->scl_speed_hz != bus->speed_hz) {
            /* See the note at the top: the rate is per peripheral here. The
             * bus is retuned to this device's rate, which is correct for a
             * single-device bus and wrong for a mixed-speed one. */
            ESP_LOGW(TAG,
                     "i2c%u retuned %lu -> %lu Hz for device 0x%02x; "
                     "per-device speeds are not supported on RP2350",
                     (unsigned)bus->port,
                     (unsigned long)bus->speed_hz,
                     (unsigned long)config->scl_speed_hz,
                     (unsigned)config->device_address);
            bus->speed_hz = i2c_set_baudrate(bus->inst, config->scl_speed_hz);
        }

        *out_handle = device;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    if (device == NULL || !device->used) {
        return ESP_ERR_INVALID_ARG;
    }
    device->used = false;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    if (bus == NULL || !bus->used) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A zero-length write is not portable across I2C controllers, so probe by
     * attempting a one-byte read: a device that is present ACKs its address. */
    uint8_t scratch = 0;
    const int result = i2c_read_timeout_us(bus->inst,
                                           (uint8_t)address,
                                           &scratch,
                                           1,
                                           false,
                                           compat_i2c_timeout_us(timeout_ms));
    return result == 1 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data,
                              size_t length,
                              int timeout_ms)
{
    if (device == NULL || !device->used || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int result = i2c_write_timeout_us(device->bus->inst,
                                            (uint8_t)device->address,
                                            data,
                                            length,
                                            false,
                                            compat_i2c_timeout_us(timeout_ms));
    if (result < 0) {
        return ESP_ERR_TIMEOUT;
    }
    return (size_t)result == length ? ESP_OK : ESP_FAIL;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t device,
                             uint8_t *data,
                             size_t length,
                             int timeout_ms)
{
    if (device == NULL || !device->used || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int result = i2c_read_timeout_us(device->bus->inst,
                                           (uint8_t)device->address,
                                           data,
                                           length,
                                           false,
                                           compat_i2c_timeout_us(timeout_ms));
    if (result < 0) {
        return ESP_ERR_TIMEOUT;
    }
    return (size_t)result == length ? ESP_OK : ESP_FAIL;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *write_data,
                                      size_t write_length,
                                      uint8_t *read_data,
                                      size_t read_length,
                                      int timeout_ms)
{
    if (device == NULL || !device->used || write_data == NULL || read_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* nostop = true on the write so the read issues a repeated START, which is
     * what ESP-IDF's combined transaction does and what register-addressed
     * devices expect. */
    int result = i2c_write_timeout_us(device->bus->inst,
                                      (uint8_t)device->address,
                                      write_data,
                                      write_length,
                                      true,
                                      compat_i2c_timeout_us(timeout_ms));
    if (result < 0 || (size_t)result != write_length) {
        return result < 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    result = i2c_read_timeout_us(device->bus->inst,
                                 (uint8_t)device->address,
                                 read_data,
                                 read_length,
                                 false,
                                 compat_i2c_timeout_us(timeout_ms));
    if (result < 0) {
        return ESP_ERR_TIMEOUT;
    }
    return (size_t)result == read_length ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* LEDC -> hardware_pwm                                               */
/* ------------------------------------------------------------------ */

/*
 * A partial mapping. ESP-IDF addresses PWM by (speed_mode, timer, channel)
 * with the GPIO bound at channel-config time; RP2350 addresses it by GPIO,
 * which determines the slice and channel. The GPIO recorded at config time is
 * therefore the key, and the timer/speed-mode arguments are ignored.
 *
 * Duty resolution is honoured by scaling into the slice's wrap value.
 */
#define COMPAT_LEDC_MAX_CHANNELS LEDC_CHANNEL_MAX

typedef struct {
    bool used;
    uint8_t gpio;
    uint32_t duty;
    uint16_t wrap;
} compat_ledc_channel_t;

static compat_ledc_channel_t compat_ledc_channels[COMPAT_LEDC_MAX_CHANNELS];
static uint32_t compat_ledc_freq_hz = 1000U;
static uint8_t compat_ledc_resolution_bits = 8U;

esp_err_t ledc_timer_config(const ledc_timer_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* There is no separate timer object here; the settings are applied when a
     * channel binds a GPIO. */
    compat_ledc_freq_hz = config->freq_hz != 0 ? config->freq_hz : 1000U;
    compat_ledc_resolution_bits = (uint8_t)config->duty_resolution;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *config)
{
    if (config == NULL || config->channel >= COMPAT_LEDC_MAX_CHANNELS ||
        config->gpio_num < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    compat_ledc_channel_t *channel = &compat_ledc_channels[config->channel];
    channel->used = true;
    channel->gpio = (uint8_t)config->gpio_num;

    gpio_set_function((uint)config->gpio_num, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num((uint)config->gpio_num);

    /*
     * Pick a wrap and clock divider that land on the requested frequency:
     *   f = clk_sys / (divider * (wrap + 1))
     * The wrap is set from the requested duty resolution so that duty values
     * from the caller map 1:1 onto counter levels.
     */
    const uint32_t levels = 1UL << compat_ledc_resolution_bits;
    const uint32_t clk = clock_get_hz(clk_sys);
    float divider = (float)clk / ((float)compat_ledc_freq_hz * (float)levels);
    if (divider < 1.0f) {
        divider = 1.0f;
    }
    if (divider > 255.0f) {
        divider = 255.0f;
    }

    channel->wrap = (uint16_t)(levels - 1U);
    pwm_config pwm = pwm_get_default_config();
    pwm_config_set_clkdiv(&pwm, divider);
    pwm_config_set_wrap(&pwm, channel->wrap);
    pwm_init(slice, &pwm, true);

    channel->duty = config->duty;
    pwm_set_gpio_level((uint)config->gpio_num, (uint16_t)config->duty);
    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty)
{
    (void)mode;
    if (channel >= COMPAT_LEDC_MAX_CHANNELS || !compat_ledc_channels[channel].used) {
        return ESP_ERR_INVALID_ARG;
    }
    compat_ledc_channels[channel].duty = duty;
    return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t channel)
{
    (void)mode;
    if (channel >= COMPAT_LEDC_MAX_CHANNELS || !compat_ledc_channels[channel].used) {
        return ESP_ERR_INVALID_ARG;
    }
    const compat_ledc_channel_t *entry = &compat_ledc_channels[channel];
    uint32_t duty = entry->duty;
    if (duty > entry->wrap) {
        duty = entry->wrap;
    }
    pwm_set_gpio_level(entry->gpio, (uint16_t)duty);
    return ESP_OK;
}

esp_err_t ledc_stop(ledc_mode_t mode, ledc_channel_t channel, uint32_t idle_level)
{
    (void)mode;
    if (channel >= COMPAT_LEDC_MAX_CHANNELS || !compat_ledc_channels[channel].used) {
        return ESP_ERR_INVALID_ARG;
    }
    const compat_ledc_channel_t *entry = &compat_ledc_channels[channel];
    const uint slice = pwm_gpio_to_slice_num(entry->gpio);
    pwm_set_enabled(slice, false);
    /* Hand the pin back to SIO so the idle level actually holds. */
    gpio_set_function(entry->gpio, GPIO_FUNC_SIO);
    gpio_set_dir(entry->gpio, GPIO_OUT);
    gpio_put(entry->gpio, idle_level != 0);
    return ESP_OK;
}

uint32_t ledc_get_duty(ledc_mode_t mode, ledc_channel_t channel)
{
    (void)mode;
    if (channel >= COMPAT_LEDC_MAX_CHANNELS || !compat_ledc_channels[channel].used) {
        return 0;
    }
    return compat_ledc_channels[channel].duty;
}

/* ------------------------------------------------------------------ */
/* Flash / partitions / OTA                                           */
/* ------------------------------------------------------------------ */

esp_flash_t *esp_flash_default_chip;

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    if (out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Build-time constant from the board header; RP2350 cannot query this at
     * runtime the way ESP32 can. */
    *out_size = PICO_FLASH_SIZE_BYTES;
    return ESP_OK;
}

/*
 * There is no partition table on this target - see esp_partition.h. Every
 * lookup finds nothing, and every access refuses. This is deliberately not a
 * silent success: code that thinks it wrote to flash and did not is worse
 * than code that is told it cannot.
 */
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    return NULL;
}

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
                                            esp_partition_subtype_t subtype,
                                            const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    return NULL;
}

const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return NULL;
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return NULL;
}

void esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
    (void)iterator;
}

esp_err_t esp_partition_read(const esp_partition_t *partition,
                             size_t offset,
                             void *dst,
                             size_t size)
{
    (void)partition;
    (void)offset;
    (void)dst;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_partition_write(const esp_partition_t *partition,
                              size_t offset,
                              const void *src,
                              size_t size)
{
    (void)partition;
    (void)offset;
    (void)src;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset,
                                    size_t size)
{
    (void)partition;
    (void)offset;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return NULL;
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return NULL;
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    (void)start_from;
    return NULL;
}

esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition,
                                            esp_app_desc_t *app_desc)
{
    (void)partition;
    (void)app_desc;
    return ESP_ERR_NOT_SUPPORTED;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    /* Enough for `pkg`/`hw` style output to have something truthful to show.
     * The version string is the port's, not an OTA image header. */
    static const esp_app_desc_t description = {
        .magic_word = ESP_APP_DESC_MAGIC_WORD,
        .version = "picocalc-port",
        .project_name = "solar_os",
        .idf_ver = "pico-sdk",
    };
    return &description;
}
