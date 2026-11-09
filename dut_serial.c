/**
 * Line-based command interface on the Flipper's second USB serial port
 * (CDC channel 1). Send "help" for the command list.
 */

#include "dut_driver_i.h"

#include <furi_hal_usb_cdc.h>
#include <cli/cli_vcp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUT_CDC_CHANNEL 1
#define DUT_LINE_MAX    64
#define DUT_TX_TIMEOUT  100

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRx = (1 << 1),
} WorkerEvt;

typedef struct {
    DutDriver* app;
    char line[DUT_LINE_MAX];
    size_t line_len;
    bool line_overflow;
} DutSerial;

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

    strlcat(buf, "\r\n", sizeof(buf));
    dut_send(app, buf);
}

/* -------------------------------------------------------------- commands -- */

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
    DutPin* pin = dut_pin_find(app, pin_token);
    bool level;
    if(!pin) {
        dut_reply(app, "ERR unknown pin '%s'", pin_token ? pin_token : "");
        return;
    }
    if(!dut_parse_level(level_token, &level)) {
        dut_reply(app, "ERR level must be 0/1/low/high");
        return;
    }
    if(!dut_pin_set(app, pin, level)) {
        dut_reply(app, "ERR %s in use by servo", pin->record->name);
        return;
    }
    dut_reply(app, "OK %s=%d", pin->record->name, level);
}

static void dut_cmd_get(DutDriver* app, char* pin_token) {
    DutPin* pin = dut_pin_find(app, pin_token);
    bool level;
    if(!pin) {
        dut_reply(app, "ERR unknown pin '%s'", pin_token ? pin_token : "");
        return;
    }
    if(!dut_pin_get(app, pin, &level)) {
        dut_reply(app, "ERR %s in use by servo", pin->record->name);
        return;
    }
    dut_reply(
        app, "OK %s=%d %s", pin->record->name, level, pin->mode == PinModeOutput ? "out" : "in");
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

    dut_servo_set(app, angle);
    dut_reply(app, "OK servo=%ld", angle);
}

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

static void dut_feed(DutSerial* serial, uint8_t byte) {
    if(byte == '\r' || byte == '\n') {
        if(serial->line_overflow) {
            dut_reply(serial->app, "ERR line too long");
        } else if(serial->line_len > 0) {
            serial->line[serial->line_len] = '\0';
            dut_process_line(serial->app, serial->line);
        }
        serial->line_len = 0;
        serial->line_overflow = false;
    } else if(serial->line_len < DUT_LINE_MAX - 1) {
        serial->line[serial->line_len++] = byte;
    } else {
        serial->line_overflow = true;
    }
}

static int32_t dut_worker(void* context) {
    DutSerial serial = {.app = context};
    uint8_t buf[CDC_DATA_SZ];

    while(true) {
        uint32_t events =
            furi_thread_flags_wait(WorkerEvtStop | WorkerEvtRx, FuriFlagWaitAny, FuriWaitForever);
        if(events & FuriFlagError) continue;
        if(events & WorkerEvtStop) break;
        if(events & WorkerEvtRx) {
            int32_t len = furi_hal_cdc_receive(DUT_CDC_CHANNEL, buf, sizeof(buf));
            for(int32_t i = 0; i < len; i++)
                dut_feed(&serial, buf[i]);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------- usb -- */

void dut_serial_start(DutDriver* app) {
    app->tx_sem = furi_semaphore_alloc(1, 1);
    app->worker = furi_thread_alloc_ex("DutDriverWorker", 2048, dut_worker, app);
    furi_thread_start(app->worker);

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

void dut_serial_stop(DutDriver* app) {
    if(app->usb_ok) {
        furi_hal_cdc_set_callbacks(DUT_CDC_CHANNEL, NULL, NULL);

        CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_disable(cli_vcp);
        furi_hal_usb_unlock();
        furi_hal_usb_set_config(app->usb_prev, NULL);
        cli_vcp_enable(cli_vcp);
        furi_record_close(RECORD_CLI_VCP);
    }

    furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerEvtStop);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    furi_semaphore_free(app->tx_sem);
}
