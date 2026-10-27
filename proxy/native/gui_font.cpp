#include "native/gui_font.h"
#include "native/gui_scale.h"
#include "native/gui.h"
#include "native/device.h"
#include "native/pixfmt.h"
#include "native/vc10.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rnative::gui {
float ScaleForView(void* view);
namespace {

template <class T> T& Field(void* p, size_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset); }
struct IPoint { int x, y; };
struct IRect { int left, top, right, bottom; };
struct Bitmap { float factor; int width, height; };
std::unordered_map<void*, Bitmap> g_bitmaps;
pixfmt::Format g_argb, g_argb1555;
bool g_enabled;

using RenderFn = void(__fastcall*)(void*, void*, void*, float, uint32_t);
using VoidFn = void(__fastcall*)(void*, void*);
using StringFn = void(__fastcall*)(void*, void*, IPoint*, int, int, int, void*, uint32_t, uint32_t, uint32_t);
using RectFn = Rect*(__fastcall*)(void*, void*, Rect*);
using CtorFn = void*(__fastcall*)(void*, void*, int, int, int);
using ClearFn = void(__fastcall*)(void*, void*, const IRect*, uint32_t, uint32_t);
using GlyphFn = void(__fastcall*)(void*, void*, const IPoint*, const void*, uint32_t);
using CopyFn = void(__fastcall*)(void*, void*, const IRect*, const IPoint*, void*, uint32_t);
using IntFn = int(__fastcall*)(void*, void*);
RenderFn g_preRender;
VoidFn g_allocate, g_renderText, g_layout, g_destroy;
StringFn g_string;
RectFn g_source;
CtorFn g_ctor;
ClearFn g_clear;
GlyphFn g_glyph;
CopyFn g_copy;
IntFn g_width, g_height;

struct Font;
struct Context { void* view = nullptr; float factor = 1; void* font = nullptr; bool allocating = false, source = false, created = false; Font* raster = nullptr; };
thread_local Context g_context;
struct Scope {
    Context saved = g_context;
    explicit Scope(Context next) { g_context = next; }
    ~Scope() { g_context = saved; }
};

Bitmap Info(void* bitmap)
{
    auto it = g_bitmaps.find(bitmap);
    return it == g_bitmaps.end() ? Bitmap{1, 0, 0} : it->second;
}
bool Target(void* bitmap) { return g_enabled && g_context.factor > 1 && g_context.view && bitmap == Field<void*>(g_context.view, 0x248); }
int Pixel(float coordinate) { return int(std::lround(coordinate)); }
float Density(void* view)
{
    const float scale = ScaleForView(view);
    if (scale <= 1) return 1;
    const float native = std::max(Field<float>(view, 0x28C), Field<float>(view, 0x290));
    return scale * (std::isfinite(native) ? std::max(1.0f, native) : 1);
}

struct Glyph { int width = 0, height = 0, x = 0, y = 0; std::vector<uint8_t> coverage; };
struct Font {
    HFONT handle = nullptr;
    HDC dc = nullptr;
    HGDIOBJ previous = nullptr;
    TEXTMETRICA metrics{};
    int cellHeight = 0;
    uint64_t used = 0;
    std::unordered_map<uint64_t, Glyph> glyphs;
    ~Font() { if (dc) { if (previous) SelectObject(dc, previous); DeleteDC(dc); } if (handle) DeleteObject(handle); }
};
std::unordered_map<std::string, std::unique_ptr<Font>> g_fonts;
uint64_t g_fontUse;
std::unordered_map<void*, float> g_attempts;

