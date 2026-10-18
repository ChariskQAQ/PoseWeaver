#include "core/serialize.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace rig {

// Version history:
//   1  bones, slots, region/mesh attachments, rotate/translate/scale + attachment keys
//   2  shear, Bezier curves, slot colour keys, draw order keys, events
//   3  mesh hull, clipping / bounding box attachments, deform keys
//   4  constraints (IK, transform, path, physics), path attachments, constraint keys
//   5  skins (placeholder attachments, skin entries, skin bones / constraints)
static constexpr int kFormatVersion = 5;

static const char* constraintTypeName(ConstraintType t) {
    switch (t) {
    case ConstraintType::Transform: return "transform";
    case ConstraintType::Path: return "path";
    case ConstraintType::Physics: return "physics";
    default: return "ik";
    }
}
static ConstraintType constraintTypeFrom(const std::string& s) {
    if (s == "transform") return ConstraintType::Transform;
    if (s == "path") return ConstraintType::Path;
    if (s == "physics") return ConstraintType::Physics;
    return ConstraintType::Ik;
}
static json valuesJson(const ConstraintValues& v) { return json::array({v[0], v[1], v[2], v[3], v[4], v[5]}); }
static ConstraintValues valuesFrom(const json& j, ConstraintValues def) {
    for (int i = 0; i < 6 && i < (int)j.size(); ++i) def[i] = j[i].get<float>();
    return def;
}

static const char* typeName(AttachmentType t) {
    switch (t) {
    case AttachmentType::Mesh: return "mesh";
    case AttachmentType::Clipping: return "clipping";
    case AttachmentType::BoundingBox: return "boundingbox";
    case AttachmentType::Path: return "path";
    default: return "region";
    }
}
static AttachmentType typeFrom(const std::string& s) {
    if (s == "mesh") return AttachmentType::Mesh;
    if (s == "clipping") return AttachmentType::Clipping;
    if (s == "boundingbox") return AttachmentType::BoundingBox;
    if (s == "path") return AttachmentType::Path;
    return AttachmentType::Region;
}

// Curves are written as "linear", "stepped" or [cx1, cy1, cx2, cy2] for Bezier.
static json curveJson(const CurveShape& c) {
    if (c.type == Curve::Stepped) return "stepped";
    if (c.type == Curve::Bezier) return json::array({c.cx1, c.cy1, c.cx2, c.cy2});
    return "linear";
}
static CurveShape curveFrom(const json& key) {
    if (!key.contains("curve")) return {};
    const json& c = key["curve"];
    if (c.is_array() && c.size() == 4)
        return CurveShape::bezier(c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>());
    if (c.is_string() && c.get<std::string>() == "stepped") return CurveShape::stepped();
    return {};
}

static json transformJson(const Transform& t) {
    json j{{"x", t.x}, {"y", t.y}, {"rotation", t.rotation}, {"scaleX", t.scaleX}, {"scaleY", t.scaleY}};
    if (t.shearX != 0) j["shearX"] = t.shearX;
    if (t.shearY != 0) j["shearY"] = t.shearY;
    return j;
}
static Transform transformFrom(const json& j) {
    Transform t;
    t.x = j.value("x", 0.0f);
    t.y = j.value("y", 0.0f);
    t.rotation = j.value("rotation", 0.0f);
    t.scaleX = j.value("scaleX", 1.0f);
    t.scaleY = j.value("scaleY", 1.0f);
    t.shearX = j.value("shearX", 0.0f);
    t.shearY = j.value("shearY", 0.0f);
    return t;
}

