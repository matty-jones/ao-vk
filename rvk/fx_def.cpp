// .rvkfx files (fx_def.h): a small JSON reader and the mapping of its fields onto the definitions.
#include "fx_def.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace rvk::fx {

namespace {

// A JSON value: what the files need (objects, arrays, numbers, strings, true / false / null).
struct Value {
    enum Type { kNull, kBool, kNumber, kString, kArray, kObject } type = kNull;
    double number = 0.0;
    bool boolean = false;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* Get(const char* key) const
    {
        if (type != kObject) return nullptr;
        for (const auto& [k, v] : object)
            if (k == key) return &v;
        return nullptr;
    }
};

struct Reader {
    const char* p;
    const char* end;
    std::string error;

    void Space()
    {
        while (p < end) {
            if (std::isspace(uint8_t(*p))) {
                ++p;
            } else if (p + 1 < end && p[0] == '/' && p[1] == '/') {   // (comments, for hand-written files)
                while (p < end && *p != '\n') ++p;
            } else {
                break;
            }
        }
    }
    bool Fail(const char* what)
    {
        if (error.empty()) error = what;
        return false;
    }
    bool String(std::string& out)
    {
        if (p >= end || *p != '"') return Fail("a string expected");
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                const char e = *p++;
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'u' ? '?' : e;
                if (e == 'u') p = std::min(p + 4, end);   // (\uXXXX: not needed by the files, skipped)
            }
            out += c;
        }
        if (p >= end) return Fail("an unterminated string");
        ++p;
        return true;
    }
    bool Parse(Value& v, int depth)
    {
        if (depth > 32) return Fail("nested too deep");
        Space();
        if (p >= end) return Fail("unexpected end");
        if (*p == '{') {
            ++p;
            v.type = Value::kObject;
            Space();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                Space();
                std::string key;
                if (!String(key)) return false;
                Space();
                if (p >= end || *p != ':') return Fail("':' expected");
                ++p;
                Value item;
                if (!Parse(item, depth + 1)) return false;
                v.object.emplace_back(std::move(key), std::move(item));
                Space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return Fail("',' or '}' expected");
            }
        }
        if (*p == '[') {
            ++p;
            v.type = Value::kArray;
            Space();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Value item;
                if (!Parse(item, depth + 1)) return false;
                v.array.push_back(std::move(item));
                Space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return Fail("',' or ']' expected");
            }
        }
        if (*p == '"') {
            v.type = Value::kString;
            return String(v.string);
        }
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; v.type = Value::kBool; v.boolean = true; return true; }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; v.type = Value::kBool; return true; }
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; return true; }
        char* stop = nullptr;
        const std::string number(p, std::min<size_t>(size_t(end - p), 64));
        v.number = std::strtod(number.c_str(), &stop);
        if (stop == number.c_str()) return Fail("a value expected");
        p += stop - number.c_str();
        v.type = Value::kNumber;
        return true;
    }
};

float Num(const Value* v, float fallback)
{
    return v && v->type == Value::kNumber && std::isfinite(v->number) ? float(v->number) : fallback;
}

void Pair(const Value* v, float out[2])
{
    if (!v) return;
    if (v->type == Value::kNumber) {
        out[0] = out[1] = Num(v, out[0]);
    } else if (v->type == Value::kArray && v->array.size() >= 2) {
        out[0] = Num(&v->array[0], out[0]);
        out[1] = Num(&v->array[1], out[1]);
        if (out[1] < out[0]) std::swap(out[0], out[1]);
    }
}

void Vec3(const Value* v, float out[3])
{
    if (v && v->type == Value::kArray && v->array.size() >= 3)
        for (int i = 0; i < 3; ++i) out[i] = Num(&v->array[size_t(i)], out[i]);
}

// "#RRGGBB" (or "RRGGBB") -> 0..1.
bool Hex(const std::string& s, float rgb[3])
{
    const char* h = s.c_str() + (s.size() && s[0] == '#' ? 1 : 0);
    if (std::strlen(h) != 6) return false;
    char* stop = nullptr;
    const unsigned long v = std::strtoul(h, &stop, 16);
    if (*stop) return false;
    rgb[0] = float((v >> 16) & 0xFF) / 255.0f;
    rgb[1] = float((v >> 8) & 0xFF) / 255.0f;
    rgb[2] = float(v & 0xFF) / 255.0f;
    return true;
}

uint32_t Choice(const Value* v, std::initializer_list<const char*> names, uint32_t fallback)
{
    if (!v || v->type != Value::kString) return fallback;
    uint32_t i = 0;
    for (const char* n : names) {
        if (v->string == n) return i;
        ++i;
    }
    return fallback;
}

