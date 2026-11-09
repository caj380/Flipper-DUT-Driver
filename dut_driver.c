/**
 * DUT Driver
 *
 * Drives GPIO pins and a hobby servo for a device under test, either from the
 * Flipper's own screen or over the second USB serial port (see dut_serial.c).
 * Pins can be given names and servo positions can be saved; both are stored on
 * the SD card (see dut_config.c).
 */

#include "dut_driver_i.h"

#define DUT_TICK_MS 250

static bool dut_custom_event_cb(void* context, uint32_t event) {
    DutDriver* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool dut_back_event_cb(void* context) {
    DutDriver* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void dut_tick_event_cb(void* context) {
    DutDriver* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static DutDriver* dut_alloc(void) {
    DutDriver* app = malloc(sizeof(DutDriver));
    memset(app, 0, sizeof(DutDriver));
    // Recursive so hardware helpers can call each other while holding it
    app->mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
    app->servo_edit_preset = -1;

    dut_hw_init(app);
    dut_config_load(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&dut_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, dut_custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, dut_back_event_cb);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, dut_tick_event_cb, DUT_TICK_MS);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->main_view = dut_main_view_alloc(app);
    view_dispatcher_add_view(
        app->view_dispatcher, DutViewMain, dut_main_view_get_view(app->main_view));
    app->servo_view = dut_servo_view_alloc(app);
    view_dispatcher_add_view(
        app->view_dispatcher, DutViewServo, dut_servo_view_get_view(app->servo_view));
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, DutViewSubmenu, submenu_get_view(app->submenu));
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, DutViewTextInput, text_input_get_view(app->text_input));

    dut_serial_start(app);
    return app;
}

static void dut_free(DutDriver* app) {
    dut_serial_stop(app);
    dut_hw_deinit(app);

    view_dispatcher_remove_view(app->view_dispatcher, DutViewMain);
    view_dispatcher_remove_view(app->view_dispatcher, DutViewServo);
    view_dispatcher_remove_view(app->view_dispatcher, DutViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, DutViewTextInput);
    dut_main_view_free(app->main_view);
    dut_servo_view_free(app->servo_view);
    submenu_free(app->submenu);
    text_input_free(app->text_input);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    furi_mutex_free(app->mutex);
    free(app);
}

int32_t dut_driver_app(void* p) {
    UNUSED(p);

    DutDriver* app = dut_alloc();
    scene_manager_next_scene(app->scene_manager, DutSceneMain);
    view_dispatcher_run(app->view_dispatcher);
    dut_free(app);

    return 0;
}