static json colorJson(const Color& c) { return json::array({c[0], c[1], c[2], c[3]}); }
static Color colorFrom(const json& j) {
    Color c;
    for (int i = 0; i < 4 && i < (int)j.size(); ++i) c[i] = j[i].get<float>();
    return c;
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

static json vec2Keys(const std::vector<Vec2Key>& keys) {
    json a = json::array();
    for (auto& k : keys) a.push_back({{"time", k.time}, {"x", k.value.x}, {"y", k.value.y}, {"curve", curveJson(k.curve)}});
    return a;
}
static std::vector<Vec2Key> vec2KeysFrom(const json& j, const char* name) {
    std::vector<Vec2Key> keys;
    if (!j.contains(name)) return keys;
    for (auto& k : j[name])
        keys.push_back({k.at("time").get<float>(), {k.at("x").get<float>(), k.at("y").get<float>()}, curveFrom(k)});
    return keys;
}

static json attachmentJson(const Attachment& a) {
    json ja{{"name", a.name}, {"image", a.image}, {"type", typeName(a.type)}};
    if (a.skinPlaceholder) {
        ja["placeholder"] = true;
        return ja;
    }
    if (a.type == AttachmentType::Region) {
        ja["transform"] = transformJson(a.transform);
        return ja;
    }
    if (a.type == AttachmentType::Mesh) {
        ja["uvs"] = vecList(a.uvs);
        ja["triangles"] = a.triangles;
        if (a.hullLength > 0) ja["hull"] = a.hullLength;
    }
    if (a.type == AttachmentType::Clipping) ja["end"] = a.endSlot;
    if (a.type == AttachmentType::Path) ja["closed"] = a.closed;
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
    return ja;
}

static Attachment attachmentFrom(const json& ja) {
    Attachment a;
    a.name = ja.at("name").get<std::string>();
    a.image = ja.value("image", -1);
    a.type = typeFrom(ja.value("type", std::string("region")));
    a.skinPlaceholder = ja.value("placeholder", false);
    if (a.skinPlaceholder) return a;
    if (a.type == AttachmentType::Region) {
        if (ja.contains("transform")) a.transform = transformFrom(ja["transform"]);
        return a;
    }
    if (a.type == AttachmentType::Mesh) {
        a.uvs = vecListFrom(ja.at("uvs"));
        a.triangles = ja.at("triangles").get<std::vector<int>>();
        a.hullLength = ja.value("hull", 0);
    }
    a.endSlot = ja.value("end", -1);
    a.closed = ja.value("closed", false);
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
    if (a.isPolygon()) a.hullLength = (int)a.vertexCount();
    return a;
}

static json toDocument(const Skeleton& sk) {
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
        if (b.skinRequired) jb["skin"] = true;
        bones.push_back(jb);
    }
    root["bones"] = bones;

    json slots = json::array();
    for (auto& s : sk.slots) {
        json js{{"name", s.name}, {"bone", s.bone}, {"active", s.activeAttachment}, {"color", colorJson(s.color)}};
        json atts = json::array();
        for (auto& a : s.attachments) atts.push_back(attachmentJson(a));
        js["attachments"] = atts;
        slots.push_back(js);
    }
    root["slots"] = slots;

    json events = json::array();
    for (auto& e : sk.events) {
        json je{{"name", e.name}, {"int", e.intValue}, {"float", e.floatValue}, {"string", e.stringValue}};
        if (!e.audioPath.empty()) {
            je["audio"] = e.audioPath;
            je["volume"] = e.volume;
            je["balance"] = e.balance;
        }
        events.push_back(je);
    }
    root["events"] = events;

    json constraints = json::array();
    for (auto& c : sk.constraints) {
        json jc{{"name", c.name}, {"type", constraintTypeName(c.type)}, {"bones", c.bones}, {"target", c.target},
                {"values", valuesJson(c.values)}};
        if (c.skinRequired) jc["skin"] = true;
        switch (c.type) {
        case ConstraintType::Ik:
            jc["compress"] = c.compress;
            jc["stretch"] = c.stretch;
            jc["uniform"] = c.uniform;
            break;
        case ConstraintType::Transform:
            jc["offset"] = {c.offsetRotation, c.offsetX, c.offsetY, c.offsetScaleX, c.offsetScaleY, c.offsetShearY};
            jc["local"] = c.local;
            jc["relative"] = c.relative;
            break;
        case ConstraintType::Path:
            jc["positionMode"] = (int)c.positionMode;
            jc["spacingMode"] = (int)c.spacingMode;
            jc["rotateMode"] = (int)c.rotateMode;
            jc["offsetRotation"] = c.offsetRotation;
            break;
        case ConstraintType::Physics:
            jc["axes"] = {c.physX, c.physY, c.physRotate};
            jc["physics"] = {c.inertia, c.strength, c.damping, c.mass, c.limit};
            break;
        }
        constraints.push_back(jc);
    }
    root["constraints"] = constraints;

    json skins = json::array();
    for (auto& skin : sk.skins) {
        json entries = json::array();
        for (auto& e : skin.attachments)
            entries.push_back({{"slot", e.slot}, {"placeholder", e.placeholder}, {"attachment", attachmentJson(e.attachment)}});
        skins.push_back({{"name", skin.name}, {"bones", skin.bones}, {"constraints", skin.constraints}, {"attachments", entries}});
    }
    root["skins"] = skins;

    json anims = json::array();
    for (auto& an : sk.animations) {
        json ja{{"name", an.name}, {"duration", an.duration}};
        json bt = json::array();
        for (auto& [bi, tl] : an.bones) {
            json jt{{"bone", bi}};
            json r = json::array();
            for (auto& k : tl.rotate) r.push_back({{"time", k.time}, {"value", k.value}, {"curve", curveJson(k.curve)}});
            jt["rotate"] = r;
            jt["translate"] = vec2Keys(tl.translate);
            jt["scale"] = vec2Keys(tl.scale);
            if (!tl.shear.empty()) jt["shear"] = vec2Keys(tl.shear);
            bt.push_back(jt);
        }
        ja["bones"] = bt;
        json st = json::array();
        for (auto& [si, tl] : an.slots) {
            json keys = json::array();
            for (auto& k : tl.attachment) keys.push_back({{"time", k.time}, {"value", k.value}});
            json jt{{"slot", si}, {"attachment", keys}};
            if (!tl.deform.empty()) {
                json dj = json::array();
                for (auto& [ai, dk] : tl.deform) {
                    json keysJ = json::array();
                    for (auto& k : dk) keysJ.push_back({{"time", k.time}, {"offsets", vecList(k.value)}, {"curve", curveJson(k.curve)}});
                    dj.push_back({{"attachment", ai}, {"keys", keysJ}});
                }
                jt["deform"] = dj;
            }
            if (!tl.color.empty()) {
                json ck = json::array();
                for (auto& k : tl.color) ck.push_back({{"time", k.time}, {"color", colorJson(k.value)}, {"curve", curveJson(k.curve)}});
                jt["color"] = ck;
            }
            st.push_back(jt);
        }
        ja["slots"] = st;
        if (!an.drawOrder.empty()) {
            json dk = json::array();
            for (auto& k : an.drawOrder) dk.push_back({{"time", k.time}, {"order", k.order}});
            ja["drawOrder"] = dk;
        }
        if (!an.constraints.empty()) {
            json ck = json::array();
            for (auto& [ci, keys] : an.constraints) {
                json kj = json::array();
                for (auto& k : keys) kj.push_back({{"time", k.time}, {"values", valuesJson(k.value)}, {"curve", curveJson(k.curve)}});
                ck.push_back({{"constraint", ci}, {"keys", kj}});
            }
            ja["constraints"] = ck;
        }
        if (!an.events.empty()) {
            json ek = json::array();
            for (auto& k : an.events)
                ek.push_back({{"time", k.time}, {"event", k.event}, {"int", k.intValue}, {"float", k.floatValue}, {"string", k.stringValue}});
            ja["events"] = ek;
        }
        anims.push_back(ja);
    }
    root["animations"] = anims;
    return root;
}

