/**
 * DUT Driver
 *
 * Exposes a line-based command interface on the Flipper's second USB serial
 * port (CDC channel 1) for driving a device under test:
 *
 *   set <pin> <0|1>    drive a GPIO pin low/high
 *   get <pin>          read the current level of a GPIO pin
 *   servo <angle>      move the servo on PA7 (pin 2) to 0..180 degrees
 *
 * Send "help" for the full command list.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb_cdc.h>
#include <furi_hal_pwm.h>
#include <gui/gui.h>
#include <cli/cli_vcp.h>
#include <stm32wbxx_ll_tim.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "DutDriver"

#define DUT_CDC_CHANNEL 1
#define DUT_LINE_MAX    64
#define DUT_TX_TIMEOUT  100

// Standard hobby servo timing: 50 Hz frame, 0.5–2.5 ms pulse for 0–180°.
// Some servos expect 1.0–2.0 ms instead; adjust these if yours does.
#define SERVO_PWM       FuriHalPwmOutputIdTim1PA7
#define SERVO_PERIOD_US 20000
#define SERVO_MIN_US    500
#define SERVO_MAX_US    2500
#define SERVO_MAX_ANGLE 180

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRx = (1 << 1),
} WorkerEvt;

typedef enum {
    PinModeUnused,
    PinModeInput,
    PinModeOutput,
} PinMode;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;

    FuriThread* worker;
    FuriSemaphore* tx_sem;
    FuriHalUsbInterface* usb_prev;
    bool usb_ok;
    bool connected;

    char line[DUT_LINE_MAX];
    size_t line_len;
    bool line_overflow;

    PinMode* pin_mode; // indexed like gpio_pins[]

    bool servo_on;
    int servo_angle;
    bool otg_enabled_by_us;

    char last_cmd[32];
    char last_resp[32];
} DutDriver;

/* ---------------------------------------------------------------- serial -- */

static void dut_cdc_tx_cb(void* context) {
    DutDriver* app = context;
    furi_semaphore_release(app->tx_sem);
}

static void dut_cdc_rx_cb(void* context) {
    DutDriver* app = context;
    furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerEvtRx);
}

static void dut_cdc_state_cb(void* context, CdcState state) {
    DutDriver* app = context;
    app->connected = (state == CdcStateConnected);
    // A disconnect can swallow the TX-complete callback; make sure we can send again
    if(app->connected) furi_semaphore_release(app->tx_sem);
    view_port_update(app->view_port);
}

static void dut_cdc_ctrl_line_cb(void* context, CdcCtrlLine ctrl_lines) {
    UNUSED(context);
    UNUSED(ctrl_lines);
}

static void dut_cdc_config_cb(void* context, struct usb_cdc_line_coding* config) {
    UNUSED(context);
    UNUSED(config);
}

static CdcCallbacks dut_cdc_callbacks = {
    .tx_ep_callback = dut_cdc_tx_cb,
    .rx_ep_callback = dut_cdc_rx_cb,
    .state_callback = dut_cdc_state_cb,
    .ctrl_line_callback = dut_cdc_ctrl_line_cb,
    .config_callback = dut_cdc_config_cb,
};

static void dut_send(DutDriver* app, const char* data) {
    size_t len = strlen(data);
    while(len > 0) {
        // Stay below the packet size so every packet is short and no ZLP is needed
        size_t chunk = MIN(len, (size_t)(CDC_DATA_SZ - 1));
        if(furi_semaphore_acquire(app->tx_sem, DUT_TX_TIMEOUT) != FuriStatusOk) {
            FURI_LOG_W(TAG, "TX timeout, dropping output");
            return;
        }
        furi_hal_cdc_send(DUT_CDC_CHANNEL, (uint8_t*)data, chunk);
        data += chunk;
        len -= chunk;
    }
}

static void dut_reply(DutDriver* app, const char* fmt, ...) {
    char buf[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strlcpy(app->last_resp, buf, sizeof(app->last_resp));
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);

    strlcat(buf, "\r\n", sizeof(buf));
    dut_send(app, buf);
}

/* ------------------------------------------------------------------ gpio -- */

