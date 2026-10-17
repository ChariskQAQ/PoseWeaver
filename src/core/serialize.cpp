#include "core/serialize.h"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace rig {

static constexpr int kFormatVersion = 1;

static const char* curveName(Curve c) { return c == Curve::Stepped ? "stepped" : "linear"; }
static Curve curveFrom(const json& j) {
    return j.value("curve", std::string("linear")) == "stepped" ? Curve::Stepped : Curve::Linear;
}

static json transformJson(const Transform& t) {
    return {{"x", t.x}, {"y", t.y}, {"rotation", t.rotation}, {"scaleX", t.scaleX}, {"scaleY", t.scaleY}};
}
static Transform transformFrom(const json& j) {
    Transform t;
    t.x = j.value("x", 0.0f);
    t.y = j.value("y", 0.0f);
    t.rotation = j.value("rotation", 0.0f);
    t.scaleX = j.value("scaleX", 1.0f);
    t.scaleY = j.value("scaleY", 1.0f);
    return t;
}

static json vecList(const std::vector<Vec2>& v) {
    json a = json::array();
    for (auto& p : v) {
        a.push_back(p.x);
        a.push_back(p.y);
    }
    return a;
}
static std::vector<Vec2> vecListFrom(const json& a) {
    std::vector<Vec2> v;
    for (size_t i = 0; i + 1 < a.size(); i += 2) v.push_back({a[i].get<float>(), a[i + 1].get<float>()});
    return v;
}

std::string toJson(const Skeleton& sk, int indent) {
    json root;
    root["format"] = "rig";
    root["version"] = kFormatVersion;

    json images = json::array();
    for (auto& im : sk.images) images.push_back({{"path", im.path}, {"width", im.width}, {"height", im.height}});
    root["images"] = images;

    json bones = json::array();
    for (auto& b : sk.bones) {
        json jb = transformJson(b.setup);
        jb["name"] = b.name;
        jb["parent"] = b.parent;
        jb["length"] = b.length;
        bones.push_back(jb);
    }
    root["bones"] = bones;

    json slots = json::array();
    for (auto& s : sk.slots) {
        json js{{"name", s.name},
                {"bone", s.bone},
                {"active", s.activeAttachment},
                {"color", {s.color[0], s.color[1], s.color[2], s.color[3]}}};
        json atts = json::array();
        for (auto& a : s.attachments) {
            json ja{{"name", a.name}, {"image", a.image}};
            if (a.type == AttachmentType::Region) {
                ja["type"] = "region";
                ja["transform"] = transformJson(a.transform);
            } else {
                ja["type"] = "mesh";
                ja["uvs"] = vecList(a.uvs);
                ja["triangles"] = a.triangles;
                if (a.isWeighted()) {
                    json ws = json::array();
                    for (auto& list : a.weights) {
                        json jl = json::array();
                        for (auto& w : list) jl.push_back({w.bone, w.local.x, w.local.y, w.weight});
                        ws.push_back(jl);
                    }
                    ja["weights"] = ws;
                } else {
                    ja["vertices"] = vecList(a.vertices);
                }
            }
            atts.push_back(ja);
        }
        js["attachments"] = atts;
        slots.push_back(js);
    }
    root["slots"] = slots;

    json anims = json::array();
    for (auto& an : sk.animations) {
        json ja{{"name", an.name}, {"duration", an.duration}};
        json bt = json::array();
        for (auto& [bi, tl] : an.bones) {
            json jt{{"bone", bi}};
            json r = json::array(), t = json::array(), s = json::array();
            for (auto& k : tl.rotate) r.push_back({{"time", k.time}, {"value", k.value}, {"curve", curveName(k.curve)}});
            for (auto& k : tl.translate)
                t.push_back({{"time", k.time}, {"x", k.value.x}, {"y", k.value.y}, {"curve", curveName(k.curve)}});
            for (auto& k : tl.scale)
                s.push_back({{"time", k.time}, {"x", k.value.x}, {"y", k.value.y}, {"curve", curveName(k.curve)}});
            jt["rotate"] = r;
            jt["translate"] = t;
            jt["scale"] = s;
            bt.push_back(jt);
        }
        ja["bones"] = bt;
        json st = json::array();
        for (auto& [si, tl] : an.slots) {
            json keys = json::array();
            for (auto& k : tl.attachment) keys.push_back({{"time", k.time}, {"value", k.value}});
            st.push_back({{"slot", si}, {"attachment", keys}});
        }
        ja["slots"] = st;
        anims.push_back(ja);
    }
    root["animations"] = anims;
    return root.dump(indent);
}

