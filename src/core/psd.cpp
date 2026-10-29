#include "core/psd.h"

#include <algorithm>
#include <stdexcept>

namespace rig {

namespace {
struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    void need(size_t n) const {
        if (pos + n > b.size()) throw std::runtime_error("unexpected end of file");
    }
    uint8_t u8() {
        need(1);
        return b[pos++];
    }
    uint16_t u16() {
        need(2);
        uint16_t v = (uint16_t)(b[pos] << 8 | b[pos + 1]);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v = (uint32_t)b[pos] << 24 | (uint32_t)b[pos + 1] << 16 | (uint32_t)b[pos + 2] << 8 | b[pos + 3];
        pos += 4;
        return v;
    }
    int32_t i32() { return (int32_t)u32(); }
    int16_t i16() { return (int16_t)u16(); }
    std::string str(size_t n) {
        need(n);
        std::string s(b.begin() + pos, b.begin() + pos + n);
        pos += n;
        return s;
    }
    void skip(size_t n) {
        need(n);
        pos += n;
    }
};

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

struct ChannelInfo {
    int16_t id;
    uint32_t length;
};

struct Record {
    PsdLayer layer;
    std::vector<ChannelInfo> channels;
    int section = 0;  // 0 normal, 1/2 group (open/closed), 3 group end marker
};

// Decodes one channel (compression header included) into `out` (width * height bytes).
void readChannel(Reader& r, size_t end, int w, int h, std::vector<uint8_t>& out) {
    out.assign(size_t(w) * h, 0);
    uint16_t compression = r.u16();
    if (w == 0 || h == 0) {
        r.pos = end;
        return;
    }
    if (compression == 0) {
        r.need(out.size());
        std::copy(r.b.begin() + r.pos, r.b.begin() + r.pos + out.size(), out.begin());
    } else if (compression == 1) {
        std::vector<uint16_t> counts(h);
        for (int y = 0; y < h; ++y) counts[y] = r.u16();
        for (int y = 0; y < h; ++y) {
            size_t rowEnd = r.pos + counts[y];
            int x = 0;
            while (r.pos < rowEnd && x < w) {
                int8_t n = (int8_t)r.u8();
                if (n >= 0) {
                    for (int k = 0; k <= n && x < w; ++k) out[size_t(y) * w + x++] = r.u8();
                } else if (n != -128) {
                    uint8_t v = r.u8();
                    for (int k = 0; k <= -n && x < w; ++k) out[size_t(y) * w + x++] = v;
                }
            }
            r.pos = rowEnd;
        }
    } else {
        throw std::runtime_error("unsupported channel compression (only raw and RLE)");
    }
    r.pos = end;
}
} // namespace

