// The game's effects as it makes and ends them: the first step of our own effects (docs/fx-plan.md).
//
// Every effect goes through Gamecode.dll's _EffectHandler_t: CreateEffect2(template id, anchor) builds the control
// (CreateGfxControl) and files it with MakeHandle(control, template id), which returns the handle the game keeps;
// TerminateEffectGracefully(handle) and DeleteEffect(handle) end it. The three are hooked (trampolines, their first
// bytes checked against the known client) and only observe. With RVK_FxLog (randy-vk.ini only) each is logged: the
// template, its control class (the vtable's RTTI name), which CreateEffect2 overload made it (the kind of anchor:
// a position, a character, a pair of them for a projectile) and the game code that asked, as module+offset return
// addresses. That tells which template an action plays, without casting every nano by hand.
//
// The console's "fx <template id>" plays a template on the player's character (CreateEffect2 with the client's
// control dynel and the template's own body location), at the next presented frame, on the game's thread.
//
// Our own effects (RVK_Fx): the .rvkfx files in <client>\randy-vk\fx (looked at again every second: an edited file
// shows at once), each standing for some of the game's templates. When the game makes one of those - for now a cast,
// _GfxControlSpell1_t - it is noted at MakeHandle, and its update (vtable slot 1, patched) announces our effect every
// frame after the game's own update: the caster's hand, the target, the bolt's head, and the phase from the cast's own
// clock (charging until +0x78, flying until it hits, +0x7C, then the impact). Replacing it ("mode": "replace"), the
// cast's own drawing is hidden: its sprites (+0x38) and the flares and cords at the hands (+0x140..+0x14C, their
// sprites at +0x30), switched off after each update that switched them on; the bolt's head and the impact's effect
// (templates +0x98, +0x9C, made later and only when non-zero) never made. Layouts: re/fx/spell1*.c, flarecord.c.
#include "rvk_backend.h"
#include "rvk_settings.h"

#include "native/native.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rvkproxy {

namespace {

using MakeHandleFn = uint32_t(__fastcall*)(void* self, void* edx, void* control, uint32_t templateId);
using HandleFn = void(__fastcall*)(void* self, void* edx, uint32_t handle);
using InstanceFn = void*(__cdecl*)();
using DynelFn = const void*(__fastcall*)(const void* engine, void* edx);
using CreateOnDynelFn = uint32_t(__fastcall*)(void* self, void* edx, int templateId, const void* dynel, int location);

MakeHandleFn g_makeHandle;
HandleFn g_terminate, g_delete;
InstanceFn g_handlerInstance, g_engineInstance;
DynelFn g_clientDynel;
CreateOnDynelFn g_createOnDynel;

bool g_installed, g_gaveUp;
std::atomic<int> g_playRequest{0};

// CreateEffect2 / CreateTracer overloads by address, to name the one a MakeHandle returns into.
struct Overload { uintptr_t at; std::string anchor; };
std::vector<Overload> g_overloads;

// Handles made since RVK_FxLog came on: template and when, for the age at terminate / delete.
struct Live { uint32_t templateId; DWORD made; };
std::unordered_map<uint32_t, Live> g_live;

bool LogOn()
{
    return rvk_settings::GetEffective("RVK_FxLog") != 0.0f;
}

bool FxOn()
{
    return rvk_settings::GetEffective("RVK_Fx") != 0.0f;
}

// --- our effects' files ---

struct EffectFile {
    std::string file;                            // its name in the folder
    FILETIME written{};
    uint32_t id = 0;
    std::shared_ptr<const rvk::fx::Effect> def;  // null: didn't parse
};
std::vector<EffectFile> g_files;
std::unordered_map<uint32_t, uint32_t> g_byTemplate;   // the game's template -> the file standing for it (index)
uint32_t g_nextId = 1;
uint64_t g_sentSerial = 0;                      // the device the effects were last given to
uint32_t g_frame = 0;

std::string FxDir()
{
    std::string dir = rvk_settings::IniPath();
    const size_t slash = dir.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : dir.substr(0, slash + 1)) + "randy-vk\\fx\\";
}

