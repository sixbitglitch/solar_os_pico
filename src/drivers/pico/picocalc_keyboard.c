/*
 * picocalc_keyboard.c - PicoCalc STM32 co-processor transport.
 *
 * See picocalc_keyboard.h for the protocol and its primary source.
 *
 * Everything here is a blocking, timeout-bounded I2C exchange. The
 * co-processor is slow (10 kHz bus, and it needs time between the register
 * write and the data read), so callers must be tasks, never ISRs.
 *
 * I2C1 is a SHARED bus (see boards/manifests/picocalc.toml's "i2c1" entry,
 * sharing = "shared"): this same physical link also carries the LCD
 * backlight (picocalc_kbd_set_lcd_backlight, called from the display driver)
 * and the battery fuel gauge (picocalc_kbd_read_battery, called from the
 * battery driver), and generic "i2c"/expansion-device shell commands can
 * reach it too via src/drivers/i2c_bus.c. The peripheral itself (i2c_init,
 * pin muxing) is brought up exactly once by the generic bus layer
 * (solar_os_buses.c, via solar_os_bus_acquire()/i2c_bus_start_config()) -
 * this file only ever calls i2c_bus_lock()/i2c_bus_unlock() around each
 * exchange, the same static mutex src/drivers/i2c_bus.c's own
 * i2c_bus_transmit_handle()/i2c_bus_receive_handle() take internally. That
 * is what keeps a concurrent generic I2C transaction from landing in the
 * middle of this driver's write-then-16ms-sleep-then-read sequence (or vice
 * versa) and getting back the co-processor's reply to the wrong command.
 */

#include "picocalc_keyboard.h"

#include <string.h>

#include "esp_log.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "i2c_bus.h"
#include "pico/stdlib.h"

static const char *TAG = "picocalc-kbd";

/*
 * The vendor's reference host driver sleeps 16 ms between writing the register
 * selector and reading the answer, and polls the keyboard on the same 16 ms
 * cadence (KEY_POLL_TIME in the STM32 firmware). That delay is the
 * co-processor's turnaround time, not a bus constraint.
 *
 * 16 ms per exchange is slow enough that it must not be done with the
 * scheduler blocked. The service layer polls from its own task.
 */
#define PICOCALC_KBD_TURNAROUND_MS 16U

/* Generous relative to a 10 kHz bus: two bytes is ~2 ms of bus time. */
#define PICOCALC_KBD_TIMEOUT_US 500000U

typedef struct {
    i2c_inst_t *i2c;
    uint8_t address;
    bool ready;
} picocalc_kbd_state_s;

static picocalc_kbd_state_s kbd;

/*
 * One register exchange.
 *
 * A "write" still reads two bytes back: the co-processor's receiveEvent()
 * stages a reply for every command, and its requestEvent() will serve stale
 * data to the next reader if nobody consumes it. Draining it here keeps the
 * register machine in step.
 */
