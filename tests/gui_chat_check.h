// Chat-sized text views through native wrapping and line rendering.
#pragma once
#include <memory>

inline void* g_guiChatFont;
inline void* __cdecl GuiChatFontSystem() { return g_guiChatFont; }
inline void* __fastcall GuiChatGetFont(void*, void*, int) { return g_guiChatFont; }

inline void* __fastcall GuiChatMaterial(void* material, void*, int width, int height, void*, int format, char*)
{
    std::memset(material, 0, 0x14);
    std::memcpy(static_cast<uint8_t*>(material) + 8, &width, 4);
    std::memcpy(static_cast<uint8_t*>(material) + 12, &height, 4);
    std::memcpy(static_cast<uint8_t*>(material) + 16, &format, 4);
    return material;
}
inline void __fastcall GuiChatMaterialDestroy(void*, void*) {}
inline int CheckGuiTextAtlas(HMODULE module)
{
    auto* bytes = reinterpret_cast<uint8_t*>(module);
    GuiCheckImport material(reinterpret_cast<void**>(bytes + 0x1A7364), reinterpret_cast<void*>(&GuiChatMaterial));
    GuiCheckImport destroy(reinterpret_cast<void**>(bytes + 0x1A7380), reinterpret_cast<void*>(&GuiChatMaterialDestroy));
    using Alloc = void*(__cdecl*)(int, int, uint32_t, int);
    using Release = void(__thiscall*)(void*);
    const auto allocate = reinterpret_cast<Alloc>(bytes + 0x153E94);
    const auto release = reinterpret_cast<Release>(bytes + 0x153E50);
    void* legacy = allocate(16, 16, 0, 8);
    void* shared = allocate(16, 16, 0, 2);
    void* independent = allocate(16, 16, 1, 2);
    auto pool = [](void* node) { void* p; std::memcpy(&p, static_cast<uint8_t*>(node) + 0xC, 4); return p; };
    auto format = [&](void* node) {
        void* texture; std::memcpy(&texture, pool(node), 4);
        int value; std::memcpy(&value, static_cast<uint8_t*>(texture) + 16, 4); return value;
    };
    const bool reproduced = pool(legacy) == pool(shared) && format(shared) == 8 && format(independent) == 2;
    release(independent); release(shared); release(legacy);
    std::printf("GUI text atlas: %s\n", reproduced ? "legacy format reuse reproduced; dedicated RGBA retained" : "FAILED");
    return reproduced ? 0 : 1;
}

struct GuiChatTexture {
    uint8_t node[0x28]{};
    void* material = this;
    int width, height;
    bool dedicated; int format;
    std::vector<uint32_t> pixels;
    GuiChatTexture(int w, int h, bool own, int f) : width(w), height(h), dedicated(own), format(f), pixels(size_t(w) * h) {
        void* slot = &material; std::memcpy(node + 0xC, &slot, 4);
    }
};
inline std::vector<std::unique_ptr<GuiChatTexture>> g_guiChatTextures;
inline void* __cdecl GuiChatTextureAlloc(int w, int h, uint32_t own, int format) {
    auto texture = std::make_unique<GuiChatTexture>(w, h, own != 0, format);
    void* node = texture->node; g_guiChatTextures.push_back(std::move(texture)); return node;
}
inline void __fastcall GuiChatTextureRef(void*, void*) {}
inline int __cdecl GuiChatTextureLimit() { return 8; }
inline void __fastcall GuiChatTextureUpload(void* target, void*, const void* source, int pitch, int format, const RECT* rect) {
    auto* texture = static_cast<GuiChatTexture*>(target);
    for (LONG y = rect->top; y < rect->bottom; ++y) for (LONG x = rect->left; x < rect->right; ++x) {
        if (x < 0 || y < 0 || x >= texture->width || y >= texture->height) std::abort();
        const auto* row = static_cast<const uint8_t*>(source) + (y - rect->top) * pitch;
        texture->pixels[size_t(y) * texture->width + x] = format == 2 ? reinterpret_cast<const uint32_t*>(row)[x - rect->left] :
            reinterpret_cast<const uint16_t*>(row)[x - rect->left];
    }
}