bool ReadEmitter(const Value& v, Emitter& e, std::string* error)
{
    if (v.type != Value::kObject) {
        if (error) *error = "an emitter isn't an object";
        return false;
    }
    if (const Value* n = v.Get("name"); n && n->type == Value::kString) e.name = n->string;
    e.at = Choice(v.Get("at"), {"head", "source", "target", "path"}, e.at);
    if (const Value* d = v.Get("during"); d && d->type == Value::kArray) {
        e.during = 0;
        for (const Value& x : d->array)
            e.during |= x.type == Value::kString ? (x.string == "charge" ? kCharge
                                                    : x.string == "flight" || x.string == "play" ? kFlight
                                                    : x.string == "impact" ? kImpact : 0u) : 0u;
    }
    e.shape = Choice(v.Get("shape"), {"point", "sphere", "shell", "ring", "disc"}, e.shape);
    e.radius = std::max(Num(v.Get("radius"), e.radius), 0.0f);
    Vec3(v.Get("offset"), e.offset);
    e.rate = std::max(Num(v.Get("rate"), e.rate), 0.0f);
    e.burst = std::max(Num(v.Get("burst"), e.burst), 0.0f);
    e.count = uint32_t(std::clamp(Num(v.Get("count"), float(e.count)), 1.0f, float(kMaxParticles)));
    Pair(v.Get("life"), e.life);
    e.life[0] = std::max(e.life[0], 0.01f);
    e.life[1] = std::max(e.life[1], e.life[0]);
    Pair(v.Get("speed"), e.speed);
    Vec3(v.Get("direction"), e.direction);
    e.spread = std::clamp(Num(v.Get("spread"), e.spread), 0.0f, 180.0f);
    if (const Value* f = v.Get("forward"); f && f->type == Value::kBool) e.forward = f->boolean;
    e.inherit = Num(v.Get("inherit"), e.inherit);
    Vec3(v.Get("gravity"), e.gravity);
    e.drag = std::max(Num(v.Get("drag"), e.drag), 0.0f);
    e.curl = Num(v.Get("curl"), e.curl);
    e.curlScale = std::max(Num(v.Get("curlScale"), e.curlScale), 0.01f);
    e.swirl = Num(v.Get("swirl"), e.swirl);
    e.pull = Num(v.Get("pull"), e.pull);
    // Size: a number, or [[t, size], ...] over the particle's life.
    if (const Value* s = v.Get("size")) {
        if (s->type == Value::kNumber) {
            e.size[0] = {0.0f, Num(s, 0.05f)};
            e.sizeKeys = 1;
        } else if (s->type == Value::kArray && !s->array.empty()) {
            e.sizeKeys = 0;
            for (const Value& k : s->array)
                if (k.type == Value::kArray && k.array.size() >= 2 && e.sizeKeys < kMaxKeys)
                    e.size[e.sizeKeys++] = {std::clamp(Num(&k.array[0], 0.0f), 0.0f, 1.0f), std::max(Num(&k.array[1], 0.05f), 0.0f)};
            if (!e.sizeKeys) e.sizeKeys = 1;
        }
    }
    // Colour: "#RRGGBB", or [[t, "#RRGGBB", alpha], ...] over the particle's life.
    if (const Value* c = v.Get("colour") ? v.Get("colour") : v.Get("color")) {
        if (c->type == Value::kString) {
            e.colour[0] = {};
            Hex(c->string, e.colour[0].rgba);
            e.colourKeys = 1;
        } else if (c->type == Value::kArray && !c->array.empty()) {
            e.colourKeys = 0;
            for (const Value& k : c->array) {
                if (k.type != Value::kArray || k.array.size() < 2 || e.colourKeys == kMaxKeys) continue;
                ColourKey key;
                key.t = std::clamp(Num(&k.array[0], 0.0f), 0.0f, 1.0f);
                if (k.array[1].type == Value::kString) Hex(k.array[1].string, key.rgba);
                key.rgba[3] = k.array.size() >= 3 ? std::clamp(Num(&k.array[2], 1.0f), 0.0f, 1.0f) : 1.0f;
                e.colour[e.colourKeys++] = key;
            }
            if (!e.colourKeys) e.colourKeys = 1;
        }
    }
    std::sort(e.size, e.size + e.sizeKeys, [](const SizeKey& a, const SizeKey& b) { return a.t < b.t; });
    // Size scale: the whole curve times this (the editor's one knob for how big the particles are).
    const float scale = std::max(Num(v.Get("scale"), 1.0f), 0.0f);
    for (uint32_t i = 0; i < e.sizeKeys; ++i) e.size[i].size *= scale;
    std::sort(e.colour, e.colour + e.colourKeys, [](const ColourKey& a, const ColourKey& b) { return a.t < b.t; });
    e.intensity = std::max(Num(v.Get("intensity"), e.intensity), 0.0f);
    e.sprite = Choice(v.Get("sprite"), {"dot", "spark", "ring", "star", "smoke", "flare"}, e.sprite);
    e.additive = Choice(v.Get("blend"), {"add", "alpha"}, e.additive ? 0u : 1u) == 0u;
    e.stretch = std::max(Num(v.Get("stretch"), e.stretch), 0.0f);
    return true;
}

}  // namespace

bool Parse(const std::string& text, Effect& out, std::string* error)
{
    Reader r{text.data(), text.data() + text.size(), {}};
    Value root;
    if (!r.Parse(root, 0)) {
        if (error) *error = r.error + " at character " + std::to_string(r.p - text.data());
        return false;
    }
    if (root.type != Value::kObject) {
        if (error) *error = "not a JSON object";
        return false;
    }
    out = Effect{};
    if (const Value* n = root.Get("name"); n && n->type == Value::kString) out.name = n->string;
    if (const Value* t = root.Get("templates"); t && t->type == Value::kArray)
        for (const Value& id : t->array)
            if (id.type == Value::kNumber && id.number > 0.0) out.templates.push_back(uint32_t(id.number));
    out.replace = Choice(root.Get("mode"), {"replace", "add"}, 0u) == 0u;
    if (const Value* list = root.Get("emitters"); list && list->type == Value::kArray)
        for (const Value& v : list->array) {
            if (out.emitters.size() == kMaxEmitters) break;
            Emitter e;
            if (!ReadEmitter(v, e, error))
                return false;
            out.emitters.push_back(std::move(e));
        }
    if (out.emitters.empty()) {
        if (error) *error = "no emitters";
        return false;
    }
    return true;
}

float LongestLife(const Effect& effect)
{
    float longest = 0.0f;
    for (const Emitter& e : effect.emitters) longest = std::max(longest, e.life[1]);
    return longest;
}

}  // namespace rvk::fx
