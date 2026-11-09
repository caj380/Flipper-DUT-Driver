#include "dut_driver_i.h"

#include <gui/elements.h>
#include <stdio.h>

#define MAIN_ROWS   4
#define MAIN_ROW_H  11
#define MAIN_LIST_Y 12
#define MAIN_ROW_W  123
#define MAIN_VALUE_X 120

typedef enum {
    DutItemPin,
    DutItemServo,
    DutItemPreset,
} DutItemKind;

/* Main list layout: every pin, then the servo, then the saved positions */
static size_t dut_item_count(DutDriver* app) {
    return app->pin_count + 1 + app->preset_count;
}

static DutItemKind dut_item_kind(DutDriver* app, size_t index, size_t* sub) {
    if(index < app->pin_count) {
        *sub = index;
        return DutItemPin;
    }
    if(index == app->pin_count) {
        *sub = 0;
        return DutItemServo;
    }
    *sub = index - app->pin_count - 1;
    return DutItemPreset;
}

// Fonts have no degree sign, so draw one by hand. Returns the width used.
static int32_t dut_draw_angle(Canvas* canvas, int32_t right, int32_t baseline, int angle) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", angle);
    int32_t width = canvas_string_width(canvas, buf);
    canvas_draw_str(canvas, right - 4 - width, baseline, buf);
    canvas_draw_circle(canvas, right - 2, baseline - 6, 1);
    return width + 4;
}

/* ------------------------------------------------------------- main view -- */

struct DutMainView {
    View* view;
    DutDriver* app;
};

typedef struct {
    DutDriver* app;
    size_t selected;
    size_t offset;
} DutMainModel;

static const char* dut_pin_state_text(DutDriver* app, const DutPin* pin) {
    if(dut_pin_is_servo(app, pin)) return "servo";
    switch(pin->mode) {
    case PinModeOutput:
        return dut_pin_peek(app, pin) ? "HIGH" : "LOW";
    case PinModeInput:
        return dut_pin_peek(app, pin) ? "in: 1" : "in: 0";
    default:
        return "Hi-Z";
    }
}

static void dut_main_draw_row(Canvas* canvas, DutDriver* app, size_t index, int32_t baseline) {
    FuriString* label = furi_string_alloc();
    int32_t value_width;
    size_t sub;

    switch(dut_item_kind(app, index, &sub)) {
    case DutItemPin: {
        const DutPin* pin = &app->pins[sub];
        const char* state = dut_pin_state_text(app, pin);
        canvas_draw_str_aligned(canvas, MAIN_VALUE_X, baseline, AlignRight, AlignBottom, state);
        value_width = canvas_string_width(canvas, state);
        if(pin->label[0])
            furi_string_set(label, pin->label);
        else
            furi_string_printf(label, "%s (pin %u)", pin->record->name, pin->record->number);
        break;
    }
    case DutItemServo:
        if(app->servo_on) {
            value_width = dut_draw_angle(canvas, MAIN_VALUE_X, baseline, app->servo_angle);
        } else {
            canvas_draw_str_aligned(canvas, MAIN_VALUE_X, baseline, AlignRight, AlignBottom, "off");
            value_width = canvas_string_width(canvas, "off");
        }
        furi_string_set(label, "Servo");
        break;
    case DutItemPreset:
    default: {
        const DutPreset* preset = &app->presets[sub];
        value_width = dut_draw_angle(canvas, MAIN_VALUE_X, baseline, preset->angle);
        // Mark the position the servo is currently sitting at
        if(app->servo_on && app->servo_angle == preset->angle) {
            value_width += 6;
            canvas_draw_box(canvas, MAIN_VALUE_X - value_width, baseline - 5, 3, 3);
        }
        furi_string_set(label, preset->name);
        break;
    }
    }

    elements_string_fit_width(canvas, label, MAIN_VALUE_X - value_width - 8);
    canvas_draw_str(canvas, 3, baseline, furi_string_get_cstr(label));
    furi_string_free(label);
}

static void dut_main_draw_footer(Canvas* canvas, DutDriver* app, size_t index) {
    char info[24];
    const char* hint = "Hold OK: edit";
    size_t sub;

    switch(dut_item_kind(app, index, &sub)) {
    case DutItemPin:
        snprintf(
            info,
            sizeof(info),
            "%s / pin %u",
            app->pins[sub].record->name,
            app->pins[sub].record->number);
        break;
    case DutItemServo:
        snprintf(info, sizeof(info), "PA7 / pin 2");
        hint = "OK: open";
        break;
    case DutItemPreset:
    default:
        snprintf(info, sizeof(info), "Saved position");
        break;
    }

    canvas_draw_str(canvas, 2, 63, info);
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, hint);
}

