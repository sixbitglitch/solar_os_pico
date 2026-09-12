/*
 * solar_os_picocalc_keyboard.c - PicoCalc keyboard as a SolarOS input source.
 *
 * Modelled directly on src/services/solar_os_cardkb.c: a polled I2C keyboard
 * expansion driver that opens a keyboard input source and pushes decoded
 * characters into it. That is the same path BLE keyboards feed, so the shell,
 * the editor, the launcher and the TUI widgets need no changes to accept
 * PicoCalc keystrokes.
 *
 * Nothing BLE-related is referenced, stubbed or linked here - the BLE HID and
 * NVS-pairing code simply is not part of this target.
 *
 * What differs from cardkb: the PicoCalc co-processor reports press/hold/
 * release transitions for modifiers as well as for normal keys, so this
 * service tracks modifier state itself and folds it into the character it
 * emits (Ctrl+letter becomes a control code, Shift+arrow becomes the
 * corresponding SOLAR_OS_KEY_SHIFT_* logical key).
 */

#include "solar_os_picocalc_keyboard.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "picocalc_keyboard.h"
#include "solar_os_board.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_task.h"

/*
 * The co-processor's own scan cadence is 16 ms (KEY_POLL_TIME), and each FIFO
 * read costs a ~16 ms turnaround, so polling faster only wastes bus time.
 */
#define PICOCALC_KEYBOARD_POLL_MS 16U
#define PICOCALC_KEYBOARD_TASK_STACK 3072U
#define PICOCALC_KEYBOARD_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

/* Drain at most this many FIFO entries per poll so a stuck-full FIFO cannot
 * starve the rest of the system. The co-processor's FIFO is 31 deep. */
#define PICOCALC_KEYBOARD_DRAIN_MAX 8U

#define MOD_SHIFT (1U << 0)
#define MOD_CTRL  (1U << 1)
#define MOD_ALT   (1U << 2)
#define MOD_SYM   (1U << 3)

static const char *TAG = "picocalc-kbd";

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    uint8_t modifiers;
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    uint32_t keys;
    uint32_t unsupported;
    uint32_t dropped;
    uint32_t bus_errors;
} solar_os_picocalc_keyboard_device_t;

static solar_os_picocalc_keyboard_device_t keyboard_device;
static bool keyboard_link_ready;

bool solar_os_picocalc_keyboard_available(void)
{
    return keyboard_link_ready && picocalc_kbd_ready();
}

/*
 * Translate one co-processor key code plus the current modifier state into a
 * SolarOS logical key.
 *
 * Returns false for codes that have no SolarOS equivalent (the C64 joystick
 * pseudo-keys, SYM combinations, the power button) - those are counted as
 * unsupported rather than delivered as garbage characters.
 */
