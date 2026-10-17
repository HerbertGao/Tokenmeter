#include "core.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace meter {
void Framer::reset() { used = 0; dropping = ready = error = false; }
void Framer::feed(const uint8_t* bytes, size_t size) {
    if (size > 180) dropping = true;
    for (size_t i = 0; i < size; ++i) {
        if (ready) { error = true; continue; } // only one outstanding frame
        if (bytes[i] == '\n') {
            data[used] = 0;
            ready = true;
            error = dropping || used == 0;
        } else if (!dropping) {
            if (used == FrameLimit) dropping = true;
            else data[used++] = static_cast<char>(bytes[i]);
        }
    }
}

static bool utf8(const char* s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = static_cast<unsigned char>(s[i++]);
        if (c < 0x80) { if (!c) return false; continue; }
        unsigned more, value, minimum;
        if (c >= 0xc2 && c <= 0xdf) { more = 1; value = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; value = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; value = c & 7; minimum = 0x10000; }
        else return false;
        if (i + more > n) return false;
        while (more--) {
            unsigned b = static_cast<unsigned char>(s[i++]);
            if ((b & 0xc0) != 0x80) return false;
            value = (value << 6) | (b & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
static bool integer(JsonVariantConst v, uint64_t& out) {
    if (!v.is<uint64_t>()) return false;
    out = v.as<uint64_t>();
    return out <= MaxInteger;
}
static bool costValue(JsonObjectConst o, Cost& cost) {
    if (!o["cost"].isUnbound() && o["cost"].isNull()) { cost = {}; return true; }
    if (!o["cost"].is<double>()) return false;
    double v = o["cost"].as<double>();
    if (!std::isfinite(v) || v < 0 || v > 1e12) return false;
    cost = {v, true};
    return true;
}
static bool copyIdentity(JsonVariantConst v, char* dest) {
    if (!v.is<const char*>()) return false;
    JsonString s = v.as<JsonString>();
    if (!s.size() || s.size() > IdentityLimit || !utf8(s.c_str(), s.size())) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s.c_str()[i];
        if (c < 0x20 || c == 0x7f || (c == 0xc2 && i + 1 < s.size() &&
            static_cast<unsigned char>(s.c_str()[i+1]) >= 0x80 &&
            static_cast<unsigned char>(s.c_str()[i+1]) <= 0x9f)) return false;
    }
    memcpy(dest, s.c_str(), s.size());
    dest[s.size()] = 0;
    return true;
}
// Gregorian civil date -> Unix day. No platform time_t range or timezone dependency.
static bool dateDay(const char* s, int64_t& days) {
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; ++i) if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + s[3]-'0';
    unsigned m = (s[5]-'0')*10 + s[6]-'0', d = (s[8]-'0')*10 + s[9]-'0';
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    if (y == 0 || m < 1 || m > 12 || d < 1 || d > unsigned(lengths[m-1] + (m == 2 && leap))) return false;
    y -= m <= 2;
    int era = y / 400;
    unsigned yoe = y - era * 400;
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    days = era * 146097LL + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return true;
}
struct Reader {
    const char* data; size_t size, pos = 0;
    int read() { return pos < size ? static_cast<unsigned char>(data[pos++]) : -1; }
    int peek() { return pos < size ? static_cast<unsigned char>(data[pos]) : -1; }
    size_t readBytes(char* out, size_t n) {
        n = std::min(n, size - pos); memcpy(out, data + pos, n); pos += n; return n;
    }
};
static bool add(uint64_t& a, uint64_t b) {
    if (b > MaxInteger - a) return false;
    a += b; return true;
}
static bool before(const Row& a, const Row& b) {
    if (a.cost.known != b.cost.known) return a.cost.known;
    if (a.cost.known && a.cost.value != b.cost.value) return a.cost.value > b.cost.value;
    int c = strcmp(a.provider, b.provider);
    return c ? c < 0 : strcmp(a.model, b.model) < 0;
}
static bool readSummary(JsonObjectConst root, DaySummary& out, int64_t& day) {
    if (!root["date"].is<const char*>() || !integer(root["issues"], out.issues)) return false;
    JsonString date = root["date"].as<JsonString>();
    if (date.size() != 10 || !dateDay(date.c_str(), day)) return false;
    memcpy(out.date, date.c_str(), 10);
    if (!root["status"].is<const char*>()) return false;
    JsonString statusString = root["status"].as<JsonString>();
    const char* status = statusString.c_str();
    if (statusString.size() != strlen(status)) return false;
    if (!strcmp(status, "ok")) out.status = Status::Ok;
    else if (!strcmp(status, "partial")) out.status = Status::Partial;
    else if (!strcmp(status, "unavailable")) out.status = Status::Unavailable;
    else return false;
    if (!root["totals"].is<JsonObjectConst>()) return false;
    JsonObjectConst t = root["totals"].as<JsonObjectConst>();
    if (!costValue(t, out.cost) || !integer(t["tokens"], out.tokens) || !integer(t["input"], out.input) ||
        !integer(t["output"], out.output) || !integer(t["cache_read"], out.cacheRead) ||
        !integer(t["cache_write"], out.cacheWrite) || !integer(t["records"], out.records) ||
        !integer(t["unpriced"], out.unpriced) || out.unpriced > out.records) return false;
    if (out.status == Status::Unavailable)
        return !out.cost.known && !out.tokens && !out.input && !out.output && !out.cacheRead &&
               !out.cacheWrite && !out.records && !out.unpriced;
    if (!out.records)
        return !out.tokens && !out.input && !out.output && !out.cacheRead && !out.cacheWrite &&
               (out.cost.known ? out.cost.value == 0 : out.status == Status::Partial);
    return out.cost.known != (out.unpriced == out.records);
}
bool parse(const char* bytes, size_t size, Snapshot& out, ArduinoJson::Allocator* allocator) {
    if (!size || size > FrameLimit || !utf8(bytes, size)) return false;
    Reader reader{bytes, size};
    JsonDocument doc;
    if (allocator) doc = JsonDocument(allocator);
    if (deserializeJson(doc, reader, DeserializationOption::NestingLimit(8))) return false;
    while (reader.pos < size) {
        char c = bytes[reader.pos++];
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    if (!doc.is<JsonObject>() || !doc["v"].is<unsigned>() || doc["v"].as<unsigned>() != 1) return false;
    JsonObjectConst root = doc.as<JsonObjectConst>();
    new (&out) Snapshot{}; // construct in PSRAM, never a snapshot-sized stack temporary
    uint64_t version;
    int64_t day;
    if (!integer(root["v"], version) || version != 1 || !integer(root["epoch"], out.epoch) || out.epoch == 0 ||
        !integer(root["day_end"], out.dayEnd) || out.dayEnd <= out.epoch || out.dayEnd - out.epoch > 27*3600 ||
        !root["utc_offset"].is<int>() || !readSummary(root, out, day)) return false;
    out.offset = root["utc_offset"].as<int>();
    if (out.offset < -840 || out.offset > 840) return false;
    int64_t local = static_cast<int64_t>(out.epoch) + out.offset * 60;
    int64_t localDay = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    if (day != localDay) return false;
    if (root.containsKey("yesterday")) {
        int64_t previousDay;
        if (!root["yesterday"].is<JsonObjectConst>() ||
            !readSummary(root["yesterday"].as<JsonObjectConst>(), out.yesterday, previousDay) || previousDay != day-1) return false;
    }
    if (!root["models"].is<JsonArrayConst>()) return false;
    JsonArrayConst rows = root["models"].as<JsonArrayConst>();
    if (rows.size() > PairLimit) return false;
    uint64_t tokens = 0, records = 0, unpriced = 0;
    double sum = 0;
    bool known = rows.size() == 0 && !(out.status == Status::Partial && !out.cost.known);
    for (JsonVariantConst v : rows) {
        if (!v.is<JsonObjectConst>()) return false;
        JsonObjectConst obj = v.as<JsonObjectConst>();
        Row& row = out.rows[out.count++];
        if (!copyIdentity(obj["provider"], row.provider) || !copyIdentity(obj["model"], row.model) ||
            !costValue(obj, row.cost) || !integer(obj["tokens"], row.tokens) ||
            !integer(obj["records"], row.records) || row.records == 0 ||
            !integer(obj["unpriced"], row.unpriced) || row.unpriced > row.records) return false;
        if (row.cost.known == (row.unpriced == row.records)) return false;
        if (!add(tokens, row.tokens) || !add(records, row.records) || !add(unpriced, row.unpriced)) return false;
        if (row.cost.known) { sum += row.cost.value; known = true; }
        for (size_t i = 0; i + 1 < out.count; ++i)
            if (!strcmp(row.provider, out.rows[i].provider) && !strcmp(row.model, out.rows[i].model)) return false;
    }
    if (tokens != out.tokens || records != out.records || unpriced != out.unpriced) return false;
    if (out.status == Status::Unavailable) return out.count == 0;
    // Absolute cap stays below one cent even for the largest allowed monetary value.
    double tolerance = std::min(0.004, std::max(1e-9, std::fabs(sum) * 2e-15));
    if (out.cost.known != known || (known && std::fabs(out.cost.value - sum) > tolerance)) return false;
    if (!out.count && (out.input || out.output || out.cacheRead || out.cacheWrite)) return false;
    std::sort(out.rows, out.rows + out.count, before);
    return true;
}
void money(Cost cost, char* out, size_t size) {
    if (!cost.known) snprintf(out, size, "--");
    else if (cost.value > 0 && cost.value < 0.01) snprintf(out, size, "<$0.01");
    else snprintf(out, size, "$%.2f", cost.value);
}
void compact(uint64_t value, char* out, size_t size) {
    if (value < 1000) snprintf(out, size, "%llu", static_cast<unsigned long long>(value));
    else {
        const char* units[] = {"K", "M", "B", "T", "Q"};
        double scaled = value; unsigned unit = 0;
        scaled /= 1000;
        while (scaled >= 999.995 && unit < 4) { scaled /= 1000; ++unit; }
        snprintf(out, size, "%.2f%s", scaled, units[unit]);
    }
}
void identity(const char* original, size_t columns, char* out, size_t size) {
    // ASCII display fallback only. State retains the full validated UTF-8 identity.
    char ascii[IdentityLimit + 1]; size_t n = 0;
    for (size_t i = 0; original[i];) {
        unsigned char c = original[i++];
        if (c >= 0x80) {
            while ((static_cast<unsigned char>(original[i]) & 0xc0) == 0x80) ++i;
            ascii[n++] = '?';
        } else ascii[n++] = c >= 32 && c <= 126 ? c : '?';
    }
    columns = std::min(columns, size ? size - 1 : 0);
    if (!size) return;
    if (n <= columns) { memcpy(out, ascii, n); out[n] = 0; return; }
    if (columns < 3) { memset(out, '.', columns); out[columns] = 0; return; }
    size_t left = (columns - 3 + 1)/2, right = columns - 3 - left;
    memcpy(out, ascii, left); memcpy(out + left, "...", 3);
    memcpy(out + left + 3, ascii + n - right, right); out[columns] = 0;
}
size_t pages(size_t pairs) { return (pairs + 1)/2; }
size_t nextPage(size_t page, size_t pairs) { return std::min(page + 1, pages(pairs)); }
size_t prevPage(size_t page) { return page > 1 ? page - 1 : page; }
Swipe swipe(int dx, int dy, View view) {
    if (std::abs(dx) >= SwipeThreshold && std::abs(dx) > std::abs(dy)*3/2) {
        if (view == View::Today) return dx < 0 ? Swipe::Models : Swipe::Yesterday;
        if ((dx < 0 && view == View::Yesterday) || (dx > 0 && view == View::Details)) return Swipe::Home;
    }
    if (view == View::Details && std::abs(dy) >= SwipeThreshold && std::abs(dy) > std::abs(dx)*3/2)
        return dy < 0 ? Swipe::Next : Swipe::Prev;
    return Swipe::None;
}
Swipe TouchGesture::update(bool pressed, int x, int y, View view) {
    if (suppressed) {
        if (!pressed) suppressed = false;
        return Swipe::None;
    }
    if (!down) {
        if (pressed) {
            down = true; moved = consumed = false; origin = view;
            startX = x; startY = y;
        }
        return Swipe::None;
    }
    Swipe action = Swipe::None;
    if (!consumed) {
        int dx = x - startX, dy = y - startY;
        moved = moved || std::abs(dx) >= SwipeThreshold || std::abs(dy) >= SwipeThreshold;
        action = swipe(dx, dy, origin);
        if (action != Swipe::None) consumed = true;
    }
    if (!pressed) down = false;
    return action;
}
bool TouchGesture::tapAllowed(int x, int y) const {
    return down && !suppressed && !moved && !consumed &&
           std::abs(x-startX) < SwipeThreshold && std::abs(y-startY) < SwipeThreshold;
}
void TouchGesture::cancel() {
    down = false; suppressed = moved = consumed = true;
}
bool Orientation::update(float ax, float ay, float az, uint64_t now, bool touching) {
    float x = std::fabs(ax), y = std::fabs(ay), g2 = ax*ax + ay*ay + az*az;
    uint8_t target = 4;
    if (!touching && std::isfinite(g2) && g2 >= 0.49f && g2 <= 1.69f &&
        std::max(x,y) >= 0.5f && std::fabs(x-y) >= 0.15f) {
        // Match the board axes; LVGL's 90-degree software rotation is counterclockwise.
        target = x > y ? (ax > 0 ? 0 : 2) : (ay > 0 ? 1 : 3);
    }
    if (target == 4 || target == rotation) { candidate = 4; return false; }
    if (target != candidate) { candidate = target; since = now; return false; }
    if (now - since < 400) return false;
    rotation = target; candidate = 4;
    return true;
}
bool HomeKey::update(bool down, uint64_t now) {
    if (down != raw) { raw = down; changed = now; }
    if (stable != raw && now - changed >= 30) { stable = raw; return stable; }
    return false;
}
bool oldDay(const Snapshot& s, uint64_t elapsed) { return elapsed >= s.dayEnd - s.epoch; }
bool stale(const Snapshot& s, uint64_t elapsed) { return elapsed >= 120 || oldDay(s, elapsed); }
}
