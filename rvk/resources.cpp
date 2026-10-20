// Texture formats, textures and render targets, samplers, vertex buffers and FVF layouts.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr VkComponentMapping kIdentity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
constexpr VkComponentMapping kOpaque = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE};
constexpr VkComponentMapping kLuminance = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                           VK_COMPONENT_SWIZZLE_ONE};
constexpr VkComponentMapping kAlphaOnly = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                           VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
constexpr VkComponentMapping kLuminanceAlpha = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                                VK_COMPONENT_SWIZZLE_G};

// Indexed by Format. D3D's 16-bit formats have the same bit layout as Vulkan's *_PACK16 formats.
const FormatInfo kFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, kIdentity, 4, false},          // A8R8G8B8
    {VK_FORMAT_B8G8R8A8_UNORM, kOpaque, 4, false},            // X8R8G8B8
    {VK_FORMAT_R5G6B5_UNORM_PACK16, kIdentity, 2, false},     // R5G6B5
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16, kIdentity, 2, false},   // A1R5G5B5
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16, kOpaque, 2, false},     // X1R5G5B5
    {VK_FORMAT_A4R4G4B4_UNORM_PACK16, kIdentity, 2, false},   // A4R4G4B4
    {VK_FORMAT_R8_UNORM, kLuminance, 1, false},               // L8
    {VK_FORMAT_R8_UNORM, kAlphaOnly, 1, false},               // A8
    {VK_FORMAT_R8G8_UNORM, kLuminanceAlpha, 2, false},        // A8L8
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, kIdentity, 8, true},     // DXT1 (1-bit alpha allowed)
    {VK_FORMAT_BC2_UNORM_BLOCK, kIdentity, 16, true},         // DXT2 (premultiplied; same storage)
    {VK_FORMAT_BC2_UNORM_BLOCK, kIdentity, 16, true},         // DXT3
    {VK_FORMAT_BC3_UNORM_BLOCK, kIdentity, 16, true},         // DXT4 (premultiplied; same storage)
    {VK_FORMAT_BC3_UNORM_BLOCK, kIdentity, 16, true},         // DXT5
    {VK_FORMAT_R16G16B16A16_SFLOAT, kIdentity, 8, false},     // RGBA16F (HDR scene)
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, kIdentity, 4, false}, // RG11B10F (HDR glow)
    {VK_FORMAT_R16G16_SFLOAT, kIdentity, 4, false},          // RG16F (ambient occlusion)
    {VK_FORMAT_R8G8B8A8_UNORM, kIdentity, 4, false},         // RGBA8 (local-light fraction, reflectivity, sun share)
};
static_assert(sizeof(kFormats) / sizeof(kFormats[0]) == size_t(Format::Count), "format table");

VkSamplerAddressMode AddressMode(uint32_t a)
{
    switch (a) {
    case d3d::TADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case d3d::TADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case d3d::TADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

}  // namespace

namespace detail {

const FormatInfo& GetFormatInfo(Format format) { return kFormats[size_t(format)]; }

FvfLayout DecodeFvf(uint32_t fvf)
{
    FvfLayout l;
    uint32_t pos = fvf & d3d::FVF_POSITION_MASK;
    l.offset[0] = 0;
    if (pos == d3d::FVF_XYZRHW) {
        l.format[0] = VK_FORMAT_R32G32B32A32_SFLOAT;
        l.stride = 16;
    } else {
        l.format[0] = VK_FORMAT_R32G32B32_SFLOAT;
        l.stride = 12 + (pos >= 0x6 ? ((pos - 0x4) / 2) * 4 : 0);   // XYZB1..5 blend weights
    }
    if (fvf & d3d::FVF_NORMAL) { l.offset[1] = l.stride; l.format[1] = VK_FORMAT_R32G32B32_SFLOAT; l.stride += 12; }
    if (fvf & d3d::FVF_PSIZE) l.stride += 4;
    if (fvf & d3d::FVF_DIFFUSE) { l.offset[2] = l.stride; l.format[2] = VK_FORMAT_B8G8R8A8_UNORM; l.stride += 4; }
    if (fvf & d3d::FVF_SPECULAR) { l.offset[3] = l.stride; l.format[3] = VK_FORMAT_B8G8R8A8_UNORM; l.stride += 4; }
    uint32_t texCount = (fvf & d3d::FVF_TEXCOUNT_MASK) >> d3d::FVF_TEXCOUNT_SHIFT;
    static const VkFormat kTexFormats[4] = {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                            VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT};
    static const uint32_t kTexSizes[4] = {8, 12, 16, 4};
    for (uint32_t i = 0; i < texCount; ++i) {
        uint32_t code = (fvf >> (16 + 2 * i)) & 3;
        if (i < 2) { l.offset[4 + i] = l.stride; l.format[4 + i] = kTexFormats[code]; }
        l.stride += kTexSizes[code];
    }
    return l;
}

}  // namespace detail

bool FormatIsCompressed(Format format) { return kFormats[size_t(format)].compressed; }

uint32_t FormatRowBytes(Format format, uint32_t width)
{
    const FormatInfo& f = kFormats[size_t(format)];
    return f.compressed ? std::max(1u, (width + 3) / 4) * f.blockBytes : width * f.blockBytes;
}

uint32_t FormatRows(Format format, uint32_t height)
{
    return kFormats[size_t(format)].compressed ? std::max(1u, (height + 3) / 4) : height;
}

uint32_t Device::FvfStride(uint32_t fvf) { return DecodeFvf(fvf).stride; }

// ---------------------------------------------------------------------------------------------------
// Textures

Texture* Device::NewTexture(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget)
{
    if (!width || !height)
        return nullptr;
    uint32_t maxLevels = 1;
    for (uint32_t s = std::max(width, height); s > 1; s >>= 1)
        ++maxLevels;
    auto* t = new Texture;
    t->m_width = width;
    t->m_height = height;
    t->m_levels = std::clamp(levels, 1u, maxLevels);
    t->m_format = format;
    t->m_renderTarget = renderTarget;
    return t;
}

bool Device::RealizeTexture(Texture* t)
{
    const FormatInfo& info = kFormats[size_t(t->m_format)];
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = info.vk;
    ci.extent = {t->m_width, t->m_height, 1};
    ci.mipLevels = t->m_levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (t->m_renderTarget)
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (t->m_storage)
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (t->m_renderTarget)
        ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(m_allocator, &ci, &ac, &t->m_image, &t->m_allocation, nullptr) != VK_SUCCESS) {
        t->m_image = VK_NULL_HANDLE;
        Log("texture creation failed (%ux%u, format %u)", t->m_width, t->m_height, uint32_t(t->m_format));
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = info.vk;
    vi.components = info.swizzle;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->m_levels, 0, 1};
    vkCreateImageView(m_device, &vi, nullptr, &t->m_view);
    RegisterBindlessTexture(t);
    VmaAllocationInfo info2;
    vmaGetAllocationInfo(m_allocator, t->m_allocation, &info2);
    t->m_bytes = info2.size;
    m_textureBytes[t->m_renderTarget ? 1 : 0] += t->m_bytes;
    ++m_textureCount[t->m_renderTarget ? 1 : 0];
    return true;
}

