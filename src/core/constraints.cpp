#include "core/constraints.h"
#include "core/pose.h"

#include <algorithm>

namespace rig {

// ---------------------------------------------------------------- helpers

void setBoneWorld(const Skeleton& sk, Pose& pose, int bone, const Affine& world) {
    int p = sk.bones[bone].parent;
    Affine parentWorld = p >= 0 ? pose.world[p] : Affine{};
    pose.applied[bone] = decompose(parentWorld.inverse() * world);
    updateSubtree(sk, pose, bone);
}

// Rotates the linear part of a world matrix by `deg` (counter-clockwise), keeping the origin.
static Affine rotated(const Affine& m, float deg) {
    Affine r = Affine::fromTRS(0, 0, deg, 1, 1);
    Affine out = r * m;
    out.tx = m.tx;
    out.ty = m.ty;
    return out;
}

static float angleOf(Vec2 v) { return radToDeg(std::atan2(v.y, v.x)); }
static float axisLength(const Affine& m) { return std::sqrt(m.a * m.a + m.c * m.c); }

// Scales the world X axis (and optionally Y) of a matrix.
static Affine scaledX(Affine m, float s, bool uniform) {
    m.a *= s;
    m.c *= s;
    if (uniform) {
        m.b *= s;
        m.d *= s;
    }
    return m;
}

// ---------------------------------------------------------------- IK

static void ikOne(const Skeleton& sk, Pose& pose, const Constraint& c, int b, Vec2 target, float mix) {
    Affine w = pose.world[b];
    Vec2 origin = w.origin();
    float delta = wrapDeg(angleOf(target - origin) - w.rotationDeg()) * mix;
    Affine out = rotated(w, delta);
    float len = sk.bones[b].length * axisLength(w);
    float d = length(target - origin);
    if (len > 1e-4f && ((c.stretch && d > len) || (c.compress && d < len))) out = scaledX(out, 1 + (d / len - 1) * mix, c.uniform);
    setBoneWorld(sk, pose, b, out);
}

static void ikTwo(const Skeleton& sk, Pose& pose, const Constraint& c, int parent, int child, Vec2 target, float mix,
                  float softness, float bend) {
    Vec2 P = pose.world[parent].origin(), C = pose.world[child].origin();
    float l1 = length(C - P), l2 = sk.bones[child].length * axisLength(pose.world[child]);
    if (l1 < 1e-4f || l2 < 1e-4f) {
        ikOne(sk, pose, c, child, target, mix);
        return;
    }
    Vec2 toT = target - P;
    float d = length(toT);
    float reach = l1 + l2;
    // Stretch: lengthen both bones when the target is out of reach.
    if (c.stretch && d > reach && reach > 1e-4f) {
        float s = 1 + (d / reach - 1) * mix;
        setBoneWorld(sk, pose, parent, scaledX(pose.world[parent], s, c.uniform));
        // The child inherits part of the parent's scale; only add what is still missing.
        float inherited = sk.bones[child].length * axisLength(pose.world[child]);
        if (inherited > 1e-4f) setBoneWorld(sk, pose, child, scaledX(pose.world[child], l2 * s / inherited, c.uniform));
        C = pose.world[child].origin();
        l1 = length(C - P);
        l2 *= s;
        reach = l1 + l2;
    }
    // Softness: ease towards full extension instead of snapping straight.
    if (softness > 0 && d > reach - softness) {
        float sd = reach - softness;
        d = sd + softness * (1 - std::exp(-(d - sd) / softness));
    }
    float cosB = std::clamp((d * d - l1 * l1 - l2 * l2) / (2 * l1 * l2), -1.0f, 1.0f);
    float theta2 = std::acos(cosB) * (bend < 0 ? -1.0f : 1.0f);
    float theta1 = std::atan2(toT.y, toT.x) - std::atan2(l2 * std::sin(theta2), l1 + l2 * std::cos(theta2));
    // Parent: the direction towards the child's origin must become theta1.
    float d1 = wrapDeg(radToDeg(theta1) - angleOf(C - P)) * mix;
    setBoneWorld(sk, pose, parent, rotated(pose.world[parent], d1));
    // Child: its X axis must point along theta1 + theta2.
    float d2 = wrapDeg(radToDeg(theta1 + theta2) - pose.world[child].rotationDeg()) * mix;
    setBoneWorld(sk, pose, child, rotated(pose.world[child], d2));
}

static void applyIk(const Skeleton& sk, Pose& pose, const Constraint& c, const ConstraintValues& v) {
    if (c.target < 0 || c.target >= (int)sk.bones.size() || c.bones.empty()) return;
    float mix = std::clamp(v[0], 0.0f, 1.0f);
    if (mix <= 0) return;
    Vec2 target = pose.world[c.target].origin();
    if (c.bones.size() >= 2 && sk.bones[c.bones[1]].parent == c.bones[0])
        ikTwo(sk, pose, c, c.bones[0], c.bones[1], target, mix, std::max(0.0f, v[1]), v[2]);
    else
        ikOne(sk, pose, c, c.bones[0], target, mix);
}

// ---------------------------------------------------------------- transform

static void applyTransform(const Skeleton& sk, Pose& pose, const Constraint& c, const ConstraintValues& v) {
    if (c.target < 0 || c.target >= (int)sk.bones.size()) return;
    const float mr = v[0], mx = v[1], my = v[2], msx = v[3], msy = v[4], msh = v[5];
    for (int b : c.bones) {
        if (b == c.target || b < 0 || b >= (int)sk.bones.size()) continue;
        if (c.local) {
            Transform t = pose.applied[b];
            const Transform& tt = pose.applied[c.target];
            if (c.relative) {
                t.rotation += (tt.rotation + c.offsetRotation) * mr;
                t.x += (tt.x + c.offsetX) * mx;
                t.y += (tt.y + c.offsetY) * my;
                t.scaleX *= 1 + (tt.scaleX - 1 + c.offsetScaleX) * msx;
                t.scaleY *= 1 + (tt.scaleY - 1 + c.offsetScaleY) * msy;
                t.shearY += (tt.shearY + c.offsetShearY) * msh;
            } else {
                t.rotation += wrapDeg(tt.rotation + c.offsetRotation - t.rotation) * mr;
                t.x += (tt.x + c.offsetX - t.x) * mx;
                t.y += (tt.y + c.offsetY - t.y) * my;
                t.scaleX += (tt.scaleX + c.offsetScaleX - t.scaleX) * msx;
                t.scaleY += (tt.scaleY + c.offsetScaleY - t.scaleY) * msy;
                t.shearY += wrapDeg(tt.shearY + c.offsetShearY - t.shearY) * msh;
            }
            pose.applied[b] = t;
            updateSubtree(sk, pose, b);
            continue;
        }
        Transform wb = decompose(pose.world[b]);
        Transform wt = decompose(pose.world[c.target]);
        Vec2 tp = pose.world[c.target].apply({c.offsetX, c.offsetY});
        if (c.relative) {
            wb.rotation += (wt.rotation + c.offsetRotation) * mr;
            wb.x += (tp.x - wt.x) * mx;  // offset from the target, applied relative to the bone
            wb.y += (tp.y - wt.y) * my;
            wb.scaleX *= 1 + (wt.scaleX - 1 + c.offsetScaleX) * msx;
            wb.scaleY *= 1 + (wt.scaleY - 1 + c.offsetScaleY) * msy;
            wb.shearY += (wt.shearY + c.offsetShearY) * msh;
        } else {
            wb.rotation += wrapDeg(wt.rotation + c.offsetRotation - wb.rotation) * mr;
            wb.x += (tp.x - wb.x) * mx;
            wb.y += (tp.y - wb.y) * my;
            wb.scaleX += (wt.scaleX + c.offsetScaleX - wb.scaleX) * msx;
            wb.scaleY += (wt.scaleY + c.offsetScaleY - wb.scaleY) * msy;
            wb.shearY += wrapDeg(wt.shearY + c.offsetShearY - wb.shearY) * msh;
        }
        setBoneWorld(sk, pose, b, wb.matrix());
    }
}

// ---------------------------------------------------------------- paths

static Vec2 bezier(const std::array<Vec2, 4>& s, float t) {
    float u = 1 - t;
    return s[0] * (u * u * u) + s[1] * (3 * u * u * t) + s[2] * (3 * u * t * t) + s[3] * (t * t * t);
}

PathCurve buildPathCurve(const std::vector<Vec2>& cp, bool closed) {
    PathCurve path;
    int n = (int)cp.size() / 3;
    if (n < 2) return path;
    auto point = [&](int i) { return cp[(i % n) * 3 + 1]; };
    auto out = [&](int i) { return cp[(i % n) * 3 + 2]; };
    auto in = [&](int i) { return cp[(i % n) * 3]; };
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        std::array<Vec2, 4> s = {point(i), out(i), in(i + 1), point(i + 1)};
        float len = 0;
        Vec2 prev = s[0];
        for (int k = 1; k <= 32; ++k) {
            Vec2 p = bezier(s, k / 32.0f);
            len += length(p - prev);
            prev = p;
        }
        path.total += len;
        path.segments.push_back(s);
        path.lengths.push_back(path.total);
    }
    return path;
}

