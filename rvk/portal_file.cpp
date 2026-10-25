// Captured interiors on disk (portal_file.h): a small binary file, its fields in order, little-endian as written.
#include "portal_file.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace rvk::portal {

namespace {

constexpr uint32_t kMagic = 0x504B5652;           // "RVKP"
constexpr uint32_t kVersion = 2;                  // (1: a character's attachments could be among the pieces)

struct Writer {
    FILE* f;
    bool ok = true;
    void Bytes(const void* p, size_t n) { if (ok && n && std::fwrite(p, 1, n, f) != n) ok = false; }
    template <class T> void Value(const T& v) { Bytes(&v, sizeof(v)); }
    template <class T> void Vector(const std::vector<T>& v)
    {
        Value(uint32_t(v.size()));
        Bytes(v.data(), v.size() * sizeof(T));
    }
};

struct Reader {
    FILE* f;
    bool ok = true;
    void Bytes(void* p, size_t n) { if (ok && n && std::fread(p, 1, n, f) != n) ok = false; }
    template <class T> void Value(T& v) { Bytes(&v, sizeof(v)); }
    template <class T> void Vector(std::vector<T>& v, uint32_t most)
    {
        uint32_t n = 0;
        Value(n);
        if (!ok || n > most) { ok = false; return; }
        v.resize(n);
        Bytes(v.data(), size_t(n) * sizeof(T));
    }
};

}  // namespace

uint64_t Hash(const void* data, size_t size, uint64_t seed)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

uint64_t GeometryHash(const CapturedPiece& p)
{
    return Hash(p.indices.data(), p.indices.size() * 2, Hash(p.vertices.data(), p.vertices.size()));
}

bool Save(const char* path, const Capture& c)
{
    // Written beside, then put in place: a crash mid-write leaves the last good file.
    const std::string temp = std::string(path) + ".new";
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f)
        return false;
    Writer w{f};
    w.Value(kMagic);
    w.Value(kVersion);
    w.Value(c.playfield);
    w.Value(uint32_t(c.entranceKnown));
    w.Value(c.entrance);
    w.Value(c.inward);
    w.Value(uint32_t(c.textures.size()));
    for (const CapturedTexture& t : c.textures) {
        w.Value(t.rdbId);
        w.Value(t.width);
        w.Value(t.height);
        w.Value(uint32_t(t.format));
        w.Value(t.hash);
        w.Vector(t.level0);
    }
    w.Value(uint32_t(c.pieces.size()));
    for (const CapturedPiece& p : c.pieces) {
        w.Value(p.key);
        w.Value(p.kind);
        w.Value(p.fvf);
        w.Value(p.primitive);
        w.Value(p.stride);
        w.Value(p.vertexCount);
        w.Value(p.world);
        w.Value(p.texture);
        w.Value(p.material);
        w.Value(p.rs);
        w.Value(p.tss);
        w.Vector(p.lights);
        w.Vector(p.vertices);
        w.Vector(p.indices);
    }
    const bool ok = w.ok && std::fclose(f) == 0;
    if (!ok) {
        std::remove(temp.c_str());
        return false;
    }
    std::remove(path);
    return std::rename(temp.c_str(), path) == 0;
}

bool Load(const char* path, Capture& c)
{
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    Reader r{f};
    uint32_t magic = 0, version = 0, entrance = 0, textures = 0, pieces = 0;
    r.Value(magic);
    r.Value(version);
    if (!r.ok || magic != kMagic || version != kVersion) {
        std::fclose(f);
        return false;
    }
    c = Capture{};
    r.Value(c.playfield);
    r.Value(entrance);
    c.entranceKnown = entrance != 0;
    r.Value(c.entrance);
    r.Value(c.inward);
    r.Value(textures);
    if (textures > 4096) r.ok = false;
    if (r.ok) c.textures.resize(textures);
    for (CapturedTexture& t : c.textures) {
        uint32_t format = 0;
        r.Value(t.rdbId);
        r.Value(t.width);
        r.Value(t.height);
        r.Value(format);
        r.Value(t.hash);
        r.Vector(t.level0, 64u << 20);
        if (format >= uint32_t(Format::Count)) r.ok = false;
        t.format = Format(format);
        if (!r.ok) break;
    }
    r.Value(pieces);
    if (pieces > 65536) r.ok = false;
    if (r.ok) c.pieces.resize(pieces);
    for (CapturedPiece& p : c.pieces) {
        r.Value(p.key);
        r.Value(p.kind);
        r.Value(p.fvf);
        r.Value(p.primitive);
        r.Value(p.stride);
        r.Value(p.vertexCount);
        r.Value(p.world);
        r.Value(p.texture);
        r.Value(p.material);
        r.Value(p.rs);
        r.Value(p.tss);
        r.Vector(p.lights, 8);
        r.Vector(p.vertices, 16u << 20);
        r.Vector(p.indices, 4u << 20);
        if (!r.ok) break;
        // (Sound: the vertices hold vertexCount of stride, the indices stay inside them, the textures exist.)
        if (size_t(p.vertexCount) * p.stride != p.vertices.size()) r.ok = false;
        for (uint16_t i : p.indices)
            if (i >= p.vertexCount) { r.ok = false; break; }
        for (int32_t t : p.texture)
            if (t >= int32_t(c.textures.size())) r.ok = false;
        if (!r.ok) break;
    }
    std::fclose(f);
    if (!r.ok)
        c = Capture{};
    return r.ok;
}

}  // namespace rvk::portal