static bool decode_key(uint8_t raw, uint8_t modifiers, uint8_t *out)
{
    const bool shift = (modifiers & MOD_SHIFT) != 0;
    const bool ctrl = (modifiers & MOD_CTRL) != 0;

    switch (raw) {
    case PICOCALC_KEY_UP:
        *out = ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_UP : SOLAR_OS_KEY_CTRL_UP)
                    : (shift ? SOLAR_OS_KEY_SHIFT_UP : SOLAR_OS_KEY_UP);
        return true;
    case PICOCALC_KEY_DOWN:
        *out = ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_DOWN : SOLAR_OS_KEY_CTRL_DOWN)
                    : (shift ? SOLAR_OS_KEY_SHIFT_DOWN : SOLAR_OS_KEY_DOWN);
        return true;
    case PICOCALC_KEY_LEFT:
        *out = ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_LEFT : SOLAR_OS_KEY_CTRL_LEFT)
                    : (shift ? SOLAR_OS_KEY_SHIFT_LEFT : SOLAR_OS_KEY_LEFT);
        return true;
    case PICOCALC_KEY_RIGHT:
        *out = ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_RIGHT : SOLAR_OS_KEY_CTRL_RIGHT)
                    : (shift ? SOLAR_OS_KEY_SHIFT_RIGHT : SOLAR_OS_KEY_RIGHT);
        return true;
    case PICOCALC_KEY_HOME:
        *out = ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_HOME : SOLAR_OS_KEY_CTRL_HOME)
                    : (shift ? SOLAR_OS_KEY_SHIFT_HOME : SOLAR_OS_KEY_HOME);
        return true;
    case PICOCALC_KEY_END:
        *out = ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_END : SOLAR_OS_KEY_CTRL_END)
                    : (shift ? SOLAR_OS_KEY_SHIFT_END : SOLAR_OS_KEY_END);
        return true;
    case PICOCALC_KEY_PAGE_UP:
        *out = shift ? SOLAR_OS_KEY_SHIFT_PAGE_UP : SOLAR_OS_KEY_PAGE_UP;
        return true;
    case PICOCALC_KEY_PAGE_DOWN:
        *out = shift ? SOLAR_OS_KEY_SHIFT_PAGE_DOWN : SOLAR_OS_KEY_PAGE_DOWN;
        return true;
    case PICOCALC_KEY_DEL:
        *out = SOLAR_OS_KEY_DELETE;
        return true;
    case PICOCALC_KEY_ESC:
        *out = SOLAR_OS_KEY_ESCAPE;
        return true;
    case PICOCALC_KEY_ENTER:
        *out = SOLAR_OS_KEY_ENTER;
        return true;
    case PICOCALC_KEY_BACKSPACE:
        *out = 0x08U;
        return true;
    case PICOCALC_KEY_TAB:
        *out = 0x09U;
        return true;
    case PICOCALC_KEY_F1: *out = SOLAR_OS_KEY_F1; return true;
    case PICOCALC_KEY_F2: *out = SOLAR_OS_KEY_F2; return true;
    case PICOCALC_KEY_F3: *out = SOLAR_OS_KEY_F3; return true;
    case PICOCALC_KEY_F4: *out = SOLAR_OS_KEY_F4; return true;
    case PICOCALC_KEY_F5: *out = SOLAR_OS_KEY_F5; return true;
    case PICOCALC_KEY_F6: *out = SOLAR_OS_KEY_F6; return true;
    case PICOCALC_KEY_F7: *out = SOLAR_OS_KEY_F7; return true;
    case PICOCALC_KEY_F8: *out = SOLAR_OS_KEY_F8; return true;
    case PICOCALC_KEY_F9: *out = SOLAR_OS_KEY_F9; return true;
    case PICOCALC_KEY_F10: *out = SOLAR_OS_KEY_F10; return true;
    default:
        break;
    }

    /*
     * Everything else the co-processor reports below 0x80 is already the
     * ASCII code for the key, with Shift and CapsLock applied on its side
     * (CFG_USE_MODS is set by default in the STM32 firmware).
     */
    if (raw >= 0x20U && raw < 0x7FU) {
        if (ctrl) {
            /* Ctrl+A..Z / Ctrl+a..z -> 0x01..0x1A, the usual terminal mapping. */
            if (raw >= 'a' && raw <= 'z') {
                *out = (uint8_t)(raw - 'a' + 1);
                return true;
            }
            if (raw >= 'A' && raw <= 'Z') {
                *out = (uint8_t)(raw - 'A' + 1);
                return true;
            }
            if (raw == '-') {
                *out = SOLAR_OS_KEY_CTRL_MINUS;
                return true;
            }
            if (raw == '=' || raw == '+') {
                *out = SOLAR_OS_KEY_CTRL_PLUS;
                return true;
            }
        }
        *out = raw;
        return true;
    }

    /* Joystick pseudo-keys, SYM combinations, the power button, and anything
     * else outside the logical-key range. */
    return false;
}

