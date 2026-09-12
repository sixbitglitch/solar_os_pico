/*
 * picocalc_keyboard.h - Clockwork PicoCalc STM32F103 co-processor, host side.
 *
 * The PicoCalc's keyboard, LCD backlight, keyboard backlight and battery gauge
 * all hang off one STM32F103R8T6 acting as an I2C slave. This driver speaks
 * that register protocol over pico-sdk's hardware_i2c.
 *
 * PROTOCOL SOURCE: clockworkpi's own STM32 firmware, github.com/clockworkpi/
 * PicoCalc, Code/picocalc_keyboard/ - reg.h (register IDs), keyboard.h (key
 * codes and key_state), conf_app.h (slave address, FIFO depth), and
 * picocalc_keyboard.ino (receiveEvent/requestEvent, the actual wire format).
 * The host-side pin assignments and the 10 kHz bus-speed requirement come from
 * the same repository's Code/picocalc_helloworld/i2ckbd/i2ckbd.h.
 *
 * This is a primary source - the vendor's own firmware - so the register map
 * below is not reverse-engineered or guessed. What has NOT been verified is
 * anything timing-related, because no hardware was available. See
 * doc/ports/picocalc.md.
 *
 * Wire format, for reference:
 *   read  register R: write 1 byte {R}, wait, read 2 bytes
 *   write register R: write 2 bytes {R | 0x80, value}, then read 2 bytes back
 *   The two bytes read back are little-endian in the vendor's host code, i.e.
 *   buffer[0] is the low byte. Their meaning is per-register (see below).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* conf_app.h: SLAVE_ADDRESS 0x1F */
#define PICOCALC_KBD_I2C_ADDRESS 0x1FU

/*
 * i2ckbd.h: "if dual i2c, then the speed of keyboard i2c should be 10khz".
 * This is a hard ceiling from the vendor, not a conservative default - the
 * STM32's software I2C slave cannot keep up above it.
 */
#define PICOCALC_KBD_I2C_SPEED_HZ 10000U

/* reg.h: enum reg_id */
typedef enum {
    PICOCALC_KBD_REG_VER = 0x01,     /* firmware version */
    PICOCALC_KBD_REG_CFG = 0x02,     /* configuration bits */
    PICOCALC_KBD_REG_INT = 0x03,     /* interrupt status */
    PICOCALC_KBD_REG_KEY = 0x04,     /* fifo count + lock state */
    PICOCALC_KBD_REG_BKL = 0x05,     /* LCD backlight */
    PICOCALC_KBD_REG_DEB = 0x06,     /* debounce config */
    PICOCALC_KBD_REG_FRQ = 0x07,     /* poll frequency config */
    PICOCALC_KBD_REG_RST = 0x08,     /* reset the co-processor */
    PICOCALC_KBD_REG_FIF = 0x09,     /* key event FIFO */
    PICOCALC_KBD_REG_BK2 = 0x0A,     /* keyboard backlight */
    PICOCALC_KBD_REG_BAT = 0x0B,     /* battery */
    PICOCALC_KBD_REG_C64_MTX = 0x0C, /* C64 matrix passthrough (unused) */
    PICOCALC_KBD_REG_C64_JS = 0x0D,  /* C64 joystick bits (unused) */
    PICOCALC_KBD_REG_OFF = 0x0E,     /* power off */
} picocalc_kbd_reg_t;

/* reg.h: WRITE_MASK */
#define PICOCALC_KBD_WRITE_MASK 0x80U

/* reg.h: REG_ID_KEY bit layout */
#define PICOCALC_KBD_KEY_COUNT_MASK 0x1FU
#define PICOCALC_KBD_KEY_CAPSLOCK   (1U << 5)
#define PICOCALC_KBD_KEY_NUMLOCK    (1U << 6)

/* keyboard.h: enum key_state */
typedef enum {
    PICOCALC_KBD_STATE_IDLE = 0,
    PICOCALC_KBD_STATE_PRESSED = 1,
    PICOCALC_KBD_STATE_HOLD = 2,
    PICOCALC_KBD_STATE_RELEASED = 3,
} picocalc_kbd_state_t;

