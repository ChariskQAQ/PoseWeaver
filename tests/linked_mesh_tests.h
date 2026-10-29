#pragma once
static rig::Attachment linkedTestMesh(int vertices=3) {
    rig::Attachment mesh; mesh.name="source"; mesh.type=rig::AttachmentType::Mesh;
    mesh.vertices={{0,0},{10,0},{0,10}}; mesh.uvs={{0,0},{1,0},{0,1}}; mesh.triangles={0,1,2};
    if (vertices==4) { mesh.vertices.push_back({10,10}); mesh.uvs.push_back({1,1}); mesh.triangles.insert(mesh.triangles.end(),{1,3,2}); }
    mesh.hullLength=vertices; return mesh;
}
static rig::Skeleton linkedTestSkeleton() {
    rig::Skeleton sk; sk.addBone("root",-1,{},0); rig::Slot slot; slot.name="parts"; slot.activeAttachment=1;
    slot.attachments.push_back(linkedTestMesh());
    auto linked=linkedTestMesh(); linked.name="linked"; linked.linkedAttachment=0;
    linked.vertices.clear(); linked.uvs.clear(); linked.triangles.clear(); linked.hullLength=0;
    slot.attachments.push_back(linked); sk.slots.push_back(slot); return sk;
}
static void testLinkedMeshesAndSkinDeforms() {
    using namespace rig;
    Skeleton sk=linkedTestSkeleton(); std::string error;
    CHECK(validateSkeleton(sk,&error));
    Animation animation; animation.name="motion";
    setDeformKey(animation,0,0,0,std::vector<Vec2>(3,{5,0}),-1);
    sk.animations.push_back(animation);
    auto pose=makeAnimatedPose(sk,&sk.animations[0],0);
    CHECK(findDeform(pose,0,1) && near((*findDeform(pose,0,1))[0],{5,0}));
    Geometry geometry; computeGeometry(sk,pose,sk.slots[0],sk.slots[0].attachments[1],geometry,findDeform(pose,0,1));
    CHECK(geometry.positions.size()==3 && near(geometry.positions[0],{5,0}) && geometry.triangles.size()==3);
    sk.slots[0].attachments[0].vertices[0].x=7;
    CHECK(near(computeVertices(sk,pose,sk.slots[0],sk.slots[0].attachments[1],findDeform(pose,0,1))[0],{12,0}));
    sk.slots[0].attachments[1].inheritDeform=false;
    setDeformKey(sk.animations[0],0,1,0,std::vector<Vec2>(3,{9,0}),-1);
    pose=makeAnimatedPose(sk,&sk.animations[0],0);
    CHECK(findDeform(pose,0,1) && near((*findDeform(pose,0,1))[0],{9,0}));
    sk.slots[0].attachments[1].inheritDeform=true;
    auto serialized=toJson(sk,-1); CHECK(serialized.find("\"version\":8")!=std::string::npos);
    CHECK(serialized.find("\"linked\"")!=std::string::npos);
    Skeleton roundtrip; CHECK(fromJson(serialized,roundtrip,&error)); CHECK(roundtrip.slots[0].attachments[1].linkedAttachment==0);
    CHECK(roundtrip.slots[0].attachments[1].vertices.empty());
    auto oversized=serialized;
    const std::string linkNeedle="\"linked\":{\"attachment\":0";
    const auto linkAt=oversized.find(linkNeedle);
    CHECK(linkAt!=std::string::npos);
    if (linkAt!=std::string::npos) { oversized.replace(linkAt,linkNeedle.size(),"\"linked\":{\"attachment\":4294967296"); CHECK(!fromJson(oversized,roundtrip,&error)); }
    sk.removeAttachment(0,0);
    CHECK(sk.slots[0].attachments[0].linkedAttachment==-1 && sk.slots[0].attachments[0].vertices.size()==3);
    pose=makeAnimatedPose(sk,&sk.animations[0],0);
    CHECK(findDeform(pose,0,0) && near((*findDeform(pose,0,0))[0],{5,0}));
    CHECK(validateSkeleton(sk,&error));

    Skeleton skins; skins.addBone("root",-1,{},0);
    Slot slot; slot.name="outfit"; slot.activeAttachment=0;
    Attachment placeholder; placeholder.name="outfit"; placeholder.skinPlaceholder=true;
    slot.attachments.push_back(placeholder); skins.slots.push_back(slot);
    skins.skins.push_back({"small",{{0,"outfit",linkedTestMesh()}},{},{}});
    skins.skins.push_back({"large",{{0,"outfit",linkedTestMesh(4)}},{},{}});
    Animation variants; variants.name="variants";
    setDeformKey(variants,0,0,0,std::vector<Vec2>(3,{11,0}),0);
    setDeformKey(variants,0,0,0,std::vector<Vec2>(4,{22,0}),1);
    CHECK(variants.slots[0].deform[0].size()==2);
    skins.animations.push_back(variants);
    auto small=makeAnimatedPose(skins,&skins.animations[0],0,{0}), large=makeAnimatedPose(skins,&skins.animations[0],0,{1});
    CHECK(findDeform(small,0,0) && findDeform(small,0,0)->size()==3 && near((*findDeform(small,0,0))[0],{11,0}));
    CHECK(findDeform(large,0,0) && findDeform(large,0,0)->size()==4 && near((*findDeform(large,0,0))[0],{22,0}));
    CHECK(skins.dropStaleDeformKeys(0,0)==0 && validateSkeleton(skins,&error));
    CHECK(fromJson(toJson(skins,-1),roundtrip,&error));
    CHECK(roundtrip.animations[0].slots[0].deform[0].size()==2);
    Animation copy; copyKey(variants,copy,0,Channel::Deform,0,0,0.5f);
    CHECK(copy.slots[0].deform[0].size()==2); moveChannelKey(copy,0,Channel::Deform,0.5f,0.75f,0);
    CHECK(copy.slots[0].deform[0].size()==2 && near(copy.slots[0].deform[0][1].time,0.75f));
    Animation merged; mergeAnimation(merged,variants,0.1f); CHECK(merged.slots[0].deform[0].size()==2);
    removeDeformKey(copy,0,0,0.75f,0); CHECK(copy.slots[0].deform[0].size()==1 && copy.slots[0].deform[0][0].skin==1);

    auto& target=skins.skins[1].attachments[0].attachment;
    removeDeformKey(skins.animations[0],0,0,0,1);
    target.linkedAttachment=0; target.linkedSkin=0; target.inheritDeform=true;
    large=makeAnimatedPose(skins,&skins.animations[0],0,{1});
    CHECK(findDeform(large,0,0) && findDeform(large,0,0)->size()==3 && near((*findDeform(large,0,0))[0],{11,0}));
    Skeleton linkedMerge; linkedMerge.addBone("existing",-1,{},0); linkedMerge.skins.push_back({"existing",{},{},{}});
    mergeSkeleton(linkedMerge,skins);
    CHECK(linkedMerge.skins[2].attachments[0].attachment.linkedSkin==1 && validateSkeleton(linkedMerge,&error));
    skins.removeSkin(0);
    CHECK(skins.skins.size()==1 && skins.skins[0].attachments[0].attachment.linkedAttachment==-1);
    large=makeAnimatedPose(skins,&skins.animations[0],0,{0});
    CHECK(findDeform(large,0,0) && near((*findDeform(large,0,0))[0],{11,0}));
    CHECK(validateSkeleton(skins,&error));

    Skeleton cycle=linkedTestSkeleton(); cycle.slots[0].attachments[0].linkedAttachment=1;
    CHECK(!cycle.meshSource(0,cycle.slots[0].attachments[1]) && !validateSkeleton(cycle,&error));
    CHECK(!fromJson(toJson(cycle,-1),roundtrip,&error));
    cycle.slots[0].attachments[0].linkedAttachment=-1; cycle.slots[0].attachments[1].linkedAttachment=999;
    CHECK(!validateSkeleton(cycle,&error));
    pose=makeSetupPose(cycle); CHECK(computeVertices(cycle,pose,cycle.slots[0],cycle.slots[0].attachments[1]).empty());

    Skeleton destination; destination.addBone("existing",-1,{},0); destination.skins.push_back({"existing",{},{},{}});
    mergeSkeleton(destination,skins);
    CHECK(validateSkeleton(destination,&error));
    CHECK(destination.animations[0].slots[0].deform[0][0].skin==1);
}