static void dut_main_draw(Canvas* canvas, void* model_ptr) {
    DutMainModel* model = model_ptr;
    DutDriver* app = model->app;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "DUT Driver");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 117, 9, AlignRight, AlignBottom, "USB");
    if(app->connected)
        canvas_draw_disc(canvas, 123, 5, 3);
    else
        canvas_draw_circle(canvas, 123, 5, 3);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    size_t count = dut_item_count(app);
    for(size_t row = 0; row < MAIN_ROWS && model->offset + row < count; row++) {
        size_t index = model->offset + row;
        int32_t top = MAIN_LIST_Y + row * MAIN_ROW_H;
        if(index == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, 0, top, MAIN_ROW_W, MAIN_ROW_H, 2);
            canvas_set_color(canvas, ColorWhite);
        }
        dut_main_draw_row(canvas, app, index, top + 9);
        canvas_set_color(canvas, ColorBlack);
    }
    elements_scrollbar_pos(canvas, 128, MAIN_LIST_Y, MAIN_ROWS * MAIN_ROW_H, model->selected, count);
    dut_main_draw_footer(canvas, app, model->selected);
    furi_mutex_release(app->mutex);
}

static void dut_main_clamp(DutMainModel* model) {
    size_t count = dut_item_count(model->app);
    if(model->selected >= count) model->selected = count - 1;
    if(model->selected < model->offset) {
        model->offset = model->selected;
    } else if(model->selected >= model->offset + MAIN_ROWS) {
        model->offset = model->selected - MAIN_ROWS + 1;
    }
}

static bool dut_main_input(InputEvent* event, void* context) {
    DutMainView* main_view = context;
    DutDriver* app = main_view->app;
    bool consumed = false;
    bool send = false;
    uint32_t custom = 0;

    if(event->key == InputKeyBack) return false;

    with_view_model(
        main_view->view,
        DutMainModel * model,
        {
            size_t count = dut_item_count(app);
            bool step = event->type == InputTypeShort || event->type == InputTypeRepeat;
            size_t sub;
            DutItemKind kind = dut_item_kind(app, model->selected, &sub);

            if(step && event->key == InputKeyUp) {
                // Wrap around on a single press, but stop at the top when holding
                if(model->selected > 0)
                    model->selected--;
                else if(event->type == InputTypeShort)
                    model->selected = count - 1;
                consumed = true;
            } else if(step && event->key == InputKeyDown) {
                if(model->selected + 1 < count)
                    model->selected++;
                else if(event->type == InputTypeShort)
                    model->selected = 0;
                consumed = true;
            } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                send = true;
                if(kind == DutItemPin)
                    custom = DUT_EVENT(DutEventPinToggle, sub);
                else if(kind == DutItemServo)
                    custom = DUT_EVENT(DutEventServoOpen, 0);
                else
                    custom = DUT_EVENT(DutEventPresetGo, sub);
                consumed = true;
            } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
                send = true;
                if(kind == DutItemPin)
                    custom = DUT_EVENT(DutEventPinEdit, sub);
                else if(kind == DutItemServo)
                    custom = DUT_EVENT(DutEventServoOpen, 0);
                else
                    custom = DUT_EVENT(DutEventPresetEdit, sub);
                consumed = true;
            } else if(event->type == InputTypeShort && kind == DutItemPin) {
                if(event->key == InputKeyLeft) {
                    send = true;
                    custom = DUT_EVENT(DutEventPinLow, sub);
                } else if(event->key == InputKeyRight) {
                    send = true;
                    custom = DUT_EVENT(DutEventPinHigh, sub);
                }
                consumed = true;
            }
            dut_main_clamp(model);
        },
        true);

    if(send) view_dispatcher_send_custom_event(app->view_dispatcher, custom);
    return consumed;
}

DutMainView* dut_main_view_alloc(DutDriver* app) {
    DutMainView* main_view = malloc(sizeof(DutMainView));
    main_view->app = app;
    main_view->view = view_alloc();
    view_allocate_model(main_view->view, ViewModelTypeLocking, sizeof(DutMainModel));
    view_set_context(main_view->view, main_view);
    view_set_draw_callback(main_view->view, dut_main_draw);
    view_set_input_callback(main_view->view, dut_main_input);
    with_view_model(
        main_view->view,
        DutMainModel * model,
        {
            model->app = app;
            model->selected = 0;
            model->offset = 0;
        },
        false);
    return main_view;
}

void dut_main_view_free(DutMainView* main_view) {
    view_free(main_view->view);
    free(main_view);
}

