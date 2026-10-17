#pragma once
#include <lvgl.h>

// Only Waveshare ESP32-S3-Touch-AMOLED-2.16. See README for reference source.
constexpr uint32_t BoardDrawBytes = 480*40*2;
bool boardBegin();
bool boardPoll(lv_display_t* display, bool touching);
bool boardHomePressed();
void boardFlush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);
void boardRound(lv_event_t* event);
void boardTouch(lv_indev_t* input, lv_indev_data_t* data);
