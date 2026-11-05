#pragma once

// Shared by ui.cpp and menu.cpp.

#include <lvgl.h>

const lv_color_t BG = lv_color_hex(0x101418);
const lv_color_t CARD_BG = lv_color_hex(0x1c2530);
const lv_color_t MUTED = lv_color_hex(0x6b7686);
const lv_color_t GOOD = lv_color_hex(0x00e040);
const lv_color_t BAD = lv_color_hex(0xff1010);
const lv_color_t WARN = lv_color_hex(0xffa000);
const lv_color_t ACCENT = lv_color_hex(0x1080ff);
const lv_color_t HOT = lv_color_hex(0xff5000);

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
void ui_style_button(lv_obj_t *btn, lv_color_t color);
// Only touches the label if the text changed (see set_text in ui.cpp).
void ui_set_text(lv_obj_t *label, const char *text);

// Loads a screen. A screen loaded with ui_load_temp() is deleted as soon as
// another one is loaded, so menu screens only use RAM while they're open.
void ui_load(lv_obj_t *scr);
void ui_load_temp(lv_obj_t *scr);

void ui_show_dashboard();
void ui_show_settings();
// The newest command result or UI message, if it's recent.
bool ui_current_notice(char *buf, size_t n);

// menu.cpp
void menu_show();
void menu_debug(char c); // serial debug keys for the menu screens