// The folder looked at again: new and changed files read, removed ones forgotten; the device told of each change (all
// of them for a new device).
void ScanEffects(bool force)
{
    const std::string dir = FxDir();
    std::vector<EffectFile> seen;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.rvkfx").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            EffectFile e;
            e.file = fd.cFileName;
            e.written = fd.ftLastWriteTime;
            seen.push_back(std::move(e));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    rvk::ThreadedDevice* device = g_rvk.device;
    const bool newDevice = device && g_sentSerial != g_rvk.deviceSerial;
    bool changed = false;
    for (EffectFile& e : seen) {
        auto old = std::find_if(g_files.begin(), g_files.end(), [&](const EffectFile& f) { return f.file == e.file; });
        if (old != g_files.end() && !force && CompareFileTime(&old->written, &e.written) == 0) {
            e = *old;
            continue;
        }
        e.id = old != g_files.end() ? old->id : g_nextId++;
        std::string text;
        if (FILE* f = std::fopen((dir + e.file).c_str(), "rb")) {
            char buf[4096];
            for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
            std::fclose(f);
        }
        auto def = std::make_shared<rvk::fx::Effect>();
        std::string error;
        if (rvk::fx::Parse(text, *def, &error)) {
            e.def = def;
            std::string ids;
            for (uint32_t t : def->templates) ids += (ids.empty() ? "" : ", ") + std::to_string(t);
            RvkLog("fx: %s: \"%s\", %zu emitters, %s templates %s", e.file.c_str(), def->name.c_str(), def->emitters.size(),
                   def->replace ? "replacing" : "with", ids.empty() ? "(none)" : ids.c_str());
        } else {
            RvkLog("fx: %s: not loaded - %s", e.file.c_str(), text.empty() ? "can't read it" : error.c_str());
        }
        changed = true;
    }
    for (const EffectFile& f : g_files)
        if (std::none_of(seen.begin(), seen.end(), [&](const EffectFile& e) { return e.file == f.file; })) {
            RvkLog("fx: %s gone", f.file.c_str());
            if (device) device->FxForget(f.id);
            changed = true;
        }
    g_files = std::move(seen);
    if (changed || newDevice) {
        g_byTemplate.clear();
        for (uint32_t i = 0; i < g_files.size(); ++i)
            if (g_files[i].def)
                for (uint32_t t : g_files[i].def->templates) g_byTemplate[t] = i;
        if (device)
            for (const EffectFile& f : g_files)
                if (f.def) device->FxSetEffect(f.id, f.def);
        if (device) g_sentSerial = g_rvk.deviceSerial;
    }
}

// --- the cast (_GfxControlSpell1_t) ---

using ProcessFn = void(__fastcall*)(void* self, void* edx);
using DisableRenderingFn = void(__fastcall*)(void* visual, void* edx, int disable);
ProcessFn g_spellProcess;
DisableRenderingFn g_disableRendering;
uintptr_t g_spellVtable, g_flareVtable, g_cordVtable;
// Gamecode.dll (image base 0x10000000): the vtables, Spell1's update (slot 1).
constexpr uint32_t kSpellVtable = 0x1016CBAC - 0x10000000, kSpellUpdate = 0x100F3A04 - 0x10000000;
constexpr uint32_t kFlareVtable = 0x1016BCF4 - 0x10000000, kCordVtable = 0x1016B524 - 0x10000000;

struct Cast {
    uint32_t file = 0;                           // index in g_files
    uint32_t id = 0;                             // its effect id
    uint32_t templateId = 0;
    bool replace = true;
    uint32_t lastFrame = 0;
};
std::unordered_map<const void*, Cast> g_casts;

