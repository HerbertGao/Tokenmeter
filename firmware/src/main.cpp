#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <new>
#include "board.h"
#include "ble.h"
#include "ui.h"

static meter::Snapshot *current, *staging;
static bool haveSnapshot=false;
static uint64_t receivedUs=0;
static lv_display_t* display;
static lv_indev_t* input;
static uint32_t tick() { return millis(); }
static void fail(const char* message) {
    Serial.println(message);
    // Allocation/hardware failure is fatal, never pass null buffers into LVGL/NimBLE.
    while (true) delay(1000);
}
static void* psram(size_t size) {
    void* p=heap_caps_malloc(size,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) fail("PSRAM allocation failed; stopped");
    return p;
}
static void readTouch(lv_indev_t* input, lv_indev_data_t* data) {
    boardTouch(input,data);
}
void setup() {
    Serial.begin(115200);
    delay(300);
    if (!psramFound()) fail("PSRAM is required");
    current=new (psram(sizeof(meter::Snapshot))) meter::Snapshot{};
    staging=new (psram(sizeof(meter::Snapshot))) meter::Snapshot{};
    auto* buffer1=psram(BoardDrawBytes);
    auto* buffer2=psram(BoardDrawBytes);
    if (!boardBegin()) fail("Display / PMU / touch initialization failed");
    lv_init();
    lv_tick_set_cb(tick);
    display=lv_display_create(480,480);
    if (!display) fail("LVGL display allocation failed");
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display,boardFlush);
    lv_display_set_buffers(display,buffer1,buffer2,BoardDrawBytes,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(display,boardRound,LV_EVENT_INVALIDATE_AREA,nullptr);
    input=lv_indev_create();
    if (!input) fail("LVGL input allocation failed");
    lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input,readTouch);
    uiBegin();
    if (!bleBegin()) fail("BLE / receive-buffer initialization failed");
    Serial.printf("Tokenmeter ready; internal free=%u, PSRAM free=%u\n",
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
void loop() {
    bool changed=blePoll(current,staging,receivedUs);
    if (changed) haveSnapshot=true;
    static uint32_t last=0;
    static bool lastConnected=false;
    static uint64_t lastElapsed=0;
    bool connected=bleConnected();
    uint64_t elapsed=haveSnapshot?(uint64_t(esp_timer_get_time())-receivedUs)/1000000:0;
    bool clockChanged=haveSnapshot &&
        (meter::stale(*current,elapsed)!=meter::stale(*current,lastElapsed) ||
         meter::oldDay(*current,elapsed)!=meter::oldDay(*current,lastElapsed));
    if (changed || connected!=lastConnected || clockChanged || millis()-last>=60000) {
        last=millis(); lastConnected=connected; lastElapsed=elapsed;
        uiUpdate(haveSnapshot?current:nullptr,elapsed,connected);
    }
    lv_timer_handler();
    lv_point_t point;
    lv_indev_get_point(input,&point);
    bool touching=lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED;
    // Read the processed point, after LVGL has applied the inverse display rotation.
    uiTouch(touching,point.x,point.y);
    if (uiPoll() && touching) lv_indev_wait_release(input);
    if (boardHomePressed()) {
        lv_indev_wait_release(input);
        uiHome();
        Serial.println("Home key: overview");
    }
    if (boardPoll(display,touching)) {
        lv_indev_reset(input,nullptr);
        uiCancelTouch();
    }
    delay(5);
}