bool parsePsd(const std::vector<uint8_t>& bytes, PsdDocument& doc, std::string* error) {
    try {
        Reader r{bytes};
        if (r.str(4) != "8BPS") throw std::runtime_error("not a PSD file");
        if (r.u16() != 1) throw std::runtime_error("only PSD (not PSB) files are supported");
        r.skip(6);
        r.u16();  // channels of the merged image
        doc.height = (int)r.u32();
        doc.width = (int)r.u32();
        int depth = r.u16(), mode = r.u16();
        if (depth != 8) throw std::runtime_error("only 8-bit documents are supported");
        if (mode != 3) throw std::runtime_error("only RGB documents are supported");
        r.skip(r.u32());  // colour mode data
        r.skip(r.u32());  // image resources
        uint32_t layerAndMask = r.u32();
        size_t lmEnd = r.pos + layerAndMask;
        doc.layers.clear();
        if (layerAndMask == 0) return true;  // no layers: nothing to import
        uint32_t layerInfoLen = r.u32();
        size_t liEnd = r.pos + layerInfoLen;
        if (layerInfoLen == 0) return true;
        int count = r.i16();
        if (count < 0) count = -count;

        std::vector<Record> records(count);
        for (Record& rec : records) {
            int top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
            rec.layer.left = left;
            rec.layer.top = top;
            rec.layer.width = std::max(0, right - left);
            rec.layer.height = std::max(0, bottom - top);
            int channels = r.u16();
            for (int c = 0; c < channels; ++c) {
                int16_t id = r.i16();
                uint32_t len = r.u32();
                rec.channels.push_back({id, len});
            }
            if (r.str(4) != "8BIM") throw std::runtime_error("bad layer blend signature");
            rec.layer.blendKey = r.str(4);
            if (rec.layer.blendKey == "lddg") rec.layer.blend = BlendMode::Additive;
            if (rec.layer.blendKey == "mul ") rec.layer.blend = BlendMode::Multiply;
            if (rec.layer.blendKey == "scrn") rec.layer.blend = BlendMode::Screen;
            rec.layer.opacity = r.u8() / 255.0f;
            r.u8();  // clipping
            uint8_t flags = r.u8();
            rec.layer.visible = !(flags & 0x02);
            r.u8();  // filler
            uint32_t extra = r.u32();
            size_t extraEnd = r.pos + extra;
            r.skip(r.u32());  // layer mask
            r.skip(r.u32());  // blending ranges
            uint8_t nameLen = r.u8();
            rec.layer.name = r.str(nameLen);
            size_t padded = (nameLen + 1 + 3) / 4 * 4;
            r.skip(padded - nameLen - 1);
            // Additional layer information.
            while (r.pos + 12 <= extraEnd) {
                std::string sig = r.str(4);
                if (sig != "8BIM" && sig != "8B64") break;
                std::string key = r.str(4);
                uint32_t len = r.u32();
                size_t blockEnd = r.pos + len;
                if (key == "luni" && len >= 4) {
                    uint32_t n = r.u32();
                    std::string name;
                    for (uint32_t i = 0; i < n && r.pos + 2 <= blockEnd; ++i) {
                        uint32_t cp = r.u16();
                        if (cp >= 0xD800 && cp < 0xDC00 && r.pos + 2 <= blockEnd) {
                            uint32_t lo = r.u16();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        if (cp) appendUtf8(name, cp);
                    }
                    rec.layer.name = name;
                } else if ((key == "lsct" || key == "lsdk") && len >= 4) {
                    rec.section = (int)r.u32();
                }
                r.pos = blockEnd + (len & 1);
            }
            r.pos = extraEnd;
        }

        // Channel image data, in record order.
        std::vector<uint8_t> plane;
        for (Record& rec : records) {
            PsdLayer& l = rec.layer;
            l.rgba.assign(size_t(l.width) * l.height * 4, 0);
            for (size_t p = 0; p < size_t(l.width) * l.height; ++p) l.rgba[p * 4 + 3] = 255;
            for (const ChannelInfo& ch : rec.channels) {
                size_t end = r.pos + ch.length;
                if (ch.id < -1 || ch.id > 2) {  // masks: skip
                    r.pos = end;
                    continue;
                }
                readChannel(r, end, l.width, l.height, plane);
                int offset = ch.id == -1 ? 3 : ch.id;
                for (size_t p = 0; p < plane.size(); ++p) l.rgba[p * 4 + offset] = plane[p];
            }
        }
        (void)liEnd;
        (void)lmEnd;

        // Group paths: walking top to bottom, a folder record opens a group and the end marker closes it.
        std::vector<std::string> stack;
        std::vector<std::string> paths(records.size());
        for (int i = (int)records.size() - 1; i >= 0; --i) {
            int sec = records[i].section;
            if (sec == 1 || sec == 2) {
                stack.push_back(records[i].layer.name);
            } else if (sec == 3) {
                if (!stack.empty()) stack.pop_back();
            } else {
                std::string path;
                for (auto& g : stack) path += (path.empty() ? "" : "/") + g;
                paths[i] = path;
            }
        }
        for (size_t i = 0; i < records.size(); ++i) {
            if (records[i].section != 0 || records[i].layer.width == 0 || records[i].layer.height == 0) continue;
            records[i].layer.groupPath = paths[i];
            doc.layers.push_back(std::move(records[i].layer));
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace rig
