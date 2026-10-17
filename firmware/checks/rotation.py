"""Check board calibration, real SensorLib/LVGL transforms and recorded tap regressions."""
from pathlib import Path
import re
import subprocess
import tempfile

firmware = Path(__file__).resolve().parents[1]
libs = firmware / ".pio/libdeps/waveshare_amoled_216"
indev = (libs / "lvgl/src/indev/lv_indev.c").read_text()
display = (libs / "lvgl/src/display/lv_display.c").read_text()
sensor = (libs / "SensorLib/src/TouchDrvInterface.cpp").read_text()
board = (firmware / "src/board.cpp").read_text()

def function(source, signature):
    match = re.search(re.escape(signature) + r".*?^}", source, re.M | re.S)
    assert match, signature
    return match.group(0)

body = indev.split("static void indev_pointer_proc(lv_indev_t * i, lv_indev_data_t * data)\n{", 1)[1]
inverse = body[:body.index("    /*Simple sanity check*/")]
assign = "\n".join(line for line in body.splitlines() if "i->pointer.act_point." in line and "= data->point." in line)
config = "\n".join(line for line in board.splitlines() if re.search(r"touch\.set(MaxCoordinates|SwapXY|MirrorXY)\(", line))
source = r'''
#include "core.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
class TouchDrvInterface {
public:
 uint16_t __xMax=0,__yMax=0;bool __swapXY=false,__mirrorX=false,__mirrorY=false;
 void setMaxCoordinates(uint16_t,uint16_t);void setSwapXY(bool);void setMirrorXY(bool,bool);
 void updateXY(uint8_t,int16_t*,int16_t*);
};
enum lv_display_rotation_t {LV_DISPLAY_ROTATION_0,LV_DISPLAY_ROTATION_90,LV_DISPLAY_ROTATION_180,LV_DISPLAY_ROTATION_270};
struct lv_point_t {int32_t x,y;};
struct lv_area_t {int32_t x1,y1,x2,y2;};
struct lv_display_t {lv_display_rotation_t rotation;int32_t hor_res=480,ver_res=480;};
struct lv_indev_data_t {lv_point_t point;};
enum {LV_INDEV_TYPE_POINTER,LV_INDEV_TYPE_BUTTON};
struct lv_indev_t {lv_display_t* disp;struct{lv_point_t last_raw_point,act_point;}pointer;int type=LV_INDEV_TYPE_POINTER;};
int32_t lv_area_get_width(lv_area_t* a){return a->x2-a->x1+1;}
int32_t lv_area_get_height(lv_area_t* a){return a->y2-a->y1+1;}
lv_display_rotation_t lv_display_get_rotation(lv_display_t* d){return d->rotation;}
'''
source += "\n".join(function(sensor, "void TouchDrvInterface::" + name + "(") for name in (
    "setMaxCoordinates", "setSwapXY", "setMirrorXY", "updateXY"))
source += "\n" + function(display, "void lv_display_rotate_area(")
source += "\n" + function(indev, "void lv_indev_get_point(")
source += "\nvoid process(lv_indev_t* i,lv_indev_data_t* data){" + inverse + assign + "}\n"
source += "int main(){TouchDrvInterface touch;\n" + config + r'''
 for(int r=0;r<4;++r){
  lv_display_t display{static_cast<lv_display_rotation_t>(r)};lv_indev_t input{&display,{}};
  auto native=[&](int16_t x,int16_t y){
   touch.updateXY(1,&x,&y);
   lv_indev_data_t data{{std::clamp<int>(x,0,479),std::clamp<int>(y,0,479)}};
   process(&input,&data);lv_point_t p;lv_indev_get_point(&input,&p);return p;
  };
  auto point=[&](int x,int y){
   lv_area_t area{x,y,x,y};lv_display_rotate_area(&display,&area);
   auto p=native(area.x1,area.y1);assert(p.x==x&&p.y==y);return p;
  };
  for(int x:{0,32,92,200,388,479})for(int y:{0,81,200,432,450,479})point(x,y);
  meter::TouchGesture swipe;
  auto a=point(200,280),b=point(100,280);
  assert(swipe.update(true,a.x,a.y,false)==meter::Swipe::None);
  assert(swipe.update(true,b.x,b.y,false)==meter::Swipe::Models);
  assert(swipe.update(false,b.x,b.y,true)==meter::Swipe::None);
  a=point(320,430);b=point(420,430);
  assert(swipe.update(true,a.x,a.y,true)==meter::Swipe::None);
  assert(swipe.update(true,b.x,b.y,true)==meter::Swipe::Home);
  struct Tap {int rotation,x,y;};
  for(auto tap:{Tap{2,93,22},Tap{2,87,19},Tap{1,445,66},Tap{1,466,59}}){
   if(tap.rotation!=r)continue;
   auto p=native(tap.x,tap.y);
   assert(p.x>=328&&p.x<448&&p.y>=426&&p.y<474);
  }
  printf("PASS: %d-degree native CST -> board calibration -> LVGL point, buttons and swipes\n",r*90);
 }
}
'''
with tempfile.TemporaryDirectory(prefix="tokenmeter-rotation-") as directory:
    cpp, binary = Path(directory) / "check.cpp", Path(directory) / "check"
    cpp.write_text(source)
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(firmware / "src"), "-I" + str(libs / "ArduinoJson/src"),
        str(firmware / "src/core.cpp"), str(cpp), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