// Rounds every float in the document (smaller files; the shortest repr is printed).
static void roundFloats(json& j, int decimals) {
    if (decimals < 0) return;
    double scale = std::pow(10.0, decimals);
    if (j.is_number_float()) {
        j = std::round(j.get<double>() * scale) / scale;
    } else if (j.is_array() || j.is_object()) {
        for (auto& child : j) roundFloats(child, decimals);
    }
}

std::string toJson(const Skeleton& sk, int indent) { return toDocument(sk).dump(indent); }

std::string toJson(const Skeleton& sk, const ExportOptions& options) {
    json doc = toDocument(sk);
    roundFloats(doc, options.decimals);
    return doc.dump(options.indent);
}

static const uint8_t kBinaryMagic[4] = {'R', 'I', 'G', 'B'};

std::vector<uint8_t> toBinary(const Skeleton& sk, int decimals) {
    json doc = toDocument(sk);
    roundFloats(doc, decimals);
    std::vector<uint8_t> body = json::to_msgpack(doc);
    std::vector<uint8_t> out(kBinaryMagic, kBinaryMagic + 4);
    out.push_back((uint8_t)kFormatVersion);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

static bool validate(const Skeleton& sk, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    int nb = (int)sk.bones.size(), ns = (int)sk.slots.size(), ni = (int)sk.images.size(), ne = (int)sk.events.size();
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
            if (a.type == AttachmentType::Region || a.skinPlaceholder) continue;
            size_t nv = a.vertexCount();
            if (a.type == AttachmentType::Mesh && a.uvs.size() != nv) return fail("mesh " + a.name + ": vertex count mismatch");
            if (a.hullLength < 0 || a.hullLength > (int)nv) return fail("mesh " + a.name + ": invalid hull");
            if (a.endSlot < -1 || a.endSlot >= ns) return fail("clipping " + a.name + ": invalid end slot");
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
        for (auto& [si, tl] : an.slots) {
            if (si < 0 || si >= ns) return fail("animation " + an.name + ": invalid slot");
            for (auto& [ai, keys] : tl.deform) {
                if (ai < 0 || ai >= (int)sk.slots[si].attachments.size()) return fail("animation " + an.name + ": invalid deform attachment");
                if (sk.slots[si].attachments[ai].skinPlaceholder) continue;  // skin attachments may differ
                size_t nv = sk.slots[si].attachments[ai].vertexCount();
                for (auto& k : keys)
                    if (k.value.size() != nv) return fail("animation " + an.name + ": deform key vertex count mismatch");
            }
        }
        for (auto& k : an.drawOrder) {
            std::vector<bool> seen(ns, false);
            if ((int)k.order.size() != ns) return fail("animation " + an.name + ": draw order key has wrong size");
            for (int s : k.order) {
                if (s < 0 || s >= ns || seen[s]) return fail("animation " + an.name + ": draw order key is not a permutation");
                seen[s] = true;
            }
        }
        for (auto& k : an.events)
            if (k.event < 0 || k.event >= ne) return fail("animation " + an.name + ": invalid event");
        for (auto& [ci, keys] : an.constraints)
            if (ci < 0 || ci >= (int)sk.constraints.size()) return fail("animation " + an.name + ": invalid constraint");
    }
    for (auto& skin : sk.skins) {
        for (int b : skin.bones)
            if (b < 0 || b >= nb) return fail("skin " + skin.name + ": invalid bone");
        for (int c : skin.constraints)
            if (c < 0 || c >= (int)sk.constraints.size()) return fail("skin " + skin.name + ": invalid constraint");
        for (auto& e : skin.attachments) {
            if (e.slot < 0 || e.slot >= ns) return fail("skin " + skin.name + ": invalid slot");
            if (e.attachment.image < -1 || e.attachment.image >= ni) return fail("skin " + skin.name + ": invalid image");
        }
    }
    for (auto& c : sk.constraints) {
        for (int b : c.bones)
            if (b < 0 || b >= nb) return fail("constraint " + c.name + ": invalid bone");
        if (c.bones.empty()) return fail("constraint " + c.name + ": no bones");
        if (c.type == ConstraintType::Path ? (c.target < 0 || c.target >= ns)
                                           : (c.type != ConstraintType::Physics && (c.target < 0 || c.target >= nb)))
            return fail("constraint " + c.name + ": invalid target");
    }
    return true;
}