Font* RasterFont(void* info, float factor)
{
    LOGFONTA description{};
    const HFONT original = Field<uint8_t>(info, 0xFFC) ? Field<HFONT>(info, 0x1018) : nullptr;
    if (!original || GetObjectA(original, sizeof(description), &description) != sizeof(description)) {
        description.lfHeight = std::max(1, Field<int>(info, 0xFF0));
        description.lfWeight = FW_NORMAL;
        std::strcpy(description.lfFaceName, "Verdana");
    }
    if (!description.lfHeight) description.lfHeight = std::max(1, Field<int>(info, 0xFF0));
    description.lfHeight = Pixel(description.lfHeight * factor);
    description.lfWidth = Pixel(description.lfWidth * factor);
    description.lfQuality = ANTIALIASED_QUALITY;
    std::string key(reinterpret_cast<const char*>(&description), sizeof(description));
    key.append(reinterpret_cast<const char*>(&factor), sizeof(factor));
    const int cellHeight = std::max(1, int(std::ceil(Field<int>(info, 0xFF0) * factor)));
    key.append(reinterpret_cast<const char*>(&cellHeight), sizeof(cellHeight));
    if (auto it = g_fonts.find(key); it != g_fonts.end()) { it->second->used = ++g_fontUse; return it->second.get(); }
    if (g_fonts.size() >= 64) {
        auto oldest = std::min_element(g_fonts.begin(), g_fonts.end(), [](const auto& a, const auto& b) { return a.second->used < b.second->used; });
        g_fonts.erase(oldest);
    }
    auto font = std::make_unique<Font>();
    font->handle = CreateFontIndirectA(&description);
    if (!font->handle) return nullptr;
    font->dc = CreateCompatibleDC(nullptr);
    if (!font->dc) return nullptr;
    font->previous = SelectObject(font->dc, font->handle);
    if (!GetTextMetricsA(font->dc, &font->metrics)) return nullptr;
    font->cellHeight = cellHeight; font->used = ++g_fontUse;
    Font* result = font.get();
    g_fonts.emplace(key, std::move(font));
    return result;
}