View* dut_main_view_get_view(DutMainView* main_view) {
    return main_view->view;
}

void dut_main_view_refresh(DutMainView* main_view) {
    with_view_model(main_view->view, DutMainModel * model, { dut_main_clamp(model); }, true);
}

/* ------------------------------------------------------------ servo view -- */

#define GAUGE_X 14
#define GAUGE_Y 39
#define GAUGE_W 101
#define GAUGE_H 7

struct DutServoView {
    View* view;
    DutDriver* app;
};

typedef struct {
    DutDriver* app;
    char title[DUT_LABEL_MAX + 8];
    bool editing;
} DutServoModel;

static void dut_servo_draw(Canvas* canvas, void* model_ptr) {
    DutServoModel* model = model_ptr;
    DutDriver* app = model->app;
    FuriString* title = furi_string_alloc_set(model->title);
    char buf[8];

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    int angle = app->servo_angle;
    bool on = app->servo_on;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontPrimary);
    elements_string_fit_width(canvas, title, 100);
    canvas_draw_str(canvas, 2, 10, furi_string_get_cstr(title));
    furi_string_free(title);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, on ? "ON" : "OFF");

    canvas_set_font(canvas, FontBigNumbers);
    snprintf(buf, sizeof(buf), "%d", angle);
    canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignBottom, buf);
    canvas_draw_circle(canvas, 64 + canvas_string_width(canvas, buf) / 2 + 4, 23, 2);

    // Position gauge with ticks at 0°, 90° and 180°
    canvas_draw_frame(canvas, GAUGE_X, GAUGE_Y, GAUGE_W, GAUGE_H);
    canvas_draw_box(
        canvas, GAUGE_X + 1, GAUGE_Y + 1, (GAUGE_W - 2) * angle / SERVO_MAX_ANGLE, GAUGE_H - 2);
    for(int tick = 0; tick <= 2; tick++) {
        int32_t x = GAUGE_X + 1 + (GAUGE_W - 3) * tick / 2;
        canvas_draw_line(canvas, x, GAUGE_Y + GAUGE_H, x, GAUGE_Y + GAUGE_H + 2);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, GAUGE_X - 10, GAUGE_Y + 6, "<");
    canvas_draw_str(canvas, GAUGE_X + GAUGE_W + 5, GAUGE_Y + 6, ">");
    canvas_draw_str(canvas, 2, 63, model->editing ? "OK: update" : "OK: save");
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "Hold OK: off");
}

static bool dut_servo_input(InputEvent* event, void* context) {
    DutServoView* servo_view = context;
    DutDriver* app = servo_view->app;
    bool step = event->type == InputTypeShort || event->type == InputTypeRepeat;
    int delta = 0;

    if(event->key == InputKeyBack) return false;

    if(step && event->key == InputKeyLeft) delta = -1;
    if(step && event->key == InputKeyRight) delta = 1;
    if(step && event->key == InputKeyDown) delta = -10;
    if(step && event->key == InputKeyUp) delta = 10;

    if(delta) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, DUT_EVENT(DutEventServoStep, delta));
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, DUT_EVENT(DutEventServoSave, 0));
    } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
        view_dispatcher_send_custom_event(app->view_dispatcher, DUT_EVENT(DutEventServoOff, 0));
    }
    return true;
}

DutServoView* dut_servo_view_alloc(DutDriver* app) {
    DutServoView* servo_view = malloc(sizeof(DutServoView));
    servo_view->app = app;
    servo_view->view = view_alloc();
    view_allocate_model(servo_view->view, ViewModelTypeLocking, sizeof(DutServoModel));
    view_set_context(servo_view->view, servo_view);
    view_set_draw_callback(servo_view->view, dut_servo_draw);
    view_set_input_callback(servo_view->view, dut_servo_input);
    with_view_model(
        servo_view->view,
        DutServoModel * model,
        {
            model->app = app;
            model->title[0] = '\0';
            model->editing = false;
        },
        false);
    return servo_view;
}

void dut_servo_view_free(DutServoView* servo_view) {
    view_free(servo_view->view);
    free(servo_view);
}

View* dut_servo_view_get_view(DutServoView* servo_view) {
    return servo_view->view;
}

void dut_servo_view_setup(DutServoView* servo_view, const char* title, bool editing) {
    with_view_model(
        servo_view->view,
        DutServoModel * model,
        {
            strlcpy(model->title, title, sizeof(model->title));
            model->editing = editing;
        },
        true);
}

void dut_servo_view_refresh(DutServoView* servo_view) {
    with_view_model(servo_view->view, DutServoModel * model, { UNUSED(model); }, true);
}
