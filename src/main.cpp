#include <Arduino.h>
#include <lvgl.h>

#include "display.h"
#include "ui.h"
#include "perf.h"

#include <esp_system.h>

void setup() {
    Serial.begin(115200);
    Serial.printf("\n[boot] reset reason %d, heap %u\n", (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());
    display_init();
    ui_init(); // also starts the Moonraker polling task
    perf_init();
}

void loop() {
    uint32_t t0 = millis();
    while (Serial.available()) ui_debug_command(Serial.read());
    lv_timer_handler();
    perf_loop(millis() - t0);
    delay(5);
}
