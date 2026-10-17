#include "ui.h"
#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern const lv_font_t font_16, font_20, font_24, font_32, font_48, font_96;
}
static constexpr uint32_t White=0xeeeeee, Gray=0xb5b5b5, Coral=0xf09082, Blue=0x84bfd8, Gold=0xf1be58;
static const meter::Snapshot* snapshot;
static uint64_t elapsed;
static bool connected;
static size_t page = 0;
static meter::View view = meter::View::Today;
static meter::Swipe pending = meter::Swipe::None;
static meter::TouchGesture gesture;
static lv_obj_t *dateLabel, *home, *details, *period, *amount, *currency, *age, *tokens, *records;
static lv_obj_t *numbers[3], *modelsButton, *detailTitle, *detailState, *dayTotal, *dayCaption, *pairCount, *pageCount;
static lv_obj_t *providers[2], *models[2], *costs[2], *rowTokens[2], *rowRecords[2];
static lv_obj_t *homeButton, *prevButton, *nextButton, *pressTarget;

static lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    auto* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o); lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}
static void setText(lv_obj_t* obj, const char* text) {
    if (strcmp(lv_label_get_text(obj),text)) lv_label_set_text(obj,text);
}
static void setFont(lv_obj_t* obj, const lv_font_t* font) {
    if (lv_obj_get_style_text_font(obj,LV_PART_MAIN)!=font) lv_obj_set_style_text_font(obj,font,0);
}
static lv_obj_t* label(lv_obj_t* parent, int x, int y, int w, const lv_font_t* font,
                        uint32_t color, const char* text, bool right = false) {
    auto* l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y); lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(l, font, 0); lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(l, text); return l;
}
static void hidden(lv_obj_t* obj, bool hide) {
    if (lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)==hide) return;
    if (hide) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
