#include "dut_driver_i.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ main -- */

static void dut_scene_main_on_enter(void* context) {
    DutDriver* app = context;
    dut_main_view_refresh(app->main_view);
    view_dispatcher_switch_to_view(app->view_dispatcher, DutViewMain);
}

static bool dut_scene_main_on_event(void* context, SceneManagerEvent event) {
    DutDriver* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        // Picks up changes made over serial and live input levels
        dut_main_view_refresh(app->main_view);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    size_t index = DUT_EVENT_VALUE(event.event);
    switch(DUT_EVENT_TYPE(event.event)) {
    case DutEventPinToggle:
        dut_pin_toggle(app, &app->pins[index]);
        break;
    case DutEventPinLow:
        dut_pin_set(app, &app->pins[index], false);
        break;
    case DutEventPinHigh:
        dut_pin_set(app, &app->pins[index], true);
        break;
    case DutEventPinEdit:
        app->edit_index = index;
        scene_manager_next_scene(app->scene_manager, DutScenePinMenu);
        break;
    case DutEventServoOpen:
        app->servo_edit_preset = -1;
        scene_manager_next_scene(app->scene_manager, DutSceneServo);
        break;
    case DutEventPresetGo:
        dut_servo_set(app, app->presets[index].angle);
        break;
    case DutEventPresetEdit:
        app->edit_index = index;
        scene_manager_next_scene(app->scene_manager, DutScenePresetMenu);
        break;
    default:
        return false;
    }
    dut_main_view_refresh(app->main_view);
    return true;
}

static void dut_scene_main_on_exit(void* context) {
    UNUSED(context);
}

/* ----------------------------------------------------------------- servo -- */

static void dut_scene_servo_on_enter(void* context) {
    DutDriver* app = context;

    if(app->servo_edit_preset >= 0) {
        DutPreset* preset = &app->presets[app->servo_edit_preset];
        dut_servo_set(app, preset->angle);
        dut_servo_view_setup(app->servo_view, preset->name, true);
    } else {
        dut_servo_view_setup(app->servo_view, "Servo", false);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, DutViewServo);
}

static bool dut_scene_servo_on_event(void* context, SceneManagerEvent event) {
    DutDriver* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        dut_servo_view_refresh(app->servo_view);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(DUT_EVENT_TYPE(event.event)) {
    case DutEventServoStep:
        dut_servo_set(app, app->servo_angle + DUT_EVENT_VALUE(event.event));
        break;
    case DutEventServoOff:
        dut_servo_stop(app);
        break;
    case DutEventServoSave:
        if(app->servo_edit_preset >= 0) {
            app->presets[app->servo_edit_preset].angle = app->servo_angle;
            dut_config_save(app);
            scene_manager_search_and_switch_to_previous_scene(app->scene_manager, DutSceneMain);
        } else if(app->preset_count < DUT_PRESETS_MAX) {
            app->text_target = DutTextTargetPresetNew;
            snprintf(app->text_buf, sizeof(app->text_buf), "Position %u", app->preset_count + 1);
            scene_manager_next_scene(app->scene_manager, DutSceneTextInput);
        }
        break;
    default:
        return false;
    }
    dut_servo_view_refresh(app->servo_view);
    return true;
}

static void dut_scene_servo_on_exit(void* context) {
    UNUSED(context);
}

/* -------------------------------------------------------------- pin menu -- */

typedef enum {
    PinMenuRename,
    PinMenuClearName,
    PinMenuRelease,
} PinMenuItem;

static void dut_submenu_cb(void* context, uint32_t index) {
    DutDriver* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, DUT_EVENT(DutEventSubmenu, index));
}