static bool fromDocument(const json& root, Skeleton& out, std::string* error);

bool fromJson(const std::string& text, Skeleton& out, std::string* error) {
    try {
        return fromDocument(json::parse(text), out, error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool fromBinary(const std::vector<uint8_t>& data, Skeleton& out, std::string* error) {
    if (data.size() < 5 || !std::equal(kBinaryMagic, kBinaryMagic + 4, data.begin())) {
        if (error) *error = "not a binary rig file";
        return false;
    }
    try {
        return fromDocument(json::from_msgpack(data.begin() + 5, data.end()), out, error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

static bool fromDocument(const json& root, Skeleton& out, std::string* error) {
    try {
        if (root.value("format", std::string()) != "rig") {
            if (error) *error = "not a rig file";
            return false;
        }
        if (root.value("version", 1) > kFormatVersion) {
            if (error) *error = "file was saved by a newer version (format " + std::to_string(root.value("version", 0)) + ")";
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
            b.skinRequired = j.value("skin", false);
            sk.bones.push_back(b);
        }
        for (auto& j : root.at("slots")) {
            Slot s;
            s.name = j.at("name").get<std::string>();
            s.bone = j.at("bone").get<int>();
            s.activeAttachment = j.value("active", -1);
            if (j.contains("color")) s.color = colorFrom(j["color"]);
            for (auto& ja : j.at("attachments")) {
                Attachment a = attachmentFrom(ja);
                s.attachments.push_back(a);
            }
            sk.slots.push_back(s);
        }
        if (root.contains("events")) {
            for (auto& je : root["events"]) {
                EventDef e;
                e.name = je.at("name").get<std::string>();
                e.intValue = je.value("int", 0);
                e.floatValue = je.value("float", 0.0f);
                e.stringValue = je.value("string", std::string());
                e.audioPath = je.value("audio", std::string());
                e.volume = je.value("volume", 1.0f);
                e.balance = je.value("balance", 0.0f);
                sk.events.push_back(e);
            }
        }
        if (root.contains("constraints")) {
            for (auto& jc : root["constraints"]) {
                Constraint c;
                c.name = jc.at("name").get<std::string>();
                c.type = constraintTypeFrom(jc.value("type", std::string("ik")));
                c.bones = jc.at("bones").get<std::vector<int>>();
                c.target = jc.value("target", -1);
                c.values = valuesFrom(jc.value("values", json::array()), defaultConstraintValues(c.type));
                c.skinRequired = jc.value("skin", false);
                c.compress = jc.value("compress", false);
                c.stretch = jc.value("stretch", false);
                c.uniform = jc.value("uniform", false);
                if (jc.contains("offset") && jc["offset"].size() == 6) {
                    const json& o = jc["offset"];
                    c.offsetRotation = o[0];
                    c.offsetX = o[1];
                    c.offsetY = o[2];
                    c.offsetScaleX = o[3];
                    c.offsetScaleY = o[4];
                    c.offsetShearY = o[5];
                }
                c.local = jc.value("local", false);
                c.relative = jc.value("relative", false);
                c.positionMode = (PathPositionMode)jc.value("positionMode", (int)PathPositionMode::Percent);
                c.spacingMode = (PathSpacingMode)jc.value("spacingMode", (int)PathSpacingMode::Length);
                c.rotateMode = (PathRotateMode)jc.value("rotateMode", (int)PathRotateMode::Tangent);
                if (c.type == ConstraintType::Path) c.offsetRotation = jc.value("offsetRotation", 0.0f);
                if (jc.contains("axes") && jc["axes"].size() == 3) {
                    c.physX = jc["axes"][0];
                    c.physY = jc["axes"][1];
                    c.physRotate = jc["axes"][2];
                }
                if (jc.contains("physics") && jc["physics"].size() == 5) {
                    const json& ph = jc["physics"];
                    c.inertia = ph[0];
                    c.strength = ph[1];
                    c.damping = ph[2];
                    c.mass = ph[3];
                    c.limit = ph[4];
                }
                sk.constraints.push_back(c);
            }
        }
        if (root.contains("skins")) {
            for (auto& js : root["skins"]) {
                Skin skin;
                skin.name = js.at("name").get<std::string>();
                skin.bones = js.value("bones", std::vector<int>{});
                skin.constraints = js.value("constraints", std::vector<int>{});
                for (auto& je : js.value("attachments", json::array())) {
                    SkinEntry e;
                    e.slot = je.at("slot").get<int>();
                    e.placeholder = je.at("placeholder").get<std::string>();
                    e.attachment = attachmentFrom(je.at("attachment"));
                    skin.attachments.push_back(std::move(e));
                }
                sk.skins.push_back(std::move(skin));
            }
        }
        for (auto& ja : root.at("animations")) {
            Animation an;
            an.name = ja.at("name").get<std::string>();
            an.duration = ja.value("duration", 1.0f);
            for (auto& jt : ja.at("bones")) {
                BoneTimeline tl;
                for (auto& k : jt.at("rotate")) tl.rotate.push_back({k.at("time").get<float>(), k.at("value").get<float>(), curveFrom(k)});
                tl.translate = vec2KeysFrom(jt, "translate");
                tl.scale = vec2KeysFrom(jt, "scale");
                tl.shear = vec2KeysFrom(jt, "shear");
                an.bones[jt.at("bone").get<int>()] = tl;
            }
            for (auto& jt : ja.at("slots")) {
                SlotTimeline tl;
                for (auto& k : jt.at("attachment")) tl.attachment.push_back({k.at("time").get<float>(), k.at("value").get<int>()});
                if (jt.contains("color"))
                    for (auto& k : jt["color"]) tl.color.push_back({k.at("time").get<float>(), colorFrom(k.at("color")), curveFrom(k)});
                if (jt.contains("deform"))
                    for (auto& dj : jt["deform"]) {
                        std::vector<DeformKey>& keys = tl.deform[dj.at("attachment").get<int>()];
                        for (auto& k : dj.at("keys")) keys.push_back({k.at("time").get<float>(), vecListFrom(k.at("offsets")), curveFrom(k)});
                    }
                an.slots[jt.at("slot").get<int>()] = tl;
            }
            if (ja.contains("drawOrder"))
                for (auto& k : ja["drawOrder"]) an.drawOrder.push_back({k.at("time").get<float>(), k.at("order").get<std::vector<int>>()});
            if (ja.contains("constraints"))
                for (auto& cj : ja["constraints"]) {
                    std::vector<ConstraintKey>& keys = an.constraints[cj.at("constraint").get<int>()];
                    for (auto& k : cj.at("keys")) keys.push_back({k.at("time").get<float>(), valuesFrom(k.at("values"), {}), curveFrom(k)});
                }
            if (ja.contains("events")) {
                for (auto& k : ja["events"]) {
                    EventKey e;
                    e.time = k.at("time").get<float>();
                    e.event = k.at("event").get<int>();
                    e.intValue = k.value("int", 0);
                    e.floatValue = k.value("float", 0.0f);
                    e.stringValue = k.value("string", std::string());
                    an.events.push_back(e);
                }
            }
            auto byTime = [](const auto& a, const auto& b) { return a.time < b.time; };
            std::stable_sort(an.drawOrder.begin(), an.drawOrder.end(), byTime);
            std::stable_sort(an.events.begin(), an.events.end(), byTime);
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

bool exportFile(const std::string& path, const Skeleton& sk, const ExportOptions& options, std::string* error) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open file for writing";
        return false;
    }
    if (options.binary) {
        std::vector<uint8_t> data = toBinary(sk, options.decimals);
        f.write((const char*)data.data(), (std::streamsize)data.size());
    } else {
        f << toJson(sk, options);
    }
    return (bool)f;
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
    std::string data = ss.str();
    if (data.size() >= 4 && std::equal(kBinaryMagic, kBinaryMagic + 4, (const uint8_t*)data.data()))
        return fromBinary(std::vector<uint8_t>(data.begin(), data.end()), out, error);
    return fromJson(data, out, error);
}

} // namespace rig