static void disabled(lv_obj_t* obj, bool disable) {
    if (disable) lv_obj_add_state(obj, LV_STATE_DISABLED);
    else lv_obj_remove_state(obj, LV_STATE_DISABLED);
}
static void refresh();
static void navigate(meter::Swipe action) {
    auto oldView=view;
    size_t oldPage=page;
    if (action==meter::Swipe::Home) { view=meter::View::Today; page=0; }
    else if (action==meter::Swipe::Yesterday) { view=meter::View::Yesterday; page=0; }
    else if (action==meter::Swipe::Models) {
        if (snapshot && snapshot->status!=meter::Status::Unavailable && snapshot->count) {
            view=meter::View::Details; page=1;
        }
    } else if (view==meter::View::Details && action==meter::Swipe::Next)
        page=meter::nextPage(page,snapshot?snapshot->count:0);
    else if (view==meter::View::Details && action==meter::Swipe::Prev)
        page=meter::prevPage(page);
    refresh();
    if (view!=oldView || page!=oldPage) {
        // Reset interrupted native fades; background data refreshes never start one.
        for (auto* object : {home,details}) {
            lv_anim_delete(object,nullptr);
            lv_obj_remove_local_style_prop(object,LV_STYLE_OPA,0);
        }
        lv_obj_fade_in(view==meter::View::Details?details:home,120,0);
    }
}
static void click(lv_event_t* e) {
    // Release callbacks precede uiTouch; inspect the final point before accepting a tap.
    auto* input=lv_event_get_indev(e);
    if (!input || pressTarget!=lv_event_get_target(e)) return;
    lv_point_t point;
    lv_indev_get_point(input,&point);
    if (!gesture.tapAllowed(point.x,point.y)) return;
    auto action=static_cast<meter::Swipe>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (view==meter::View::Yesterday && pressTarget==modelsButton) action=meter::Swipe::Home;
    navigate(action);
}
static lv_obj_t* button(lv_obj_t* parent, int x, int y, int w, const char* text, meter::Swipe action, bool dark=false) {
    auto* b = lv_button_create(parent);
    lv_obj_remove_style_all(b); lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, 48);
    lv_obj_set_style_bg_color(b, lv_color_hex(dark ? 0 : 0x202326), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_outline_color(b, lv_color_hex(Blue), LV_STATE_FOCUSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    auto* l = label(b, 0, 0, w, &font_16, White, text);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(l);
    lv_obj_add_event_cb(b, click, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
    return b;
}
static void fittedMoney(lv_obj_t* l, meter::Cost cost, int width, bool large) {
    char text[40]; meter::money(cost, text, sizeof text);
    const lv_font_t* big[] = {&font_96, &font_48, &font_32};
    const lv_font_t* small[] = {&font_32, &font_24, &font_20, &font_16};
    auto fonts = large ? big : small;
    size_t count = large ? 3 : 4;
    const lv_font_t* selected = fonts[count-1];
    for (size_t i=0; i<count; ++i) {
        if (lv_text_get_width(text, strlen(text), fonts[i], 0) <= width) { selected = fonts[i]; break; }
    }
    setFont(l, selected); setText(l, text);
}
void uiBegin() {
    auto* root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    // Pixel Pi mark, not a splash or transmitted bitmap.
    box(root,34,30,24,8,Coral); box(root,50,38,8,8,Coral);
    box(root,34,38,8,24,0x4d9abf); box(root,42,46,8,8,0x4d9abf);
    box(root,58,46,8,16,Gold);
    label(root,79,35,230,&font_20,White,"PI METER");
    dateLabel=label(root,320,39,128,&font_16,Gray,"--",true);
    box(root,32,80,416,1,0x3a3a3a);
    home=box(root,0,81,480,399,0); details=box(root,0,81,480,399,0);
    // Coordinates below are relative to y=81, matching the approved previews.
    period=label(home,31,17,418,&font_16,Coral,"NO DATA YET");
    amount=label(home,26,31,422,&font_96,Coral,"--");
    currency=label(home,32,149,306,&font_16,Gray,"USD · WAITING");
    age=label(home,340,149,108,&font_16,Gray,"--",true);
    tokens=label(home,29,184,300,&font_48,White,"--");
    label(home,208,209,240,&font_20,Gray,"tokens");
    box(home,32,255,416,1,0x3a3a3a);
    const int xs[]={31,184,319}; const char* captions[]={"IN","OUT","CACHE READ"};
    for (int i=0;i<3;++i) {
        label(home,xs[i],271,130,&font_16,i==1?Gold:Blue,captions[i]);
        numbers[i]=label(home,xs[i],298,130,&font_24,i==1?Gold:Blue,"--");
    }
    box(home,32,341,416,1,0x3a3a3a);
    records=label(home,31,357,295,&font_16,Gray,"-- records");
    modelsButton=button(home,328,345,120,"MODELS >",meter::Swipe::Models,true);

    detailTitle=label(details,31,14,240,&font_20,White,"TODAY MODELS");
    detailState=label(details,31,45,246,&font_16,Gray,"provider/model");
    dayTotal=label(details,250,10,198,&font_32,Coral,"--",true);
    dayCaption=label(details,310,45,138,&font_16,Gray,"DAY TOTAL",true);
    box(details,32,69,416,1,0x3a3a3a);
    for (int i=0;i<2;++i) {
        int y=83+i*114;
        providers[i]=label(details,31,y,213,&font_16,Blue,"");
        models[i]=label(details,31,y+23,213,&font_24,White,"");
        costs[i]=label(details,250,y+12,198,&font_32,Coral,"",true);
        rowTokens[i]=label(details,31,y+58,213,&font_20,Blue,"");
        rowRecords[i]=label(details,230,y+62,218,&font_16,Gray,"",true);
        box(details,32,y+94,416,1,0x3a3a3a);
    }
    pairCount=label(details,31,299,330,&font_16,Gray,"");
    pageCount=label(details,365,299,83,&font_16,White,"",true);
    homeButton=button(details,32,327,120,"< HOME",meter::Swipe::Home);
    prevButton=button(details,168,327,112,"< PREV",meter::Swipe::Prev);
    nextButton=button(details,296,327,152,"NEXT >",meter::Swipe::Next);
    refresh();
}
static void refresh() {
    const auto* data=snapshot;
    bool todayUsable=data && data->status!=meter::Status::Unavailable;
    if (view==meter::View::Details) {
        if (data) page=std::min(page,meter::pages(data->count));
        if (!todayUsable || !page) { view=meter::View::Today; page=0; }
    }
    bool yesterday=view==meter::View::Yesterday;
    const meter::DaySummary* s=data;
    if (yesterday) s=data && data->yesterday.date[0]?&data->yesterday:nullptr;
    bool usable=s && s->status!=meter::Status::Unavailable;
    bool noRecords=usable && s->status==meter::Status::Partial && !s->records && !s->cost.known;
    bool showCounts=usable && !noRecords;
    bool old=data && meter::oldDay(*data,elapsed);
    bool stale=data && meter::stale(*data,elapsed);
    hidden(home,view==meter::View::Details); hidden(details,view!=meter::View::Details);
    char text[96], value[40];
    // Always retain the source date; never relabel an old day TODAY.
    setText(dateLabel,s?s->date:"--");
    const char* day=old?"OLD DAY":yesterday?"YESTERDAY":"TODAY";
    if (!s) snprintf(text,sizeof text,"%s",yesterday?"YESTERDAY · NO DATA":"NO DATA YET");
    else if (!usable && yesterday) snprintf(text,sizeof text,"%s · SOURCE UNAVAILABLE%s",day,stale?" · STALE":"");
    else if (!usable) snprintf(text,sizeof text,"SOURCE UNAVAILABLE%s",stale?" · STALE":"");
    else snprintf(text,sizeof text,"%s%s%s",day,stale?" · STALE":"",s->status==meter::Status::Partial?" · PARTIAL SOURCE":"");
    setText(period,text);
    fittedMoney(amount,usable?s->cost:meter::Cost{},422,true);
    const char* price=!s?"WAITING":!usable?"NO SOURCE":noRecords?"INCOMPLETE":!s->cost.known?"UNPRICED":s->unpriced?"PARTIAL PRICE":"RECORDED";
    snprintf(text,sizeof text,"USD · %s",price); setText(currency,text);
    if (!s) snprintf(text,sizeof text,"--");
    else if (elapsed<3600) snprintf(text,sizeof text,"%llum ago",static_cast<unsigned long long>(elapsed/60));
    else if (elapsed<86400) snprintf(text,sizeof text,"%lluh ago",static_cast<unsigned long long>(elapsed/3600));
    else snprintf(text,sizeof text,"%llud ago",static_cast<unsigned long long>(elapsed/86400));
    setText(age,text);
    if (showCounts) meter::compact(s->tokens,value,sizeof value); else snprintf(value,sizeof value,"--");
    setText(tokens,value);
    // Large compact totals must not collide with the unit label at x208.
    setFont(tokens,lv_text_get_width(value,strlen(value),&font_48,0)<=170?&font_48:&font_32);
    uint64_t fields[]={showCounts?s->input:0,showCounts?s->output:0,showCounts?s->cacheRead:0};
    for (int i=0;i<3;++i) {
        if (showCounts) meter::compact(fields[i],value,sizeof value); else snprintf(value,sizeof value,"--");
        setText(numbers[i],value);
        setFont(numbers[i],lv_text_get_width(value,strlen(value),&font_24,0)<=129?&font_24:&font_20);
    }
    if (showCounts) {
        meter::compact(s->records,value,sizeof value);
        snprintf(text,sizeof text,"%s records%s",value,connected?"":" · OFFLINE");
    } else snprintf(text,sizeof text,"%s%s",noRecords?"NO READABLE RECORDS":"WAITING",connected?"":" · OFFLINE");
    setText(records,text);
    setText(lv_obj_get_child(modelsButton,0),yesterday?"HOME >":"MODELS >");
    disabled(modelsButton,!yesterday && (!usable || !data->count));
    if (view!=meter::View::Details) return;
    snprintf(text,sizeof text,"%s MODELS",day); setText(detailTitle,text);
    snprintf(text,sizeof text,"%s%s",s->status==meter::Status::Partial?"PARTIAL SOURCE":"provider/model",stale?" · STALE":"");
    setText(detailState,text);
    fittedMoney(dayTotal,s->cost,198,false);
    setText(dayCaption,!s->cost.known?"UNPRICED":s->unpriced?"PARTIAL PRICE":"DAY TOTAL");
    for (size_t i=0;i<2;++i) {
        size_t index=(page-1)*2+i;
        bool empty=index>=data->count;
        for (auto* object : {providers[i],models[i],costs[i],rowTokens[i],rowRecords[i]}) hidden(object,empty);
        if (empty) continue;
        const auto& row=data->rows[index];
        meter::identity(row.provider,20,value,sizeof value); snprintf(text,sizeof text,"%s/",value);
        setText(providers[i],text);
        meter::identity(row.model,14,value,sizeof value); setText(models[i],value);
        fittedMoney(costs[i],row.cost,198,false);
        meter::compact(row.tokens,value,sizeof value); snprintf(text,sizeof text,"%s tokens",value);
        setText(rowTokens[i],text);
        meter::compact(row.records,value,sizeof value);
        snprintf(text,sizeof text,"%s rec%s",value,!row.cost.known?" · UNPRICED":row.unpriced?" · PARTIAL":"");
        setText(rowRecords[i],text);
    }
    snprintf(text,sizeof text,"%u pairs · by cost%s",unsigned(data->count),connected?"":" · OFFLINE");
    setText(pairCount,text);
    snprintf(text,sizeof text,"%u/%u",unsigned(page),unsigned(meter::pages(data->count)));
    setText(pageCount,text);
    disabled(prevButton,page==1); disabled(nextButton,page==meter::pages(data->count));
}
void uiUpdate(const meter::Snapshot* s, uint64_t seconds, bool online) {
    snapshot=s; elapsed=seconds; connected=online; refresh();
}
void uiTouch(bool pressed, int x, int y) {
    if (pressed && !gesture.down && !gesture.suppressed) {
        lv_point_t point={x,y};
        pressTarget=nullptr;
        if (view!=meter::View::Details) {
            if (lv_obj_hit_test(modelsButton,&point)) pressTarget=modelsButton;
        } else {
            for (auto* b : {homeButton,prevButton,nextButton})
                if (lv_obj_hit_test(b,&point)) { pressTarget=b; break; }
        }
    }
    auto action=gesture.update(pressed,x,y,view);
    if (action!=meter::Swipe::None) { pending=action; pressTarget=nullptr; }
}
void uiCancelTouch() {
    pending=meter::Swipe::None; pressTarget=nullptr; gesture.cancel();
}
void uiHome() { uiCancelTouch(); navigate(meter::Swipe::Home); }
bool uiPoll() {
    auto action=pending; pending=meter::Swipe::None;
    if (action!=meter::Swipe::None) navigate(action);
    return action!=meter::Swipe::None;
}