static bool validate(const Skeleton& sk, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    int nb = (int)sk.bones.size(), ns = (int)sk.slots.size(), ni = (int)sk.images.size();
    for (int i = 0; i < nb; ++i) {
        int p = sk.bones[i].parent;
        if (p < -1 || p >= nb || p == i) return fail("bone " + sk.bones[i].name + ": invalid parent");
    }
    if ((int)sk.updateOrder().size() != nb) return fail("bone hierarchy contains a cycle");
    for (auto& s : sk.slots) {
        if (s.bone < 0 || s.bone >= nb) return fail("slot " + s.name + ": invalid bone");
        if (s.activeAttachment >= (int)s.attachments.size()) return fail("slot " + s.name + ": invalid active attachment");
        for (auto& a : s.attachments) {
            if (a.image < -1 || a.image >= ni) return fail("attachment " + a.name + ": invalid image");
            if (a.type != AttachmentType::Mesh) continue;
            size_t nv = a.uvs.size();
            if (a.isWeighted() ? a.weights.size() != nv : a.vertices.size() != nv)
                return fail("mesh " + a.name + ": vertex count mismatch");
            for (int t : a.triangles)
                if (t < 0 || t >= (int)nv) return fail("mesh " + a.name + ": invalid triangle index");
            for (auto& list : a.weights)
                for (auto& w : list)
                    if (w.bone < 0 || w.bone >= nb) return fail("mesh " + a.name + ": invalid weight bone");
        }
    }
    for (auto& an : sk.animations) {
        for (auto& [bi, tl] : an.bones)
            if (bi < 0 || bi >= nb) return fail("animation " + an.name + ": invalid bone");
        for (auto& [si, tl] : an.slots)
            if (si < 0 || si >= ns) return fail("animation " + an.name + ": invalid slot");
    }
    return true;
}

bool fromJson(const std::string& text, Skeleton& out, std::string* error) {
    try {
        json root = json::parse(text);
        if (root.value("format", std::string()) != "rig") {
            if (error) *error = "not a rig file";
            return false;
        }
        Skeleton sk;
        for (auto& j : root.at("images")) {
            Image im;
            im.path = j.at("path").get<std::string>();
            im.width = j.value("width", 0);
            im.height = j.value("height", 0);
            sk.images.push_back(im);
        }
        for (auto& j : root.at("bones")) {
            Bone b;
            b.name = j.at("name").get<std::string>();
            b.parent = j.value("parent", -1);
            b.length = j.value("length", 0.0f);
            b.setup = transformFrom(j);
            sk.bones.push_back(b);
        }
        for (auto& j : root.at("slots")) {
            Slot s;
            s.name = j.at("name").get<std::string>();
            s.bone = j.at("bone").get<int>();
            s.activeAttachment = j.value("active", -1);
            if (j.contains("color"))
                for (int i = 0; i < 4; ++i) s.color[i] = j["color"][i].get<float>();
            for (auto& ja : j.at("attachments")) {
                Attachment a;
                a.name = ja.at("name").get<std::string>();
                a.image = ja.value("image", -1);
                if (ja.value("type", std::string("region")) == "mesh") {
                    a.type = AttachmentType::Mesh;
                    a.uvs = vecListFrom(ja.at("uvs"));
                    a.triangles = ja.at("triangles").get<std::vector<int>>();
                    if (ja.contains("weights")) {
                        for (auto& jl : ja["weights"]) {
                            std::vector<VertexWeight> list;
                            for (auto& jw : jl) {
                                VertexWeight w;
                                w.bone = jw[0].get<int>();
                                w.local = {jw[1].get<float>(), jw[2].get<float>()};
                                w.weight = jw[3].get<float>();
                                list.push_back(w);
                            }
                            a.weights.push_back(list);
                        }
                    } else {
                        a.vertices = vecListFrom(ja.at("vertices"));
                    }
                } else {
                    a.type = AttachmentType::Region;
                    if (ja.contains("transform")) a.transform = transformFrom(ja["transform"]);
                }
                s.attachments.push_back(a);
            }
            sk.slots.push_back(s);
        }
        for (auto& ja : root.at("animations")) {
            Animation an;
            an.name = ja.at("name").get<std::string>();
            an.duration = ja.value("duration", 1.0f);
            for (auto& jt : ja.at("bones")) {
                BoneTimeline tl;
                for (auto& k : jt.at("rotate")) tl.rotate.push_back({k.at("time").get<float>(), k.at("value").get<float>(), curveFrom(k)});
                for (auto& k : jt.at("translate"))
                    tl.translate.push_back({k.at("time").get<float>(), {k.at("x").get<float>(), k.at("y").get<float>()}, curveFrom(k)});
                for (auto& k : jt.at("scale"))
                    tl.scale.push_back({k.at("time").get<float>(), {k.at("x").get<float>(), k.at("y").get<float>()}, curveFrom(k)});
                an.bones[jt.at("bone").get<int>()] = tl;
            }
            for (auto& jt : ja.at("slots")) {
                SlotTimeline tl;
                for (auto& k : jt.at("attachment")) tl.attachment.push_back({k.at("time").get<float>(), k.at("value").get<int>()});
                an.slots[jt.at("slot").get<int>()] = tl;
            }
            sk.animations.push_back(an);
        }
        if (!validate(sk, error)) return false;
        out = std::move(sk);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool saveFile(const std::string& path, const Skeleton& sk, std::string* error) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open file for writing";
        return false;
    }
    f << toJson(sk);
    return (bool)f;
}

bool loadFile(const std::string& path, Skeleton& out, std::string* error) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open file";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return fromJson(ss.str(), out, error);
}

} // namespace rig