template <class T> T At(const void* object, uint32_t offset)
{
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(object) + offset, sizeof(T));
    return v;
}

// A cast the game just made: one of ours stands for its template? Then noted, and (replacing it) the bolt's head and
// the impact's effect made by it later left unmade.
void NoteCast(void* control, uint32_t templateId)
{
    if (!g_spellVtable || !control || At<uintptr_t>(control, 0) != g_spellVtable || !FxOn())
        return;
    auto bound = g_byTemplate.find(templateId);
    if (bound == g_byTemplate.end() || !g_files[bound->second].def)
        return;
    const EffectFile& f = g_files[bound->second];
    Cast c{bound->second, f.id, templateId, f.def->replace, g_frame};
    g_casts[control] = c;
    if (c.replace) {
        const uint32_t none = 0;
        std::memcpy(static_cast<uint8_t*>(control) + 0x98, &none, 4);
        std::memcpy(static_cast<uint8_t*>(control) + 0x9C, &none, 4);
    }
}

void __fastcall SpellProcessHook(void* self, void* edx)
{
    g_spellProcess(self, edx);
    auto it = g_casts.find(self);
    if (it == g_casts.end() || At<uint32_t>(self, 4) != it->second.templateId)
        return;
    Cast& c = it->second;
    c.lastFrame = g_frame;
    if (At<uint8_t>(self, 0x14) || !FxOn() || !g_rvk.device)   // (done: the game deletes it)
        return;
    if (c.replace && g_disableRendering) {
        if (void* visual = At<void*>(self, 0x38)) g_disableRendering(visual, nullptr, 1);
        for (uint32_t offset = 0x140; offset <= 0x14C; offset += 4) {
            const void* child = At<const void*>(self, offset);
            if (!child) continue;
            const uintptr_t vt = At<uintptr_t>(child, 0);
            if (vt == g_flareVtable || vt == g_cordVtable)
                if (void* visual = At<void*>(child, 0x30)) g_disableRendering(visual, nullptr, 1);
        }
    }
    const float age = At<float>(self, 0x0C), leaves = At<float>(self, 0x78), hits = At<float>(self, 0x7C);
    rvk::Device::FxAnchors a;
    std::memcpy(a.source, static_cast<const uint8_t*>(self) + 0xE4, 12);
    std::memcpy(a.target, static_cast<const uint8_t*>(self) + 0xFC, 12);
    if (age < leaves) {
        a.phase = rvk::fx::kCharge;
        std::memcpy(a.head, a.source, 12);
    } else if (age <= hits) {
        a.phase = rvk::fx::kFlight;
        std::memcpy(a.head, static_cast<const uint8_t*>(self) + 0x130, 12);
    } else {
        a.phase = rvk::fx::kImpact;
        std::memcpy(a.head, a.target, 12);
    }
    g_rvk.device->FxInstance(uint64_t(reinterpret_cast<uintptr_t>(self)) ^ (uint64_t(c.templateId) << 32), c.id, a);
}

// "Gamecode.dll+0x0d1b5a" for a code address.
std::string Where(uintptr_t address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const char*>(address), &module) || !module) {
        char b[16];
        std::snprintf(b, sizeof(b), "%08x", unsigned(address));
        return b;
    }
    char path[MAX_PATH] = "";
    GetModuleFileNameA(module, path, sizeof(path));
    const char* name = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
    char b[MAX_PATH + 16];
    std::snprintf(b, sizeof(b), "%s+0x%06x", name ? name + 1 : path, unsigned(address - uintptr_t(module)));
    return b;
}