void evaluatePath(const PathCurve& path, float s, bool closed, Vec2& point, float& tangentDeg) {
    if (path.segments.empty()) {
        point = {};
        tangentDeg = 0;
        return;
    }
    if (closed && path.total > 0) {
        s = std::fmod(s, path.total);
        if (s < 0) s += path.total;
    }
    auto tangentAt = [&](const std::array<Vec2, 4>& seg, float t) {
        float t0 = std::max(0.0f, t - 0.01f), t1 = std::min(1.0f, t + 0.01f);
        return bezier(seg, t1) - bezier(seg, t0);
    };
    if (s <= 0) {  // extrapolate before the start along the first tangent
        Vec2 d = tangentAt(path.segments.front(), 0);
        float l = std::max(length(d), 1e-6f);
        point = path.segments.front()[0] + d * (s / l);
        tangentDeg = angleOf(d);
        return;
    }
    if (s >= path.total) {
        Vec2 d = tangentAt(path.segments.back(), 1);
        float l = std::max(length(d), 1e-6f);
        point = path.segments.back()[3] + d * ((s - path.total) / l);
        tangentDeg = angleOf(d);
        return;
    }
    size_t i = std::lower_bound(path.lengths.begin(), path.lengths.end(), s) - path.lengths.begin();
    i = std::min(i, path.segments.size() - 1);
    float segStart = i == 0 ? 0 : path.lengths[i - 1];
    // Walk the segment's arc length to find the parameter for `s`.
    const auto& seg = path.segments[i];
    float target = s - segStart, acc = 0, t = 1;
    Vec2 prev = seg[0];
    for (int k = 1; k <= 64; ++k) {
        Vec2 p = bezier(seg, k / 64.0f);
        float step = length(p - prev);
        if (acc + step >= target) {
            t = (k - 1 + (step > 0 ? (target - acc) / step : 0)) / 64.0f;
            break;
        }
        acc += step;
        prev = p;
    }
    point = bezier(seg, t);
    tangentDeg = angleOf(tangentAt(seg, t));
}

