#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/flash.h"
#include "hardware/clocks.h"
#include "pico/binary_info.h"
#include "button.h"
#include "ssd1306.h"
#include "textRenderer/TextRenderer.h"
#include "shapeRenderer/ShapeRenderer.h"

using namespace pico_ssd1306;

// GPIO pin assignments
#define PIN_BUTTON_UP           2
#define PIN_BUTTON_DOWN         3
#define PIN_BUTTON_NEXT         4
#define PIN_BUTTON_PREV         5

#define PIN_SENSE_SPARKING     26    // TBD!!!
#define PIN_SENSE_CONTACT      27    // TBD!!!
#define PIN_SENSE_LIMIT_UP      0    // may need to swap these two; amazon page says these are normally open..
#define PIN_SENSE_LIMIT_DOWN    1

#define PIN_SPARK_PWM          22

// display stuff
#define PIN_I2C_CLOCK 7
#define PIN_I2C_DATA 6
SSD1306 *display;

// Stepper stuff
#define PIN_STEPPER_MS1        20
#define PIN_STEPPER_MS2        19
#define PIN_STEPPER_MS3        18
// #define PIN_STEPPER_EN         xx
#define PIN_STEPPER_DIR        16
#define PIN_STEPPER_STEP       17

#define MICROSTEP_RATE          8

// ========================= STATE =========================
// UI states
typedef enum {
    UI_JOG = 0,
    UI_DEPTH0,
    UI_DEPTH1,
    UI_DEPTH2,
    UI_DEPTH3,
    UI_TON,
    UI_TOFF,
    UI_STATE_COUNT,
    UI_PREBURN,
    UI_BURN,
    UI_POSTBURN
} ui_state_t;
static volatile ui_state_t uiState = UI_JOG;
static volatile bool rezeroOnBurn = true;

// motion
volatile float targetDepth = 0.0f;
volatile bool moveDirectionDown = true;
volatile bool shouldMove = false;

// settings saving
#define SETTINGS_MAGIC         0xBEEFCAFE
#define FLASH_TARGET_OFFSET    (1024 * 500)
#define SETTINGS_BLOCK_SIZE    4096
#define SETTINGS_ROTATE_EVERY  5000
#define SETTINGS_MAX_BLOCKS    10

#define SAVE_DELAY_MS 5000
static uint64_t nextSaveTimeMS = 0;

typedef struct {
    uint32_t magic;
    uint16_t ton_us;
    uint16_t toff_us;
    uint8_t depthDigits[4];
    uint32_t numWrites;
} settings_t;

static settings_t settings = {
    .magic = SETTINGS_MAGIC,
    .ton_us = 50,
    .toff_us = 200,
    .depthDigits = {0, 0, 0, 0},
    .numWrites = 0
};

// ========================= BUTTONS =========================
volatile bool upPressed = false;
volatile bool downPressed = false;

#define REPEAT_TIME_MS 150
uint64_t upRepeatTime = 0;
uint64_t downRepeatTime = 0;

// ========================= PROCESS SETTINGS RANGES =========================
#define TON_MIN 10
#define TON_MAX 200
#define TON_STEP 10

#define TOFF_MIN 50
#define TOFF_MAX 500
#define TOFF_STEP 25

// ========================= SAVING =========================
// --- Rate-limited flash save variables and functions ---
// Use a global timer (milliseconds since boot) to schedule a flash save.
// Flash write routine, marked to run from RAM.
// this does not work... system hangs :/ yes, I hate it too
void __not_in_flash_func(SaveSettings)(void) {
    settings.magic = SETTINGS_MAGIC;
    settings.numWrites++;

    uint8_t buffer[SETTINGS_BLOCK_SIZE];
    uint32_t offset = FLASH_TARGET_OFFSET +
                      ((settings.numWrites / SETTINGS_ROTATE_EVERY) % SETTINGS_MAX_BLOCKS) * SETTINGS_BLOCK_SIZE;
    
    memcpy(buffer, &settings, sizeof(settings));
    
    printf("Saving settings...\n");
    fflush(stdout);
    
    uint32_t interrupts = save_and_disable_interrupts();
    printf("Interrupts saved\n");
    fflush(stdout);
    
    
    // Lock out the other core (and disable flash access from interrupts)
    multicore_lockout_start_blocking();
    // printf("Locked out!\n");
    // fflush(stdout);
    
    
    flash_range_erase(offset, SETTINGS_BLOCK_SIZE);
    // printf("Erase done\n");
    // fflush(stdout);
    
    flash_range_program(offset, buffer, SETTINGS_BLOCK_SIZE);
    // printf("Write done\n");
    // fflush(stdout);
    
    multicore_lockout_end_blocking();
    
    restore_interrupts(interrupts);
    
    printf("Settings saved.\n");
    fflush(stdout);
}