Texture* Device::CreateImage(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget, bool storage)
{
    Texture* t = NewTexture(width, height, format, levels, renderTarget);
    if (t) t->m_storage = storage;
    if (t && !RealizeTexture(t)) {
        delete t;
        return nullptr;
    }
    return t;
}

Texture* Device::CreateTexture(uint32_t width, uint32_t height, Format format, uint32_t levels)
{
    if (!m_formatSupported[size_t(format)]) {
        Log("texture format %u not supported by the GPU\n", uint32_t(format));
        return nullptr;
    }
    return CreateImage(width, height, format, levels, false);
}

Texture* Device::CreateTexture(uint32_t width, uint32_t height, const void* argbPixels)
{
    Texture* t = CreateTexture(width, height, Format::A8R8G8B8, 1);
    if (t)
        UpdateTexture(t, 0, 0, 0, width, height, argbPixels, width * 4);
    return t;
}

Texture* Device::CreateRenderTarget(uint32_t width, uint32_t height)
{
    return CreateImage(width, height, Format::A8R8G8B8, 1, true);
}

// Whether the pixels prove a texture fully opaque (alpha 1 everywhere), so a draw that blends it as a static (the
// game's cut-out idiom) can take the opaque path instead: no blend and no discard, so early-Z works and the
// overdraw of a screen full of blended statics collapses. Conservative: false means "not known to be opaque".
static bool PixelsOpaque(Format fmt, const void* data, uint32_t width, uint32_t height, uint32_t pitch)
{
    const uint8_t* base = static_cast<const uint8_t*>(data);
    auto rows = [&](auto check) {               // check(row, x) for every pixel
        for (uint32_t y = 0; y < height; ++y)
            if (!check(base + size_t(y) * pitch)) return false;
        return true;
    };
    switch (fmt) {
    case Format::X8R8G8B8: case Format::R5G6B5: case Format::X1R5G5B5: case Format::L8:
        return true;                            // no alpha channel: the shader reads alpha 1
    case Format::A8R8G8B8:
        return rows([&](const uint8_t* row) {
            for (uint32_t x = 0; x < width; ++x) if (row[size_t(x) * 4 + 3] != 0xFF) return false;
            return true; });
    case Format::A1R5G5B5:
        return rows([&](const uint8_t* row) {
            auto r = reinterpret_cast<const uint16_t*>(row);
            for (uint32_t x = 0; x < width; ++x) if ((r[x] & 0x8000u) == 0) return false;
            return true; });
    case Format::A4R4G4B4:
        return rows([&](const uint8_t* row) {
            auto r = reinterpret_cast<const uint16_t*>(row);
            for (uint32_t x = 0; x < width; ++x) if (((r[x] >> 12) & 0xF) != 0xF) return false;
            return true; });
    case Format::A8:
        return rows([&](const uint8_t* row) {
            for (uint32_t x = 0; x < width; ++x) if (row[x] != 0xFF) return false;
            return true; });
    case Format::A8L8:
        return rows([&](const uint8_t* row) {
            for (uint32_t x = 0; x < width; ++x) if (row[size_t(x) * 2 + 1] != 0xFF) return false;
            return true; });
    case Format::DXT1: {
        uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
        for (uint32_t y = 0; y < by; ++y) {
            const uint8_t* row = base + size_t(y) * pitch;
            for (uint32_t x = 0; x < bx; ++x) {
                const uint8_t* blk = row + size_t(x) * 8;
                uint16_t c0 = uint16_t(blk[0] | blk[1] << 8), c1 = uint16_t(blk[2] | blk[3] << 8);
                if (c0 <= c1) {                 // 3-colour mode: the index-3 pixels are transparent
                    uint32_t bits;
                    std::memcpy(&bits, blk + 4, 4);
                    for (int p = 0; p < 16; ++p) if (((bits >> (2 * p)) & 3u) == 3u) return false;
                }
            }
        }
        return true;
    }
    case Format::DXT2: case Format::DXT3: {
        uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
        for (uint32_t y = 0; y < by; ++y) {
            const uint8_t* row = base + size_t(y) * pitch;
            for (uint32_t x = 0; x < bx; ++x)
                for (int b = 0; b < 8; ++b)      // explicit 4-bit alpha: opaque iff every nibble is 0xF
                    if (row[size_t(x) * 16 + b] != 0xFF) return false;
        }
        return true;
    }
    case Format::DXT4: case Format::DXT5: {
        uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
        for (uint32_t y = 0; y < by; ++y) {
            const uint8_t* row = base + size_t(y) * pitch;
            for (uint32_t x = 0; x < bx; ++x) {
                const uint8_t* blk = row + size_t(x) * 16;
                if (blk[0] != 0xFF || blk[1] != 0xFF) return false;   // both ends 255: every interpolated alpha is 255
            }
        }
        return true;
    }
    default:
        return false;
    }
}