std::vector<Vec2> samplePath(const PathCurve& path, int perSegment) {
    std::vector<Vec2> out;
    for (auto& seg : path.segments)
        for (int k = out.empty() ? 0 : 1; k <= perSegment; ++k) out.push_back(bezier(seg, (float)k / perSegment));
    return out;
}

static void applyPath(const Skeleton& sk, Pose& pose, const Constraint& c, const ConstraintValues& v) {
    if (c.target < 0 || c.target >= (int)sk.slots.size()) return;
    const Slot& slot = sk.slots[c.target];
    int ai = c.target < (int)pose.slotAttachment.size() ? pose.slotAttachment[c.target] : slot.activeAttachment;
    if (ai < 0 || ai >= (int)slot.attachments.size() || slot.attachments[ai].type != AttachmentType::Path) return;
    const Attachment& att = slot.attachments[ai];
    PathCurve path = buildPathCurve(computeVertices(sk, pose, slot, att, findDeform(pose, c.target, ai)), att.closed);
    if (path.segments.empty()) return;
    const float position = v[0], spacing = v[1], mr = v[2], mx = v[3], my = v[4];
    float s = c.positionMode == PathPositionMode::Percent ? position * path.total : position;
    for (int b : c.bones) {
        if (b < 0 || b >= (int)sk.bones.size()) continue;
        const Affine& w = pose.world[b];
        float boneLen = sk.bones[b].length * axisLength(w);
        float space = c.spacingMode == PathSpacingMode::Length ? boneLen + spacing
                      : c.spacingMode == PathSpacingMode::Fixed ? spacing
                                                                : spacing * path.total;
        Vec2 p, next;
        float tangent, nextTangent;
        evaluatePath(path, s, att.closed, p, tangent);
        evaluatePath(path, s + space, att.closed, next, nextTangent);
        float angle = c.rotateMode == PathRotateMode::Tangent ? tangent : angleOf(next - p);
        Affine out = rotated(w, wrapDeg(angle + c.offsetRotation - w.rotationDeg()) * mr);
        out.tx = w.tx + (p.x - w.tx) * mx;
        out.ty = w.ty + (p.y - w.ty) * my;
        if (c.rotateMode == PathRotateMode::ChainScale && boneLen > 1e-4f) out = scaledX(out, 1 + (length(next - p) / boneLen - 1) * mr, false);
        setBoneWorld(sk, pose, b, out);
        s += space;
    }
}

