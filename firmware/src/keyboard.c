#include "keyboard.h"
#include "hid_codes.h"
#include "usb_hid.h"

#include <string.h>

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

#define ROW_GPIO_PORT GPIOA
#define COL_GPIO_PORT GPIOB

#define ROW_START_PIN 0
#define COL_START_PIN 0

#define NUM_ROWS (uint16_t)7
#define NUM_COLS (uint16_t)16

// mapping of (row, column) to key code
static const uint8_t keyboard_key_map[NUM_ROWS][NUM_COLS] = {
    {KEY_GRAVE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, 0, 0},
    {KEY_TAB, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, 0, 0},
    {KEY_CAPSLOCK, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_ENTER, KEY_SYSRQ, KEY_SCROLLLOCK, KEY_PAUSE},
    {0, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, 0, KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE},
    {0, 0, 0, KEY_SPACE, 0, 0, KEY_PROPS, 0, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_END, KEY_PAGEDOWN},
    {KEY_NUMLOCK, KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPPLUS, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KPENTER, KEY_KP0},
    {KEY_KPDOT, KEY_UP, KEY_LEFT, KEY_DOWN, KEY_RIGHT, KEY_ESC, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, 0, 0, 0, 0},
};

// mapping of (row, column) to modifier
static const uint8_t keyboard_modifier_map[NUM_ROWS][NUM_COLS] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {KEY_MOD_LSHIFT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_MOD_RSHIFT, 0, 0, 0, 0},
    {KEY_MOD_LCTRL, KEY_MOD_LMETA, KEY_MOD_LALT, 0, KEY_MOD_RALT, KEY_MOD_RMETA, 0, KEY_MOD_RCTRL, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static uint8_t keyboard_key_pressed[NUM_ROWS][NUM_COLS] = {0};

static uint16_t keyboard_poll_row = 0;

static struct usb_hid_report keyboard_hid_report;

static bool keyboard_data_updated = false;

static void add_key(uint8_t key_code) {
    // don't bother trying if this key doesn't have a key code (i.e. it's a modifier key)
    if (key_code == KEY_NONE) {
        return;
    }
    
    for (int slot = 0; slot < MAX_NUM_KEY_CODES; slot++) {
        // do not add a key multiple times
        if (keyboard_hid_report.key_codes[slot] == key_code) {
            return;
        }

        // find the next free slot, add the key code
        if (keyboard_hid_report.key_codes[slot] == KEY_NONE) {
            keyboard_hid_report.key_codes[slot] = key_code;
            keyboard_data_updated = true;
            return;
        }
    }
}

static void remove_key(uint8_t key_code) {
    // don't bother trying if this key doesn't have a key code (i.e. it's a modifier key)
    if (key_code == KEY_NONE) {
        return;
    }

    for (int slot = 0; slot < MAX_NUM_KEY_CODES; slot++) {
        // reset slot to zero
        if (keyboard_hid_report.key_codes[slot] == key_code) {
            keyboard_hid_report.key_codes[slot] = KEY_NONE;
            keyboard_data_updated = true;
            // don't immediately return in case the key is in multiple slots (shouldn't happen)
        }
    }
}

static void add_modifier(uint8_t mod_mask) {
    keyboard_hid_report.modifiers |= mod_mask;
    keyboard_data_updated = true;
}

static void remove_modifier(uint8_t mod_mask) {
    keyboard_hid_report.modifiers &= ~mod_mask;
    keyboard_data_updated = true;
}

static void send_key_data(void) {
    usb_hid_send_report(&keyboard_hid_report);
}

void keyboard_init(void) {
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);

    // Set rows as outputs
    uint16_t row_pins = ~(uint16_t)((uint16_t)0xFFFF << NUM_ROWS) << ROW_START_PIN;
    gpio_mode_setup(ROW_GPIO_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, row_pins);
    gpio_set_output_options(ROW_GPIO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, row_pins);

    // Set columns as inputs
    uint16_t col_pins = ~(uint16_t)((uint16_t)0xFFFF << NUM_COLS) << COL_START_PIN;
    gpio_mode_setup(COL_GPIO_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, col_pins);

    // ensure keyboard data is zeroed out
    memset(&keyboard_hid_report, 0, sizeof(keyboard_hid_report));

    // select first row
    gpio_set(ROW_GPIO_PORT, (1 << ++keyboard_poll_row) << ROW_START_PIN);
}

void keyboard_poll(void) {
    uint16_t col_states = gpio_port_read(COL_GPIO_PORT) >> COL_START_PIN;

    for (int col = 0; col < NUM_COLS; col++) {
        // get key code and modifier mask for this key
        uint8_t key_code = keyboard_key_map[keyboard_poll_row][col];
        uint8_t modifier_mask = keyboard_modifier_map[keyboard_poll_row][col];

        // add and remove key codes and modifier masks when keys are pressed and released
        if ((col_states & (1 << col)) && !keyboard_key_pressed[keyboard_poll_row][col]) {
            // key pressed
            keyboard_key_pressed[keyboard_poll_row][col] = 1;
            add_modifier(modifier_mask);
            add_key(key_code);
        } else if ((col_states & (1 << col)) == 0 && keyboard_key_pressed[keyboard_poll_row][col]) {
            // key released
            keyboard_key_pressed[keyboard_poll_row][col] = 0;
            remove_modifier(modifier_mask);
            remove_key(key_code);
        }
    }

    if (keyboard_data_updated) {
        send_key_data();
        keyboard_data_updated = false;
    }

    // deselect this row
    gpio_clear(ROW_GPIO_PORT, (1 << keyboard_poll_row) << ROW_START_PIN);

    // select the next row
    if (++keyboard_poll_row == NUM_ROWS) {
        keyboard_poll_row = 0;
    }
    gpio_set(ROW_GPIO_PORT, (1 << keyboard_poll_row) << ROW_START_PIN);
}