// One texel's colour (RGB, 0..255), 8-bit per channel, for the ground-texture classification thumbnail below. Handles
// the formats the game's terrain textures use; false for one it can't read (then the texture classifies as not grass).
static bool TexelRgb(Format fmt, const uint8_t* base, uint32_t pitch, uint32_t x, uint32_t y, uint8_t out[3])
{
    auto rgb565 = [](uint16_t v, uint8_t o[3]) {
        o[0] = uint8_t((v >> 11) << 3 | (v >> 13));
        o[1] = uint8_t(((v >> 5) & 0x3F) << 2 | ((v >> 9) & 3));
        o[2] = uint8_t((v & 0x1F) << 3 | ((v >> 2) & 7));
    };
    switch (fmt) {
    case Format::A8R8G8B8: case Format::X8R8G8B8: {
        const uint8_t* p = base + size_t(y) * pitch + size_t(x) * 4;
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0];
        return true;
    }
    case Format::R5G6B5: {
        uint16_t v;
        std::memcpy(&v, base + size_t(y) * pitch + size_t(x) * 2, 2);
        rgb565(v, out);
        return true;
    }
    case Format::X1R5G5B5: case Format::A1R5G5B5: {
        uint16_t v;
        std::memcpy(&v, base + size_t(y) * pitch + size_t(x) * 2, 2);
        out[0] = uint8_t(((v >> 10) & 0x1F) * 255 / 31);
        out[1] = uint8_t(((v >> 5) & 0x1F) * 255 / 31);
        out[2] = uint8_t((v & 0x1F) * 255 / 31);
        return true;
    }
    case Format::A4R4G4B4: {
        uint16_t v;
        std::memcpy(&v, base + size_t(y) * pitch + size_t(x) * 2, 2);
        out[0] = uint8_t(((v >> 8) & 0xF) * 17);
        out[1] = uint8_t(((v >> 4) & 0xF) * 17);
        out[2] = uint8_t((v & 0xF) * 17);
        return true;
    }
    case Format::L8: {
        out[0] = out[1] = out[2] = base[size_t(y) * pitch + x];
        return true;
    }
    case Format::A8L8: {
        out[0] = out[1] = out[2] = base[size_t(y) * pitch + size_t(x) * 2];
        return true;
    }
    case Format::DXT1: case Format::DXT2: case Format::DXT3: case Format::DXT4: case Format::DXT5: {
        const uint32_t blockBytes = fmt == Format::DXT1 ? 8 : 16;
        const uint8_t* blk = base + size_t(y / 4) * pitch + size_t(x / 4) * blockBytes;
        uint16_t c0, c1;
        std::memcpy(&c0, blk, 2);
        std::memcpy(&c1, blk + 2, 2);
        uint32_t bits;
        std::memcpy(&bits, blk + 4, 4);
        const uint32_t ci = (bits >> (2 * ((y % 4) * 4 + (x % 4)))) & 3u;
        uint8_t a[3], b[3];
        rgb565(c0, a);
        rgb565(c1, b);
        const bool transparent = fmt == Format::DXT1 && c0 <= c1;
        if (ci == 0) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; }
        else if (ci == 1) { out[0] = b[0]; out[1] = b[1]; out[2] = b[2]; }
        else if (ci == 2 || !transparent) {
            const int w0 = ci == 2 ? 2 : 1, w1 = ci == 2 ? 1 : 2;
            out[0] = uint8_t((w0 * a[0] + w1 * b[0]) / 3);
            out[1] = uint8_t((w0 * a[1] + w1 * b[1]) / 3);
            out[2] = uint8_t((w0 * a[2] + w1 * b[2]) / 3);
        } else {
            out[0] = out[1] = out[2] = 0;
        }
        return true;
    }
    default:
        return false;
    }
}