// ---------------------------------------------------------------- dispatch

void applyConstraints(const Skeleton& sk, Pose& pose) {
    for (size_t i = 0; i < sk.constraints.size(); ++i) {
        const Constraint& c = sk.constraints[i];
        const ConstraintValues& v = i < pose.constraints.size() ? pose.constraints[i] : c.values;
        switch (c.type) {
        case ConstraintType::Ik: applyIk(sk, pose, c, v); break;
        case ConstraintType::Transform: applyTransform(sk, pose, c, v); break;
        case ConstraintType::Path: applyPath(sk, pose, c, v); break;
        case ConstraintType::Physics: break;  // needs time, see applyPhysics
        }
    }
}

// ---------------------------------------------------------------- physics

void applyPhysics(const Skeleton& sk, Pose& pose, PhysicsState& state, float dt) {
    state.bodies.resize(sk.constraints.size());
    const float h = 1.0f / 60.0f;
    for (size_t i = 0; i < sk.constraints.size(); ++i) {
        const Constraint& c = sk.constraints[i];
        if (c.type != ConstraintType::Physics || c.bones.empty()) continue;
        int b = c.bones[0];
        if (b < 0 || b >= (int)sk.bones.size()) continue;
        const ConstraintValues& v = i < pose.constraints.size() ? pose.constraints[i] : c.values;
        float mix = std::clamp(v[0], 0.0f, 1.0f);
        Vec2 external{v[1], -v[2]};  // wind (+x), gravity (-y)
        PhysicsState::Body& body = state.bodies[i];
        const Affine& w = pose.world[b];
        Vec2 origin = w.origin();
        Vec2 tip = w.apply({std::max(sk.bones[b].length, 10.0f), 0});
        if (!body.initialised) {
            body = PhysicsState::Body{};
            body.lastOrigin = origin;
            body.lastTip = tip;
            body.initialised = true;
        }
        if (dt > 0) {
            // Inertia: when the bone moves, the simulated point stays behind.
            body.offset -= (origin - body.lastOrigin) * c.inertia;
            body.tipOffset -= (tip - body.lastTip) * c.inertia;
            body.lastOrigin = origin;
            body.lastTip = tip;
            body.remainder = std::min(body.remainder + dt, 0.25f);
            // Damped spring pulling the lag back to 0; damping 1 = critically damped (no wobble).
            float mass = std::max(c.mass, 0.01f);
            float k = std::max(c.strength, 0.0f);
            float damp = std::max(c.damping, 0.0f) * 2.0f * std::sqrt(k * mass);
            auto step = [&](Vec2& off, Vec2& vel) {
                Vec2 acc = (off * -k - vel * damp) * (1.0f / mass) + external;
                vel += acc * h;
                float speed = length(vel);
                if (speed > c.limit) vel = vel * (c.limit / speed);
                off += vel * h;
                float dist = length(off);
                if (dist > 1000) off = off * (1000 / dist);
            };
            while (body.remainder >= h) {
                step(body.offset, body.velocity);
                step(body.tipOffset, body.tipVelocity);
                body.remainder -= h;
            }
        }
        if (mix <= 0) continue;
        Affine out = w;
        if (c.physRotate) {
            float angle = wrapDeg(angleOf(tip + body.tipOffset - origin) - angleOf(tip - origin));
            out = rotated(out, angle * mix);
        }
        if (c.physX) out.tx += body.offset.x * mix;
        if (c.physY) out.ty += body.offset.y * mix;
        setBoneWorld(sk, pose, b, out);
    }
}

} // namespace rig
