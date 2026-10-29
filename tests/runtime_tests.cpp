#include "runtime/runtime.h"
#include "core/serialize.h"
#include "core/pose.h"
#include <json.hpp>
#include <cstdio>
#include <filesystem>
#include <limits>
using nlohmann::json;
static int failures=0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); ++failures; } } while(false)
struct Client {
    PwRuntime* handle=pw_create();
    ~Client() { pw_destroy(handle); }
    json call(json request) { auto result=json::parse(pw_request(handle,request.dump().c_str())); return result; }
};
int main() {
    try {
        rig::Skeleton sk; std::string error;
        CHECK(rig::loadFile((std::filesystem::u8path(POSEWEAVER_SOURCE_DIR)/"samples/robot-starter.rig.json").u8string(),sk,&error));
        Client client; CHECK(client.handle);
        CHECK(!client.call({{"op","step"},{"dt",0}}).at("ok").get<bool>());
        auto loaded=client.call({{"op","load"},{"data",rig::toJson(sk)}}); CHECK(loaded["ok"]==true && loaded["bones"].size()==15 && loaded["skins"].size()==2);
        CHECK(client.call({{"op","play"},{"name","wave"},{"mix",0},{"loop",false}})["ok"]==true);
        auto frame=client.call({{"op","step"},{"dt",0.5}}); CHECK(frame["ok"]==true && frame["batches"].size()==12 && frame["world"].size()==15*6 && !frame["events"].empty());
        auto expected=rig::makeAnimatedPose(sk,&sk.animations[2],0.5f,{0});
        CHECK(std::abs(frame["world"][4].get<float>()-expected.world[0].tx)<0.001f);
        CHECK(client.call({{"op","skin"},{"names",{"ember"}}})["ok"]==true);
        auto ember=client.call({{"op","step"},{"dt",0}}); CHECK(ember["batches"][0]["image"]!=frame["batches"][0]["image"]);
        CHECK(client.call({{"op","bone"},{"name","foot_target_l"},{"position",true},{"x",65},{"y",-80}})["ok"]==true);
        auto controlled=client.call({{"op","step"},{"dt",0}});
        int foot=sk.findBone("foot_target_l"); CHECK(foot>=0 && controlled["world"][foot*6+4]==65);
        CHECK(client.call({{"op","clear_bone"},{"name","foot_target_l"}})["ok"]==true);
        CHECK(!client.call({{"op","bone"},{"name","missing"},{"position",true}})["ok"].get<bool>());
        CHECK(!client.call({{"op","step"},{"dt",-1}})["ok"].get<bool>());
        CHECK(!client.call({{"op","play"},{"name","wave"},{"track",99}})["ok"].get<bool>());
        CHECK(!json::parse(pw_request(client.handle,"{bad"))["ok"].get<bool>());
        CHECK(!client.call({{"op","load"},{"data","{}"}})["ok"].get<bool>());
        CHECK(client.call({{"op","step"},{"dt",0}})["ok"]==true);
        CHECK(client.call({{"op","play"},{"name","walk"},{"track",0},{"mix",0.1}})["ok"]==true);
        CHECK(client.call({{"op","play"},{"name","wave"},{"track",1},{"mix",0.1}})["ok"]==true);
        CHECK(client.call({{"op","alpha"},{"track",1},{"alpha",0.5}})["ok"]==true);
        CHECK(client.call({{"op","queue"},{"name","idle"},{"track",0},{"delay",0}})["ok"]==true);
        CHECK(client.call({{"op","step"},{"dt",1}})["ok"]==true);
        CHECK(client.call({{"op","empty"},{"track",1},{"mix",0.1}})["ok"]==true);
        auto effects=sk; effects.effects.light.enabled=true;
        CHECK(!client.call({{"op","load"},{"data",rig::toJson(effects)}})["ok"].get<bool>());
        CHECK(!json::parse(pw_request(nullptr,"{}"))["ok"].get<bool>());
    } catch (const std::exception& error) { std::printf("Runtime test exception: %s\n",error.what()); ++failures; }
    std::printf("native runtime: %d failure(s)\n",failures); return failures ? 1 : 0;
}
