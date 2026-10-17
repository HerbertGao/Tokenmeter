// Run via checks/ui.py; include the real UI to inspect static state without a device API.
#include "../src/ui.cpp"
#include <array>
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 2 && LVGL_VERSION_PATCH == 2);
static constexpr int Width=480, Height=480;
alignas(64) static uint8_t buffer1[Width*40*2], buffer2[Width*40*2];
static std::array<uint8_t,Width*Height*2> frame;
static std::array<bool,Width*Height> covered;
static uint32_t clockMs;
static unsigned flushes, reads;
static lv_indev_t* input;
static lv_indev_data_t touchData;
static uint32_t tick() { return clockMs; }
static void readTouch(lv_indev_t*, lv_indev_data_t* data) { *data=touchData; ++reads; }
static void roundArea(lv_event_t* event) {
    // Same expansion as boardRound, including even y for the panel.
    auto* a=static_cast<lv_area_t*>(lv_event_get_param(event));
    a->x1 &= ~1; a->y1 &= ~1; a->x2 |= 1; a->y2 |= 1;
}
static void flush(lv_display_t* display, const lv_area_t* a, uint8_t* pixels) {
    assert(a->x1>=0 && a->y1>=0 && a->x2<Width && a->y2<Height);
    assert(a->x1%2==0 && a->x2%2==1 && a->y1%2==0 && a->y2%2==1);
    int width=lv_area_get_width(a);
    int stride=lv_draw_buf_width_to_stride(width,LV_COLOR_FORMAT_RGB565);
    for (int y=a->y1;y<=a->y2;++y) {
        memcpy(frame.data()+(y*Width+a->x1)*2,pixels+(y-a->y1)*stride,width*2);
        for (int x=a->x1;x<=a->x2;++x) covered[y*Width+x]=true;
    }
    ++flushes;
    lv_display_flush_ready(display);
}
static void advance(unsigned ms) {
    for (unsigned i=0;i<ms;i+=10) { clockMs+=std::min(10u,ms-i); lv_timer_handler(); }
}
static void textIs(lv_obj_t* object, const char* expected) {
    const char* actual=lv_label_get_text(object);
    if (strcmp(actual,expected)) fprintf(stderr,"label: expected [%s], got [%s]\n",expected,actual);
    assert(strcmp(actual,expected)==0);
}
static void state(meter::View expected, size_t expectedPage=0) {
    assert(view==expected && page==expectedPage);
    assert(lv_obj_has_flag(home,LV_OBJ_FLAG_HIDDEN)==(view==meter::View::Details));
    assert(lv_obj_has_flag(details,LV_OBJ_FLAG_HIDDEN)==(view!=meter::View::Details));
}
static void fades(lv_obj_t* target=nullptr) {
    auto* a=lv_anim_get(home,nullptr);
    auto* b=lv_anim_get(details,nullptr);
    // No other animation sources in this fixture: this also catches duplicate parent fades.
    assert(lv_anim_count_running()==unsigned(a!=nullptr)+unsigned(b!=nullptr));
    assert(lv_anim_count_running()==unsigned(target!=nullptr));
    if (target) {
        auto* fade=lv_anim_get(target,nullptr);
        assert(fade && fade->duration==120 && fade->start_value==0 && fade->end_value==255);
    }
}
static void settled() {
    advance(160);
    fades();
    for (auto* object : {home,details}) assert(lv_obj_get_style_opa(object,LV_PART_MAIN)==LV_OPA_COVER);
}
static void sample(bool pressed, int x=240, int y=260) {
    touchData.state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    touchData.point={x,y};
    unsigned before=reads;
    lv_indev_read(input); // LVGL release/click callbacks run BEFORE uiTouch, as in main.cpp.
    assert(reads==before+1);
    lv_point_t point;
    lv_indev_get_point(input,&point);
    bool touching=lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED;
    uiTouch(touching,point.x,point.y);
    if (uiPoll() && touching) lv_indev_wait_release(input);
}
static void tap(int x, int y) { sample(true,x,y); sample(false,x,y); }
static void stroke(int dx, int dy) {
    sample(true,240,260); sample(true,240+dx,260+dy); sample(false,240+dx,260+dy);
}
static void gpioHome() { lv_indev_wait_release(input); uiHome(); }
static void parseDocument(JsonDocument& doc, meter::Snapshot& result) {
    std::string json;
    serializeJson(doc,json);
    assert(meter::parse(json.data(),json.size(),result));
}
static void capture(lv_display_t* display, const char* path) {
    settled();
    covered.fill(false);
    unsigned before=flushes;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    assert(flushes>before);
    for (bool pixel : covered) assert(pixel);
    FILE* file=fopen(path,"wb");
    assert(file && fwrite(frame.data(),1,frame.size(),file)==frame.size());
    assert(fclose(file)==0);
}
int main(int argc, char** argv) {
    assert(argc==2 || argc==3);
    std::ifstream file(argv[1]);
    assert(file);
    std::string json{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    static meter::Snapshot data, legacy, unavailable, newer;
    assert(meter::parse(json.data(),json.size(),data));
    assert(data.count==4 && data.records==34 && data.yesterday.records==27);
    assert(data.epoch==1792209600 && data.dayEnd-data.epoch==12*3600 && data.offset==480);
    JsonDocument doc;
    assert(!deserializeJson(doc,json));
    doc.remove("yesterday");
    parseDocument(doc,legacy);
    assert(!legacy.yesterday.date[0]);
    assert(!deserializeJson(doc,json));
    doc["status"]="unavailable";
    doc["models"].to<JsonArray>();
    for (auto item : doc["totals"].as<JsonObject>()) item.value().set(0);
    doc["totals"]["cost"]=nullptr;
    parseDocument(doc,unavailable);
    assert(unavailable.status==meter::Status::Unavailable && unavailable.yesterday.cost.known);
    assert(!deserializeJson(doc,json));
    doc["date"]="2026-10-18";
    doc["epoch"]=data.epoch+86400;
    doc["day_end"]=data.dayEnd+86400;
    doc["yesterday"]["date"]="2026-10-17";
    doc["yesterday"]["totals"]["cost"]=2.50;
    doc["yesterday"]["totals"]["tokens"]=2600000;
    doc["yesterday"]["totals"]["input"]=200000;
    doc["yesterday"]["totals"]["output"]=100000;
    doc["yesterday"]["totals"]["cache_read"]=2300000;
    doc["yesterday"]["totals"]["records"]=29;
    parseDocument(doc,newer);

    lv_init();
    lv_tick_set_cb(tick);
    auto* display=lv_display_create(Width,Height);
    assert(display);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display,flush);
    lv_display_set_buffers(display,buffer1,buffer2,sizeof buffer1,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(display,roundArea,LV_EVENT_INVALIDATE_AREA,nullptr);
    input=lv_indev_create();
    assert(input);
    lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input,display);
    lv_indev_set_read_cb(input,readTouch);
    lv_indev_set_mode(input,LV_INDEV_MODE_EVENT); // Explicit reads, no extra timer-driven input samples.
    uiBegin();
    uiUpdate(&data,60,true);
    lv_refr_now(display);
    state(meter::View::Today); fades();
    textIs(dateLabel,"2026-10-17"); textIs(period,"TODAY"); textIs(amount,"$0.79");
    textIs(tokens,"3.16M"); textIs(records,"34 records");
    textIs(lv_obj_get_child(modelsButton,0),"MODELS >");
    if (argc==3) capture(display,(std::string(argv[2])+"/home.rgb565").c_str());

    stroke(100,0); state(meter::View::Yesterday); fades(home);
    textIs(dateLabel,"2026-10-16"); textIs(period,"YESTERDAY"); textIs(amount,"$1.24");
    textIs(tokens,"2.40M"); textIs(numbers[0],"180.00K"); textIs(numbers[1],"20.00K");
    textIs(numbers[2],"2.20M"); textIs(records,"27 records");
    textIs(lv_obj_get_child(modelsButton,0),"HOME >");
    assert(!lv_obj_has_state(modelsButton,LV_STATE_DISABLED));
    settled();
    if (argc==3) capture(display,(std::string(argv[2])+"/yesterday.rgb565").c_str());
    stroke(100,0); state(meter::View::Yesterday); fades();
    stroke(0,100); state(meter::View::Yesterday); fades();
    stroke(0,-100); state(meter::View::Yesterday); fades();
    // Release over a button after a non-tap must not trigger its LV_EVENT_CLICKED action.
    sample(true,388,450); sample(false,450,450);
    state(meter::View::Yesterday); fades();
    sample(true,388,450); sample(false,388,350);
    state(meter::View::Yesterday); fades();
    stroke(-100,0); state(meter::View::Today); fades(home); settled();
    stroke(100,0); settled(); tap(388,450);
    state(meter::View::Today); fades(home); settled();
    textIs(dateLabel,"2026-10-17"); textIs(amount,"$0.79");
    textIs(lv_obj_get_child(modelsButton,0),"MODELS >");
    puts("PASS today/yesterday dates, totals, gestures and native HOME > click");

    tap(388,450); state(meter::View::Details,1); fades(details);
    textIs(pageCount,"1/2"); textIs(models[0],"gpt-5.4"); textIs(costs[0],"$0.35");
    settled();
    stroke(0,100); state(meter::View::Details,1); fades();
    tap(224,432); state(meter::View::Details,1); fades(); // Disabled PREV.
    stroke(0,-100); state(meter::View::Details,2); fades(details); settled();
    textIs(pageCount,"2/2"); textIs(providers[0],"openrouter/"); textIs(costs[0],"$0.13");
    stroke(0,-100); state(meter::View::Details,2); fades();
    tap(372,432); state(meter::View::Details,2); fades(); // Disabled NEXT.
    stroke(0,100); state(meter::View::Details,1); fades(details); settled();
    tap(372,432); state(meter::View::Details,2); fades(details); settled();
    tap(224,432); state(meter::View::Details,1); fades(details); settled();
    tap(92,432); state(meter::View::Today); fades(home); settled();
    stroke(-100,0); state(meter::View::Details,1); fades(details); settled();
    stroke(100,0); state(meter::View::Today); fades(home); settled();
    puts("PASS native MODELS / HOME / PREV / NEXT clicks, today left swipe and detail vertical boundaries");

    for (auto origin : {meter::View::Today,meter::View::Yesterday,meter::View::Details}) {
        if (origin==meter::View::Yesterday) stroke(100,0);
        if (origin==meter::View::Details) stroke(-100,0);
        settled(); state(origin,origin==meter::View::Details?1:0);
        sample(true,240,260);
        gpioHome(); state(meter::View::Today);
        fades(origin==meter::View::Today?nullptr:home);
        sample(true,360,260); sample(true,100,260); // Same held stroke stays cancelled.
        state(meter::View::Today);
        sample(false,100,260); settled();
        stroke(100,0); state(meter::View::Yesterday); // A new stroke works again.
        gpioHome(); sample(false); settled();
    }
    // Cancel a queued action, not just a stroke below threshold.
    uiTouch(true,240,260); uiTouch(true,340,260);
    assert(pending==meter::Swipe::Yesterday);
    gpioHome(); assert(!uiPoll()); sample(false); state(meter::View::Today); settled();
    puts("PASS GPIO uiHome from all views cancels held/queued gestures");

    uiUpdate(&legacy,60,true); fades(); stroke(100,0); settled();
    state(meter::View::Yesterday); textIs(period,"YESTERDAY · NO DATA"); textIs(dateLabel,"--");
    textIs(amount,"--"); textIs(tokens,"--"); textIs(records,"WAITING");
    for (auto* number : numbers) textIs(number,"--");
    textIs(lv_obj_get_child(modelsButton,0),"HOME >");
    tap(388,450); state(meter::View::Today); settled();
    uiUpdate(&unavailable,60,true); fades(); textIs(period,"SOURCE UNAVAILABLE");
    assert(lv_obj_has_state(modelsButton,LV_STATE_DISABLED));
    tap(388,450); state(meter::View::Today); fades();
    stroke(100,0); state(meter::View::Yesterday); textIs(amount,"$1.24"); settled();
    tap(388,450); state(meter::View::Today); settled();
    uiUpdate(&data,60,true); stroke(100,0); settled();
    uiUpdate(&data,119,true); textIs(period,"YESTERDAY"); fades();
    uiUpdate(&data,120,true); textIs(period,"YESTERDAY · STALE"); fades();
    uiUpdate(&data,12*3600-1,true); textIs(period,"YESTERDAY · STALE"); fades();
    uiUpdate(&data,12*3600,true); textIs(period,"OLD DAY · STALE"); fades();
    textIs(dateLabel,"2026-10-16"); textIs(amount,"$1.24");
    tap(388,450); settled(); textIs(dateLabel,"2026-10-17"); textIs(period,"OLD DAY · STALE");
    stroke(-100,0); settled(); textIs(detailTitle,"OLD DAY MODELS"); textIs(detailState,"provider/model · STALE");
    gpioHome(); sample(false); settled(); uiUpdate(&data,60,true); stroke(100,0); settled();
    uiUpdate(&newer,60,true); state(meter::View::Yesterday); fades();
    textIs(dateLabel,"2026-10-17"); textIs(amount,"$2.50"); textIs(records,"29 records");
    textIs(tokens,"2.60M"); textIs(numbers[0],"200.00K"); textIs(numbers[1],"100.00K");
    textIs(numbers[2],"2.30M"); textIs(period,"YESTERDAY");
    puts("PASS legacy NO DATA, unavailable today, root freshness/midnight and in-place yesterday refresh");

    gpioHome(); sample(false); settled(); uiUpdate(&data,60,true);
    stroke(-100,0); advance(60); fades(details);
    auto* fade=lv_anim_get(details,nullptr);
    int32_t at=fade->act_time;
    auto opacity=lv_obj_get_style_opa(details,LV_PART_MAIN);
    assert(opacity>LV_OPA_TRANSP && opacity<LV_OPA_COVER);
    uiUpdate(&data,61,true); fades(details);
    assert(lv_anim_get(details,nullptr)->act_time==at);
    assert(lv_obj_get_style_opa(details,LV_PART_MAIN)==opacity);
    stroke(0,100); state(meter::View::Details,1); fades(details);
    assert(lv_anim_get(details,nullptr)->act_time==at); // No new fade on a boundary.
    advance(70); fades();
    assert(lv_obj_get_style_opa(details,LV_PART_MAIN)==LV_OPA_COVER);
    for (int i=0;i<4;++i) {
        stroke(100,0); state(meter::View::Today); fades(home); advance(40);
        stroke(100,0); state(meter::View::Yesterday); fades(home); advance(40);
        tap(388,450); state(meter::View::Today); fades(home); advance(40);
        tap(388,450); state(meter::View::Details,1); fades(details); advance(40);
        stroke(0,-100); state(meter::View::Details,2); fades(details); advance(40);
        uiUpdate(&data,60,true); fades(details);
    }
    settled();
    puts("PASS 120ms native fades: refresh/boundary do not restart; rapid interrupts leave <=1 parent animation and opaque finish");

    for (auto origin : {meter::View::Details,meter::View::Yesterday,meter::View::Today}) {
        if (origin!=meter::View::Details) { gpioHome(); sample(false); settled(); }
        if (origin==meter::View::Yesterday) { stroke(100,0); settled(); }
        size_t oldPage=page;
        const auto* oldSnapshot=snapshot;
        std::string oldData(reinterpret_cast<const char*>(snapshot),sizeof *snapshot);
        std::string oldDate=lv_label_get_text(dateLabel), oldAmount=lv_label_get_text(amount);
        sample(true,240,260);
        for (auto rotation : {LV_DISPLAY_ROTATION_90,LV_DISPLAY_ROTATION_180,LV_DISPLAY_ROTATION_270,LV_DISPLAY_ROTATION_0}) {
            lv_display_set_rotation(display,rotation);
            uiCancelTouch(); // Same cancellation performed after boardPoll rotation in main.
            state(origin,oldPage); assert(snapshot==oldSnapshot); fades();
            assert(memcmp(snapshot,oldData.data(),oldData.size())==0);
            textIs(dateLabel,oldDate.c_str()); textIs(amount,oldAmount.c_str());
        }
        sample(false); state(origin,oldPage);
    }
    lv_refr_now(display);
    assert(flushes && reads);
    lv_deinit();
    puts("PASS manual display rotation preserves view/page/snapshot; all native UI assertions passed");
}