const Glyph* RasterGlyph(Font& font, uint32_t character)
{
    const uint64_t key = character;
    if (auto it = font.glyphs.find(key); it != font.glyphs.end()) return &it->second;
    if (font.glyphs.size() >= 1024) font.glyphs.clear();
    if (character > 0xFFFF) return nullptr;
    const WCHAR code = WCHAR(character);
    WORD glyphIndex = 0xFFFF;
    if (GetGlyphIndicesW(font.dc, &code, 1, &glyphIndex, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || glyphIndex == 0xFFFF)
        return nullptr;
    MAT2 transform{};
    transform.eM11.value = transform.eM22.value = 1;
    GLYPHMETRICS metrics{};
    const DWORD bytes = GetGlyphOutlineW(font.dc, character, GGO_GRAY8_BITMAP, &metrics, 0, nullptr, &transform);
    if (bytes == GDI_ERROR || bytes > 1024 * 1024) return nullptr;
    std::vector<uint8_t> pixels(bytes);
    if (bytes && GetGlyphOutlineW(font.dc, character, GGO_GRAY8_BITMAP, &metrics, bytes, pixels.data(), &transform) == GDI_ERROR)
        return nullptr;
    Glyph glyph;
    glyph.width = int(metrics.gmBlackBoxX); glyph.height = int(metrics.gmBlackBoxY);
    glyph.x = metrics.gmptGlyphOrigin.x;
    glyph.y = std::clamp(int(font.metrics.tmAscent - metrics.gmptGlyphOrigin.y), 0, std::max(0, font.cellHeight - glyph.height));
    glyph.coverage.resize(size_t(glyph.width) * glyph.height);
    const size_t stride = (size_t(glyph.width) + 3) & ~size_t(3);
    if (stride * glyph.height > pixels.size()) return nullptr;
    for (int y = 0; y < glyph.height; ++y) for (int x = 0; x < glyph.width; ++x)
        glyph.coverage[size_t(y) * glyph.width + x] = uint8_t(std::min(255u, unsigned(pixels[size_t(y) * stride + x]) * 255 / 64));
    return &font.glyphs.emplace(key, std::move(glyph)).first->second;
}

void Plot(void* bitmap, int x, int y, uint32_t colour, uint32_t alpha)
{
    if (!alpha || x < 0 || y < 0 || x >= Field<int>(bitmap, 0xC) || y >= Field<int>(bitmap, 8)) return;
    auto* row = reinterpret_cast<uint32_t*>(Field<uint8_t*>(bitmap, 0) + size_t(y) * Field<int>(bitmap, 0x20));
    const uint32_t old = row[x], oldAlpha = old >> 24;
    if (alpha == 255 || oldAlpha == 0) { row[x] = (alpha << 24) | (colour & 0xFFFFFF); return; }
    const uint32_t remaining = (oldAlpha * (255 - alpha) + 127) / 255;
    const uint32_t outAlpha = alpha + remaining;
    uint32_t out = outAlpha << 24;
    for (int shift : {0, 8, 16})
        out |= (((((colour >> shift) & 255) * alpha + ((old >> shift) & 255) * remaining) + outAlpha / 2) / outAlpha) << shift;
    row[x] = out;
}

uint32_t ReadPixel(void* bitmap, int x, int y)
{
    const auto* row = Field<uint8_t*>(bitmap, 0) + size_t(y) * Field<int>(bitmap, 0x20);
    if (Field<int>(bitmap, 0x24) == 4) return reinterpret_cast<const uint32_t*>(row)[x];
    const uint32_t pixel = reinterpret_cast<const uint16_t*>(row)[x];
    return (pixel & 0x8000 ? 0xFF000000 : 0) | (((pixel >> 10) & 31) * 255 / 31 << 16) |
           (((pixel >> 5) & 31) * 255 / 31 << 8) | ((pixel & 31) * 255 / 31);
}

void __fastcall GlyphHook(void* bitmap, void* edx, const IPoint* position, const void* glyph, uint32_t colour)
{
    if (!Target(bitmap)) { g_glyph(bitmap, edx, position, glyph, colour); return; }
    const float factor = Info(bitmap).factor;
    const int width = Field<const int16_t>(const_cast<void*>(glyph), 0xE);
    const uint32_t character = Field<const uint32_t>(const_cast<void*>(glyph), 8);
    const bool bitmapLetter = g_context.font && !Field<uint8_t>(g_context.font, 0xFFC) && character >= 32 && character <= 255;
    Font* font = width >= 0 || bitmapLetter ? g_context.raster : nullptr;
    const Glyph* raster = font ? RasterGlyph(*font, character) : nullptr;
    if (raster) {
        const int left = Pixel(position->x * factor) + raster->x, top = Pixel(position->y * factor) + raster->y;
        for (int y = 0; y < raster->height; ++y) for (int x = 0; x < raster->width; ++x)
            Plot(bitmap, left + x, top + y, colour, raster->coverage[size_t(y) * raster->width + x]);
        return;
    }
    // Bitmap-only symbols retain their native drawing, expanded into the larger cache.
    const int height = std::max(1, int(Field<const int16_t>(const_cast<void*>(glyph), 0x10)));
    void* icon = width < 0 ? Field<void*>(const_cast<void*>(glyph), 0x14) : nullptr;
    const int cell = width >= 0 ? std::max(1, width) : std::max(1, Field<int>(icon, 0xC));
    const int padding = icon ? std::max(0, (Field<int>(icon, 8) - height + 1) / 2) : 0;
    const int canvasHeight = height + padding * 2;
    alignas(16) uint8_t temporary[0x44]{};
    CreateTextBitmap(temporary, cell, canvasHeight, 1);
    const IPoint origin{0, padding};
    g_glyph(temporary, nullptr, &origin, glyph, colour);
    for (int y = 0; y < Pixel(canvasHeight * factor); ++y) for (int x = 0; x < Pixel(cell * factor); ++x) {
        const uint32_t pixel = ReadPixel(temporary, std::min(cell - 1, int(x / factor)), std::min(canvasHeight - 1, int(y / factor)));
        Plot(bitmap, Pixel(position->x * factor) + x, Pixel((position->y - padding) * factor) + y, pixel, pixel >> 24);
    }
    vc10::FreeArray(Field<void*>(temporary, 0));
    ForgetTextBitmap(temporary);
}

void __fastcall ClearHook(void* bitmap, void* edx, const IRect* rectangle, uint32_t colour, uint32_t transparent)
{
    if (!Target(bitmap)) { g_clear(bitmap, edx, rectangle, colour, transparent); return; }
    const float factor = Info(bitmap).factor;
    IRect area{0, 0, Field<int>(bitmap, 0xC), Field<int>(bitmap, 8)};
    if (rectangle) area = {Pixel(rectangle->left * factor), Pixel(rectangle->top * factor),
                           Pixel(rectangle->right * factor), Pixel(rectangle->bottom * factor)};
    area.left = std::clamp(area.left, 0, Field<int>(bitmap, 0xC)); area.right = std::clamp(area.right, area.left, Field<int>(bitmap, 0xC));
    area.top = std::clamp(area.top, 0, Field<int>(bitmap, 8)); area.bottom = std::clamp(area.bottom, area.top, Field<int>(bitmap, 8));
    const uint32_t fill = (colour & 0xFFFFFF) | (transparent ? 0u : 0xFF000000u);
    for (int y = area.top; y < area.bottom; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(Field<uint8_t*>(bitmap, 0) + size_t(y) * Field<int>(bitmap, 0x20));
        std::fill(row + area.left, row + area.right, fill);
    }
}

void __fastcall CopyHook(void* bitmap, void* edx, const IRect* rectangle, const IPoint* at, void* source, uint32_t key)
{
    if (!Target(bitmap)) { g_copy(bitmap, edx, rectangle, at, source, key); return; }
    const int width = rectangle->right - rectangle->left, height = rectangle->bottom - rectangle->top;
    if (width <= 0 || height <= 0) return;
    alignas(16) uint8_t temporary[0x44]{};
    CreateTextBitmap(temporary, width, height, 1);
    const IPoint zero{};
    g_copy(temporary, nullptr, rectangle, &zero, source, key);
    const float factor = Info(bitmap).factor;
    for (int y = 0; y < Pixel(height * factor); ++y) for (int x = 0; x < Pixel(width * factor); ++x) {
        const uint32_t pixel = ReadPixel(temporary, std::min(width - 1, int(x / factor)), std::min(height - 1, int(y / factor)));
        Plot(bitmap, Pixel(at->x * factor) + x, Pixel(at->y * factor) + y, pixel, pixel >> 24);
    }
    vc10::FreeArray(Field<void*>(temporary, 0));
    ForgetTextBitmap(temporary);
}

void* __fastcall CtorHook(void* bitmap, void* edx, int width, int height, int format)
{
    if (g_enabled && g_context.allocating) {
        g_context.created = true;
        if (g_context.factor > 1) return CreateTextBitmap(bitmap, width, height, g_context.factor);
    }
    return g_ctor(bitmap, edx, width, height, format);
}
void __fastcall DestroyHook(void* bitmap, void* edx) { ForgetTextBitmap(bitmap); g_destroy(bitmap, edx); }
int Dimension(void* bitmap, IntFn original)
{
    if (g_enabled && g_context.allocating && !g_context.created && bitmap == Field<void*>(g_context.view, 0x248)) {
        const Bitmap info = Info(bitmap);
        if (info.factor != g_context.factor) return 0;
        if (info.factor > 1) return original == g_width ? info.width : info.height;
    }
    return original(bitmap, nullptr);
}
int __fastcall WidthHook(void* bitmap, void*) { return Dimension(bitmap, g_width); }
int __fastcall HeightHook(void* bitmap, void*) { return Dimension(bitmap, g_height); }

void __fastcall AllocateHook(void* view, void* edx)
{
    Scope scope({view, g_enabled ? Density(view) : 1, nullptr, true});
    g_allocate(view, edx);
}
void __fastcall RenderTextHook(void* view, void* edx)
{
    const int64_t start = device::GameClock();
    Scope scope({view, g_enabled ? Info(Field<void*>(view, 0x248)).factor : 1});
    g_renderText(view, edx);
    device::GameSectionSince("game: UI font raster", start);
}
void __fastcall StringHook(void* view, void* edx, IPoint* position, int height, int start, int end,
                           void* font, uint32_t colour, uint32_t selected, uint32_t underline)
{
    Context context{view, g_enabled ? Info(Field<void*>(view, 0x248)).factor : 1, font};
    if (context.factor > 1) context.raster = RasterFont(font, context.factor);
    Scope scope(context);
    g_string(view, edx, position, height, start, end, font, colour, selected, underline);
}
void __fastcall LayoutHook(void* view, void* edx)
{
    Scope scope({view, g_enabled ? Info(Field<void*>(view, 0x248)).factor : 1, nullptr, false, true});
    g_layout(view, edx);
}
Rect* __fastcall SourceHook(void* surface, void* edx, Rect* out)
{
    g_source(surface, edx, out);
    if (g_enabled && g_context.source && g_context.factor > 1 &&
        (surface == Field<void*>(g_context.view, 0x240) || surface == Field<void*>(g_context.view, 0x244))) {
        void* bitmap = Field<void*>(g_context.view, 0x248);
        const Bitmap logical = Info(bitmap);
        if (out->left == 0 && out->top == 0 && out->right + 1 == Field<int>(bitmap, 0xC) && out->bottom + 1 == Field<int>(bitmap, 8))
            *out = {0, 0, float(logical.width - 1), float(logical.height - 1)};
        else Scale{g_context.factor}.FromScreen(*out);
    }
    return out;
}
void __fastcall PreRenderHook(void* view, void* edx, void* viewport, float time, uint32_t colour)
{
    if (!ScalingActive() && g_bitmaps.empty()) { g_preRender(view, edx, viewport, time, colour); return; }
    const float wanted = Density(view);
    void* bitmap = Field<void*>(view, 0x248);
    const auto attempt = bitmap ? g_attempts.end() : g_attempts.find(view);
    const float density = bitmap ? Info(bitmap).factor : attempt != g_attempts.end() ? attempt->second : 1;
    if (g_enabled && density != wanted) {
        Field<uint8_t>(view, 0x288) = 1;
        Field<uint8_t>(view, 0x28A) = 1;
    }
    g_preRender(view, edx, viewport, time, colour);
    if (!Field<void*>(view, 0x248)) {
        if (g_attempts.size() >= 8192) g_attempts.clear();
        g_attempts[view] = wanted;
    } else g_attempts.erase(view);
}

struct Entry { const char* name; uint32_t rva; const char* bytes; size_t count; void* target; void** original; };
Entry g_entries[] = {
    {"?PreRender@TextRenderer_c@@UAEXPAVRViewPort_t@@MI@Z", 0x163320, "\x55\x8B\xEC\x53\x56", 5, reinterpret_cast<void*>(&PreRenderHook), reinterpret_cast<void**>(&g_preRender)},
    {"?_AllocateBitmap@TextRenderer_c@@AAEXXZ", 0x16095B, "", 10, reinterpret_cast<void*>(&AllocateHook), reinterpret_cast<void**>(&g_allocate)},
    {"?_RenderText@TextRenderer_c@@AAEXXZ", 0x16142C, "", 10, reinterpret_cast<void*>(&RenderTextHook), reinterpret_cast<void**>(&g_renderText)},
    {"?_RenderString@TextRenderer_c@@AAEXPAVIPoint@@HHHPAVFontInfo_t@@I_N2@Z", 0x15FFDD, "", 10, reinterpret_cast<void*>(&StringHook), reinterpret_cast<void**>(&g_string)},
    {"?_LayoutRenderSurface@TextRenderer_c@@AAEXXZ", 0x16047C, "", 10, reinterpret_cast<void*>(&LayoutHook), reinterpret_cast<void**>(&g_layout)},
    {"?GetSrcFrame@ViewSurface_c@@QBE?AVRect@@XZ", 0x151330, "\x55\x8B\xEC\x51\x83\x65\xFC\x00", 8, reinterpret_cast<void*>(&SourceHook), reinterpret_cast<void**>(&g_source)},
};
struct Import { const char* name; uint32_t slot; void* target; void** original; };
Import g_imports[] = {
    {"??0SpriteInfo_t@@QAE@HHW4Format_e@@@Z", 0x1A73F0, reinterpret_cast<void*>(&CtorHook), reinterpret_cast<void**>(&g_ctor)},
    {"??1SpriteInfo_t@@QAE@XZ", 0x1A740C, reinterpret_cast<void*>(&DestroyHook), reinterpret_cast<void**>(&g_destroy)},
    {"?Clear@SpriteInfo_t@@QAEXPBVIRect@@H_N@Z", 0x1A73F4, reinterpret_cast<void*>(&ClearHook), reinterpret_cast<void**>(&g_clear)},
    {"?RenderGlyph@SpriteInfo_t@@QAEXABVIPoint@@PBUGlyph_s@@H@Z", 0x1A7394, reinterpret_cast<void*>(&GlyphHook), reinterpret_cast<void**>(&g_glyph)},
    {"?Copy@SpriteInfo_t@@QAEXABVIRect@@ABVIPoint@@PBV1@H@Z", 0x1A7390, reinterpret_cast<void*>(&CopyHook), reinterpret_cast<void**>(&g_copy)},
    {"?GetWidth@SpriteInfo_t@@QBEHXZ", 0x1A73FC, reinterpret_cast<void*>(&WidthHook), reinterpret_cast<void**>(&g_width)},
    {"?GetHeight@SpriteInfo_t@@QBEHXZ", 0x1A7400, reinterpret_cast<void*>(&HeightHook), reinterpret_cast<void**>(&g_height)},
};

}  // namespace

