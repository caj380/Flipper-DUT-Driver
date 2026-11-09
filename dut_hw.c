#include "dut_driver_i.h"

#include <furi_hal_pwm.h>
#include <expansion/expansion.h>
#include <stm32wbxx_ll_tim.h>

#include <ctype.h>
#include <stdlib.h>

#define SERVO_PWM FuriHalPwmOutputIdTim1PA7

void dut_hw_init(DutDriver* app) {
    // Build the pin list in header-number order, skipping the SWD debug pins
    app->pin_count = 0;
    for(size_t i = 0; i < gpio_pins_count && app->pin_count < DUT_PINS_MAX; i++) {
        if(gpio_pins[i].debug) continue;
        size_t pos = app->pin_count++;
        while(pos > 0 && app->pins[pos - 1].record->number > gpio_pins[i].number) {
            app->pins[pos] = app->pins[pos - 1];
            pos--;
        }
        app->pins[pos] = (DutPin){.record = &gpio_pins[i], .mode = PinModeUnused};
    }
    app->servo_angle = SERVO_DEFAULT_ANGLE;

    // The expansion module service listens on pins 13/14; free them for us
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);
    furi_record_close(RECORD_EXPANSION);
}

void dut_hw_deinit(DutDriver* app) {
    // Leave the hardware the way we found it
    dut_servo_stop(app);
    for(size_t i = 0; i < app->pin_count; i++)
        dut_pin_release(app, &app->pins[i]);

    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);
}

DutPin* dut_pin_find(DutDriver* app, const char* token) {
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

    for(size_t i = 0; record && i < app->pin_count; i++) {
        if(app->pins[i].record == record) return &app->pins[i];
    }
    return NULL;
}

bool dut_pin_is_servo(DutDriver* app, const DutPin* pin) {
    return app->servo_on && pin->record->pin == &gpio_ext_pa7;
}

bool dut_pin_set(DutDriver* app, DutPin* pin, bool level) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool ok = !dut_pin_is_servo(app, pin);
    if(ok) {
        // Latch the level before switching to output so the pin doesn't glitch
        furi_hal_gpio_write(pin->record->pin, level);
        if(pin->mode != PinModeOutput) {
            furi_hal_gpio_init(pin->record->pin, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
            pin->mode = PinModeOutput;
        }
    }
    furi_mutex_release(app->mutex);
    return ok;
}

bool dut_pin_toggle(DutDriver* app, DutPin* pin) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    // A pin that isn't driven yet goes HIGH on the first toggle
    bool level = pin->mode == PinModeOutput ? !furi_hal_gpio_read(pin->record->pin) : true;
    bool ok = dut_pin_set(app, pin, level);
    furi_mutex_release(app->mutex);
    return ok;
}

bool dut_pin_get(DutDriver* app, DutPin* pin, bool* level) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool ok = !dut_pin_is_servo(app, pin);
    if(ok) {
        // Outputs are read back as-is; anything else becomes a floating input
        if(pin->mode == PinModeUnused) {
            furi_hal_gpio_init(pin->record->pin, GpioModeInput, GpioPullNo, GpioSpeedLow);
            pin->mode = PinModeInput;
        }
        *level = furi_hal_gpio_read(pin->record->pin);
    }
    furi_mutex_release(app->mutex);
    return ok;
}

bool dut_pin_peek(DutDriver* app, const DutPin* pin) {
    UNUSED(app);
    return furi_hal_gpio_read(pin->record->pin);
}

void dut_pin_release(DutDriver* app, DutPin* pin) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(pin->mode != PinModeUnused) {
        furi_hal_gpio_init_simple(pin->record->pin, GpioModeAnalog);
        pin->mode = PinModeUnused;
    }
    furi_mutex_release(app->mutex);
}

void dut_servo_set(DutDriver* app, int angle) {
    angle = CLAMP(angle, SERVO_MAX_ANGLE, 0);
    uint32_t pulse_us = SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * angle / SERVO_MAX_ANGLE;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(!app->servo_on) {
        // Servos are normally powered from pin 1; turn on 5V if nobody else has
        if(!furi_hal_power_is_otg_enabled() && furi_hal_power_enable_otg()) {
            app->otg_enabled_by_us = true;
        }

        // PA7 was possibly a GPIO before; the PWM driver takes it over
        for(size_t i = 0; i < app->pin_count; i++) {
            if(app->pins[i].record->pin == &gpio_ext_pa7) app->pins[i].mode = PinModeUnused;
        }

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
    app->servo_angle = angle;
    furi_mutex_release(app->mutex);
}

void dut_servo_stop(DutDriver* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->servo_on) {
        furi_hal_pwm_stop(SERVO_PWM);
        app->servo_on = false;
    }
    if(app->otg_enabled_by_us) {
        furi_hal_power_disable_otg();
        app->otg_enabled_by_us = false;
    }
    furi_mutex_release(app->mutex);
}