// Call this function when a setting changes to schedule a flash save 10 sec in the future.
void ScheduleSaveSettings(void) {
    uint64_t now_ms = time_us_64() / 1000;
    nextSaveTimeMS = now_ms + SAVE_DELAY_MS;  // schedule a save SAVE_DELAY_MS later
    printf("Scheduled settings save at %llu ms\n", nextSaveTimeMS);
    fflush(stdout);
}

// Check if it's time to save settings and, if so, perform the flash write.
void MaybeSaveSettings(void) {
    if (nextSaveTimeMS == 0) return;  // No save scheduled.
    uint64_t now_ms = time_us_64() / 1000;
    if (now_ms >= nextSaveTimeMS) {
        __not_in_flash_func(SaveSettings)();
        nextSaveTimeMS = 0;
    }
}

// Load settings from flash.
void LoadSettings(void) {
    for (int i = SETTINGS_MAX_BLOCKS - 1; i >= 0; --i) {
        uint32_t offset = FLASH_TARGET_OFFSET + i * SETTINGS_BLOCK_SIZE;
        const settings_t *candidate = (const settings_t *)(XIP_BASE + offset);
        if (candidate->magic == SETTINGS_MAGIC) {
            memcpy((void *)&settings, candidate, sizeof(settings_t));
            return;
        }
    }
    settings.magic = SETTINGS_MAGIC;
}


// =========================== HELPERS ================================
// Converts an int to a const char*; returns a pointer to a static buffer.
const char * Int2Text(int value) {
    static char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    return buffer;
}

