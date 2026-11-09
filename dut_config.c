/**
 * Pin labels and servo presets, stored at /ext/apps_data/dut_driver/config.txt:
 *
 *   Filetype: DUT Driver
 *   Version: 1
 *   Label PC1: DC Power Relay
 *   Preset count: 1
 *   Preset 1 name: Lid open
 *   Preset 1 angle: 45
 */

#include "dut_driver_i.h"

#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define DUT_CONFIG_PATH     APP_DATA_PATH("config.txt")
#define DUT_CONFIG_FILETYPE "DUT Driver"
#define DUT_CONFIG_VERSION  1

static bool dut_config_read_string(FlipperFormat* ff, const char* key, char* out, size_t size) {
    FuriString* value = furi_string_alloc();
    // Rewind so keys can be found in any order and missing ones are just skipped
    bool ok = flipper_format_rewind(ff) && flipper_format_read_string(ff, key, value);
    if(ok) strlcpy(out, furi_string_get_cstr(value), size);
    furi_string_free(value);
    return ok;
}

static bool dut_config_read_uint32(FlipperFormat* ff, const char* key, uint32_t* out) {
    return flipper_format_rewind(ff) && flipper_format_read_uint32(ff, key, out, 1);
}

void dut_config_load(DutDriver* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* filetype = furi_string_alloc();
    char key[32];

    do {
        uint32_t version;
        if(!flipper_format_file_open_existing(ff, DUT_CONFIG_PATH)) break;
        if(!flipper_format_read_header(ff, filetype, &version)) break;
        if(furi_string_cmp_str(filetype, DUT_CONFIG_FILETYPE) || version != DUT_CONFIG_VERSION) {
            FURI_LOG_W(TAG, "Ignoring config with unknown format");
            break;
        }

        for(size_t i = 0; i < app->pin_count; i++) {
            DutPin* pin = &app->pins[i];
            snprintf(key, sizeof(key), "Label %s", pin->record->name);
            dut_config_read_string(ff, key, pin->label, sizeof(pin->label));
        }

        uint32_t count = 0;
        dut_config_read_uint32(ff, "Preset count", &count);
        app->preset_count = 0;
        for(uint32_t i = 1; i <= count && app->preset_count < DUT_PRESETS_MAX; i++) {
            DutPreset* preset = &app->presets[app->preset_count];
            uint32_t angle;
            snprintf(key, sizeof(key), "Preset %lu name", i);
            if(!dut_config_read_string(ff, key, preset->name, sizeof(preset->name))) continue;
            snprintf(key, sizeof(key), "Preset %lu angle", i);
            if(!dut_config_read_uint32(ff, key, &angle) || angle > SERVO_MAX_ANGLE) continue;
            preset->angle = angle;
            app->preset_count++;
        }
    } while(false);

    furi_string_free(filetype);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

void dut_config_save(DutDriver* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    char key[32];
    bool ok = false;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    do {
        if(!flipper_format_file_open_always(ff, DUT_CONFIG_PATH)) break;
        if(!flipper_format_write_header_cstr(ff, DUT_CONFIG_FILETYPE, DUT_CONFIG_VERSION)) break;

        bool written = true;
        for(size_t i = 0; i < app->pin_count && written; i++) {
            DutPin* pin = &app->pins[i];
            if(pin->label[0] == '\0') continue;
            snprintf(key, sizeof(key), "Label %s", pin->record->name);
            written = flipper_format_write_string_cstr(ff, key, pin->label);
        }
        if(!written) break;

        uint32_t count = app->preset_count;
        if(!flipper_format_write_uint32(ff, "Preset count", &count, 1)) break;
        for(uint32_t i = 0; i < count && written; i++) {
            uint32_t angle = app->presets[i].angle;
            snprintf(key, sizeof(key), "Preset %lu name", i + 1);
            written = flipper_format_write_string_cstr(ff, key, app->presets[i].name);
            snprintf(key, sizeof(key), "Preset %lu angle", i + 1);
            written = written && flipper_format_write_uint32(ff, key, &angle, 1);
        }
        ok = written;
    } while(false);
    furi_mutex_release(app->mutex);

    if(!ok) FURI_LOG_E(TAG, "Failed to save config");
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

void dut_preset_delete(DutDriver* app, size_t index) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(index < app->preset_count) {
        memmove(
            &app->presets[index],
            &app->presets[index + 1],
            (app->preset_count - index - 1) * sizeof(DutPreset));
        app->preset_count--;
    }
    furi_mutex_release(app->mutex);
}