static const GpioPinRecord* dut_find_pin(const char* token) {
    if(!token) return NULL;

    const GpioPinRecord* record = NULL;
    char* end;
    long number = strtol(token, &end, 10);
    if(*end == '\0' && end != token) {
        if(number > 0 && number < 256) record = furi_hal_resources_pin_by_number(number);
    } else {
        char name[8];
        size_t i = 0;
        for(; token[i] && i < sizeof(name) - 1; i++)
            name[i] = toupper((unsigned char)token[i]);
        name[i] = '\0';
        record = furi_hal_resources_pin_by_name(name);
    }

    // Don't let the host clobber the SWD debug pins
    if(record && record->debug) return NULL;
    return record;
}

static size_t dut_pin_index(const GpioPinRecord* record) {
    return record - gpio_pins;
}

static bool dut_pin_is_servo(DutDriver* app, const GpioPinRecord* record) {
    return app->servo_on && record->pin == &gpio_ext_pa7;
}

static bool dut_parse_level(const char* token, bool* level) {
    if(!token) return false;
    if(!strcmp(token, "1") || !strcasecmp(token, "high") || !strcasecmp(token, "on")) {
        *level = true;
        return true;
    }
    if(!strcmp(token, "0") || !strcasecmp(token, "low") || !strcasecmp(token, "off")) {
        *level = false;
        return true;
    }
    return false;
}

static void dut_cmd_set(DutDriver* app, char* pin_token, char* level_token) {
    const GpioPinRecord* record = dut_find_pin(pin_token);
    bool level;
    if(!record) {
        dut_reply(app, "ERR unknown pin '%s'", pin_token ? pin_token : "");
        return;
    }
    if(!dut_parse_level(level_token, &level)) {
        dut_reply(app, "ERR level must be 0/1/low/high");
        return;
    }
    if(dut_pin_is_servo(app, record)) {
        dut_reply(app, "ERR %s in use by servo", record->name);
        return;
    }

    // Latch the level before switching to output so the pin doesn't glitch
    furi_hal_gpio_write(record->pin, level);
    if(app->pin_mode[dut_pin_index(record)] != PinModeOutput) {
        furi_hal_gpio_init(record->pin, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
        app->pin_mode[dut_pin_index(record)] = PinModeOutput;
    }
    dut_reply(app, "OK %s=%d", record->name, level);
}

static void dut_cmd_get(DutDriver* app, char* pin_token) {
    const GpioPinRecord* record = dut_find_pin(pin_token);
    if(!record) {
        dut_reply(app, "ERR unknown pin '%s'", pin_token ? pin_token : "");
        return;
    }
    if(dut_pin_is_servo(app, record)) {
        dut_reply(app, "ERR %s in use by servo", record->name);
        return;
    }

    // Outputs are read back as-is; anything else becomes a floating input
    if(app->pin_mode[dut_pin_index(record)] == PinModeUnused) {
        furi_hal_gpio_init(record->pin, GpioModeInput, GpioPullNo, GpioSpeedLow);
        app->pin_mode[dut_pin_index(record)] = PinModeInput;
    }
    bool level = furi_hal_gpio_read(record->pin);
    dut_reply(
        app,
        "OK %s=%d %s",
        record->name,
        level,
        app->pin_mode[dut_pin_index(record)] == PinModeOutput ? "out" : "in");
}

static void dut_release_pins(DutDriver* app) {
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(app->pin_mode[i] == PinModeUnused) continue;
        furi_hal_gpio_init_simple(gpio_pins[i].pin, GpioModeAnalog);
        app->pin_mode[i] = PinModeUnused;
    }
}

/* ----------------------------------------------------------------- servo -- */