// A control's class from its vtable's RTTI (CompleteObjectLocator -> TypeDescriptor ".?AV_GfxControlStars_t@@").
std::string ClassOf(const void* control)
{
    if (!control)
        return "(none)";
    const uintptr_t* vtable = *static_cast<uintptr_t* const*>(control);
    const uint8_t* locator = reinterpret_cast<const uint8_t*>(vtable[-1]);
    const uint8_t* type = locator ? *reinterpret_cast<const uint8_t* const*>(locator + 12) : nullptr;
    const char* name = type ? reinterpret_cast<const char*>(type + 8) : nullptr;
    if (!name || std::strncmp(name, ".?AV", 4) != 0)
        return "?";
    std::string s(name + 4);
    if (size_t at = s.find("@@"); at != std::string::npos) s.resize(at);
    for (const char* prefix : {"_GfxControl", "GfxControl"})
        if (s.rfind(prefix, 0) == 0) {
            s.erase(0, std::strlen(prefix));
            break;
        }
    return s;
}

// "dynel+int" from "?CreateEffect2@_EffectHandler_t@@QAEIHABVn3Dynel_t@@H@Z": the parameters after the template id
// (MSVC mangling: ABV / AAV <class>@@ a reference, H an int, 0 the first class again).
std::string AnchorOf(const char* mangled)
{
    const char* p = std::strstr(mangled, "@@QAEIH");
    if (!p)
        return "?";
    p += 7;
    std::string out, first;
    while (*p && *p != '@') {
        std::string part;
        if ((p[0] == 'A' && (p[1] == 'B' || p[1] == 'A') && p[2] == 'V')) {
            const char* end = std::strstr(p + 3, "@@");
            if (!end) break;
            part.assign(p + 3, end);
            if (first.empty()) first = part;
            p = end + 2;
        } else if (*p == 'H') {
            part = "int";
            ++p;
        } else if (*p == '0') {
            part = first;
            ++p;
        } else {
            break;
        }
        for (const char* t : {"Vector3_t", "TMatrix4_t", "n3Dynel_t"})
            if (part == t) part = t[0] == 'V' ? "pos" : t[0] == 'T' ? "matrix" : "dynel";
        if (part == "RConnector") part = "connector";
        out += (out.empty() ? "" : "+") + part;
    }
    if (std::strstr(mangled, "CreateTracer")) out = "tracer " + out;
    return out.empty() ? "none" : out;
}

void ReadOverloads(HMODULE gc)
{
    const auto* base = reinterpret_cast<const uint8_t*>(gc);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress)
        return;
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const uint16_t*>(base + exp->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const uint32_t*>(base + exp->AddressOfFunctions);
    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (std::strncmp(name, "?CreateEffect2@_EffectHandler_t@@", 33) == 0 ||
            std::strncmp(name, "?CreateTracer@_EffectHandler_t@@", 32) == 0)
            g_overloads.push_back({uintptr_t(base + functions[ordinals[i]]), AnchorOf(name)});
    }
    std::sort(g_overloads.begin(), g_overloads.end(), [](const Overload& a, const Overload& b) { return a.at < b.at; });
}

const char* OverloadAt(uintptr_t returnAddress)
{
    const Overload* best = nullptr;
    for (const Overload& o : g_overloads)
        if (o.at <= returnAddress) best = &o;
    return best && returnAddress - best->at < 0x100 ? best->anchor.c_str() : nullptr;
}

// Return addresses up the frame-pointer chain from a hook's frame: CreateEffect2's caller and theirs.
std::string Callers(void* const* frame, int count)
{
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    std::string out;
    for (int i = 0; i < count && frame; ++i) {
        if (frame <= tib->StackLimit || frame >= tib->StackBase || (uintptr_t(frame) & 3))
            break;
        const uintptr_t ret = uintptr_t(frame[1]);
        if (!ret)
            break;
        out += (out.empty() ? "" : " < ") + Where(ret);
        frame = static_cast<void* const*>(frame[0]);
    }
    return out;
}