// Converts a float to text with 2 decimal places.
const char * Float2Text(float value) {
    static char buffer[16];
    snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

// Button callback using the button library.
void OnButtonStateChange(button_t *button_p) {
    button_t *button = (button_t*)button_p;
    // Only process on button press; ignore release.
    // if(button->state) return;
    
    // Do not process button events during burn modes.
    if (uiState == UI_BURN || uiState == UI_POSTBURN)
        return;
    
    // tracking up/down buttons, and time since last state change (for repetition)
    if( button->pin == PIN_BUTTON_UP ) {
        upPressed = button->state;
        if( upPressed )
            upRepeatTime = time_us_64() / 1000 - 1;
        else
            upRepeatTime = 0;
    }
    if( button->pin == PIN_BUTTON_DOWN ) {
        downPressed = button->state;
        if( downPressed )
            downRepeatTime = time_us_64() / 1000 - 1;
        else
            downRepeatTime = 0;
    }
    
    // also block state switching when we're in pre-burn
    if (uiState == UI_PREBURN)
        return;
    
    switch(button->pin){
        case PIN_BUTTON_NEXT:
            if(!button->state)
                uiState = (ui_state_t)((uiState + 1) % UI_STATE_COUNT);
            break;
        case PIN_BUTTON_PREV:
            if(!button->state)
                uiState = (ui_state_t)((uiState + UI_STATE_COUNT - 1) % UI_STATE_COUNT);
            break;
    }
}

bool TryUseUpButton(void) {
    if( (upRepeatTime > 0) && (time_us_64() / 1000 > upRepeatTime) ) {
        upRepeatTime += REPEAT_TIME_MS;
        return true;
    }
    return false;
}

bool TryUseDownButton(void) {
    if( (downRepeatTime > 0) && (time_us_64() / 1000 > downRepeatTime) ) {
        downRepeatTime += REPEAT_TIME_MS;
        return true;
    }
    return false;
}

// Setup buttons with the button library.
void SetupButtons(void) {
    // The library sets internal pull-ups.
    create_button_active_high(PIN_BUTTON_UP, OnButtonStateChange);
    create_button_active_high(PIN_BUTTON_DOWN, OnButtonStateChange);
    create_button_active_high(PIN_BUTTON_NEXT, OnButtonStateChange);
    create_button_active_high(PIN_BUTTON_PREV, OnButtonStateChange);
}

// Setup additional inputs.
void SetupInputs(void) {
    gpio_init(PIN_SENSE_SPARKING);
    gpio_set_dir(PIN_SENSE_SPARKING, GPIO_IN);
    gpio_pull_down(PIN_SENSE_SPARKING);

    gpio_init(PIN_SENSE_CONTACT);
    gpio_set_dir(PIN_SENSE_CONTACT, GPIO_IN);
    gpio_pull_down(PIN_SENSE_CONTACT);
}

void SetMicrostepA4988(uint16_t microstep) {
    bool ms1 = 0, ms2 = 0, ms3 = 0;
    switch (microstep) {
        case 1:   break; // all low
        case 2:   ms1 = 1; break;
        case 4:   ms2 = 1; break;
        case 8:   ms1 = ms2 = 1; break;
        case 16:  ms1 = ms2 = ms3 = 1; break;
        default:  return; // invalid input — ignore or add error handling
    }
    
    gpio_put(PIN_STEPPER_MS1, ms1);
    gpio_put(PIN_STEPPER_MS2, ms2);
    gpio_put(PIN_STEPPER_MS3, ms3);
}

void SetupStepperPins(void) {
    const uint8_t stepperPins[] = {
        PIN_STEPPER_MS1,
        PIN_STEPPER_MS2,
        PIN_STEPPER_MS3,
        // PIN_STEPPER_EN,
        PIN_STEPPER_DIR,
        PIN_STEPPER_STEP
    };
    
    for (int i = 0; i < sizeof(stepperPins) / sizeof(stepperPins[0]); ++i) {
        gpio_init(stepperPins[i]);
        gpio_set_dir(stepperPins[i], GPIO_OUT);
    }
    
    gpio_put( PIN_STEPPER_DIR, moveDirectionDown );
    SetMicrostepA4988( MICROSTEP_RATE );
}

// =========================== UI Handlers ===========================
void handle_ui_jog(void) {
    shouldMove = upPressed ^ downPressed;
    moveDirectionDown = !upPressed;
    
    display->clear();
    drawText(display, font_12x16, "Jog-jog", 0, 0);
    display->sendBuffer();
    sleep_ms( 10 );
}

void handle_ui_depth(void) {
    int digit_index = uiState - UI_DEPTH0;
    bool changed = false;
    
    if (TryUseUpButton()) {
        if (++settings.depthDigits[digit_index] > 9)
            settings.depthDigits[digit_index] = 0;
        changed = true;
    }
    if (TryUseDownButton()) {
        if (settings.depthDigits[digit_index] == 0)
            settings.depthDigits[digit_index] = 9;
        else
            settings.depthDigits[digit_index]--;
        changed = true;
    }
    
    if (changed)
        ScheduleSaveSettings();
    
    targetDepth = settings.depthDigits[0] * 10.0f +
               settings.depthDigits[1] * 1.0f +
               settings.depthDigits[2] * 0.1f +
               settings.depthDigits[3] * 0.01f;
    
    display->clear();
    drawText(display, font_12x16, "Depth:", 0, 0);
    drawChar(display, font_12x16, '0' + settings.depthDigits[0], 40, 15);
    drawChar(display, font_12x16, '0' + settings.depthDigits[1], 40+13, 15);
    drawChar(display, font_12x16, '.', 40+13*2, 15);
    drawChar(display, font_12x16, '0' + settings.depthDigits[2], 40+13*3-1, 15);
    drawChar(display, font_12x16, '0' + settings.depthDigits[3], 40+13*4-1, 15);
    
    
    int rectX = 40 + 13*digit_index + (digit_index > 1 ? 12 : 0) - 3;
    int rectWidth = 18;
    
    int rectY = 12;
    int rectHeight = 20;
    drawRect(display, rectX, rectY, rectX + rectWidth, rectY + rectHeight);
    drawRect(display, rectX + 1, rectY + 1, rectX + rectWidth - 1, rectY + rectHeight - 1);
    
    display->sendBuffer();
    
    sleep_ms( 10 );
}

void draw_frequency_data(void) {
    int f = 1000000 /(settings.ton_us + settings.toff_us);
    int startX = f < 10000 ? 70 : 70-12;
    drawText(display, font_12x16, Int2Text(f), startX, 18);
    drawText(display, font_5x8, "Hz", 120, 24);
}

void handle_ui_ton(void) {
    bool changed = false;

    if (TryUseUpButton()) {
        if( settings.ton_us < TON_MAX ) {
            settings.ton_us += TON_STEP;
            changed = true;
        }
    }
    if (TryUseDownButton()) {
        if( settings.ton_us > TON_MIN ) {
            settings.ton_us -= TON_STEP;
            changed = true;
        }
    }
    
    if (changed) {
        ScheduleSaveSettings();
    }
    
    display->clear();
    drawText(display, font_12x16, "T_on:", 0, 0);
    int startX = settings.ton_us < 100 ? 90 : 90-12;
    drawText(display, font_12x16, Int2Text(settings.ton_us), startX, 0);
    drawText(display, font_5x8, "us", 90+12*2, 8);
    draw_frequency_data();
    display->sendBuffer();
    
    sleep_ms( 10 );
}

void handle_ui_toff(void) {
    bool changed = false;
    
    if (TryUseUpButton()) {
        if( settings.toff_us < TOFF_MAX ) {
            settings.toff_us += TOFF_STEP;
            changed = true;
        }
    }
    if (TryUseDownButton()) {
        if( settings.toff_us > TOFF_MIN ) {
            settings.toff_us -= TOFF_STEP;
            changed = true;
        }
    }
    
    if (changed) {
        ScheduleSaveSettings();
    }
    
    display->clear();
    drawText(display, font_12x16, "T_off:", 0, 0);
    int startX = settings.toff_us < 100 ? 90 : 90-12;
    drawText(display, font_12x16, Int2Text(settings.toff_us), startX, 0);
    drawText(display, font_5x8, "us", 90+12*2, 8);
    draw_frequency_data();
    display->sendBuffer();
    
    sleep_ms( 10 );
}

void handle_ui_burn(void) {
    // TODO: Implement burn mode UI logic.
    display->clear();
    drawText(display, font_12x16, "BURN", 0, 0);
    display->sendBuffer();
    
    sleep_ms( 10 );
}

void handle_ui_preburn(void) {
    if (TryUseUpButton()) {
        rezeroOnBurn = true;
        uiState = UI_BURN;
    } else if (TryUseDownButton()) {
        rezeroOnBurn = false;
        uiState = UI_BURN;
    }
    
    sleep_ms( 10 );
}

void handle_ui_postburn(void) {
    // TODO: Display "burn finished, disable spark switch"
    // Waiting for sparking switch off (handled in SparklingThread)
    display->clear();
    drawText(display, font_12x16, "Burn done", 0, 0);
    display->sendBuffer();
    
    sleep_ms( 10 );
}

// --- Core1: Sparkling Thread (Timing-Critical Tasks) ---
void SparklingThread(void) {
    multicore_lockout_victim_init();
    
    bool wasSparking = false;
    
    while (true) {
        bool sparking = gpio_get(PIN_SENSE_SPARKING);
        
        if (sparking != wasSparking) {
            if (uiState == UI_POSTBURN && !sparking) {
                uiState = UI_JOG;
            } else if (sparking) {
                uiState = UI_PREBURN;
            } else {
                uiState = UI_JOG;
            }
        }
        wasSparking = sparking;
        
        // sleep_ms( 10 );
        
        if( uiState == UI_JOG ) {
            if( shouldMove ) {
                gpio_put( PIN_STEPPER_DIR, moveDirectionDown );
                
                gpio_put( PIN_STEPPER_STEP, 1 );
                sleep_us(10);
                gpio_put( PIN_STEPPER_STEP, 0 );
                sleep_us(200);
            }
            else {
                sleep_ms( 5 );
            }
        }
        else if( uiState == UI_BURN ) {
            // TODO: sparking... sensing contact.. automatic moves.. etc
            sleep_ms(10);
        }
        else {
            sleep_ms(10);
        }
    }
}

// --- Setup Display ---
void SetupDisplay(void) {
    i2c_init(i2c1, 1000000);
    gpio_set_function(PIN_I2C_CLOCK, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_DATA, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C_CLOCK);
    gpio_pull_up(PIN_I2C_DATA);
    
    sleep_ms(250);  // let the display wake up
    
    display = new SSD1306(i2c1, 0x3C, Size::W128xH32);
    display->setOrientation(0);
}

// --- Main Function ---
int main(void) {
    stdio_init_all();
    set_sys_clock_khz(100000, true);
    LoadSettings();
    SetupButtons();
    SetupInputs();
    SetupStepperPins();
    SetupDisplay();

    multicore_launch_core1(SparklingThread);

    // Main loop: Process UI events and check for scheduled flash saves.
    while (true) {
        switch (uiState) {
            case UI_JOG:
                handle_ui_jog();
                break;
            case UI_DEPTH0:
            case UI_DEPTH1:
            case UI_DEPTH2:
            case UI_DEPTH3:
                handle_ui_depth();
                break;
            case UI_TON:
                handle_ui_ton();
                break;
            case UI_TOFF:
                handle_ui_toff();
                break;
            case UI_PREBURN:
                handle_ui_preburn();
                break;
            case UI_BURN:
                handle_ui_burn();
                break;
            case UI_POSTBURN:
                handle_ui_postburn();
                break;
            default:
                break;
        }
        
        // MaybeSaveSettings();
        
        // Let the loop run at a reasonable rate of 50Hz
        sleep_ms(20);
    }
    return 0;
}