static void dut_servo_write(DutDriver* app, int angle) {
    uint32_t pulse_us = SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * angle / SERVO_MAX_ANGLE;

    if(!app->servo_on) {
        // Servos are normally powered from pin 1; turn on 5V if nobody else has
        if(!furi_hal_power_is_otg_enabled() && furi_hal_power_enable_otg()) {
            app->otg_enabled_by_us = true;
        }

        // PA7 was possibly a GPIO before; the PWM driver takes it over
        const GpioPinRecord* record = furi_hal_resources_pin_by_name("PA7");
        if(record) app->pin_mode[dut_pin_index(record)] = PinModeUnused;

        // Let the HAL set up TIM1 and the pin, then retime it with a 1 µs tick
        // so the pulse width can be set precisely (the HAL only does whole %)
        furi_hal_pwm_start(SERVO_PWM, 50, 7);
        LL_TIM_SetPrescaler(TIM1, SystemCoreClock / 1000000 - 1);
        LL_TIM_SetAutoReload(TIM1, SERVO_PERIOD_US - 1);
        LL_TIM_OC_SetCompareCH1(TIM1, pulse_us);
        LL_TIM_GenerateEvent_UPDATE(TIM1);
        app->servo_on = true;
    } else {
        LL_TIM_OC_SetCompareCH1(TIM1, pulse_us);
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->servo_angle = angle;
    furi_mutex_release(app->mutex);
}

static void dut_servo_stop(DutDriver* app) {
    if(app->servo_on) {
        furi_hal_pwm_stop(SERVO_PWM);
        app->servo_on = false;
    }
    if(app->otg_enabled_by_us) {
        furi_hal_power_disable_otg();
        app->otg_enabled_by_us = false;
    }
}

static void dut_cmd_servo(DutDriver* app, char* arg) {
    if(!arg) {
        if(app->servo_on)
            dut_reply(app, "OK servo=%d", app->servo_angle);
        else
            dut_reply(app, "OK servo=off");
        return;
    }
    if(!strcasecmp(arg, "off")) {
        dut_servo_stop(app);
        dut_reply(app, "OK servo=off");
        return;
    }

    char* end;
    long value = strtol(arg, &end, 10);
    if(*end != '\0' || end == arg) {
        dut_reply(app, "ERR angle must be a number");
        return;
    }

    // "+90" / "-90" rotate relative to the current position
    long angle = value;
    if(arg[0] == '+' || arg[0] == '-') {
        angle = (app->servo_on ? app->servo_angle : 0) + value;
    }
    if(angle < 0 || angle > SERVO_MAX_ANGLE) {
        dut_reply(app, "ERR angle out of range 0-%d", SERVO_MAX_ANGLE);
        return;
    }

    dut_servo_write(app, angle);
    dut_reply(app, "OK servo=%ld", angle);
}

/* -------------------------------------------------------------- commands -- */

static void dut_cmd_help(DutDriver* app) {
    dut_send(
        app,
        "Commands:\r\n"
        "  set <pin> <0|1>   drive pin low/high\r\n"
        "  get <pin>         read pin level\r\n"
        "  servo <0-180>     move servo (PA7/pin 2) to angle\r\n"
        "  servo +N | -N     rotate servo relative\r\n"
        "  servo off         stop servo PWM\r\n"
        "  servo             report servo angle\r\n"
        "  ping              liveness check\r\n"
        "Pins: number (2-7,15,16) or name (PA7,PA6,PA4,PB3,PB2,PC3,PC1,PC0)\r\n");
    dut_reply(app, "OK");
}

static void dut_process_line(DutDriver* app, char* line) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strlcpy(app->last_cmd, line, sizeof(app->last_cmd));
    furi_mutex_release(app->mutex);

    char* save;
    char* cmd = strtok_r(line, " \t", &save);
    char* arg1 = strtok_r(NULL, " \t", &save);
    char* arg2 = strtok_r(NULL, " \t", &save);
    if(!cmd) return;

    if(!strcasecmp(cmd, "set")) {
        dut_cmd_set(app, arg1, arg2);
    } else if(!strcasecmp(cmd, "get")) {
        dut_cmd_get(app, arg1);
    } else if(!strcasecmp(cmd, "servo")) {
        dut_cmd_servo(app, arg1);
    } else if(!strcasecmp(cmd, "ping")) {
        dut_reply(app, "OK pong");
    } else if(!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
        dut_cmd_help(app);
    } else {
        dut_reply(app, "ERR unknown command '%s' (try help)", cmd);
    }
}

static void dut_feed(DutDriver* app, uint8_t byte) {
    if(byte == '\r' || byte == '\n') {
        if(app->line_overflow) {
            dut_reply(app, "ERR line too long");
        } else if(app->line_len > 0) {
            app->line[app->line_len] = '\0';
            dut_process_line(app, app->line);
        }
        app->line_len = 0;
        app->line_overflow = false;
    } else if(app->line_len < DUT_LINE_MAX - 1) {
        app->line[app->line_len++] = byte;
    } else {
        app->line_overflow = true;
    }
}