/* Modifier press/release bookkeeping. Returns true if raw was a modifier. */
static bool track_modifier(solar_os_picocalc_keyboard_device_t *device,
                           uint8_t raw,
                           picocalc_kbd_state_t state)
{
    uint8_t bit = 0;
    switch (raw) {
    case PICOCALC_KEY_MOD_SHL:
    case PICOCALC_KEY_MOD_SHR:
        bit = MOD_SHIFT;
        break;
    case PICOCALC_KEY_MOD_CTRL:
        bit = MOD_CTRL;
        break;
    case PICOCALC_KEY_MOD_ALT:
        bit = MOD_ALT;
        break;
    case PICOCALC_KEY_MOD_SYM:
        bit = MOD_SYM;
        break;
    default:
        return false;
    }

    if (state == PICOCALC_KBD_STATE_RELEASED) {
        device->modifiers &= (uint8_t)~bit;
    } else {
        device->modifiers |= bit;
    }
    return true;
}

static void deliver_key(solar_os_picocalc_keyboard_device_t *device, uint8_t raw)
{
    uint8_t key = 0;
    if (!decode_key(raw, device->modifiers, &key)) {
        device->unsupported++;
        return;
    }

    /*
     * Alt is delivered as a prefix byte followed by the key, matching how the
     * rest of SolarOS expects Alt combinations to arrive.
     */
    if ((device->modifiers & MOD_ALT) != 0) {
        if (solar_os_input_write_char(device->input_source,
                                      (char)SOLAR_OS_KEY_ALT_PREFIX) != ESP_OK) {
            device->dropped++;
            return;
        }
    }

    if (solar_os_input_write_char(device->input_source, (char)key) != ESP_OK) {
        device->dropped++;
        return;
    }
    device->keys++;
}

