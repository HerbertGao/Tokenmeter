#include "core.h"
#include <cassert>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

static const std::string example = R"({"v":1,"date":"2024-01-01","epoch":1704100000,"day_end":1704153600,"utc_offset":0,"status":"ok","issues":0,"totals":{"cost":0.5,"tokens":7,"input":1,"output":1,"cache_read":0,"cache_write":0,"records":1,"unpriced":0},"models":[{"provider":"pi","model":"test","cost":0.5,"tokens":7,"records":1,"unpriced":0}]})";
static std::string replace(std::string text, const std::string& a, const std::string& b) {
    size_t p=text.find(a); assert(p!=std::string::npos); text.replace(p,a.size(),b); return text;
}
int main(int argc,char** argv) {
    auto snapshot=std::make_unique<meter::Snapshot>();
    auto valid=[&](const std::string& s) { return meter::parse(s.data(),s.size(),*snapshot); };
    assert(valid(example)); // tokens intentionally differ from the component sum
    assert(snapshot->tokens==7 && snapshot->input+snapshot->output==2);
    std::string precise=replace(replace(example,"0.5","0.575568"),"0.5,","0.575568,");
    assert(valid(precise) && std::fabs(snapshot->cost.value-0.575568)<1e-15);
    std::string maximum=replace(replace(example,"0.5","1000000000000"),"0.5,","1000000000000,");
    assert(valid(maximum) && snapshot->cost.value==1e12);
    // Transport's exact 64KiB boundary, discard-to-LF and next-frame recovery.
    std::vector<char> buffer(meter::FrameLimit+1);
    meter::Framer frame(buffer.data());
    std::string full(meter::FrameLimit,'x');
    auto feed=[&](const std::string& s) {
        for (size_t i=0;i<s.size();i+=180)
            frame.feed(reinterpret_cast<const uint8_t*>(s.data()+i),std::min<size_t>(180,s.size()-i));
    };
    feed(full); feed("\n");
    assert(frame.ready && !frame.error && frame.used==meter::FrameLimit);
    frame.reset(); feed(full); feed("extra");
    assert(frame.dropping && !frame.ready);
    feed("still dropped\n"); assert(frame.ready && frame.error);
    frame.reset(); feed(example+"\n"); assert(frame.ready && !frame.error && valid(frame.data));
    frame.reset(); feed("unfinished"); frame.reset(); feed(example+"\n");
    assert(frame.ready && !frame.error); // disconnect drops unfinished bytes
    assert(!valid(example+"{}") && !valid(example+"junk"));
    assert(!valid(replace(example,"\"v\":1","\"v\":true")));
    assert(!valid(replace(example,"\"tokens\":7","\"tokens\":7.5")));
    assert(!valid(replace(example,"\"tokens\":7","\"tokens\":9007199254740992")));
    assert(!valid(replace(example,"\"records\":1","\"records\":-1")));
    assert(!valid(replace(example,"2024-01-01","2024-02-30")));
    assert(!valid(replace(example,"2024-01-01","2024-01-02")));
    assert(!valid(replace(example,"1704153600","1704200001")));
    assert(!valid(replace(example,"\"cost\":0.5","\"cost\":0.51")));
    assert(!valid(replace(example,"\"cost\":0.5","\"cost\":\"0.5\"")));
    assert(!valid(replace(example,"\"cost\":0.5","\"cost\":null")));
    assert(valid(replace(example,"\"pi\"","\""+std::string(192,'p')+"\"")));
    assert(!valid(replace(example,"\"pi\"","\""+std::string(193,'p')+"\"")));
    assert(!valid(replace(example,"\"pi\"","\"\"")));
    assert(!valid(replace(example,"\"pi\"","\"bad\\u0000identity\"")));
    assert(!valid(replace(example,"\"pi\"","\"bad\\nidentity\"")));
    assert(!valid(replace(example,"\"pi\"","\"bad\\u007fidentity\"")));
    assert(!valid(replace(example,"\"pi\"","\"bad\\u0085identity\"")));
    JsonDocument doc; assert(!deserializeJson(doc,example));
    doc["models"][0]["cost"]=nullptr; doc["models"][0]["unpriced"]=1;
    doc["totals"]["cost"]=nullptr; doc["totals"]["unpriced"]=1;
    std::string json; serializeJson(doc,json); assert(valid(json));
    doc["status"]="partial";
    json.clear(); serializeJson(doc,json); assert(valid(json));
    doc["models"].to<JsonArray>(); doc["totals"]["cost"]=0;
    for (auto key : {"tokens","input","output","cache_read","cache_write","records","unpriced"}) doc["totals"][key]=0;
    json.clear(); serializeJson(doc,json); assert(valid(json) && snapshot->cost.known);
    doc["totals"]["cost"]=nullptr;
    json.clear(); serializeJson(doc,json); assert(valid(json) && !snapshot->cost.known);
    doc["status"]="ok";
    json.clear(); serializeJson(doc,json); assert(!valid(json));
    doc["status"]="unavailable";
    json.clear(); serializeJson(doc,json); assert(valid(json) && !snapshot->cost.known);
    // Preserve pair identity: same model, different providers; all 256 remain pageable.
    doc["status"]="ok"; doc["totals"]["cost"]=0;
    for (int i=0;i<256;++i) {
        auto row=doc["models"].add<JsonObject>(); row["provider"]=std::to_string(i); row["model"]="same";
        row["cost"]=0; row["tokens"]=0; row["records"]=1; row["unpriced"]=0;
    }
    doc["totals"]["records"]=256;
    json.clear(); serializeJson(doc,json); assert(valid(json) && snapshot->count==256);
    doc["models"][0]["tokens"]=meter::MaxInteger; doc["models"][1]["tokens"]=1;
    doc["totals"]["tokens"]=meter::MaxInteger;
    json.clear(); serializeJson(doc,json); assert(!valid(json));
    doc["models"][0]["tokens"]=0; doc["models"][1]["tokens"]=0; doc["totals"]["tokens"]=0;
    auto duplicate=doc["models"].add<JsonObject>(); duplicate.set(doc["models"][0]);
    doc["totals"]["records"]=257;
    json.clear(); serializeJson(doc,json); assert(!valid(json));
    const std::string yesterdayJson=R"({"date":"2023-12-31","status":"ok","issues":0,"totals":{"cost":0.25,"tokens":3,"input":1,"output":2,"cache_read":0,"cache_write":0,"records":1,"unpriced":0}})";
    auto withYesterday=[](const std::string& prior,const std::string& current=example) {
        return current.substr(0,current.size()-1)+",\"yesterday\":"+prior+"}";
    };
    assert(valid(withYesterday(yesterdayJson)));
    assert(!strcmp(snapshot->yesterday.date,"2023-12-31") && snapshot->yesterday.tokens==3);
    assert(snapshot->cost.value==0.5 && snapshot->yesterday.cost.value==0.25 && snapshot->count==1);
    assert(!valid(withYesterday("null")));
    assert(!valid(withYesterday(replace(yesterdayJson,"2023-12-31","2024-01-01"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"2023-12-31","2023-12-30"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"tokens\":3","\"tokens\":true"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"issues\":0","\"issues\":false"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"tokens\":3","\"tokens\":9007199254740992"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"unpriced\":0","\"unpriced\":2"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"cost\":0.25","\"cost\":-0.25"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"cost\":0.25","\"cost\":null"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"records\":1","\"records\":0"))));
    assert(!valid(withYesterday(replace(yesterdayJson,"\"status\":\"ok\"","\"status\":\"unavailable\""))));
    auto unpricedYesterday=replace(replace(yesterdayJson,"\"cost\":0.25","\"cost\":null"),"\"unpriced\":0","\"unpriced\":1");
    assert(valid(withYesterday(unpricedYesterday)) && !snapshot->yesterday.cost.known);
    JsonDocument noToday; assert(!deserializeJson(noToday,example));
    noToday["status"]="unavailable"; noToday["models"].to<JsonArray>();
    for (auto key : {"tokens","input","output","cache_read","cache_write","records","unpriced"}) noToday["totals"][key]=0;
    noToday["totals"]["cost"]=nullptr;
    std::string noTodayJson; serializeJson(noToday,noTodayJson);
    assert(valid(withYesterday(yesterdayJson,noTodayJson)) && snapshot->status==meter::Status::Unavailable);
    assert(snapshot->yesterday.status==meter::Status::Ok && snapshot->yesterday.cost.value==0.25);
    assert(!valid(withYesterday(replace(yesterdayJson,"2023-12-31","2024-01-01"),noTodayJson)));
    noToday["date"]="2024-03-01"; noToday["epoch"]=1709251200; noToday["day_end"]=1709337600;
    noTodayJson.clear(); serializeJson(noToday,noTodayJson);
    assert(valid(withYesterday(replace(yesterdayJson,"2023-12-31","2024-02-29"),noTodayJson)));
    assert(!valid(withYesterday(replace(yesterdayJson,"2023-12-31","2024-02-28"),noTodayJson)));
    const std::string noPrevious=R"({"date":"2023-12-31","status":"unavailable","issues":1,"totals":{"cost":null,"tokens":0,"input":0,"output":0,"cache_read":0,"cache_write":0,"records":0,"unpriced":0}})";
    assert(valid(withYesterday(noPrevious)) && snapshot->yesterday.status==meter::Status::Unavailable);
    auto partialPrevious=replace(noPrevious,"unavailable","partial");
    assert(valid(withYesterday(partialPrevious)) && !snapshot->yesterday.cost.known);
    auto emptyPrevious=replace(replace(noPrevious,"unavailable","ok"),"\"cost\":null","\"cost\":0");
    assert(valid(withYesterday(emptyPrevious)) && snapshot->yesterday.cost.known && snapshot->yesterday.cost.value==0);
    assert(valid(example) && !snapshot->yesterday.date[0]); // Legacy sender clears, never reuses an older summary.
    assert(!meter::stale(*snapshot,119) && meter::stale(*snapshot,120));
    assert(meter::stale(*snapshot,(1ULL<<32)/1000));
    assert(meter::oldDay(*snapshot,snapshot->dayEnd-snapshot->epoch));
    assert(meter::nextPage(127,256)==128 && meter::nextPage(128,256)==128);
    assert(meter::prevPage(0)==0 && meter::prevPage(1)==1 && meter::prevPage(2)==1);
    using meter::Swipe;
    assert(meter::swipe(-70,5,meter::View::Today)==Swipe::Models);
    assert(meter::swipe(70,5,meter::View::Details)==Swipe::Home);
    assert(meter::swipe(70,0,meter::View::Today)==Swipe::Yesterday && meter::swipe(-70,0,meter::View::Details)==Swipe::None);
    assert(meter::swipe(0,-70,meter::View::Details)==Swipe::Next && meter::swipe(0,70,meter::View::Details)==Swipe::Prev);
    assert(meter::swipe(0,-70,meter::View::Today)==Swipe::None && meter::swipe(0,70,meter::View::Today)==Swipe::None);
    assert(meter::swipe(20,10,meter::View::Today)==Swipe::None && meter::swipe(70,70,meter::View::Today)==Swipe::None);
    meter::TouchGesture gesture;
    assert(gesture.update(true,200,280,meter::View::Today)==Swipe::None);
    assert(gesture.update(true,151,280,meter::View::Today)==Swipe::None);
    assert(gesture.update(true,150,280,meter::View::Today)==Swipe::Models); // no release required
    assert(gesture.down && !gesture.tapAllowed(150,280));
    assert(gesture.update(true,400,280,meter::View::Details)==Swipe::None); // page changed; same stroke
    assert(gesture.update(true,0,280,meter::View::Details)==Swipe::None);
    assert(gesture.update(false,0,280,meter::View::Details)==Swipe::None);
    assert(gesture.update(true,320,430,meter::View::Details)==Swipe::None); // disabled NEXT region
    assert(gesture.update(true,420,430,meter::View::Details)==Swipe::Home);
    assert(!gesture.tapAllowed(420,430));
    assert(gesture.update(false,420,430,meter::View::Today)==Swipe::None);
    meter::TouchGesture quick;
    assert(quick.update(true,400,440,meter::View::Today)==Swipe::None);
    assert(!quick.tapAllowed(330,440)); // final point known before the release callback
    assert(quick.update(false,330,440,meter::View::Today)==Swipe::Models);
    assert(!quick.tapAllowed(330,440));
    meter::TouchGesture tap;
    assert(tap.update(true,400,440,meter::View::Today)==Swipe::None);
    assert(tap.update(true,403,442,meter::View::Today)==Swipe::None && tap.tapAllowed(403,442));
    assert(tap.update(false,403,442,meter::View::Today)==Swipe::None);
    assert(!tap.tapAllowed(403,442));
    meter::TouchGesture diagonal;
    assert(diagonal.update(true,100,200,meter::View::Today)==Swipe::None);
    assert(diagonal.update(true,170,270,meter::View::Today)==Swipe::None);
    assert(!diagonal.tapAllowed(100,200)); // moving back does not restore a click
    assert(diagonal.update(false,100,200,meter::View::Today)==Swipe::None);
    meter::TouchGesture cancelled;
    assert(cancelled.update(true,200,280,meter::View::Today)==Swipe::None);
    cancelled.cancel();
    assert(cancelled.update(true,300,280,meter::View::Today)==Swipe::None);
    assert(!cancelled.tapAllowed(200,280));
    assert(cancelled.update(false,300,280,meter::View::Today)==Swipe::None);
    assert(cancelled.update(true,200,280,meter::View::Today)==Swipe::None);
    assert(cancelled.update(true,150,280,meter::View::Today)==Swipe::Models);
    meter::TouchGesture vertical;
    assert(vertical.update(true,200,200,meter::View::Details)==Swipe::None);
    assert(vertical.update(true,200,150,meter::View::Details)==Swipe::Next);
    assert(vertical.update(true,200,0,meter::View::Details)==Swipe::None);
    assert(vertical.update(false,200,0,meter::View::Details)==Swipe::None);
    assert(meter::swipe(70,0,meter::View::Today)==Swipe::Yesterday);
    assert(meter::swipe(-70,0,meter::View::Yesterday)==Swipe::Home);
    assert(meter::swipe(70,0,meter::View::Yesterday)==Swipe::None);
    assert(meter::swipe(0,70,meter::View::Yesterday)==Swipe::None);
    assert(meter::swipe(0,-70,meter::View::Yesterday)==Swipe::None);
    meter::TouchGesture yesterdayGesture;
    assert(yesterdayGesture.update(true,100,280,meter::View::Today)==Swipe::None);
    assert(yesterdayGesture.update(true,150,280,meter::View::Today)==Swipe::Yesterday);
    assert(yesterdayGesture.update(true,300,280,meter::View::Yesterday)==Swipe::None);
    assert(yesterdayGesture.update(false,300,280,meter::View::Yesterday)==Swipe::None);
    assert(yesterdayGesture.update(true,400,440,meter::View::Yesterday)==Swipe::None);
    assert(yesterdayGesture.update(true,350,440,meter::View::Yesterday)==Swipe::Home);
    meter::Orientation orientation;
    auto turn=[&](float x,float y,uint8_t expected,uint64_t when) {
        assert(!orientation.update(x,y,0,when,false));
        assert(!orientation.update(x,y,0,when+399,false));
        assert(orientation.update(x,y,0,when+400,false));
        assert(orientation.rotation==expected);
    };
    turn(0,-1,3,0); turn(-1,0,2,1000); turn(0,1,1,2000); turn(1,0,0,3000);
    assert(!orientation.update(0,0,1,4000,false)); // flat: retain the previous direction
    assert(!orientation.update(.707f,.707f,0,4100,false)); // diagonal dead band
    assert(!orientation.update(2,0,0,4200,false)); // moving, not a gravity-only sample
    assert(!orientation.update(NAN,0,1,4300,false));
    assert(!orientation.update(0,-1,0,5000,false));
    assert(!orientation.update(0,-1,0,5400,true)); // touch cancels the pending rotation
    assert(orientation.rotation==0);
    turn(0,-1,3,5500);
    meter::Orientation longUptime;
    assert(!longUptime.update(0,1,0,(1ULL<<32)-100,false));
    assert(longUptime.update(0,1,0,(1ULL<<32)+300,false));
    meter::HomeKey key;
    assert(!key.update(false,0) && !key.update(true,10) && !key.update(false,20));
    assert(!key.update(true,25) && !key.update(true,54) && key.update(true,55));
    assert(!key.update(true,500)); // held key does not repeat
    assert(!key.update(false,510) && !key.update(false,540));
    assert(!key.update(true,550) && key.update(true,580));
    char text[64]; meter::money({0.001,true},text,sizeof text); assert(!strcmp(text,"<$0.01"));
    meter::money({0,true},text,sizeof text); assert(!strcmp(text,"$0.00"));
    meter::identity("abcdefghijk",7,text,sizeof text); assert(!strcmp(text,"ab...jk"));
    meter::identity("pi-\xe6\xa8\xa1\xe5\x9e\x8b",20,text,sizeof text); assert(!strcmp(text,"pi-??"));
    for (int i=1;i<argc;++i) {
        std::ifstream input(argv[i],std::ios::binary);
        if (!input) { std::cerr<<"Cannot open "<<argv[i]<<'\n'; return 1; }
        std::string bytes(meter::FrameLimit+2,'\0');
        input.read(bytes.data(),bytes.size()); bytes.resize(input.gcount());
        if (!bytes.empty() && bytes.back()=='\n') bytes.pop_back();
        if (!valid(bytes)) { std::cerr<<"REJECT "<<argv[i]<<'\n'; return 1; }
        std::cout<<"ACCEPT "<<argv[i]<<": "<<snapshot->count<<" pairs, "<<snapshot->tokens<<" tokens\n";
    }
    std::cout<<"host checks passed\n";
}
