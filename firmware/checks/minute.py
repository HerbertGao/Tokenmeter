"""Replay the actual UI scheduling code without hardware or real-time waits."""
from pathlib import Path
import subprocess
import tempfile

firmware = Path(__file__).resolve().parents[1]
main = (firmware / "src/main.cpp").read_text()
ui = (firmware / "src/ui.cpp").read_text()
schedule = main.split("void loop() {\n", 1)[1].split("    lv_timer_handler();", 1)[0]
start = ui.index('    if (!s) snprintf(text,sizeof text,"--");\n    else if (elapsed<3600)')
age = ui[start:ui.index("    setText(age,text);", start)]
source = r'''
#include "core.h"
#include <cassert>
#include <cstdio>
#include <string>
static meter::Snapshot snapshot;
static meter::Snapshot *current=&snapshot, *staging=nullptr;
static bool haveSnapshot=false, dirty=true, connected=false;
static uint64_t receivedUs=0, now=0, shownAge=0;
static int updates=0;
uint32_t millis(){return static_cast<uint32_t>(now);}
int64_t esp_timer_get_time(){return now*1000;}
bool bleConnected(){return connected;}
bool blePoll(meter::Snapshot*&,meter::Snapshot*&,uint64_t& received){
    if(!dirty)return false;
    dirty=false;received=now*1000;return true;
}
void uiUpdate(const meter::Snapshot*,uint64_t elapsed,bool){++updates;shownAge=elapsed;}
void pollUI(){
''' + schedule + r'''}
std::string ageText(uint64_t elapsed){const auto* s=current;char text[32];
''' + age + r'''return text;}
int main(){
    snapshot.epoch=1000;snapshot.dayEnd=2000;
    pollUI();assert(updates==1);
    for(now=1000;now<60000;now+=1000)pollUI();
    assert(updates==1);pollUI();assert(updates==2&&shownAge==60);
    now=61000;connected=true;pollUI();assert(updates==3);
    now=119000;pollUI();assert(updates==3);
    now=120000;pollUI();assert(updates==4&&shownAge==120);
    snapshot.epoch=5000;snapshot.dayEnd=5020;
    now=121000;dirty=true;pollUI();assert(updates==5&&shownAge==0);
    now=140999;pollUI();assert(updates==5);
    now=141000;pollUI();assert(updates==6&&shownAge==20);
    now=uint64_t(UINT32_MAX)-1000;pollUI();assert(updates==7);
    now+=60000;pollUI();assert(updates==8);
    assert(ageText(0)=="0m ago"&&ageText(59)=="0m ago");
    assert(ageText(60)=="1m ago"&&ageText(3599)=="59m ago");
    assert(ageText(3600)=="1h ago"&&ageText(86400)=="1d ago");
    current=nullptr;assert(ageText(0)=="--");
    puts("PASS: minute cadence, immediate data/connection/STALE/day-end, millis rollover and minute labels");
}
'''
with tempfile.TemporaryDirectory(prefix="tokenmeter-minute-") as directory:
    cpp, binary = Path(directory) / "check.cpp", Path(directory) / "check"
    cpp.write_text(source)
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(firmware / "src"),
        "-I" + str(firmware / ".pio/libdeps/waveshare_amoled_216/ArduinoJson/src"),
        str(firmware / "src/core.cpp"), str(cpp), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