static void picocalc_keyboard_worker(void *arg)
{
    solar_os_picocalc_keyboard_device_t *device = arg;
    bool bus_error_reported = false;

    while (!device->stop_requested) {
        for (unsigned drained = 0; drained < PICOCALC_KEYBOARD_DRAIN_MAX; drained++) {
            picocalc_kbd_event_t event = {0};
            const esp_err_t err = picocalc_kbd_read_event(&event);
            if (err != ESP_OK) {
                device->bus_errors++;
                if (!bus_error_reported) {
                    ESP_LOGW(TAG,
                             "%s read failed on %s: %s",
                             device->name,
                             device->i2c_bus,
                             esp_err_to_name(err));
                    bus_error_reported = true;
                }
                break;
            }

            bus_error_reported = false;
            if (event.state == PICOCALC_KBD_STATE_IDLE || event.key == 0) {
                /* FIFO drained. */
                break;
            }

            if (track_modifier(device, event.key, event.state)) {
                continue;
            }

            /*
             * PRESSED delivers the keystroke; HOLD delivers auto-repeat.
             * RELEASED is dropped: the SolarOS input path is a character
             * stream, not a key-state stream.
             */
            if (event.state == PICOCALC_KBD_STATE_PRESSED ||
                event.state == PICOCALC_KBD_STATE_HOLD) {
                deliver_key(device, event.key);
            }
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PICOCALC_KEYBOARD_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address ||
                binding->value != SOLAR_OS_PICOCALC_KEYBOARD_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(solar_os_picocalc_keyboard_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_picocalc_keyboard_attach(const char *name,
                                            const solar_os_expansion_binding_t *bindings,
                                            size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Exactly one co-processor exists, soldered to the mainboard. Unlike
     * cardkb there is no device table: a second attach is a configuration
     * error, not a second keyboard.
     */
    if (keyboard_device.active) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address),
                        TAG,
                        "invalid bindings");

    /*
     * i2c1 is a shared bus (display backlight and the battery gauge are also
     * on it, plus generic "i2c" expansion commands) - see
     * boards/manifests/picocalc.toml. Taking a lease here is what makes the
     * generic bus layer bring the peripheral up (or confirms it already did,
     * for a board profile where something else got there first) instead of
     * this driver silently calling i2c_init() itself and racing whoever else
     * touches it. The lease is intentionally never released on the success
     * path: exactly one co-processor exists, soldered to the mainboard, for
     * the life of the firmware.
     */
    ESP_RETURN_ON_ERROR(solar_os_bus_acquire(i2c_bus, SOLAR_OS_BUS_PROTOCOL_I2C, name),
                        TAG,
                        "i2c bus lease failed");

    i2c_master_bus_handle_t bus_handle = NULL;
    int port = -1;
    esp_err_t ret = solar_os_bus_i2c_get_handle(i2c_bus, &bus_handle, &port);
    if (ret != ESP_OK) {
        (void)solar_os_bus_release(i2c_bus, SOLAR_OS_BUS_PROTOCOL_I2C, name);
        ESP_RETURN_ON_ERROR(ret, TAG, "i2c bus handle unavailable");
    }

    const picocalc_kbd_config_t config = {
        .i2c_index = (uint8_t)port,
        .address = address,
    };
    ret = picocalc_kbd_init(&config);
    if (ret != ESP_OK) {
        (void)solar_os_bus_release(i2c_bus, SOLAR_OS_BUS_PROTOCOL_I2C, name);
        ESP_RETURN_ON_ERROR(ret, TAG, "co-processor not found");
    }

    memset(&keyboard_device, 0, sizeof(keyboard_device));
    keyboard_device.active = true;
    keyboard_device.address = address;
    strlcpy(keyboard_device.name, name, sizeof(keyboard_device.name));
    strlcpy(keyboard_device.i2c_bus, i2c_bus, sizeof(keyboard_device.i2c_bus));

    const esp_err_t err =
        solar_os_input_keyboard_source_open(keyboard_device.name,
                                            true,
                                            &keyboard_device.input_source);
    if (err != ESP_OK) {
        picocalc_kbd_deinit();
        (void)solar_os_bus_release(i2c_bus, SOLAR_OS_BUS_PROTOCOL_I2C, name);
        clear_device(&keyboard_device);
        return err;
    }

    if (solar_os_task_create_pinned_internal(picocalc_keyboard_worker,
                                             keyboard_device.name,
                                             PICOCALC_KEYBOARD_TASK_STACK,
                                             &keyboard_device,
                                             PICOCALC_KEYBOARD_TASK_PRIORITY,
                                             &keyboard_device.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        picocalc_kbd_deinit();
        (void)solar_os_bus_release(i2c_bus, SOLAR_OS_BUS_PROTOCOL_I2C, name);
        clear_device(&keyboard_device);
        return ESP_ERR_NO_MEM;
    }

    keyboard_link_ready = true;
    ESP_LOGI(TAG, "%s attached on %s address 0x%02x", name, i2c_bus, address);
    return ESP_OK;
}

esp_err_t solar_os_picocalc_keyboard_detach(const char *name)
{
    if (name == NULL || !keyboard_device.active ||
        strcmp(keyboard_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    keyboard_device.stop_requested = true;
    if (keyboard_device.worker_task != NULL) {
        (void)xTaskNotifyGive(keyboard_device.worker_task);
    }
    if (!solar_os_task_wait_done(keyboard_device.worker_task,
                                 &keyboard_device.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "%s detached: %lu keys, %lu unsupported, %lu dropped, %lu bus errors",
             name,
             (unsigned long)keyboard_device.keys,
             (unsigned long)keyboard_device.unsupported,
             (unsigned long)keyboard_device.dropped,
             (unsigned long)keyboard_device.bus_errors);

    /*
     * The display and battery drivers share this I2C link, so the bus is left
     * up; only the polling task and the input source are torn down.
     */
    keyboard_link_ready = false;
    clear_device(&keyboard_device);
    return ESP_OK;
}
