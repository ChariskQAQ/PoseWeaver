#include "core/serialize.h"
#include "core/pose.h"
#include "core/diagnostics.h"
#include <cstdio>
using namespace rig;
int main(int argc,char** argv) {
    if (argc != 2) return 1;
    Skeleton sk;
    sk.addBone("root",-1,{},0);
    int body = sk.addBone("body",0,{0,100},45);
    int head = sk.addBone("head",body,{0,62},26);
    int leftArm = sk.addBone("arm_l",body,{-39,31},48); sk.bones[leftArm].setup.rotation = -125;
    int leftHand = sk.addBone("forearm_l",leftArm,{48,0},43); sk.bones[leftHand].setup.rotation = 25;
    int rightArm = sk.addBone("arm_r",body,{39,31},48); sk.bones[rightArm].setup.rotation = -55;
    int rightHand = sk.addBone("forearm_r",rightArm,{48,0},43); sk.bones[rightHand].setup.rotation = -25;
    int legs[2][3], targets[2];
    for (int i = 0; i < 2; ++i) {
        std::string side = i ? "r" : "l"; float x = i ? 19.f : -19.f;
        legs[i][0] = sk.addBone("thigh_"+side,0,{x,65},42); sk.bones[legs[i][0]].setup.rotation = -90;
        legs[i][1] = sk.addBone("shin_"+side,legs[i][0],{42,0},36);
        legs[i][2] = sk.addBone("foot_"+side,legs[i][1],{36,0},20); sk.bones[legs[i][2]].setup.rotation = 90;
        targets[i] = sk.addBone("foot_target_"+side,0,{x,-10},0);
        Constraint ik; ik.name = "leg_ik_"+side; ik.type = ConstraintType::Ik;
        ik.bones = {legs[i][0],legs[i][1]}; ik.target = targets[i]; ik.values = defaultConstraintValues(ConstraintType::Ik);
        ik.values[2] = i ? -1.f : 1.f; sk.constraints.push_back(ik);
    }
    const char* parts[] = {"body","head","limb","foot"}; const char* skins[] = {"ocean","ember"};
    const int widths[] = {64,64,52,36}, heights[] = {80,56,22,18};
    for (const char* skin : skins) {
        Skin s; s.name = skin; sk.skins.push_back(s);
        for (int p = 0; p < 4; ++p) sk.images.push_back({std::string("robot_")+parts[p]+"_"+skin+".png",widths[p],heights[p]});
    }
    auto slot = [&](const std::string& name,int bone,int part,float center,float width,float height) {
        Slot s; s.name = name; s.bone = bone; s.activeAttachment = 0;
        Attachment placeholder; placeholder.name = name; placeholder.skinPlaceholder = true; s.attachments.push_back(placeholder);
        int index = (int)sk.slots.size(); sk.slots.push_back(s);
        for (int skin = 0; skin < 2; ++skin) {
            Attachment attachment; attachment.name = name; attachment.image = skin*4+part;
            attachment.transform.scaleX = width/widths[part]; attachment.transform.scaleY = height/heights[part]; attachment.transform.x = center;
            sk.skins[skin].attachments.push_back({index,name,attachment});
        }
    };
    for (int i = 0; i < 2; ++i) {
        std::string side = i ? "r" : "l";
        slot("thigh_"+side,legs[i][0],2,21,46,23); slot("shin_"+side,legs[i][1],2,18,40,20); slot("shoe_"+side,legs[i][2],3,6,36,18);
    }
    slot("left_upper",leftArm,2,24,52,22); slot("left_lower",leftHand,2,21,47,20);
    slot("body",body,0,0,64,80); slot("head",head,1,0,64,56);
    slot("right_upper",rightArm,2,24,52,22); slot("right_lower",rightHand,2,21,47,20);
    Animation idle; idle.name = "idle"; idle.duration = 1.6f;
    for (float t : {0.f,0.8f,1.6f}) { setTranslateKey(idle,body,t,{0,t == 0.8f ? 3.f : 0.f}); setRotateKey(idle,head,t,t == 0.8f ? 3.f : -2.f); }
    sk.animations.push_back(idle);
    Animation walk; walk.name = "walk"; walk.duration = 1;
    for (int i = 0; i <= 4; ++i) {
        float t = i/4.f;
        setRotateKey(walk,leftArm,t,(i == 1 ? 20.f : i == 3 ? -20.f : 0));
        setRotateKey(walk,rightArm,t,(i == 1 ? -20.f : i == 3 ? 20.f : 0));
        for (int leg = 0; leg < 2; ++leg) {
            int phase = (i+leg*2)%4;
            setTranslateKey(walk,targets[leg],t,{phase == 1 ? 12.f : phase == 3 ? -12.f : 0.f,phase == 1 ? 13.f : 0.f});
        }
    }
    sk.animations.push_back(walk);
    Animation wave; wave.name = "wave"; wave.duration = 1.4f;
    for (float t : {0.f,0.25f,0.5f,0.75f,1.f,1.15f,1.4f}) {
        setRotateKey(wave,rightArm,t,t > 0 && t < 1.4f ? 150.f : 0.f);
        setRotateKey(wave,rightHand,t,t == 0.5f || t == 1 ? 10.f : t == 0 || t == 1.4f ? 0.f : -45.f);
    }
    EventDef sound; sound.name = "wave_peak"; sound.audioPath = "swoosh.wav"; sk.events.push_back(sound); setEventKey(wave,0.5f,0,sound);
    sk.animations.push_back(wave);
    std::string error;
    if (!validateSkeleton(sk,&error) || !saveFile(argv[1],sk,&error)) { std::puts(error.c_str()); return 1; }
    std::printf("Robot starter: %zu bones, %zu slots, 2 skins, 3 animations\n",sk.bones.size(),sk.slots.size()); return 0;
}