static void dut_scene_pin_menu_on_enter(void* context) {
    DutDriver* app = context;
    DutPin* pin = &app->pins[app->edit_index];
    char header[DUT_LABEL_MAX + 16];

    if(pin->label[0])
        snprintf(header, sizeof(header), "%s (%s)", pin->label, pin->record->name);
    else
        snprintf(header, sizeof(header), "%s (pin %u)", pin->record->name, pin->record->number);

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, header);
    submenu_add_item(app->submenu, "Rename", PinMenuRename, dut_submenu_cb, app);
    if(pin->label[0]) {
        submenu_add_item(app->submenu, "Clear name", PinMenuClearName, dut_submenu_cb, app);
    }
    if(pin->mode != PinModeUnused) {
        submenu_add_item(app->submenu, "Release (Hi-Z)", PinMenuRelease, dut_submenu_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, DutViewSubmenu);
}

static bool dut_scene_pin_menu_on_event(void* context, SceneManagerEvent event) {
    DutDriver* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(DUT_EVENT_TYPE(event.event) != DutEventSubmenu) return false;

    DutPin* pin = &app->pins[app->edit_index];
    switch(DUT_EVENT_VALUE(event.event)) {
    case PinMenuRename:
        app->text_target = DutTextTargetPinLabel;
        strlcpy(app->text_buf, pin->label, sizeof(app->text_buf));
        scene_manager_next_scene(app->scene_manager, DutSceneTextInput);
        break;
    case PinMenuClearName:
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        pin->label[0] = '\0';
        furi_mutex_release(app->mutex);
        dut_config_save(app);
        scene_manager_previous_scene(app->scene_manager);
        break;
    case PinMenuRelease:
        dut_pin_release(app, pin);
        scene_manager_previous_scene(app->scene_manager);
        break;
    default:
        return false;
    }
    return true;
}

static void dut_scene_menu_on_exit(void* context) {
    DutDriver* app = context;
    submenu_reset(app->submenu);
}

/* ----------------------------------------------------------- preset menu -- */

typedef enum {
    PresetMenuGo,
    PresetMenuAdjust,
    PresetMenuRename,
    PresetMenuDelete,
} PresetMenuItem;

static void dut_scene_preset_menu_on_enter(void* context) {
    DutDriver* app = context;
    DutPreset* preset = &app->presets[app->edit_index];

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, preset->name);
    submenu_add_item(app->submenu, "Go to position", PresetMenuGo, dut_submenu_cb, app);
    submenu_add_item(app->submenu, "Adjust angle", PresetMenuAdjust, dut_submenu_cb, app);
    submenu_add_item(app->submenu, "Rename", PresetMenuRename, dut_submenu_cb, app);
    submenu_add_item(app->submenu, "Delete", PresetMenuDelete, dut_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, DutViewSubmenu);
}

static bool dut_scene_preset_menu_on_event(void* context, SceneManagerEvent event) {
    DutDriver* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(DUT_EVENT_TYPE(event.event) != DutEventSubmenu) return false;

    DutPreset* preset = &app->presets[app->edit_index];
    switch(DUT_EVENT_VALUE(event.event)) {
    case PresetMenuGo:
        dut_servo_set(app, preset->angle);
        scene_manager_previous_scene(app->scene_manager);
        break;
    case PresetMenuAdjust:
        app->servo_edit_preset = app->edit_index;
        scene_manager_next_scene(app->scene_manager, DutSceneServo);
        break;
    case PresetMenuRename:
        app->text_target = DutTextTargetPresetRename;
        strlcpy(app->text_buf, preset->name, sizeof(app->text_buf));
        scene_manager_next_scene(app->scene_manager, DutSceneTextInput);
        break;
    case PresetMenuDelete:
        dut_preset_delete(app, app->edit_index);
        dut_config_save(app);
        scene_manager_previous_scene(app->scene_manager);
        break;
    default:
        return false;
    }
    return true;
}

/* ------------------------------------------------------------ text input -- */

static void dut_text_input_cb(void* context) {
    DutDriver* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, DUT_EVENT(DutEventTextDone, 0));
}