static int32_t dut_worker(void* context) {
    DutDriver* app = context;
    uint8_t buf[CDC_DATA_SZ];

    while(true) {
        uint32_t events =
            furi_thread_flags_wait(WorkerEvtStop | WorkerEvtRx, FuriFlagWaitAny, FuriWaitForever);
        if(events & FuriFlagError) continue;
        if(events & WorkerEvtStop) break;
        if(events & WorkerEvtRx) {
            int32_t len = furi_hal_cdc_receive(DUT_CDC_CHANNEL, buf, sizeof(buf));
            for(int32_t i = 0; i < len; i++)
                dut_feed(app, buf[i]);
        }
    }

    // Leave the hardware the way we found it
    dut_servo_stop(app);
    dut_release_pins(app);
    return 0;
}

/* ------------------------------------------------------------------- usb -- */

static void dut_usb_init(DutDriver* app) {
    app->usb_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    app->usb_ok = furi_hal_usb_set_config(&usb_cdc_dual, NULL);
    if(!app->usb_ok) {
        FURI_LOG_E(TAG, "Failed to switch USB to dual CDC");
        return;
    }

    // Keep the regular CLI alive on channel 0
    CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
    cli_vcp_enable(cli_vcp);
    furi_record_close(RECORD_CLI_VCP);

    furi_hal_cdc_set_callbacks(DUT_CDC_CHANNEL, &dut_cdc_callbacks, app);
}

static void dut_usb_deinit(DutDriver* app) {
    if(!app->usb_ok) return;

    furi_hal_cdc_set_callbacks(DUT_CDC_CHANNEL, NULL, NULL);

    CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
    cli_vcp_disable(cli_vcp);
    furi_hal_usb_unlock();
    furi_hal_usb_set_config(app->usb_prev, NULL);
    cli_vcp_enable(cli_vcp);
    furi_record_close(RECORD_CLI_VCP);
}

/* ------------------------------------------------------------------- gui -- */

static void dut_draw_cb(Canvas* canvas, void* context) {
    DutDriver* app = context;
    char buf[48];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "DUT Driver");

    canvas_set_font(canvas, FontSecondary);
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    if(!app->usb_ok)
        canvas_draw_str(canvas, 2, 22, "USB: setup failed");
    else
        canvas_draw_str(canvas, 2, 22, app->connected ? "USB: connected" : "USB: waiting for host");

    if(app->servo_on)
        snprintf(buf, sizeof(buf), "Servo (pin 2): %d deg", app->servo_angle);
    else
        snprintf(buf, sizeof(buf), "Servo (pin 2): off");
    canvas_draw_str(canvas, 2, 33, buf);

    snprintf(buf, sizeof(buf), "> %s", app->last_cmd);
    canvas_draw_str(canvas, 2, 45, buf);
    snprintf(buf, sizeof(buf), "< %s", app->last_resp);
    canvas_draw_str(canvas, 2, 55, buf);

    furi_mutex_release(app->mutex);

    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "Back: exit");
}

static void dut_input_cb(InputEvent* event, void* context) {
    DutDriver* app = context;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

/* ------------------------------------------------------------------ main -- */

int32_t dut_driver_app(void* p) {
    UNUSED(p);

    DutDriver* app = malloc(sizeof(DutDriver));
    memset(app, 0, sizeof(DutDriver));
    app->pin_mode = malloc(gpio_pins_count * sizeof(PinMode));
    memset(app->pin_mode, 0, gpio_pins_count * sizeof(PinMode));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->tx_sem = furi_semaphore_alloc(1, 1);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, dut_draw_cb, app);
    view_port_input_callback_set(app->view_port, dut_input_cb, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->worker = furi_thread_alloc_ex("DutDriverWorker", 2048, dut_worker, app);
    furi_thread_start(app->worker);

    dut_usb_init(app);
    view_port_update(app->view_port);

    InputEvent event;
    while(true) {
        if(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) != FuriStatusOk)
            continue;
        if(event.key == InputKeyBack && event.type == InputTypeShort) break;
    }

    dut_usb_deinit(app);

    furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerEvtStop);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_message_queue_free(app->input_queue);
    furi_semaphore_free(app->tx_sem);
    furi_mutex_free(app->mutex);
    free(app->pin_mode);
    free(app);

    return 0;
}