static esp_err_t picocalc_kbd_transfer(uint8_t reg,
                                       bool write,
                                       uint8_t write_value,
                                       uint8_t *out_low,
                                       uint8_t *out_high)
{
    if (!kbd.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t command[2];
    size_t command_len = 1;
    command[0] = reg;
    if (write) {
        command[0] |= PICOCALC_KBD_WRITE_MASK;
        command[1] = write_value;
        command_len = 2;
    }

    /*
     * Held across the whole write-sleep-read exchange, not just each half:
     * see the file header comment. i2c_bus_lock()/unlock() take the same
     * mutex src/drivers/i2c_bus.c uses internally, so a generic caller going
     * through that file blocks here rather than interleaving.
     */
    i2c_bus_lock();

    int result = i2c_write_timeout_us(kbd.i2c,
                                      kbd.address,
                                      command,
                                      command_len,
                                      false,
                                      PICOCALC_KBD_TIMEOUT_US);
    if (result < 0 || (size_t)result != command_len) {
        i2c_bus_unlock();
        return ESP_ERR_TIMEOUT;
    }

    sleep_ms(PICOCALC_KBD_TURNAROUND_MS);

    uint8_t response[2] = {0, 0};
    result = i2c_read_timeout_us(kbd.i2c,
                                 kbd.address,
                                 response,
                                 sizeof(response),
                                 false,
                                 PICOCALC_KBD_TIMEOUT_US);
    if (result < 0 || (size_t)result != sizeof(response)) {
        i2c_bus_unlock();
        return ESP_ERR_TIMEOUT;
    }

    i2c_bus_unlock();

    if (out_low != NULL) {
        *out_low = response[0];
    }
    if (out_high != NULL) {
        *out_high = response[1];
    }
    return ESP_OK;
}

static esp_err_t picocalc_kbd_read_reg(uint8_t reg, uint8_t *low, uint8_t *high)
{
    return picocalc_kbd_transfer(reg, false, 0, low, high);
}

static esp_err_t picocalc_kbd_write_reg(uint8_t reg, uint8_t value)
{
    return picocalc_kbd_transfer(reg, true, value, NULL, NULL);
}

esp_err_t picocalc_kbd_init(const picocalc_kbd_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->i2c_index > 1) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * The peripheral itself (i2c_init, pin muxing, the 10 kHz clamp - the
     * vendor documents that as this co-processor's ceiling) is brought up
     * exactly once by the generic bus layer, driven by
     * boards/manifests/picocalc.toml's "i2c1" entry: see
     * solar_os_picocalc_keyboard_attach(), which calls solar_os_bus_acquire()
     * before this function. This file only ever locks and transfers.
     */
    memset(&kbd, 0, sizeof(kbd));
    kbd.i2c = (config->i2c_index == 0) ? i2c0 : i2c1;
    kbd.address = config->address != 0 ? config->address : PICOCALC_KBD_I2C_ADDRESS;
    kbd.ready = true;

    /* Probe: REG_VER answers {0, BIOSVERSION}. A co-processor that is absent
     * or wedged will time out here rather than at the first keystroke. */
    uint8_t version = 0;
    const esp_err_t err = picocalc_kbd_get_version(&version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no response at 0x%02x: %s", kbd.address, esp_err_to_name(err));
        kbd.ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,
             "co-processor at 0x%02x, firmware 0x%02x",
             kbd.address,
             version);
    return ESP_OK;
}

void picocalc_kbd_deinit(void)
{
    /*
     * Marks this driver's own state not-ready only. It must NOT call
     * i2c_deinit(): the peripheral is shared (see the file header comment)
     * and owned by the generic bus layer, not by this file - tearing it down
     * here would take the display backlight and battery gauge down with it.
     * The caller (solar_os_picocalc_keyboard_attach()'s failure paths) is
     * responsible for releasing its own bus lease separately.
     */
    kbd.ready = false;
}

bool picocalc_kbd_ready(void)
{
    return kbd.ready;
}

esp_err_t picocalc_kbd_get_version(uint8_t *version)
{
    uint8_t low = 0;
    uint8_t high = 0;
    const esp_err_t err = picocalc_kbd_read_reg(PICOCALC_KBD_REG_VER, &low, &high);
    if (err != ESP_OK) {
        return err;
    }
    /* REG_VER stages {0, BIOSVERSION}. */
    if (version != NULL) {
        *version = high;
    }
    return ESP_OK;
}

esp_err_t picocalc_kbd_read_event(picocalc_kbd_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t state = 0;
    uint8_t key = 0;
    const esp_err_t err = picocalc_kbd_read_reg(PICOCALC_KBD_REG_FIF, &state, &key);
    if (err != ESP_OK) {
        return err;
    }

    /* REG_FIF stages {item.state, item.key}. An empty FIFO dequeues a zeroed
     * item, which is state == IDLE. */
    event->state = (picocalc_kbd_state_t)state;
    event->key = key;
    return ESP_OK;
}

esp_err_t picocalc_kbd_read_status(uint8_t *pending, bool *caps_lock, bool *num_lock)
{
    uint8_t status = 0;
    const esp_err_t err = picocalc_kbd_read_reg(PICOCALC_KBD_REG_KEY, &status, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (pending != NULL) {
        *pending = status & PICOCALC_KBD_KEY_COUNT_MASK;
    }
    if (caps_lock != NULL) {
        *caps_lock = (status & PICOCALC_KBD_KEY_CAPSLOCK) != 0;
    }
    if (num_lock != NULL) {
        *num_lock = (status & PICOCALC_KBD_KEY_NUMLOCK) != 0;
    }
    return ESP_OK;
}

esp_err_t picocalc_kbd_set_lcd_backlight(uint8_t value)
{
    return picocalc_kbd_write_reg(PICOCALC_KBD_REG_BKL, value);
}

esp_err_t picocalc_kbd_get_lcd_backlight(uint8_t *value)
{
    uint8_t reg = 0;
    uint8_t level = 0;
    const esp_err_t err = picocalc_kbd_read_reg(PICOCALC_KBD_REG_BKL, &reg, &level);
    if (err != ESP_OK) {
        return err;
    }
    if (value != NULL) {
        *value = level;
    }
    return ESP_OK;
}

esp_err_t picocalc_kbd_set_keyboard_backlight(uint8_t value)
{
    return picocalc_kbd_write_reg(PICOCALC_KBD_REG_BK2, value);
}

esp_err_t picocalc_kbd_read_battery(uint8_t *percent, bool *charging)
{
    uint8_t reg = 0;
    uint8_t value = 0;
    const esp_err_t err = picocalc_kbd_read_reg(PICOCALC_KBD_REG_BAT, &reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * sync_bat() in the STM32 firmware builds this byte: the AXP2101's
     * state-of-charge percentage, with bit 7 set while charging. A
     * disconnected battery reports 0.
     */
    if (charging != NULL) {
        *charging = (value & 0x80U) != 0;
    }
    uint8_t level = value & 0x7FU;
    if (level > 100U) {
        level = 100U;
    }
    if (percent != NULL) {
        *percent = level;
    }
    return ESP_OK;
}