// Samples a texture on a small grid (RGB, nw x nh, each sample at its cell's centre) for the ground grass (grass.cpp:
// the ground's colour and whether it is grass, and the lightmaps' baked light).
static bool PixelsThumbnail(Format fmt, const void* data, uint32_t width, uint32_t height, uint32_t pitch,
                            uint8_t* out, uint32_t nw, uint32_t nh)
{
    const uint8_t* base = static_cast<const uint8_t*>(data);
    for (uint32_t j = 0; j < nh; ++j)
        for (uint32_t i = 0; i < nw; ++i) {
            uint32_t x = ((2 * i + 1) * width) / (2 * nw), y = ((2 * j + 1) * height) / (2 * nh);
            x = x < width ? x : width - 1;
            y = y < height ? y : height - 1;
            if (!TexelRgb(fmt, base, pitch, x, y, out + (size_t(j) * nw + i) * 3))
                return false;
        }
    return true;
}

// How a ground tile looks up close (Texture::m_line, m_edge, m_grain; the native ground's pebbles): its brightness on a
// 64 x 64 grid - how much its rows' and columns' means stray (straight joints and edges run whole rows) - and the step
// two texels apart on a 32 x 32 grid of its own. False for a format it can't read.
static bool PixelsLook(Format fmt, const void* data, uint32_t width, uint32_t height, uint32_t pitch, float* line,
                       float* edge, float* grain)
{
    const uint8_t* base = static_cast<const uint8_t*>(data);
    auto luma = [&](uint32_t x, uint32_t y, float* l) {
        uint8_t c[3];
        if (!TexelRgb(fmt, base, pitch, std::min(x, width - 1), std::min(y, height - 1), c)) return false;
        *l = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
        return true;
    };
    constexpr int N = 64;
    float rows[N] = {}, cols[N] = {};
    double sum = 0.0, sum2 = 0.0;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            float l;
            if (!luma(uint32_t((4 * i + 2) * width / 256), uint32_t((4 * j + 2) * height / 256), &l)) return false;
            rows[j] += l / N;
            cols[i] += l / N;
            sum += l;
            sum2 += double(l) * l;
        }
    const double mean = sum / (N * N), spread = std::sqrt(std::max(sum2 / (N * N) - mean * mean, 1.0));
    auto stray = [&](const float* m) {
        double s2 = 0.0;
        for (int k = 0; k < N; ++k) s2 += (m[k] - mean) * (m[k] - mean);
        return std::sqrt(s2 / N);
    };
    float step = 0.0f;
    for (int k = 1; k < N; ++k) step = std::max({step, std::fabs(rows[k] - rows[k - 1]), std::fabs(cols[k] - cols[k - 1])});
    *line = float(std::max(stray(rows), stray(cols)) / spread);
    *edge = float(step / spread);
    double g = 0.0;
    for (int j = 0; j < 32; ++j)
        for (int i = 0; i < 32; ++i) {
            const uint32_t x = (8 * i + 2) * width / 256, y = (8 * j + 2) * height / 256, two = std::max(width / 128, 1u);
            float a, b, c;
            if (!luma(x, y, &a) || !luma(x + two, y, &b) || !luma(x, y + two, &c)) return false;
            g += 0.5 * (std::fabs(a - b) + std::fabs(a - c));
        }
    *grain = float(g / (32 * 32));
    return true;
}

// One texel's alpha (0..255), for the leaves' alpha mask below; false for a format it can't read.
// Whether every texel is black (an alpha-only format reads black; false for a format it can't read): with no alpha,
// the PF map's tiles in AI missions (RVK_AlienMap).
static bool PixelsBlack(Format fmt, const void* data, uint32_t width, uint32_t height, uint32_t pitch)
{
    const uint8_t* base = static_cast<const uint8_t*>(data);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t rgb[3] = {0, 0, 0};
            if (fmt != Format::A8 && !TexelRgb(fmt, base, pitch, x, y, rgb))
                return false;
            if ((rgb[0] | rgb[1] | rgb[2]) != 0)
                return false;
        }
    return true;
}

static bool TexelAlpha(Format fmt, const uint8_t* base, uint32_t pitch, uint32_t x, uint32_t y, uint8_t* out)
{
    switch (fmt) {
    case Format::A8R8G8B8:
        *out = base[size_t(y) * pitch + size_t(x) * 4 + 3];
        return true;
    case Format::A1R5G5B5: {
        uint16_t v;
        std::memcpy(&v, base + size_t(y) * pitch + size_t(x) * 2, 2);
        *out = (v & 0x8000u) ? 255 : 0;
        return true;
    }
    case Format::A4R4G4B4: {
        uint16_t v;
        std::memcpy(&v, base + size_t(y) * pitch + size_t(x) * 2, 2);
        *out = uint8_t((v >> 12) * 17);
        return true;
    }
    case Format::A8:
        *out = base[size_t(y) * pitch + x];
        return true;
    case Format::A8L8:
        *out = base[size_t(y) * pitch + size_t(x) * 2 + 1];
        return true;
    case Format::DXT1: {
        const uint8_t* blk = base + size_t(y / 4) * pitch + size_t(x / 4) * 8;
        uint16_t c0 = uint16_t(blk[0] | blk[1] << 8), c1 = uint16_t(blk[2] | blk[3] << 8);
        uint32_t bits;
        std::memcpy(&bits, blk + 4, 4);
        *out = c0 <= c1 && ((bits >> (2 * ((y % 4) * 4 + (x % 4)))) & 3u) == 3u ? 0 : 255;
        return true;
    }
    case Format::DXT2: case Format::DXT3: {
        const uint8_t* blk = base + size_t(y / 4) * pitch + size_t(x / 4) * 16;
        const uint32_t p = (y % 4) * 4 + (x % 4);
        *out = uint8_t(((blk[p / 2] >> (4 * (p & 1))) & 0xF) * 17);
        return true;
    }
    case Format::DXT4: case Format::DXT5: {
        const uint8_t* blk = base + size_t(y / 4) * pitch + size_t(x / 4) * 16;
        const uint32_t a0 = blk[0], a1 = blk[1], p = (y % 4) * 4 + (x % 4);
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
        const uint32_t ci = uint32_t(bits >> (3 * p)) & 7u;
        uint32_t a;
        if (ci == 0) a = a0;
        else if (ci == 1) a = a1;
        else if (a0 > a1) a = ((8 - ci) * a0 + (ci - 1) * a1) / 7;
        else if (ci == 6) a = 0;
        else if (ci == 7) a = 255;
        else a = ((6 - ci) * a0 + (ci - 1) * a1) / 5;
        *out = uint8_t(a);
        return true;
    }
    default:
        return false;
    }
}