inline int CheckGuiChat(HMODULE module, void* controller, void* parent)
{
    using namespace rnative::gui;
    auto* bytes = reinterpret_cast<uint8_t*>(module);
    using FontCtor = void*(__thiscall*)(void*);
    using Void = void(__thiscall*)(void*);
    using TextCtor = void*(__thiscall*)(void*, const Rect*, const char*, const rnative::vc10::String*, int, uint32_t, uint32_t);
    using Size = Point*(__thiscall*)(void*, Point*);
    const auto fontCtor = reinterpret_cast<FontCtor>(bytes + 0x12EABA);
    const auto fontDtor = reinterpret_cast<Void>(bytes + 0x12EB82);
    const auto textCtor = reinterpret_cast<TextCtor>(bytes + 0x163412);
    const auto textDtor = reinterpret_cast<Void>(bytes + 0x161A18);
    const auto contentSize = reinterpret_cast<Size>(bytes + 0x16269A);
    const auto render = reinterpret_cast<Void>(bytes + 0x16142C);
    GuiCheckPatch instance(bytes + 0x26B9, reinterpret_cast<void*>(&GuiChatFontSystem));
    GuiCheckPatch lookup(bytes + 0x12E13F, reinterpret_cast<void*>(&GuiChatGetFont));
    int failures = CheckGuiTextAtlas(module);
    GuiCheckPatch allocation(bytes + 0x153E94, reinterpret_cast<void*>(&GuiChatTextureAlloc));
    GuiCheckPatch release(bytes + 0x153E50, reinterpret_cast<void*>(&GuiChatTextureRef));
    GuiCheckPatch reference(bytes + 0x153BA8, reinterpret_cast<void*>(&GuiChatTextureRef));
    GuiCheckPatch textureLimit(bytes + 0x150726, reinterpret_cast<void*>(&GuiChatTextureLimit));
    GuiCheckImport upload(reinterpret_cast<void**>(bytes + 0x1A737C), reinterpret_cast<void*>(&GuiChatTextureUpload));
    using SurfaceCtor = void*(__thiscall*)(void*, void*, uint32_t);
    const auto surfaceCtor = reinterpret_cast<SurfaceCtor>(bytes + 0x1521E5);
    const auto surfaceDtor = reinterpret_cast<Void>(bytes + 0x151C0B);
    const auto layout = reinterpret_cast<Void>(bytes + 0x16047C);
    GuiCheckPatch viewRender(bytes + 0x14AE48, reinterpret_cast<void*>(&GuiCheckRender));
    using PreRender = void(__thiscall*)(void*, void*, float, uint32_t);
    using Frame = void(__thiscall*)(void*, const Rect*, uint32_t);
    const auto preRender = reinterpret_cast<PreRender>(bytes + 0x163320);
    const auto setFrame = reinterpret_cast<Frame>(GetProcAddress(module, "?SetFrame@View@@UAEXABVRect@@_N@Z"));
    std::printf("GUI.dll: checking native chat line bounds\n");
    for (int size : {9, 12, 13, 14, 16}) {
        alignas(16) uint8_t font[0x1418]{};
        fontCtor(font); g_guiChatFont = font;
        LOGFONTA description{}; description.lfHeight = size; description.lfCharSet = DEFAULT_CHARSET;
        std::memcpy(description.lfFaceName, "Verdana", 8);
        HFONT handle = CreateFontIndirectA(&description);
        HDC dc = CreateCompatibleDC(nullptr); HGDIOBJ previous = SelectObject(dc, handle);
        TEXTMETRICA metrics{}; GetTextMetricsA(dc, &metrics);
        SelectObject(dc, previous); DeleteDC(dc);
        std::memcpy(font + 0x1018, &handle, 4); font[0xFFC] = 1;
        std::memcpy(font + 0xFF0, &metrics.tmHeight, 4); std::memcpy(font + 0x1014, &metrics.tmOverhang, 4);
        alignas(16) uint8_t text[0x400]{};
        rnative::vc10::String string; string.init();
        const char* message = "NPC: The bracer registers mutations in your DNA, so it is important to monitor the data frequently.\n"
                              "We must be able to act quickly if there is an unexpected increase in spontaneous mutation frequency, "
                              "both to prevent and repair undesirable results in the subject. gjpqy 123";
        string.assign(message, std::strlen(message));
        const Rect frame{0, 0, 511, 63};
        textCtor(text, &frame, "chat_probe", &string, 0, 0, 0);
        using Features = void(__thiscall*)(void*, uint32_t);
        reinterpret_cast<Features>(bytes + 0x15DB7A)(text + 0x148, 0x60);
        Point preferred; contentSize(text, &preferred);
        int* first; int* last; std::memcpy(&first, text + 0x260, 4); std::memcpy(&last, text + 0x264, 4);
        if ((last - first) / 5 < 3) ++failures;
        void* data; std::memcpy(&data, text + 0xAC, 4);
        const Rect tight{0, 0, 511, preferred.y};
        std::memcpy(static_cast<uint8_t*>(data) + 0x20, &tight, sizeof(tight));
        std::memcpy(static_cast<uint8_t*>(data) + 0x10, &parent, 4);
        const int height = int(preferred.y + 1);
        for (float factor : {1.0f, 1.5f, 1.76f, 2.0f}) {
            SetScale(factor * 100, 100); BeginScaleFrame(controller);
            alignas(16) uint8_t bitmap[0x44]{};
            CreateTextBitmap(bitmap, 512, (height + 15) & ~15, factor);
            void* cache = bitmap; std::memcpy(text + 0x248, &cache, 4);
            render(text);
            alignas(16) uint8_t surface[0x68]{};
            surfaceCtor(surface, bitmap, 0);
            using Update = void(__thiscall*)(void*, void*);
            reinterpret_cast<Update>(bytes + 0x151619)(surface, bitmap);
            void* displayed = surface; std::memcpy(text + 0x240, &displayed, 4);
            layout(text);
            Rect destination; std::memcpy(&destination, surface + 0x24, sizeof(destination));
            if (std::fabs(destination.bottom - destination.top + 1 - ((height + 15) & ~15)) > 0.01f) ++failures;
            uint64_t uploadedInk = 0;
            for (const auto& texture : g_guiChatTextures) for (uint32_t pixel : texture->pixels)
                uploadedInk += factor > 1 ? (pixel >> 24) != 0 : (pixel & 0x8000) != 0;
            surfaceDtor(surface); g_guiChatTextures.clear();
            displayed = nullptr; std::memcpy(text + 0x240, &displayed, 4);
            uint8_t* pixels; int width, rows, pitch, pixelSize;
            std::memcpy(&pixels, bitmap, 4); std::memcpy(&width, bitmap + 0xC, 4);
            std::memcpy(&rows, bitmap + 8, 4); std::memcpy(&pitch, bitmap + 0x20, 4); std::memcpy(&pixelSize, bitmap + 0x24, 4);
            int bottom = -1; uint64_t bitmapInk = 0;
            for (int y = 0; y < rows; ++y) for (int x = 0; x < width; ++x) {
                const bool ink = pixelSize == 4 ? (reinterpret_cast<uint32_t*>(pixels + y * pitch)[x] >> 24) != 0 :
                    (reinterpret_cast<uint16_t*>(pixels + y * pitch)[x] & 0x8000) != 0;
                if (ink) { bottom = y; ++bitmapInk; }
            }
            if (bottom < 0 || bottom >= int(std::ceil(height * factor)) || uploadedInk != bitmapInk) {
                std::printf("GUI chat: cell %ld scale %.2f bottom %d/%d upload %llu/%llu ink\n",
                            metrics.tmHeight, factor, bottom, int(std::ceil(height * factor)), uploadedInk, bitmapInk);
                ++failures;
            }
            rnative::vc10::FreeArray(pixels); ForgetTextBitmap(bitmap);
            cache = nullptr; std::memcpy(text + 0x248, &cache, 4);
        }
        for (float factor : {1.5f, 1.76f, 2.0f, 1.5f}) {
            SetScale(factor * 100, 100); BeginScaleFrame(controller);
            preRender(text, nullptr, 0, 0xFFFFFFFF);
            void* bitmap; std::memcpy(&bitmap, text + 0x248, 4);
            int rows = 0; if (bitmap) std::memcpy(&rows, static_cast<uint8_t*>(bitmap) + 8, 4);
            void* surface; std::memcpy(&surface, text + 0x240, 4);
            const auto& areas = *reinterpret_cast<rnative::vc10::Vector<void*>*>(static_cast<uint8_t*>(surface) + 0x58);
            if (areas.size() != 1) ++failures;
            for (const auto& texture : g_guiChatTextures) if (texture->format != 2 || !texture->dedicated) ++failures;
            if (rows < int(std::ceil(height * factor))) ++failures;
            for (int width : {300, 220, 511}) {
                const Rect resized{0, 0, float(width), preferred.y};
                void* detached = nullptr; std::memcpy(static_cast<uint8_t*>(data) + 0x10, &detached, 4);
                setFrame(text, &resized, 0);
                std::memcpy(static_cast<uint8_t*>(data) + 0x10, &parent, 4);
                preRender(text, nullptr, 0, 0xFFFFFFFF);
            }
        }
        if (size == 16) {
            char path[MAX_PATH]{};
            if (GetEnvironmentVariableA("RANDYVK_GUI_CACHE_DUMP", path, sizeof(path)) > 0) {
                void* cache; std::memcpy(&cache, text + 0x248, 4);
                const auto before = *reinterpret_cast<Rect*>(static_cast<uint8_t*>(data) + 0x20);
                uint8_t* pixels; int pitch, rows;
                std::memcpy(&pixels, cache, 4); std::memcpy(&pitch, static_cast<uint8_t*>(cache) + 0x20, 4);
                std::memcpy(&rows, static_cast<uint8_t*>(cache) + 8, 4);
                const std::vector<uint8_t> saved(pixels, pixels + size_t(pitch) * rows);
                if (!DumpFontCaches(path) || std::memcmp(saved.data(), pixels, saved.size())) ++failures;
                void* after; std::memcpy(&after, text + 0x248, 4);
                if (cache != after || std::memcmp(&before, static_cast<uint8_t*>(data) + 0x20, sizeof(before))) ++failures;
            }
        }
        void* noParent = nullptr; std::memcpy(static_cast<uint8_t*>(data) + 0x10, &noParent, 4);
        textDtor(text); string.release(); fontDtor(font); g_guiChatTextures.clear();
    }
    SetScale(100, 100); BeginScaleFrame(controller);
    std::printf("GUI chat: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures;
}