void* CreateTextBitmap(void* storage, int width, int height, float factor)
{
    pixfmt::Init(&g_argb, nullptr, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
    pixfmt::Init(&g_argb1555, nullptr, 16, 0x7C00, 0x3E0, 0x1F, 0x8000);
    const int bytesPerPixel = factor > 1 ? 4 : 2;
    const int physicalWidth = std::max(1, int(std::ceil(width * factor))), physicalHeight = std::max(1, int(std::ceil(height * factor)));
    auto* pixels = static_cast<uint8_t*>(vc10::AllocateArray(size_t(physicalWidth) * physicalHeight * bytesPerPixel));
    std::memset(pixels, 0, size_t(physicalWidth) * physicalHeight * bytesPerPixel);
    std::memset(storage, 0, 0x44);
    Field<void*>(storage, 0) = pixels;
    Field<int>(storage, 8) = physicalHeight; Field<int>(storage, 0xC) = physicalWidth;
    Field<int>(storage, 0x20) = physicalWidth * bytesPerPixel; Field<int>(storage, 0x24) = bytesPerPixel;
    Field<int>(storage, 0x38) = factor > 1 ? 2 : 8; Field<int>(storage, 0x3C) = factor > 1 ? 5 : 2;
    Field<void*>(storage, 0x40) = factor > 1 ? &g_argb : &g_argb1555;
    g_bitmaps[storage] = {factor, width, height};
    return storage;
}
void ForgetTextBitmap(void* bitmap) { g_bitmaps.erase(bitmap); }
bool FontScalingReady() { return g_enabled; }

void InstallFontScaling(HMODULE gui)
{
    if (g_enabled) return;
    auto* base = reinterpret_cast<uint8_t*>(gui);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (pe->FileHeader.TimeDateStamp != 0x5CD32BEA || pe->OptionalHeader.SizeOfImage != 0x2B0000) return;
    HMODULE display = GetModuleHandleA("DisplaySystem.dll");
    if (!display) return;
    for (const Entry& entry : g_entries) {
        auto* at = base + entry.rva;
        if (reinterpret_cast<void*>(GetProcAddress(gui, entry.name)) != at) return;
        if (*entry.bytes) { if (std::memcmp(at, entry.bytes, entry.count) != 0) return; }
        else {
            int32_t relative; std::memcpy(&relative, at + 6, 4);
            if (at[0] != 0xB8 || at[5] != 0xE8 || at + 10 + relative != base + 0x1738A4) return;
        }
    }
    for (const Import& entry : g_imports) {
        void* original = reinterpret_cast<void*>(GetProcAddress(display, entry.name));
        if (!original || Field<void*>(base, entry.slot) != original) return;
        *entry.original = original;
    }
    for (const Entry& entry : g_entries) {
        const size_t relative = 6;
        *entry.original = HookAt(base + entry.rva, entry.count, *entry.bytes ? nullptr : &relative,
                                 *entry.bytes ? 0 : 1, entry.target, entry.name);
        if (!*entry.original) return;
    }
    for (const Import& entry : g_imports) {
        DWORD protect;
        if (!VirtualProtect(base + entry.slot, sizeof(void*), PAGE_READWRITE, &protect)) return;
        Field<void*>(base, entry.slot) = entry.target;
        VirtualProtect(base + entry.slot, sizeof(void*), protect, &protect);
    }
    g_enabled = true;
    Log("interface fonts: display-resolution text cache hooked");
}

}  // namespace rnative::gui
