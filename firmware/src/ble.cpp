#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <utility>

static constexpr const char* Service = "544f4b45-4e4d-4554-4552-000000000001";
static constexpr const char* Rx = "544f4b45-4e4d-4554-4552-000000000002";
static constexpr const char* Ack = "544f4b45-4e4d-4554-4552-000000000003";
static SemaphoreHandle_t mutex;
static meter::Framer frame(nullptr);
static char* work;
static bool connected = false, parsing = false, violated = false;
static uint32_t generation = 0;
static uint16_t connection;
static NimBLECharacteristic* ack;

class PsramJson final : public ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static PsramJson jsonAllocator;

class ServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        xSemaphoreTake(mutex, portMAX_DELAY);
        ++generation; connected = true; connection = info.getConnHandle();
        frame.reset(); parsing = violated = false;
        ack->setValue("");
        xSemaphoreGive(mutex);
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        xSemaphoreTake(mutex, portMAX_DELAY);
        ++generation; connected = false;
        frame.reset(); parsing = violated = false;
        ack->setValue("");
        xSemaphoreGive(mutex);
        NimBLEDevice::startAdvertising();
    }
};
class RxCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        // Attribute length is bounded to 180; never parse JSON, ACK or call LVGL here.
        const auto& value = characteristic->getValue();
        xSemaphoreTake(mutex, portMAX_DELAY);
        if (connected) {
            if (parsing) violated = true;
            else frame.feed(value.data(), value.size());
        }
        xSemaphoreGive(mutex);
    }
};
static ServerCallbacks serverCallbacks;
static RxCallbacks rxCallbacks;

bool bleBegin() {
    mutex = xSemaphoreCreateMutex();
    frame.data = static_cast<char*>(heap_caps_malloc(meter::FrameLimit+1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    work = static_cast<char*>(heap_caps_malloc(meter::FrameLimit+1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mutex || !frame.data || !work) return false;
    if (!NimBLEDevice::init("Tokenmeter")) return false;
    // macOS can retain the old HID profile for the factory address after reflashing.
    ble_addr_t address = *NimBLEDevice::getAddress().getBase();
    address.type = BLE_ADDR_RANDOM;
    address.val[5] |= 0xc0; // Bluetooth static-random address, derived without NVS/eFuse writes.
    const NimBLEAddress identity(address);
    if (!NimBLEDevice::setOwnAddr(identity) || !NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM) ||
        NimBLEDevice::getAddress() != identity) return false;
    Serial.printf("Tokenmeter BLE identity: %s (static random)\n", identity.toString().c_str());
    NimBLEDevice::setMTU(185);
    auto* server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks);
    auto* service = server->createService(Service);
    if (!service) return false;
    auto* rx = service->createCharacteristic(Rx, NIMBLE_PROPERTY::WRITE, 180);
    ack = service->createCharacteristic(Ack, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 5);
    if (!rx || !ack) return false;
    rx->setCallbacks(&rxCallbacks);
    ack->setValue("");
    if (!service->start()) return false;
    auto* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(Service);
    advertising->enableScanResponse(true);
    advertising->setName("Tokenmeter");
    return advertising->start();
}
bool bleConnected() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool result = connected;
    xSemaphoreGive(mutex);
    return result;
}
bool blePoll(meter::Snapshot*& current, meter::Snapshot*& staging, uint64_t& receivedUs) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!frame.ready) { xSemaphoreGive(mutex); return false; }
    size_t size = frame.used;
    bool error = frame.error;
    uint32_t ticket = generation;
    std::swap(frame.data, work);
    frame.reset(); parsing = true; violated = false;
    xSemaphoreGive(mutex);

    bool valid = !error && meter::parse(work, size, *staging, &jsonAllocator);
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool committed = false;
    if (connected && generation == ticket) {
        valid = valid && !violated;
        if (valid) {
            std::swap(current, staging);
            receivedUs = esp_timer_get_time();
            committed = true;
        }
        // Serialize with disconnect/reconnect: an old frame can never ACK a new peer.
        ack->setValue(valid ? "ok" : "error");
        ack->notify(connection);
        parsing = violated = false;
    }
    xSemaphoreGive(mutex);
    return committed;
}
