// Exercise the checked GUI.dll hooks on synthetic objects in the supported client layout.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native/gui_scale.h"
#include "native/gui.h"
#include "native/gui_font.h"
#include "native/vc10.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>

namespace rnative::gui {
void InstallScaling(HMODULE gui);
bool BeginScaleFrame(void* controller);
void ScaleDrawing(bool drawing);
void EndScaleFrame();
}

inline int __fastcall CaptureGuiWindowRect(void* message, void*, const char*, const rnative::gui::Rect* frame)
{
    std::memcpy(message, frame, sizeof(*frame));
    return 0;
}

inline int CheckGuiClient(const char* client)
{
    using namespace rnative::gui;
    SetDllDirectoryA(client);
    const std::string path = std::string(client) + "/GUI.dll";
    HMODULE module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { std::printf("GUI.dll: load failed (%lu)\n", GetLastError()); return 1; }
    auto symbol = [&](const char* name) { return GetProcAddress(module, name); };
    using ValuePoint = Point*(__thiscall*)(void*, Point*, const Point*, uint32_t);
    using PointerPoint = void(__thiscall*)(void*, Point*, uint32_t);
    using ValueRect = Rect*(__thiscall*)(void*, Rect*, const Rect*, uint32_t);
    using ParentPoint = Point*(__thiscall*)(void*, Point*, const Point*);
    using Getter = Point*(__thiscall*)(void*, Point*);
    using Screen = Point*(__cdecl*)(Point*);
    auto to = reinterpret_cast<ValuePoint>(symbol("?ConvertToScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    auto from = reinterpret_cast<ValuePoint>(symbol("?ConvertFromScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    auto toPointer = reinterpret_cast<PointerPoint>(symbol("?ConvertToScreen@View@@QBEXPAVPoint@@_N@Z"));
    auto fromPointer = reinterpret_cast<PointerPoint>(symbol("?ConvertFromScreen@View@@QBEXPAVPoint@@_N@Z"));
    auto toRect = reinterpret_cast<ValueRect>(symbol("?ConvertToScreen@View@@QBE?AVRect@@ABV2@_N@Z"));
    auto fromRect = reinterpret_cast<ValueRect>(symbol("?ConvertFromScreen@View@@QBE?AVRect@@ABV2@_N@Z"));
    auto parent = reinterpret_cast<ParentPoint>(symbol("?ConvertFromParent@View@@QBE?AVPoint@@ABV2@@Z"));
    auto screen = reinterpret_cast<Screen>(symbol("?GetScreenSize@Window@@SA?AVPoint@@XZ"));
    auto physical = reinterpret_cast<Getter>(symbol("?GetScreenSize@WindowController_c@@QBE?AVPoint@@XZ"));
    auto windowMouse = reinterpret_cast<Getter>(symbol("?GetMousePosition@Window@@QBE?AVPoint@@XZ"));
    auto mouse = reinterpret_cast<Getter>(symbol("?GetMousePosition@WindowController_c@@QBE?AVPoint@@XZ"));
    auto** instance = reinterpret_cast<void**>(symbol("?s_pcInstance@WindowController_c@@0PAV1@A"));
    if (!to || !from || !toPointer || !fromPointer || !toRect || !fromRect || !parent || !screen || !physical || !mouse || !windowMouse || !instance)
        return 1;
    struct Object { alignas(16) uint8_t bytes[0x400]{}; };
    Object controller, top, topData, root, rootData, owner, ownerData, child, childData;
    Object hud, hudData, hudOwner, hudOwnerData, hudChild, hudChildData;
    auto put = [](Object& o, size_t offset, const auto& value) { std::memcpy(o.bytes + offset, &value, sizeof(value)); };
    auto pointer = [&](Object& o, size_t offset, Object& target) { void* p = target.bytes; put(o, offset, p); };
    auto view = [&](Object& object, Object& data, Object& window, Rect frame) {
        pointer(object, 0xAC, data); pointer(object, 0x40, window);
        put(data, 0x20, frame); put(data, 0x100, Point{1, 1});
    };
    rnative::vc10::String name, hudName;
    name.init(); hudName.init();
    std::memcpy(name.buffer, "test", 5); name.size = 4;
    std::memcpy(hudName.buffer, "cc_test", 8); hudName.size = 7;
    void* n = &name; put(owner, 0x60, n); n = &hudName; put(hudOwner, 0x60, n);
    pointer(owner, 0x70, ownerData); pointer(ownerData, 0x80, root);
    pointer(hudOwner, 0x70, hudOwnerData); pointer(hudOwnerData, 0x80, hud);
    pointer(top, 0xAC, topData); put(topData, 0x100, Point{1, 1});
    view(root, rootData, owner, {100, 50, 299, 149});
    view(child, childData, owner, {20, 10, 99, 49});
    view(hud, hudData, hudOwner, {500, 50, 699, 149});
    view(hudChild, hudChildData, hudOwner, {10, 10, 89, 49});
    pointer(rootData, 0x10, top); pointer(childData, 0x10, root);
    pointer(hudData, 0x10, top); pointer(hudChildData, 0x10, hud);
    pointer(root, 0x1B8, child); pointer(hud, 0x1B8, hudChild);
    pointer(rootData, 0, child); pointer(hudData, 0, hudChild);
    pointer(topData, 0, root); pointer(rootData, 8, hud);
    pointer(controller, 0x94, top); put(controller, 0x80, Point{3840, 2160});
    put(controller, 0x48, Point{246, 128});
    void* saved = *instance; *instance = controller.bytes;
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("GUI.dll: %s\n", what); } };
    auto equal = [](Point a, Point b) { return a.x == b.x && a.y == b.y; };
    InstallScaling(module);
    SetScale(200, 150);
    Point pending;
    screen(&pending);
    check(equal(pending, {3840, 2160}), "scale change waits for the next GUI frame");
    BeginScaleFrame(controller.bytes);
    ScaleDrawing(true);
    Point input{3, 4}, output, restored;
    to(child.bytes, &output, &input, 1);
    check(equal(output, {246, 128}), "window drawing uses 200%");
    from(child.bytes, &restored, &output, 1);
    check(equal(restored, input), "window hit test inverts the display transform once");
    Point pointerInput = input;
    toPointer(child.bytes, &pointerInput, 1);
    check(equal(pointerInput, output), "pointer and value drawing overloads agree");
    fromPointer(child.bytes, &pointerInput, 1);
    check(equal(pointerInput, input), "pointer input overload inverts drawing");
    to(hudChild.bytes, &output, &input, 1);
    check(equal(output, {769.5f, 96}), "HUD drawing uses its independent 150%");
    from(hudChild.bytes, &restored, &output, 1);
    check(equal(restored, input), "HUD input matches drawing");
    Point hit{246, 128};
    parent(root.bytes, &output, &hit);
    parent(child.bytes, &restored, &output);
    check(equal(restored, input), "root hit testing and nested local coordinates agree");
    Rect rectangle{0, 0, 19, 9}, drawn, inverse;
    toRect(child.bytes, &drawn, &rectangle, 1);
    check(drawn.left == 240 && drawn.top == 120 && drawn.right == 279 && drawn.bottom == 139, "clip bounds map to physical pixels");
    fromRect(child.bytes, &inverse, &drawn, 1);
    check(std::memcmp(&rectangle, &inverse, sizeof(rectangle)) == 0, "clip inverse retains inclusive bounds");
    screen(&output); check(equal(output, {3840, 2160}), "native layout retains the canonical 4K workspace");
    physical(controller.bytes, &output); check(equal(output, {3840, 2160}), "physical output extent stays unchanged");
    mouse(controller.bytes, &output); check(equal(output, {246, 128}), "cursor and world mouse coordinates stay physical");
    SetScale(100, 100);
    to(child.bytes, &output, &input, 1);
    check(equal(output, {246, 128}), "drawing and input keep the displayed scale until the next frame");
    SetScale(200, 150);
    put(child, 0x188, Rect{4, 6, 0, 0});
    windowMouse(owner.bytes, &output);
    check(equal(output, {-1, -2}), "window mouse query accounts for scaled borders");
    // Compare a clipped textured quad against the native 100% reference.
    using Vertices = void(__thiscall*)(void*, void*, const Rect*);
    auto vertices = reinterpret_cast<Vertices>(symbol("?_UpdateVertices@ViewArea_c@@AAEXPAVView@@ABVRect@@@Z"));
    Object area, node, slot, texture;
    put(area, 0x80, Rect{10, 20, 109, 69});
    put(area, 0x90, Rect{0, 0, 99, 49});
    pointer(area, 0xA0, node); pointer(node, 0xC, slot); pointer(slot, 0, texture);
    put(texture, 8, uint32_t(256)); put(texture, 12, uint32_t(256));
    auto uv = [&](int i, int coord) {
        float v; std::memcpy(&v, area.bytes + i * 28 + 20 + coord * 4, 4); return v;
    };
    put(rootData, 0x20, Rect{0, 0, 199, 99});
    put(hudData, 0x20, Rect{0, 0, 199, 99});
    put(childData, 0x20, Rect{120, 60, 199, 99});
    SetScale(100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    const Rect clip100{150, 60, 199, 109};
    vertices(area.bytes, child.bytes, &clip100);
    float reference[8], positions[8];
    for (int i = 0; i < 4; ++i) for (int c = 0; c < 2; ++c) {
        reference[i * 2 + c] = uv(i, c);
        std::memcpy(&positions[i * 2 + c], area.bytes + i * 28 + c * 4, 4);
    }
    for (float factor : {0.5f, 0.75f, 1.25f, 1.5f, 2.0f, 1.0f}) {
        SetScale(factor * 100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
        Rect clip = clip100;
        Scale{factor}.ToScreen(clip);
        vertices(area.bytes, child.bytes, &clip);
        bool sameUv = true, correctPositions = true;
        for (int i = 0; i < 4; ++i) for (int c = 0; c < 2; ++c) {
            sameUv = sameUv && reference[i * 2 + c] == uv(i, c);
            float position;
            std::memcpy(&position, area.bytes + i * 28 + c * 4, 4);
            correctPositions = correctPositions && position == (positions[i * 2 + c] + 0.5f) * factor - 0.5f;
        }
        check(sameUv, "clipped atlas sampling must be invariant under scaling");
        check(correctPositions, "clipped vertices scale with their half-pixel convention intact");
    }
    // Slider changes leave the native frames used by preferences untouched.
    const Rect edge{3500, 1900, 3799, 2099};
    put(rootData, 0x20, edge);
    for (float percent : {100.0f, 200.0f, 75.0f, 150.0f, 100.0f}) {
        SetScale(percent, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
        using WindowFrame = Rect*(__thiscall*)(void*, Rect*, uint32_t);
        auto windowFrame = reinterpret_cast<WindowFrame>(symbol("?GetFrame@Window@@QBE?AVRect@@_N@Z"));
        Rect actual;
        ScaleDrawing(false);
        windowFrame(owner.bytes, &actual, 0);
        check(std::memcmp(&actual, &edge, sizeof(actual)) == 0, "native window queries and preference saves use canonical coordinates");
        using SaveWindow = void(__thiscall*)(void*, void*);
        auto saveWindow = reinterpret_cast<SaveWindow>(symbol("?SaveWndConfig@Window@@QAEXPAVMessage@@@Z"));
        auto** rectSink = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(module) + 0x1A800C);
        void* originalSink = *rectSink;
        DWORD protection;
        if (VirtualProtect(rectSink, sizeof(void*), PAGE_READWRITE, &protection)) {
            *rectSink = reinterpret_cast<void*>(&CaptureGuiWindowRect);
            put(ownerData, 0x3C, uint32_t(0x800));
            Rect savedFrame{};
            saveWindow(owner.bytes, &savedFrame);
            check(std::memcmp(&savedFrame, &edge, sizeof(savedFrame)) == 0, "actual native preference serialization excludes display transforms");
            put(ownerData, 0x3C, uint32_t(0));
            *rectSink = originalSink;
            VirtualProtect(rectSink, sizeof(void*), protection, &protection);
        } else check(false, "preference serialization fixture could not install its sink");
        ScaleDrawing(true);
        std::memcpy(&actual, rootData.bytes + 0x20, sizeof(actual));
        check(std::memcmp(&actual, &edge, sizeof(actual)) == 0, "scale changes never move or resize the native frame");
        to(child.bytes, &output, &input, 1);
        from(child.bytes, &restored, &output, 1);
        check(equal(restored, input), "edge-clamped drawing and input agree");
    }
    to(child.bytes, &output, &input, 1);
    check(equal(output, {3623, 1964}), "100% restores the original edge placement exactly");
    put(hudData, 0x20, Rect{0, 0, 3839, 2159});
    put(hudChildData, 0x20, Rect{3700, 2000, 3799, 2099});
    SetScale(100, 200); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    to(hudChild.bytes, &output, &input, 1);
    check(equal(output, {3566, 1848}), "full-screen HUD children scale around their screen edge");
    from(hudChild.bytes, &restored, &output, 1);
    check(equal(restored, input), "anchored HUD input inverts its display transform");
    Rect full;
    std::memcpy(&full, hudData.bytes + 0x20, sizeof(full));
    check(full.right == 3839 && full.bottom == 2159, "HUD scaling never reflows the native full-screen root");
    Point hudHit{3566, 1848};
    parent(hud.bytes, &output, &hudHit);
    parent(hudChild.bytes, &restored, &output);
    check(equal(restored, input), "HUD hit testing crosses the anchored subtree boundary once");
    SetScale(100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    to(hudChild.bytes, &output, &input, 1);
    check(equal(output, {3703, 2004}), "HUD scale round trip restores original coordinates");
    Object icon, iconData;
    view(icon, iconData, hudOwner, {3700, 0, 3799, 79});
    pointer(iconData, 0x10, hudChild); pointer(hudChildData, 0, icon);
    put(hudChildData, 0x20, Rect{0, 2000, 3839, 2099});
    SetScale(100, 200); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    to(icon.bytes, &output, &input, 1);
    check(equal(output, {3566, 1848}), "wide HUD containers preserve child anchoring");
    Point iconHit = output;
    parent(hud.bytes, &output, &iconHit);
    parent(hudChild.bytes, &restored, &output);
    parent(icon.bytes, &output, &restored);
    check(equal(output, input), "nested HUD input crosses a full-width container correctly");
    const Rect strip{0, 0, 3839, 99};
    toRect(hudChild.bytes, &drawn, &strip, 1);
    check(drawn.top == 1840 && drawn.bottom == 2099, "HUD container clipping includes enlarged children");
    SetScale(100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    to(icon.bytes, &output, &input, 1);
    check(equal(output, {3703, 2004}), "nested HUD layout round trip restores original positions");
    check(FontScalingReady(), "native text-cache hooks installed");
    using FontCtor = void*(__thiscall*)(void*);
    using FontDtor = void(__thiscall*)(void*);
    struct IPoint { int x, y; };
    using RenderString = void(__thiscall*)(void*, IPoint*, int, int, int, void*, uint32_t, uint32_t, uint32_t);
    auto fontCtor = reinterpret_cast<FontCtor>(symbol("??0FontInfo_t@@QAE@XZ"));
    auto fontDtor = reinterpret_cast<FontDtor>(symbol("??1FontInfo_t@@QAE@XZ"));
    auto renderString = reinterpret_cast<RenderString>(symbol("?_RenderString@TextRenderer_c@@AAEXPAVIPoint@@HHHPAVFontInfo_t@@I_N2@Z"));
    alignas(16) uint8_t fontInfo[0x1418]{};
    fontCtor(fontInfo);
    LOGFONTA description{};
    description.lfHeight = -16; description.lfWeight = FW_NORMAL; description.lfCharSet = DEFAULT_CHARSET;
    std::memcpy(description.lfFaceName, "Verdana", 8);
    HFONT fontHandle = CreateFontIndirectA(&description);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldFont = SelectObject(dc, fontHandle);
    TEXTMETRICA metrics{}; GetTextMetricsA(dc, &metrics);
    char face[LF_FACESIZE]{}; GetTextFaceA(dc, LF_FACESIZE, face);
    std::printf("GUI font: requested Verdana, resolved %s (charset %u)\n", face, unsigned(metrics.tmCharSet));
    SelectObject(dc, oldFont); DeleteDC(dc);
    auto rawPut = [](void* object, size_t offset, const auto& value) { std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value)); };
    rawPut(fontInfo, 0x1018, fontHandle); rawPut(fontInfo, 0xFFC, uint8_t(1)); rawPut(fontInfo, 0xFF0, int(metrics.tmHeight));
    Object text, textData;
    view(text, textData, owner, {0, 0, 383, 39});
    rnative::vc10::String content{}; content.init(); content.assign("Verdana 4K: Agjp 123", 20);
    put(text, 0x14C, content);
    std::vector<uint32_t> baseline;
    IPoint baselinePen{};
    for (float factor : {1.0f, 2.0f, 1.5f, 1.1f, 1.76f, 1.0f}) {
        alignas(16) uint8_t bitmap[0x44]{};
        CreateTextBitmap(bitmap, 384, 40, factor);
        void* cache = bitmap; put(text, 0x248, cache);
        IPoint pen{4, 4};
        renderString(text.bytes, &pen, metrics.tmHeight, 0, int(content.size), fontInfo, 0xFFFFFF, 0, 0);
        uint8_t* data; int width, height, pitch, bytesPerPixel;
        std::memcpy(&data, bitmap, 4); std::memcpy(&width, bitmap + 0xC, 4); std::memcpy(&height, bitmap + 8, 4);
        std::memcpy(&pitch, bitmap + 0x20, 4); std::memcpy(&bytesPerPixel, bitmap + 0x24, 4);
        std::vector<uint32_t> rendered(size_t(width) * height);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            if (bytesPerPixel == 4) std::memcpy(&rendered[size_t(y) * width + x], data + size_t(y) * pitch + x * 4, 4);
            else {
                uint16_t pixel; std::memcpy(&pixel, data + size_t(y) * pitch + x * 2, 2);
                rendered[size_t(y) * width + x] = (pixel & 0x8000 ? 0xFF000000 : 0) | (pixel & 0x7FFF ? 0xFFFFFF : 0);
            }
        }
        const auto* pixels = rendered.data();
        int visible = 0, antialias = 0;
        for (int i = 0; i < width * height; ++i) {
            const uint32_t alpha = pixels[i] >> 24;
            if (alpha) ++visible;
            if (alpha && alpha < 255) ++antialias;
        }
        check(visible > 0, "native text string produces visible glyphs");
        if (factor == 1.0f && baseline.empty()) { baseline.assign(pixels, pixels + width * height); baselinePen = pen; }
        check(pen.x == baselinePen.x && pen.y == baselinePen.y, "font raster size leaves native text advances unchanged");
        if (factor > 1.0f) check(antialias > 0, "larger text uses freshly rasterized antialiased outlines");
        if (factor == 2.0f) {
            int differences = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                if (pixels[size_t(y) * width + x] != baseline[size_t(y / 2) * 384 + x / 2]) ++differences;
            check(differences > 0, "200% text is not a magnified copy of the native bitmap");
        }
        if (factor > 1.0f) {
            IPoint selectedPen{4, 4};
            renderString(text.bytes, &selectedPen, metrics.tmHeight, 0, int(content.size), fontInfo, 0xFFFFFF, 1, 1);
            uint32_t selectedPixel;
            const int corner = int(std::lround(4 * factor));
            std::memcpy(&selectedPixel, data + size_t(corner) * pitch + corner * 4, 4);
            check((selectedPixel >> 24) == 255, "selection and underline drawing use the high-resolution cache");
            check(selectedPen.x == baselinePen.x && selectedPen.y == baselinePen.y, "selected text retains native advances");
        }
        Object surface;
        put(surface, 0x3D, uint8_t(1)); put(surface, 0x3E, uint8_t(1));
        put(surface, 4, Rect{0, 0, float(width - 1), float(height - 1)});
        put(surface, 0x14, Rect{0, 0, float(width - 1), float(height - 1)});
        put(surface, 0x34, float(width)); put(surface, 0x38, float(height));
        void* textureSurface = surface.bytes; put(text, 0x240, textureSurface);
        put(text, 0x28C, 1.0f); put(text, 0x290, 1.0f);
        using LayoutSurface = void(__thiscall*)(void*);
        using SurfaceFrame = Rect*(__thiscall*)(void*, Rect*);
        auto layoutSurface = reinterpret_cast<LayoutSurface>(symbol("?_LayoutRenderSurface@TextRenderer_c@@AAEXXZ"));
        auto sourceFrame = reinterpret_cast<SurfaceFrame>(symbol("?GetSrcFrame@ViewSurface_c@@QBE?AVRect@@XZ"));
        auto destinationFrame = reinterpret_cast<SurfaceFrame>(symbol("?GetDstFrame@ViewSurface_c@@QBE?AVRect@@XZ"));
        layoutSurface(text.bytes);
        Rect textureFrame, logicalFrame;
        sourceFrame(surface.bytes, &textureFrame); destinationFrame(surface.bytes, &logicalFrame);
        check(textureFrame.right - textureFrame.left + 1 == width, "text texture retains its full raster resolution");
        check(logicalFrame.right - logicalFrame.left + 1 == 384 && logicalFrame.bottom - logicalFrame.top + 1 == 40,
              "larger font texture retains its native logical layout size");
        if (factor == 1.0f) check(std::memcmp(pixels, baseline.data(), baseline.size() * 4) == 0, "100% retains the exact native text raster");
        char* prefix = nullptr; size_t prefixSize = 0;
        _dupenv_s(&prefix, &prefixSize, "RANDYVK_GUI_FONT_SHOT");
        if (prefix) {
            const std::string path = std::string(prefix) + "-" + std::to_string(int(factor * 100)) + ".bmp";
            FILE* outputFile = nullptr;
            if (fopen_s(&outputFile, path.c_str(), "wb") == 0 && outputFile) {
                BITMAPFILEHEADER file{}; file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + width * height * 4;
                BITMAPINFOHEADER image{}; image.biSize = sizeof(image); image.biWidth = width; image.biHeight = -height; image.biPlanes = 1; image.biBitCount = 32;
                std::fwrite(&file, sizeof(file), 1, outputFile); std::fwrite(&image, sizeof(image), 1, outputFile);
                for (int i = 0; i < width * height; ++i) {
                    const uint32_t value = (pixels[i] >> 24) * 0x010101;
                    std::fwrite(&value, 4, 1, outputFile);
                }
                std::fclose(outputFile);
            }
        }
        std::free(prefix);
        rnative::vc10::FreeArray(data); ForgetTextBitmap(bitmap);
    }
    content.release(); fontDtor(fontInfo);
    EndScaleFrame(); *instance = saved;
    std::printf("GUI.dll: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