uint32_t __fastcall MakeHandleHook(void* self, void* edx, void* control, uint32_t templateId)
{
    const uint32_t handle = g_makeHandle(self, edx, control, templateId);
    if (handle)
        NoteCast(control, templateId);
    if (!handle || !LogOn())
        return handle;
    // This hook stands in MakeHandle's place: its return address is in CreateEffect2, and CreateEffect2's frame
    // (push ebp; mov ebp, esp) is the one ours was entered from.
    void* const* frame = static_cast<void* const*>(__builtin_frame_address(0));
    const uintptr_t inCreate = uintptr_t(__builtin_return_address(0));
    const char* anchor = OverloadAt(inCreate);
    if (g_live.size() > 4096) g_live.clear();      // (effects that end by themselves never pass DeleteEffect)
    g_live[handle] = {templateId, GetTickCount()};
    RvkLog("fx: make %u %s handle %u anchor %s from %s", templateId, ClassOf(control).c_str(), handle,
           anchor ? anchor : ("? (" + Where(inCreate) + ")").c_str(), Callers(static_cast<void* const*>(frame[0]), 3).c_str());
    return handle;
}

void Ended(const char* how, uint32_t handle)
{
    auto it = g_live.find(handle);
    if (it == g_live.end()) {
        RvkLog("fx: %s handle %u", how, handle);
        return;
    }
    RvkLog("fx: %s %u handle %u after %.1f s", how, it->second.templateId, handle, (GetTickCount() - it->second.made) / 1000.0);
    if (how[0] == 'd') g_live.erase(it);
}

void __fastcall TerminateHook(void* self, void* edx, uint32_t handle)
{
    if (LogOn()) Ended("terminate", handle);
    g_terminate(self, edx, handle);
}

void __fastcall DeleteHook(void* self, void* edx, uint32_t handle)
{
    if (LogOn()) Ended("delete", handle);
    g_delete(self, edx, handle);
}

bool Bytes(const uint8_t* at, std::initializer_list<int> expected)
{
    size_t i = 0;
    for (int b : expected) {
        if (at[i] != uint8_t(b))
            return false;
        ++i;
    }
    return true;
}

