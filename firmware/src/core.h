#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

namespace meter {
constexpr size_t FrameLimit = 65536, PairLimit = 256, IdentityLimit = 192;
constexpr uint64_t MaxInteger = 9007199254740991ULL;
struct Cost { double value = 0; bool known = false; };
struct Row {
    char provider[IdentityLimit + 1]{}, model[IdentityLimit + 1]{};
    Cost cost;
    uint64_t tokens = 0, records = 0, unpriced = 0;
};
enum class Status { Ok, Partial, Unavailable };
struct DaySummary {
    char date[11]{};
    uint64_t issues = 0;
    Status status = Status::Unavailable;
    Cost cost;
    uint64_t tokens = 0, input = 0, output = 0, cacheRead = 0, cacheWrite = 0;
    uint64_t records = 0, unpriced = 0;
};
struct Snapshot : DaySummary {
    uint64_t epoch = 0, dayEnd = 0;
    int offset = 0;
    DaySummary yesterday;
    size_t count = 0;
    Row rows[PairLimit];
};
// Caller owns FrameLimit+1 bytes. Access under the transport mutex on device.
struct Framer {
    char* data;
    size_t used = 0;
    bool dropping = false, ready = false, error = false;
    explicit Framer(char* buffer) : data(buffer) {}
    void feed(const uint8_t* bytes, size_t size);
    void reset();
};
bool parse(const char* bytes, size_t size, Snapshot& out, ArduinoJson::Allocator* allocator = nullptr);
void money(Cost cost, char* out, size_t size);
void compact(uint64_t value, char* out, size_t size);
void identity(const char* original, size_t columns, char* out, size_t size);
size_t pages(size_t pairs);
size_t nextPage(size_t page, size_t pairs); // 0 is overview, 1..N are details
size_t prevPage(size_t page);
enum class View { Today, Yesterday, Details };
enum class Swipe { None, Models, Home, Yesterday, Next, Prev };
constexpr int SwipeThreshold = 50;
Swipe swipe(int dx, int dy, View view);
struct TouchGesture {
    bool down = false, moved = false, consumed = false, suppressed = false;
    View origin = View::Today;
    int startX = 0, startY = 0;
    Swipe update(bool pressed, int x, int y, View view);
    bool tapAllowed(int x, int y) const;
    void cancel();
};
struct Orientation {
    uint8_t rotation = 0, candidate = 4; // LVGL quadrant; 4 means undecided.
    uint64_t since = 0;
    bool update(float ax, float ay, float az, uint64_t nowMs, bool touching);
};
struct HomeKey {
    bool raw = false, stable = false;
    uint64_t changed = 0;
    bool update(bool down, uint64_t nowMs);
};
bool oldDay(const Snapshot& s, uint64_t elapsedSeconds);
bool stale(const Snapshot& s, uint64_t elapsedSeconds);
}