// The mean alpha on a grid of nw x nh cells (each from four texels spread over it), for the leaves (leaves.cpp).
static bool PixelsAlphaMask(Format fmt, const void* data, uint32_t width, uint32_t height, uint32_t pitch,
                            uint8_t* out, uint32_t nw, uint32_t nh)
{
    const uint8_t* base = static_cast<const uint8_t*>(data);
    for (uint32_t j = 0; j < nh; ++j)
        for (uint32_t i = 0; i < nw; ++i) {
            uint32_t sum = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                uint32_t x = ((4 * i + 1 + 2 * (k & 1)) * width) / (4 * nw);
                uint32_t y = ((4 * j + 1 + (k & 2)) * height) / (4 * nh);
                uint8_t a;
                if (!TexelAlpha(fmt, base, pitch, std::min(x, width - 1), std::min(y, height - 1), &a))
                    return false;
                sum += a;
            }
            out[size_t(j) * nw + i] = uint8_t(sum / 4);
        }
    return true;
}

void Device::UpdateTexture(Texture* t, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                           const void* data, uint32_t pitch)
{
    if (!t || !t->m_image || level >= t->m_levels || !width || !height)
        return;
    uint32_t rowBytes = FormatRowBytes(t->m_format, width), rows = FormatRows(t->m_format, height);
    ++t->m_uploads;
    t->m_uploadFrame = m_frameNumber;
    if (level == 0 && x == 0 && y == 0 && width >= t->m_width && height >= t->m_height) {
        t->m_level0NonZero = ~0u;
        if (uint64_t(rowBytes) * rows <= 256 * 256 * 4) {
            uint32_t n = 0;
            for (uint32_t r = 0; r < rows; ++r) {
                const uint8_t* row = static_cast<const uint8_t*>(data) + size_t(r) * pitch;
                for (uint32_t i = 0; i < rowBytes; ++i) n += row[i] != 0;
            }
            t->m_level0NonZero = n;
        }
    }

    // Whether the texture is fully opaque, for the static-draw opaque fast path (Draw): only a full level-0 upload
    // proves it; a partial update makes it unknown again.
    if (level == 0) {
        if (x == 0 && y == 0 && width >= t->m_width && height >= t->m_height &&
            pitch >= FormatRowBytes(t->m_format, t->m_width))
            t->m_opaque = PixelsOpaque(t->m_format, data, t->m_width, t->m_height, pitch);
        else if (x == 0 && width >= t->m_width && y + height <= t->m_height)   // whole rows: still opaque if they are
            t->m_opaque = t->m_opaque && PixelsOpaque(t->m_format, data, t->m_width, height, pitch);
        else
            t->m_opaque = false;
        // Black all over (RVK_AlienMap): small textures. A full level-0 upload decides; whole rows (the surface sends
        // only the rows a Lock wrote, once the GPU has the level whole) keep it if they are black too.
        const bool small = t->m_width <= 64 && t->m_height <= 64 && pitch >= FormatRowBytes(t->m_format, t->m_width);
        if (small && x == 0 && y == 0 && width >= t->m_width && height >= t->m_height)
            t->m_blackRgb = PixelsBlack(t->m_format, data, t->m_width, t->m_height, pitch);
        else if (small && x == 0 && width >= t->m_width && y + height <= t->m_height)
            t->m_blackRgb = t->m_blackRgb && PixelsBlack(t->m_format, data, t->m_width, height, pitch);
        else
            t->m_blackRgb = false;
        // The colour grid the ground-grass classification reads (grass.cpp): a full level-0 upload only.
        // Small textures (the terrain's 64 x 64 lightmaps) are kept whole, bigger ones on a 16 x 16 grid.
        // (A ground texture's old grid, to tell a new picture from the same one uploaded again.)
        std::vector<uint8_t> before;
        if (t->m_groundBase && t->m_thumbValid)
            before = t->m_thumb;
        t->m_thumbValid = false;
        if (x == 0 && y == 0 && width >= t->m_width && height >= t->m_height &&
            pitch >= FormatRowBytes(t->m_format, t->m_width)) {
            const bool whole = t->m_width <= 64 && t->m_height <= 64;
            t->m_thumbW = whole ? t->m_width : 16;
            t->m_thumbH = whole ? t->m_height : 16;
            t->m_thumb.resize(size_t(t->m_thumbW) * t->m_thumbH * 3);
            t->m_thumbValid = PixelsThumbnail(t->m_format, data, t->m_width, t->m_height, pitch, t->m_thumb.data(),
                                              t->m_thumbW, t->m_thumbH);
            if (!t->m_thumbValid)
                std::vector<uint8_t>().swap(t->m_thumb);
            // (Its mean colour, from that grid: the cliffs' rock is tinted to a ground cell's tile - terrain.cpp.)
            for (int c = 0; c < 3; ++c) {
                double sum = 0.0;
                for (size_t i = c; t->m_thumbValid && i < t->m_thumb.size(); i += 3) sum += t->m_thumb[i];
                t->m_mean[c] = t->m_thumbValid ? float(sum / (255.0 * double(t->m_thumb.size() / 3))) : -1.0f;
            }
            // How it looks up close (the native ground's pebbles): square textures of 128 texels or more.
            t->m_line = t->m_edge = t->m_grain = -1.0f;
            if (t->m_thumbValid && t->m_width == t->m_height && t->m_width >= 128 &&
                !PixelsLook(t->m_format, data, t->m_width, t->m_height, pitch, &t->m_line, &t->m_edge, &t->m_grain))
                t->m_line = t->m_edge = t->m_grain = -1.0f;
            // A ground texture with a new picture (a stand-in replaced as the area loads): the grass classified
            // from it is stale.
            if (t->m_groundBase && m_grassOn && t->m_thumbValid && before != t->m_thumb)
                m_groundTexUploaded.push_back(t);
            if (t->m_lightmap && m_grassOn)
                m_lightmapsUploaded.push_back(t);   // the grass's light baked from it is stale
            // Where it is leaf and where hole (leaves.cpp): textures with transparency, up to 64 x 64 cells.
            std::vector<uint8_t> mask;
            uint32_t mw = std::min<uint32_t>(t->m_width, 64), mh = std::min<uint32_t>(t->m_height, 64);
            if (!t->m_opaque && t->m_width >= 16 && t->m_height >= 16) {
                mask.resize(size_t(mw) * mh);
                if (!PixelsAlphaMask(t->m_format, data, t->m_width, t->m_height, pitch, mask.data(), mw, mh))
                    mask.clear();
            }
            if (mask != t->m_alphaMask) {
                t->m_alphaHardEdges = 1.0f;
                t->m_alphaSoftShare = 0.0f;
                {
                    uint32_t seen = 0, soft = 0;
                    for (uint8_t a : mask) {
                        seen += a > 12;
                        soft += a > 12 && a < 230;
                    }
                    t->m_alphaSoftShare = seen ? float(soft) / float(seen) : 0.0f;
                }
                if (!mask.empty() && mw > 1 && mh > 1) {
                    uint32_t hard = 0, pairs = 0;
                    for (uint32_t y = 0; y < mh; ++y)
                        for (uint32_t x = 0; x < mw; ++x) {
                            const int a = mask[size_t(y) * mw + x];
                            if (x + 1 < mw) { hard += std::abs(a - int(mask[size_t(y) * mw + x + 1])) > 76; ++pairs; }
                            if (y + 1 < mh) { hard += std::abs(a - int(mask[size_t(y + 1) * mw + x])) > 76; ++pairs; }
                        }
                    t->m_alphaHardEdges = pairs ? float(hard) / float(pairs) : 1.0f;
                }
                t->m_alphaMask.swap(mask);
                t->m_alphaW = t->m_alphaMask.empty() ? 0 : mw;
                t->m_alphaH = t->m_alphaMask.empty() ? 0 : mh;
                ++t->m_alphaVersion;
            }
        }
    }

    // Stage tightly packed rows in this frame's ring buffer; the upload command buffer runs before the
    // frame's draws.
    EnsureRingSpace(VkDeviceSize(rowBytes) * rows);
    VkCommandBuffer cmd = UploadCommands();
    void* cpu;
    VkDeviceSize offset = Allocate(VkDeviceSize(rowBytes) * rows, 16, &cpu);
    for (uint32_t r = 0; r < rows; ++r)
        std::memcpy(static_cast<uint8_t*>(cpu) + size_t(r) * rowBytes, static_cast<const uint8_t*>(data) + size_t(r) * pitch,
                    rowBytes);

    // Render targets change layout in the main command buffer; uploads into them aren't supported.
    if (t->m_renderTarget)
        return;
    VkImageLayout from = t->m_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageOffset = {int32_t(x), int32_t(y), 0};
    uint32_t levelW = std::max(1u, t->m_width >> level), levelH = std::max(1u, t->m_height >> level);
    region.imageExtent = {std::min(width, levelW - x), std::min(height, levelH - y), 1};
    vkCmdCopyBufferToImage(cmd, m_frames[m_frameIndex].ring, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void Device::DestroyTexture(Texture* texture)
{
    if (!texture || texture == m_main)
        return;
    for (auto& t : m_textures)
        if (t == texture) t = nullptr;
    for (auto& l : m_heldLabels)                 // (a label held back for after the post passes)
        for (auto& t : l.textures)
            if (t == texture) t = nullptr;
    if (m_target == texture)
        SetRenderTarget(nullptr);
    ForgetCasterTexture(texture);
    for (const CloudPuff& p : m_cloudPuffsLast)
        if (p.texture == texture) { m_cloudPuffsLast.clear(); break; }   // (kept for frames without the sky's puffs)
    for (Texture** map : {&texture->m_normalMap, &texture->m_ormMap, &texture->m_albedoMap, &texture->m_emissiveMap,
                          &texture->m_leafArt, &texture->m_leafArtNormal})
        if (*map) {
            DestroyTexture(*map);
            *map = nullptr;
        }
    if (HoldParticleTexture(texture))
        return;                                  // freed once the particles drawn with it have faded out
    m_deadTextures.push_back({DeathTag(), texture});   // freed once every submission that may use it is done
}

void Device::SetNormalMap(Texture* texture, Texture* normal)
{
    if (!texture || texture->m_normalMap == normal)
        return;
    if (texture->m_normalMap)
        DestroyTexture(texture->m_normalMap);
    texture->m_normalMap = normal;
    m_constantsDirty = true;
}

void Device::SetMaterialMaps(Texture* texture, Texture* orm, Texture* albedo)
{
    if (!texture)
        return;
    for (auto [map, now] : {std::pair{&texture->m_ormMap, orm}, std::pair{&texture->m_albedoMap, albedo}})
        if (*map != now) {
            if (*map) DestroyTexture(*map);
            *map = now;
        }
    m_constantsDirty = true;
}

void Device::SetEmissiveMap(Texture* texture, Texture* emissive)
{
    if (!texture || texture->m_emissiveMap == emissive)
        return;
    if (texture->m_emissiveMap)
        DestroyTexture(texture->m_emissiveMap);
    texture->m_emissiveMap = emissive;
    m_constantsDirty = true;
}

void Device::SetLeafArt(Texture* texture, Texture* atlas, Texture* normal)
{
    if (!texture)
        return;
    for (auto [map, now] : {std::pair{&texture->m_leafArt, atlas}, std::pair{&texture->m_leafArtNormal, normal}})
        if (*map != now) {
            if (*map) DestroyTexture(*map);
            *map = now;
        }
    ++texture->m_leafArtVersion;
}

void Device::SetPbr(const PbrSettings& s)
{
    m_pbr = s;
    m_constantsDirty = true;
    m_frameLightsDirty = true;
}

void Device::DestroyTextureNow(Texture* t)
{
    UnregisterBindlessTexture(t);                // clears the slot to the black image before the view goes
    if (t->m_view) vkDestroyImageView(m_device, t->m_view, nullptr);
    if (t->m_image) {
        vmaDestroyImage(m_allocator, t->m_image, t->m_allocation);
        m_textureBytes[t->m_renderTarget ? 1 : 0] -= t->m_bytes;
        --m_textureCount[t->m_renderTarget ? 1 : 0];
    }
    delete t;
}

// Bindless textures (M1): a slot in set 1's image array, written when the texture appears and cleared to black when
// it goes. Update-after-bind lets these update while frames are in flight.
void Device::RegisterBindlessTexture(Texture* t)
{
    if (!m_bindless || !t || !t->m_view || m_bindlessSet == VK_NULL_HANDLE)
        return;
    uint32_t index;
    if (!m_freeBindlessImages.empty()) {
        index = m_freeBindlessImages.back();
        m_freeBindlessImages.pop_back();
    } else if (m_nextBindlessImage < kMaxBindlessImages) {
        index = m_nextBindlessImage++;
    } else {
        static bool logged;
        if (!logged) { logged = true; Log("bindless textures: out of slots (%u)", kMaxBindlessImages); }
        return;
    }
    t->m_bindless = index;
    VkDescriptorImageInfo img{VK_NULL_HANDLE, t->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

void Device::UnregisterBindlessTexture(Texture* t)
{
    if (!m_bindless || !t || t->m_bindless == ~0u)
        return;
    if (m_bindlessSet != VK_NULL_HANDLE) {
        // A null view is allowed for a sampled-image slot (and the binding is partially bound). It must not be
        // sampled, but no draw should still hold this index.
        VkDescriptorImageInfo img{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_bindlessSet;
        w.dstBinding = 0;
        w.dstArrayElement = t->m_bindless;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &img;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    }
    m_freeBindlessImages.push_back(t->m_bindless);
    t->m_bindless = ~0u;
}

uint32_t Device::BindlessImage(Texture* t)
{
    if (!m_bindless)
        return 0;
    if (!t)
        t = m_blackTexture;
    if (t && t->m_bindless == ~0u)
        RegisterBindlessTexture(t);
    if (t && t->m_bindless != ~0u)
        return t->m_bindless;
    return m_blackTexture ? m_blackTexture->m_bindless : 0u;
}

uint32_t Device::BindlessSampler(VkSampler s)
{
    if (!m_bindless)
        return 0;
    uint32_t index = RegisterBindlessSampler(s);
    return index == ~0u ? 0u : index;
}

uint32_t Device::RegisterBindlessSampler(VkSampler s)
{
    if (!m_bindless || !s || m_bindlessSet == VK_NULL_HANDLE)
        return ~0u;
    auto it = m_bindlessSamplerIndex.find(uint64_t(s));
    if (it != m_bindlessSamplerIndex.end())
        return it->second;
    uint32_t index;
    if (!m_freeBindlessSamplers.empty()) {
        index = m_freeBindlessSamplers.back();
        m_freeBindlessSamplers.pop_back();
    } else if (m_nextBindlessSampler < kMaxBindlessSamplers) {
        index = m_nextBindlessSampler++;
    } else {
        return ~0u;
    }
    m_bindlessSamplerIndex.emplace(uint64_t(s), index);
    VkDescriptorImageInfo img{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = 1;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    return index;
}

uint64_t Device::SamplerKey(uint32_t stage) const
{
    const auto& t = m_tss[stage];
    return uint64_t(t[d3d::TSS_MAGFILTER] & 7) | uint64_t(t[d3d::TSS_MINFILTER] & 7) << 3 |
           uint64_t(t[d3d::TSS_MIPFILTER] & 7) << 6 | uint64_t(t[d3d::TSS_ADDRESSU] & 7) << 9 |
           uint64_t(t[d3d::TSS_ADDRESSV] & 7) << 12 | uint64_t(t[d3d::TSS_MAXMIPLEVEL] & 15) << 15 |
           uint64_t(t[d3d::TSS_BORDERCOLOR] >> 24 ? 1 : 0) << 19 |
           uint64_t(m_anisotropy) << 20 |      // samplers made at another level stay valid for work in flight
           uint64_t(SceneMipBias() ? 100 - m_renderScale : 0) << 26;
}

// RVK_RenderScale: the scene's textures sample a finer mip level by log2(scale), as sharp as at the output's size
// (the TAA resolves the extra detail); not the interface's, drawn at the output's size.
bool Device::SceneMipBias() const { return m_target == m_scene && SceneScaled(); }

// Samplers are never destroyed while the device lives (SamplerFor's map owns them), so a stage whose sampler state
// is unchanged keeps its slot. Only a successful registration is remembered.
uint32_t Device::StageSamplerSlot(uint32_t stage)
{
    if (!m_bindless)
        return 0;
    uint64_t key = SamplerKey(stage);
    if (key == m_stageSamplerKey[stage])
        return m_stageSamplerSlot[stage];
    uint32_t index = RegisterBindlessSampler(SamplerFor(stage));
    if (index == ~0u)
        return 0;
    m_stageSamplerKey[stage] = key;
    m_stageSamplerSlot[stage] = index;
    return index;
}

uint32_t Device::FixedSamplerSlot(uint32_t which, VkSampler s)
{
    if (!m_bindless)
        return 0;
    if (s && s == m_fixedSampler[which])
        return m_fixedSamplerSlot[which];
    uint32_t index = RegisterBindlessSampler(s);
    if (index == ~0u)
        return 0;
    m_fixedSampler[which] = s;
    m_fixedSamplerSlot[which] = index;
    return index;
}

VkSampler Device::SamplerFor(uint32_t stage)
{
    const auto& t = m_tss[stage];
    uint32_t addrU = t[d3d::TSS_ADDRESSU], addrV = t[d3d::TSS_ADDRESSV];
    uint64_t key = SamplerKey(stage);
    auto it = m_samplers.find(key);
    if (it != m_samplers.end())
        return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = t[d3d::TSS_MAGFILTER] == d3d::TFG_POINT ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = t[d3d::TSS_MINFILTER] == d3d::TFN_POINT ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = t[d3d::TSS_MIPFILTER] == d3d::TFP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.minLod = float(t[d3d::TSS_MAXMIPLEVEL]);           // D3D MAXMIPLEVEL = most detailed level used
    ci.maxLod = t[d3d::TSS_MIPFILTER] == d3d::TFP_NONE ? ci.minLod : VK_LOD_CLAMP_NONE;
    ci.addressModeU = AddressMode(addrU);
    ci.addressModeV = AddressMode(addrV);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.borderColor = (t[d3d::TSS_BORDERCOLOR] >> 24) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                     : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // Enhancement: anisotropic filtering for textures the game filters linearly (point-filtered ones stay crisp), as
    // DXVK's forced anisotropy does.
    if (SceneMipBias() && t[d3d::TSS_MIPFILTER] != d3d::TFP_NONE)
        ci.mipLodBias = std::log2(float(m_renderScale) / 100.0f);
    if (m_anisotropy > 1 && ci.minFilter == VK_FILTER_LINEAR && ci.magFilter == VK_FILTER_LINEAR) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = float(m_anisotropy);
    }
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(m_device, &ci, nullptr, &sampler);
    m_samplers.emplace(key, sampler);
    RegisterBindlessSampler(sampler);
    return sampler;
}

// ---------------------------------------------------------------------------------------------------
// Vertex buffers

VertexBuffer* Device::CreateVertexBuffer(uint32_t fvf, uint32_t vertexCount)
{
    auto* vb = new VertexBuffer;
    vb->m_fvf = fvf;
    vb->m_stride = FvfStride(fvf);
    vb->m_count = vertexCount;
    vb->m_data.resize(size_t(vb->m_stride) * vertexCount);
    return vb;
}

void* Device::Lock(VertexBuffer* vb)
{
    vb->m_locked = true;
    return vb->m_data.data();
}

void Device::Unlock(VertexBuffer* vb) { vb->m_locked = false; }

void Device::DestroyVertexBuffer(VertexBuffer* vb) { delete vb; }   // draws already copied what they used

}  // namespace rvk
