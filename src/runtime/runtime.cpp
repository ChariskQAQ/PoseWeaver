#include "runtime/runtime.h"
#include "core/anim_state.h"
#include "core/clipping.h"
#include "core/constraints.h"
#include "core/diagnostics.h"
#include "core/serialize.h"
#include <json.hpp>
#include <cmath>
#include <cstring>
#include <stdexcept>
using nlohmann::json;
using namespace rig;
namespace {
constexpr size_t MaxDocument=16*1024*1024;
float number(const json& j,const char* name,float fallback,float minimum,float maximum) {
    float value=j.value(name,fallback);
    if (!std::isfinite(value) || value<minimum || value>maximum) throw std::runtime_error(std::string("Invalid ")+name);
    return value;
}
template<class T> int named(const std::vector<T>& items,const std::string& name) {
    for (int i=0;i<(int)items.size();++i) if (items[i].name==name) return i;
    throw std::runtime_error("Unknown name: "+name);
}
struct BoneOverride { bool position=false,rotation=false; Vec2 xy; float angle=0; };
}
struct PwRuntime {
    Skeleton sk; Pose pose; AnimationState state; PhysicsState physics;
    std::vector<int> skins; std::map<int,BoneOverride> bones; std::map<int,int> slots;
    bool loaded=false; std::string reply;
    json frame(float dt) {
        auto events=state.update(sk,dt);
        resetToSetup(sk,pose); pose.skins=skins; state.apply(sk,pose);
        for (const auto& item : bones) {
            auto& t=pose.local[item.first];
            if (item.second.position) { t.x=item.second.xy.x; t.y=item.second.xy.y; }
            if (item.second.rotation) t.rotation=item.second.angle;
        }
        for (const auto& slot : slots) pose.slotAttachment[slot.first]=slot.second;
        updateWorld(sk,pose); applyPhysics(sk,pose,physics,dt);
        json result{{"ok",true},{"batches",json::array()},{"world",json::array()},{"events",json::array()}};
        for (const auto& m : pose.world) for (float v : {m.a,m.b,m.c,m.d,m.tx,m.ty}) result["world"].push_back(v);
        Geometry geometry,clipped; ClipTriangles clip; int clipEnd=-2; size_t vertices=0;
        for (int s : pose.drawOrder) {
            bool ends=clipEnd!=-2 && s==clipEnd;
            auto* attachment=poseAttachment(sk,pose,s);
            int index=pose.slotAttachment[s];
            if (attachment) {
                if (attachment->type==AttachmentType::Clipping) {
                    clip=clipTrianglesOf(computeVertices(sk,pose,sk.slots[s],*attachment,findDeform(pose,s,index))); clipEnd=attachment->endSlot;
                } else if (!attachment->isPolygon() && attachment->image>=0) {
                    computeGeometry(sk,pose,sk.slots[s],*attachment,geometry,findDeform(pose,s,index));
                    if (!clip.empty()) { clipGeometry(geometry,clip,clipped); std::swap(geometry,clipped); }
                    vertices+=geometry.positions.size(); if (vertices>1000000) throw std::runtime_error("Runtime frame exceeds one million vertices");
                    json batch{{"slot",s},{"image",attachment->image},{"blend",(int)sk.slots[s].blend},
                        {"xy",json::array()},{"uv",json::array()},{"triangles",geometry.triangles},{"color",pose.slotColor[s].v}};
                    for (auto p : geometry.positions) { batch["xy"].push_back(p.x); batch["xy"].push_back(p.y); }
                    for (auto p : geometry.uvs) { batch["uv"].push_back(p.x); batch["uv"].push_back(p.y); }
                    result["batches"].push_back(std::move(batch));
                }
            }
            if (ends) { clip.clear(); clipEnd=-2; }
        }
        for (const auto& e : events) {
            const auto& definition=sk.events[e.key.event];
            result["events"].push_back({{"name",definition.name},{"track",e.track},{"integer",e.key.intValue},{"number",e.key.floatValue},
                {"text",e.key.stringValue},{"audio",definition.audioPath},{"volume",definition.volume},{"balance",definition.balance}});
        }
        return result;
    }
    json request(const json& j) {
        const auto op=j.at("op").get<std::string>();
        if (op=="load") {
            auto text=j.at("data").get<std::string>(); if (text.size()>MaxDocument) throw std::runtime_error("Skeleton exceeds 16 MiB");
            Skeleton proposed; std::string error;
            if (!fromJson(text,proposed,&error) || !validateSkeleton(proposed,&error)) throw std::runtime_error(error);
            if (proposed.bones.empty() || proposed.bones.size()>4096 || proposed.slots.size()>10000 || proposed.images.size()>10000) throw std::runtime_error("Skeleton is empty or exceeds runtime limits");
            if (!(proposed.effects==RenderEffects{})) throw std::runtime_error("Use baked frames for lighting and pixel effects");
            for (const auto& slot : proposed.slots) if (slot.blend==BlendMode::Screen) throw std::runtime_error("Screen blending requires baked frames in this runtime version");
            sk=std::move(proposed); state={}; physics.reset(); bones.clear(); slots.clear(); skins=sk.skins.empty() ? std::vector<int>{} : std::vector<int>{0}; loaded=true;
            pose=makeSetupPose(sk,skins);
            json result{{"ok",true},{"bones",json::array()},{"skins",json::array()},{"animations",json::array()},{"images",json::array()},{"slots",sk.slots.size()}};
            for (const auto& b : sk.bones) result["bones"].push_back(b.name);
            for (const auto& s : sk.skins) result["skins"].push_back(s.name);
            for (const auto& a : sk.animations) result["animations"].push_back(a.name);
            for (const auto& i : sk.images) result["images"].push_back(i.path);
            return result;
        }
        if (!loaded) throw std::runtime_error("Load a skeleton first");
        const int track=j.value("track",0); if (track<0 || track>7) throw std::runtime_error("Track must be 0 to 7");
        if (op=="step") return frame(number(j,"dt",0,0,1));
        if (op=="play" || op=="queue") {
            int a=named(sk.animations,j.at("name").get<std::string>());
            float duration=number(j,"mix",0.2f,0,10);
            float delay=op=="queue" ? number(j,"delay",0,-3600,3600) : 0;
            bool loop=j.value("loop",true); state.defaultMix=duration;
            if (op=="play") state.setAnimation(track,a,loop);
            else state.addAnimation(sk,track,a,loop,delay);
        } else if (op=="skin") {
            std::vector<int> proposed;
            for (const auto& name : j.at("names")) proposed.push_back(named(sk.skins,name.get<std::string>()));
            skins=std::move(proposed); physics.reset();
        } else if (op=="bone") {
            int bone=named(sk.bones,j.at("name").get<std::string>()); auto value=bones[bone];
            if (j.value("position",false)) { value.xy={number(j,"x",0,-1e7f,1e7f),number(j,"y",0,-1e7f,1e7f)}; value.position=true; }
            if (j.value("rotate",false)) { value.angle=number(j,"angle",0,-1e7f,1e7f); value.rotation=true; }
            bones[bone]=value;
        } else if (op=="clear_bone") bones.erase(named(sk.bones,j.at("name").get<std::string>()));
        else if (op=="attachment") {
            int s=named(sk.slots,j.at("name").get<std::string>()); auto name=j.at("attachment").get<std::string>();
            slots[s]=name.empty() ? -1 : named(sk.slots[s].attachments,name);
        } else if (op=="clear_attachment") slots.erase(named(sk.slots,j.at("name").get<std::string>()));
        else if (op=="alpha") state.setTrackAlpha(track,number(j,"alpha",1,0,1));
        else if (op=="speed") state.timeScale=number(j,"speed",1,0,4);
        else if (op=="empty") state.setEmptyAnimation(track,number(j,"mix",0.2f,0,10));
        else if (op=="clear") state.clearTracks();
        else throw std::runtime_error("Unknown runtime operation");
        return {{"ok",true}};
    }
};
extern "C" {
PwRuntime* pw_create() { try { return new PwRuntime; } catch (...) { return nullptr; } }
void pw_destroy(PwRuntime* runtime) { delete runtime; }
const char* pw_request(PwRuntime* runtime,const char* request) {
    if (!runtime) return "{\"ok\":false,\"error\":\"Missing runtime instance\"}";
    try {
        if (!request) throw std::runtime_error("Missing request");
        size_t length=0; while (length<=MaxDocument*2 && request[length]) ++length;
        if (length>MaxDocument*2) throw std::runtime_error("Request exceeds 32 MiB");
        // shortcut: JSON geometry crosses engine bindings; replace with packed buffers when large-character profiling warrants it.
        runtime->reply=runtime->request(json::parse(request,request+length)).dump();
    } catch (const std::exception& error) { runtime->reply=json{{"ok",false},{"error",error.what()}}.dump(); }
    catch (...) { runtime->reply="{\"ok\":false,\"error\":\"Runtime failure\"}"; }
    return runtime->reply.c_str();
}
}