static void dut_scene_text_input_on_enter(void* context) {
    DutDriver* app = context;
    const char* header;

    switch(app->text_target) {
    case DutTextTargetPinLabel:
        header = "Pin name (_ = space)";
        break;
    case DutTextTargetPresetRename:
    case DutTextTargetPresetNew:
    default:
        header = "Position name (_ = space)";
        break;
    }

    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, header);
    // An empty pin name clears the label; positions always need one
    text_input_set_minimum_length(app->text_input, app->text_target == DutTextTargetPinLabel ? 0 : 1);
    text_input_set_result_callback(
        app->text_input,
        dut_text_input_cb,
        app,
        app->text_buf,
        sizeof(app->text_buf),
        app->text_target == DutTextTargetPresetNew);
    view_dispatcher_switch_to_view(app->view_dispatcher, DutViewTextInput);
}

// The keyboard may not have a space key, so '_' stands in for one
static void dut_clean_name(char* name) {
    for(char* c = name; *c; c++)
        if(*c == '_') *c = ' ';

    size_t len = strlen(name);
    while(len > 0 && name[len - 1] == ' ')
        name[--len] = '\0';
    size_t lead = strspn(name, " ");
    memmove(name, name + lead, len - lead + 1);
}

static bool dut_scene_text_input_on_event(void* context, SceneManagerEvent event) {
    DutDriver* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(DUT_EVENT_TYPE(event.event) != DutEventTextDone) return false;

    dut_clean_name(app->text_buf);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    switch(app->text_target) {
    case DutTextTargetPinLabel:
        strlcpy(app->pins[app->edit_index].label, app->text_buf, DUT_LABEL_MAX);
        break;
    case DutTextTargetPresetRename:
        if(app->text_buf[0])
            strlcpy(app->presets[app->edit_index].name, app->text_buf, DUT_LABEL_MAX);
        break;
    case DutTextTargetPresetNew:
        if(app->text_buf[0] && app->preset_count < DUT_PRESETS_MAX) {
            DutPreset* preset = &app->presets[app->preset_count++];
            strlcpy(preset->name, app->text_buf, DUT_LABEL_MAX);
            preset->angle = app->servo_angle;
        }
        break;
    }
    furi_mutex_release(app->mutex);

    dut_config_save(app);
    scene_manager_search_and_switch_to_previous_scene(app->scene_manager, DutSceneMain);
    return true;
}

static void dut_scene_text_input_on_exit(void* context) {
    DutDriver* app = context;
    text_input_reset(app->text_input);
}

/* -------------------------------------------------------------- handlers -- */

static void (*const dut_scene_on_enter[])(void*) = {
    [DutSceneMain] = dut_scene_main_on_enter,
    [DutSceneServo] = dut_scene_servo_on_enter,
    [DutScenePinMenu] = dut_scene_pin_menu_on_enter,
    [DutScenePresetMenu] = dut_scene_preset_menu_on_enter,
    [DutSceneTextInput] = dut_scene_text_input_on_enter,
};

static bool (*const dut_scene_on_event[])(void*, SceneManagerEvent) = {
    [DutSceneMain] = dut_scene_main_on_event,
    [DutSceneServo] = dut_scene_servo_on_event,
    [DutScenePinMenu] = dut_scene_pin_menu_on_event,
    [DutScenePresetMenu] = dut_scene_preset_menu_on_event,
    [DutSceneTextInput] = dut_scene_text_input_on_event,
};

static void (*const dut_scene_on_exit[])(void*) = {
    [DutSceneMain] = dut_scene_main_on_exit,
    [DutSceneServo] = dut_scene_servo_on_exit,
    [DutScenePinMenu] = dut_scene_menu_on_exit,
    [DutScenePresetMenu] = dut_scene_menu_on_exit,
    [DutSceneTextInput] = dut_scene_text_input_on_exit,
};

const SceneManagerHandlers dut_scene_handlers = {
    .on_enter_handlers = dut_scene_on_enter,
    .on_event_handlers = dut_scene_on_event,
    .on_exit_handlers = dut_scene_on_exit,
    .scene_num = DutSceneCount,
};