/* keyboard.h: raw key codes reported in the FIFO's high byte. */
#define PICOCALC_KEY_JOY_UP      0x01U
#define PICOCALC_KEY_JOY_DOWN    0x02U
#define PICOCALC_KEY_JOY_LEFT    0x03U
#define PICOCALC_KEY_JOY_RIGHT   0x04U
#define PICOCALC_KEY_JOY_CENTER  0x05U
#define PICOCALC_KEY_BTN_LEFT1   0x06U
#define PICOCALC_KEY_BTN_RIGHT1  0x07U
#define PICOCALC_KEY_BACKSPACE   0x08U
#define PICOCALC_KEY_TAB         0x09U
#define PICOCALC_KEY_ENTER       0x0AU
#define PICOCALC_KEY_BTN_LEFT2   0x11U
#define PICOCALC_KEY_BTN_RIGHT2  0x12U
#define PICOCALC_KEY_MOD_ALT     0xA1U
#define PICOCALC_KEY_MOD_SHL     0xA2U
#define PICOCALC_KEY_MOD_SHR     0xA3U
#define PICOCALC_KEY_MOD_SYM     0xA4U
#define PICOCALC_KEY_MOD_CTRL    0xA5U
#define PICOCALC_KEY_ESC         0xB1U
#define PICOCALC_KEY_LEFT        0xB4U
#define PICOCALC_KEY_UP          0xB5U
#define PICOCALC_KEY_DOWN        0xB6U
#define PICOCALC_KEY_RIGHT       0xB7U
#define PICOCALC_KEY_BREAK       0xD0U
#define PICOCALC_KEY_INSERT      0xD1U
#define PICOCALC_KEY_HOME        0xD2U
#define PICOCALC_KEY_DEL         0xD4U
#define PICOCALC_KEY_END         0xD5U
#define PICOCALC_KEY_PAGE_UP     0xD6U
#define PICOCALC_KEY_PAGE_DOWN   0xD7U
#define PICOCALC_KEY_CAPS_LOCK   0xC1U
#define PICOCALC_KEY_F1          0x81U
#define PICOCALC_KEY_F2          0x82U
#define PICOCALC_KEY_F3          0x83U
#define PICOCALC_KEY_F4          0x84U
#define PICOCALC_KEY_F5          0x85U
#define PICOCALC_KEY_F6          0x86U
#define PICOCALC_KEY_F7          0x87U
#define PICOCALC_KEY_F8          0x88U
#define PICOCALC_KEY_F9          0x89U
#define PICOCALC_KEY_F10         0x90U
#define PICOCALC_KEY_POWER       0x91U

typedef struct {
    /*
     * 0 or 1: which RP2350 I2C block. The PicoCalc wires the co-processor to
     * i2c1 on GPIO6/7. This is the only hardware detail picocalc_kbd_init()
     * still needs directly - pin muxing, pull-ups and the bus clock are the
     * generic bus layer's job (solar_os_bus_acquire(), driven by
     * boards/manifests/picocalc.toml), since this is a shared bus. See the
     * .c file's header comment.
     */
    uint8_t i2c_index;
    uint8_t address;
} picocalc_kbd_config_t;

typedef struct {
    picocalc_kbd_state_t state;
    uint8_t key; /* raw PICOCALC_KEY_* / ASCII code */
} picocalc_kbd_event_t;

/* Brings up the I2C block and probes the co-processor by reading REG_VER.
 * Returns ESP_ERR_NOT_FOUND if the device does not answer. */
esp_err_t picocalc_kbd_init(const picocalc_kbd_config_t *config);
void picocalc_kbd_deinit(void);
bool picocalc_kbd_ready(void);

/* Firmware version byte from REG_VER (BIOSVERSION in the STM32 firmware). */
esp_err_t picocalc_kbd_get_version(uint8_t *version);

/*
 * Pops one event from the co-processor's 31-deep FIFO.
 *
 * Returns ESP_OK with event->state == PICOCALC_KBD_STATE_IDLE when the FIFO is
 * empty - that is the co-processor's own "nothing to report" encoding (it
 * returns a zeroed item), not an error.
 */
esp_err_t picocalc_kbd_read_event(picocalc_kbd_event_t *event);

/* REG_KEY: pending FIFO depth plus the two lock LEDs. Any output may be NULL. */
esp_err_t picocalc_kbd_read_status(uint8_t *pending, bool *caps_lock, bool *num_lock);

/*
 * LCD backlight, 0..255. The PicoCalc's LCD backlight is not wired to a Pico
 * GPIO at all - it is a co-processor register - which is why the display
 * driver's brightness op routes here.
 */
esp_err_t picocalc_kbd_set_lcd_backlight(uint8_t value);
esp_err_t picocalc_kbd_get_lcd_backlight(uint8_t *value);
esp_err_t picocalc_kbd_set_keyboard_backlight(uint8_t value);

/*
 * REG_BAT. The co-processor reports the AXP2101 PMU's state-of-charge as a
 * PERCENTAGE (0..100) with bit 7 set while charging - not a voltage.
 * percent may be NULL; charging may be NULL.
 */
esp_err_t picocalc_kbd_read_battery(uint8_t *percent, bool *charging);

#ifdef __cplusplus
}
#endif
