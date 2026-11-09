#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>

#define TAG "DutDriver"

#define DUT_PINS_MAX    16
#define DUT_PRESETS_MAX 16
#define DUT_LABEL_MAX   25 // including terminator

// Standard hobby servo timing: 50 Hz frame, 0.5–2.5 ms pulse for 0–180°.
// Some servos expect 1.0–2.0 ms instead; adjust these if yours does.
#define SERVO_PERIOD_US     20000
#define SERVO_MIN_US        500
#define SERVO_MAX_US        2500
#define SERVO_MAX_ANGLE     180
#define SERVO_DEFAULT_ANGLE 90

typedef enum {
    PinModeUnused,
    PinModeInput,
    PinModeOutput,
} PinMode;

typedef struct {
    const GpioPinRecord* record;
    PinMode mode;
    char label[DUT_LABEL_MAX];
} DutPin;

typedef struct {
    char name[DUT_LABEL_MAX];
    uint8_t angle;
} DutPreset;

typedef enum {
    DutViewMain,
    DutViewServo,
    DutViewSubmenu,
    DutViewTextInput,
} DutView;

typedef enum {
    DutSceneMain,
    DutSceneServo,
    DutScenePinMenu,
    DutScenePresetMenu,
    DutSceneTextInput,
    DutSceneCount,
} DutScene;

typedef enum {
    DutTextTargetPinLabel,
    DutTextTargetPresetRename,
    DutTextTargetPresetNew,
} DutTextTarget;

// Custom events carry a type in the high half and an index/delta in the low half
typedef enum {
    DutEventPinToggle = 1,
    DutEventPinLow,
    DutEventPinHigh,
    DutEventPinEdit,
    DutEventServoOpen,
    DutEventPresetGo,
    DutEventPresetEdit,
    DutEventServoStep,
    DutEventServoSave,
    DutEventServoOff,
    DutEventSubmenu,
    DutEventTextDone,
} DutEventType;

#define DUT_EVENT(type, value) (((uint32_t)(type) << 16) | ((uint16_t)(value)))
#define DUT_EVENT_TYPE(event)  ((event) >> 16)
#define DUT_EVENT_VALUE(event) ((int16_t)((event) & 0xFFFF))

typedef struct DutMainView DutMainView;
typedef struct DutServoView DutServoView;

typedef struct {
    // Guards hardware and everything below that both the GUI and serial touch
    FuriMutex* mutex;

    DutPin pins[DUT_PINS_MAX];
    size_t pin_count;
    DutPreset presets[DUT_PRESETS_MAX];
    size_t preset_count;

    bool servo_on;
    int servo_angle;
    bool otg_enabled_by_us;

    // Serial
    FuriThread* worker;
    FuriSemaphore* tx_sem;
    FuriHalUsbInterface* usb_prev;
    bool usb_ok;
    bool connected;

    // GUI
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    DutMainView* main_view;
    DutServoView* servo_view;
    Submenu* submenu;
    TextInput* text_input;

    // Editing state shared between scenes
    size_t edit_index;
    int servo_edit_preset; // preset being adjusted on the servo screen, or -1
    DutTextTarget text_target;
    char text_buf[DUT_LABEL_MAX];
} DutDriver;

/* Hardware (dut_hw.c), all thread-safe */
void dut_hw_init(DutDriver* app);
void dut_hw_deinit(DutDriver* app);
DutPin* dut_pin_find(DutDriver* app, const char* token);
bool dut_pin_is_servo(DutDriver* app, const DutPin* pin);
bool dut_pin_set(DutDriver* app, DutPin* pin, bool level);
bool dut_pin_toggle(DutDriver* app, DutPin* pin);
bool dut_pin_get(DutDriver* app, DutPin* pin, bool* level);
bool dut_pin_peek(DutDriver* app, const DutPin* pin);
void dut_pin_release(DutDriver* app, DutPin* pin);
void dut_servo_set(DutDriver* app, int angle);
void dut_servo_stop(DutDriver* app);

/* Serial (dut_serial.c) */
void dut_serial_start(DutDriver* app);
void dut_serial_stop(DutDriver* app);

/* Labels and presets on SD (dut_config.c) */
void dut_config_load(DutDriver* app);
void dut_config_save(DutDriver* app);
void dut_preset_delete(DutDriver* app, size_t index);

/* Views (dut_views.c) */
DutMainView* dut_main_view_alloc(DutDriver* app);
void dut_main_view_free(DutMainView* view);
View* dut_main_view_get_view(DutMainView* view);
void dut_main_view_refresh(DutMainView* view);

DutServoView* dut_servo_view_alloc(DutDriver* app);
void dut_servo_view_free(DutServoView* view);
View* dut_servo_view_get_view(DutServoView* view);
void dut_servo_view_setup(DutServoView* view, const char* title, bool editing);
void dut_servo_view_refresh(DutServoView* view);

/* Scenes (dut_scenes.c) */
extern const SceneManagerHandlers dut_scene_handlers;
