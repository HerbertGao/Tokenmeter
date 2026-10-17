#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <SensorQMI8658.hpp>
#include <Wire.h>
#include <XPowersLib.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "core.h"

static Arduino_ESP32QSPI bus(12, 38, 4, 5, 6, 7);
static Arduino_CO5300 panel(&bus, 2, 0, 480, 480, 0, 0, 0, 0);
static XPowersPMU pmu;
static TouchDrvCST92xx touch;
static SensorQMI8658 imu;
static meter::Orientation orientation;
static meter::HomeKey homeKey;
static uint8_t* rotationBuffer;
static bool imuReady = false, touchPressed = false;
static int16_t touchX = 0, touchY = 0;
static volatile bool touchReady = false;
static void IRAM_ATTR touchInterrupt() { touchReady = true; }
static void updateTouch() {
    if (!touchReady) return;
    touchReady = false;
    int16_t tx[5], ty[5];
    uint8_t count = touch.getPoint(tx, ty, std::min<uint8_t>(5, touch.getSupportTouchPoint()));
    touchPressed = count > 0;
    if (touchPressed) { touchX = constrain(tx[0], 0, 479); touchY = constrain(ty[0], 0, 479); }
}

bool boardBegin() {
    rotationBuffer = static_cast<uint8_t*>(heap_caps_malloc(BoardDrawBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!rotationBuffer) return false;
    pinMode(18, INPUT_PULLUP);
    Wire.begin(15, 14);
    if (!panel.begin()) return false;
    panel.fillScreen(0);
    panel.setBrightness(200);
    // Keep the reference board's display -> PMU -> touch order (shared reset GPIO2).
    if (!pmu.begin(Wire, 0x34, 15, 14)) return false;
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_8S);
    touch.setPins(2, 11);
    if (!touch.begin(Wire, 0x5a, 15, 14)) return false;
    touch.setMaxCoordinates(480, 480);
    // Measured native CST axes match panel pixels; LVGL applies the live rotation.
    touch.setSwapXY(false);
    touch.setMirrorXY(false, false);
    pinMode(11, INPUT_PULLUP);
    attachInterrupt(11, touchInterrupt, FALLING);
    imuReady = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, 15, 14);
    if (imuReady) {
        imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                                SensorQMI8658::ACC_ODR_LOWPOWER_21Hz, SensorQMI8658::LPF_MODE_3);
        imu.enableAccelerometer();
    }
    Serial.println(imuReady ? "QMI8658 ready; auto orientation enabled" : "QMI8658 unavailable; fixed orientation");
    return true;
}
bool boardPoll(lv_display_t* display, bool touching) {
    updateTouch();
    if (!imuReady) return false;
    if (touchPressed || touching) { orientation.candidate = 4; return false; }
    uint64_t now = uint64_t(esp_timer_get_time()) / 1000;
    static uint64_t last = 0;
    if (now - last < 100) return false;
    last = now;
    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) { orientation.candidate = 4; return false; }
    static bool logged = false;
    if (!logged) {
        // Do not mistake the sensor's power-on filter transient for settled gravity.
        float g2 = ax*ax + ay*ay + az*az;
        if (g2 >= 0.49f && g2 <= 1.69f) {
            Serial.printf("Acceleration g: %.3f %.3f %.3f\n", ax, ay, az);
            logged = true;
        }
    }
    if (!orientation.update(ax, ay, az, now, false)) return false;
    lv_display_set_rotation(display, static_cast<lv_display_rotation_t>(orientation.rotation));
    Serial.printf("Orientation: LVGL %u degrees\n", unsigned(orientation.rotation)*90);
    return true;
}
bool boardHomePressed() {
    return homeKey.update(digitalRead(18) == LOW, uint64_t(esp_timer_get_time()) / 1000);
}
void boardFlush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    lv_area_t destination = *area;
    int32_t width = lv_area_get_width(area), height = lv_area_get_height(area);
    auto rotation = lv_display_get_rotation(display);
    if (rotation != LV_DISPLAY_ROTATION_0) {
        if (size_t(width)*height*2 > BoardDrawBytes) {
            Serial.println("Rotation strip exceeds buffer; flush skipped");
            lv_display_flush_ready(display);
            return;
        }
        lv_display_rotate_area(display, &destination);
        lv_draw_sw_rotate(pixels, rotationBuffer, width, height,
                          lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565),
                          lv_area_get_width(&destination)*2, rotation, LV_COLOR_FORMAT_RGB565);
        pixels = rotationBuffer;
    }
    panel.draw16bitRGBBitmap(destination.x1, destination.y1, reinterpret_cast<uint16_t*>(pixels),
                            lv_area_get_width(&destination), lv_area_get_height(&destination));
    lv_display_flush_ready(display);
}
void boardRound(lv_event_t* event) {
    auto* a = static_cast<lv_area_t*>(lv_event_get_param(event));
    a->x1 &= ~1; a->y1 &= ~1; a->x2 |= 1; a->y2 |= 1;
}
void boardTouch(lv_indev_t*, lv_indev_data_t* data) {
    updateTouch();
    // LVGL applies the display's inverse rotation after this callback.
    data->point = {touchX, touchY};
    data->state = touchPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
