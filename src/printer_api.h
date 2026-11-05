#pragma once

#include <Arduino.h>

// Moonraker actions for the menu screens (G-code, print start/cancel,
// restarts, macro and file lists). They run one at a time in their own task,
// so a long command (G28 takes a while) never holds up the status polling
// or the e-stop. Results show up via moonraker_last_result().

void api_start();

// Queue a G-code script. label names it in the result ("Home all done").
// False if the queue is full.
bool api_gcode(const String &script, const char *label);
// Queue a POST to a Moonraker path, e.g. "/printer/print/cancel".
bool api_post(const String &path, const char *label);

enum class ApiList { Files, Macros };
constexpr int API_LIST_MAX = 16; // RAM: each row on screen is ~300 bytes
constexpr int API_NAME_LEN = 80;

// Fetch the list again in the background.
void api_fetch(ApiList which);
// The list's length (files newest first, macros A-Z); *loading is true
// while a fetch is running, gen changes on every update. Only the list
// fetched last is kept.
int api_list(ApiList which, bool *loading, uint32_t *gen);
// Entry i of that list.
bool api_list_name(ApiList which, int i, char *buf, size_t n);