void Install()
{
    HMODULE gc = GetModuleHandleA("Gamecode.dll"), n3 = GetModuleHandleA("N3.dll");
    if (!gc || !n3)
        return;
    auto exported = [&](HMODULE m, const char* name) { return reinterpret_cast<uint8_t*>(GetProcAddress(m, name)); };
    uint8_t* make = exported(gc, "?MakeHandle@_EffectHandler_t@@IAEIPAV_GfxControl_t@@I@Z");
    uint8_t* terminate = exported(gc, "?TerminateEffectGracefully@_EffectHandler_t@@QAEXI@Z");
    uint8_t* del = exported(gc, "?DeleteEffect@_EffectHandler_t@@QAEXI@Z");
    g_handlerInstance = reinterpret_cast<InstanceFn>(exported(gc, "?GetInstance@_EffectHandler_t@@SAPAV1@XZ"));
    g_createOnDynel = reinterpret_cast<CreateOnDynelFn>(exported(gc, "?CreateEffect2@_EffectHandler_t@@QAEIHABVn3Dynel_t@@H@Z"));
    g_engineInstance = reinterpret_cast<InstanceFn>(exported(n3, "?GetInstance@n3EngineClient_t@@SAPAV1@XZ"));
    g_clientDynel = reinterpret_cast<DynelFn>(exported(n3, "?GetClientControlDynel@n3EngineClient_t@@QBEPAVn3VisualDynel_t@@XZ"));
    // MakeHandle: push ebp; mov ebp, esp; push ecx; push ecx; push esi; mov esi, ecx.
    // TerminateEffectGracefully: push ebp; mov ebp, esp; push ecx; push esi; mov esi, [ecx+18h].
    // DeleteEffect: push ebp; mov ebp, esp; push ecx; push ebx; push esi; push edi; mov ebx, ecx.
    g_gaveUp = true;
    if (!make || !terminate || !del || !Bytes(make, {0x55, 0x8B, 0xEC, 0x51, 0x51, 0x56, 0x8B, 0xF1}) ||
        !Bytes(terminate, {0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0x71, 0x18}) ||
        !Bytes(del, {0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x57, 0x8B, 0xD9})) {
        RvkLog("fx: Gamecode.dll's _EffectHandler_t not as expected (another build?) - effects not watched");
        return;
    }
    ReadOverloads(gc);
    g_makeHandle = reinterpret_cast<MakeHandleFn>(rnative::HookAt(make, 8, nullptr, 0, reinterpret_cast<void*>(&MakeHandleHook), "fx: MakeHandle"));
    g_terminate = reinterpret_cast<HandleFn>(rnative::HookAt(terminate, 8, nullptr, 0, reinterpret_cast<void*>(&TerminateHook), "fx: TerminateEffectGracefully"));
    g_delete = reinterpret_cast<HandleFn>(rnative::HookAt(del, 9, nullptr, 0, reinterpret_cast<void*>(&DeleteHook), "fx: DeleteEffect"));
    if (!g_makeHandle || !g_terminate || !g_delete) {
        RvkLog("fx: _EffectHandler_t hooks failed (make %p terminate %p delete %p)", (void*)g_makeHandle,
               (void*)g_terminate, (void*)g_delete);
        return;
    }
    g_gaveUp = false;
    g_installed = true;
    RvkLog("fx: _EffectHandler_t watched (%zu CreateEffect2 / CreateTracer overloads)", g_overloads.size());
    // The cast's update, for our effects in its place.
    if (HMODULE ds = GetModuleHandleA("DisplaySystem.dll"))
        g_disableRendering = reinterpret_cast<DisableRenderingFn>(GetProcAddress(ds, "?DisableRendering@GfxVisual@@QAEXH@Z"));
    void* update = nullptr;
    if (g_disableRendering && PatchSlot(gc, kSpellVtable, 1, kSpellUpdate, reinterpret_cast<void*>(&SpellProcessHook), &update,
                                        "fx: _GfxControlSpell1_t update")) {
        g_spellProcess = reinterpret_cast<ProcessFn>(update);
        const uintptr_t base = reinterpret_cast<uintptr_t>(gc);
        g_spellVtable = base + kSpellVtable;
        g_flareVtable = base + kFlareVtable;
        g_cordVtable = base + kCordVtable;
        RvkLog("fx: casts (_GfxControlSpell1_t) can be ours");
    }
}

void Play(int templateId)
{
    void* handler = g_handlerInstance ? g_handlerInstance() : nullptr;
    void* engine = g_engineInstance ? g_engineInstance() : nullptr;
    const void* dynel = engine && g_clientDynel ? g_clientDynel(engine, nullptr) : nullptr;
    if (!handler || !dynel || !g_createOnDynel) {
        RvkLog("fx: play %d: no effect handler or character yet", templateId);
        return;
    }
    const uint32_t handle = g_createOnDynel(handler, nullptr, templateId, dynel, 0);
    RvkLog("fx: play %d on the player -> handle %u", templateId, handle);
}

}  // namespace

void FxPlay(int templateId)
{
    g_playRequest.store(templateId, std::memory_order_relaxed);
}

void FxFrame()
{
    ++g_frame;
    if (!g_installed && !g_gaveUp)
        Install();
    if (!g_installed)
        return;
    if (g_frame % 60 == 1 || (g_rvk.device && g_sentSerial != g_rvk.deviceSerial))
        ScanEffects(false);
    if (g_frame % 120 == 0)                      // (casts the game has deleted: no update for a while)
        std::erase_if(g_casts, [](const auto& c) { return c.second.lastFrame + 30 < g_frame; });
    if (int id = g_playRequest.exchange(0, std::memory_order_relaxed)) Play(id);
}

void FxReload()
{
    ScanEffects(true);
}

}  // namespace rvkproxy
