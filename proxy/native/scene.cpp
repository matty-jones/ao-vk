// Which visual is drawing (scene.h).
#include "native/scene.h"
#include "native/cat.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rnative::scene {

namespace {

using RenderFn = void(__fastcall*)(void* viewport, void*, int listFrom, int listTo, int type, uint32_t from, uint32_t to);
RenderFn g_render, g_renderRefraction;
void* g_viewport;                                  // the viewport inside Render / RenderRefraction
void* const* g_renderInstance;                     // render_t::m_pcInstance (its +0x288: the render list being drawn)
uint32_t g_renders;                                // Render calls so far (cache refresh clock)
constexpr uint32_t kCurrentVisual = 0x164;         // RViewPort_t: the visual being rendered
constexpr uint32_t kParent = 0x14;                 // RRefFrame_t: its parent frame

bool g_active;                                     // Visuals on (hooked here, or driven by viewport.cpp)

void ForgetPages();

void* Enter(void* viewport)
{
    void* previous = g_viewport;
    g_viewport = viewport;
    ++g_renders;
    ForgetPages();
    return previous;
}

void __fastcall RenderHook(void* viewport, void*, int listFrom, int listTo, int type, uint32_t from, uint32_t to)
{
    void* previous = Enter(viewport);
    g_render(viewport, nullptr, listFrom, listTo, type, from, to);
    g_viewport = previous;
}

void __fastcall RenderRefractionHook(void* viewport, void*, int listFrom, int listTo, int type, uint32_t from,
                                     uint32_t to)
{
    void* previous = Enter(viewport);
    g_renderRefraction(viewport, nullptr, listFrom, listTo, type, from, to);
    g_viewport = previous;
}

// Reads through pointers that may be stale (Randy's light list can hold lights whose frames are gone) must never
// fault - not even inside __try: the game's crash handler sees every access violation first and ends the game. So
// memory is checked first (VirtualQuery). What it says is cached as the run of pages it reports in the same state (a
// heap segment: one query covers most of a frame's objects) - each query is a system call, and with a page at a time,
// cleared at each visual's description, the walks up visuals' frames made thousands a frame (~0.5 ms of the game
// thread at a lamp-lit outpost, 2026-10-20). Cleared at each scene update and each viewport's render: the stale reads
// (the lights' frames) come right after the scene update; what is described while drawing is alive.
struct RegionCache {
    uintptr_t begin[32], end[32];
    bool readable[32];
    int count = 0, next = 0;
} g_regions;
uint32_t g_memoryQueries = 0;                       // VirtualQuery calls (the scene update's log line)

void ForgetPages() { g_regions.count = 0; }

bool PageReadable(uintptr_t page)
{
    for (int i = 0; i < g_regions.count; ++i)
        if (g_regions.begin[i] <= page && page < g_regions.end[i]) return g_regions.readable[i];
    MEMORY_BASIC_INFORMATION mbi{};
    ++g_memoryQueries;
    const bool queried = VirtualQuery(reinterpret_cast<const void*>(page), &mbi, sizeof(mbi)) == sizeof(mbi);
    bool ok = queried && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
              (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    // (BaseAddress is the queried page's; RegionSize runs from it to the end of the pages in the same state.)
    const uintptr_t end = queried && mbi.RegionSize ? page + mbi.RegionSize : page + 0x1000;
    const int slot = g_regions.count < 32 ? g_regions.count++ : (g_regions.next++ & 31);
    g_regions.begin[slot] = page;
    g_regions.end[slot] = end > page ? end : page + 0x1000;
    g_regions.readable[slot] = ok;
    return ok;
}

bool Readable(const void* p, size_t bytes)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < 0x10000 || a + bytes < a) return false;
    for (uintptr_t page = a & ~uintptr_t(0xFFF); page < a + bytes; page += 0x1000)
        if (!PageReadable(page)) return false;
    return true;
}

// A pointer field of an object that may not be there any more: null if it isn't readable.
const void* SafeRead(const void* object, uint32_t offset)
{
    const void* at = static_cast<const uint8_t*>(object) + offset;
    if (!object || !Readable(at, sizeof(void*))) return nullptr;
    return *static_cast<const void* const*>(at);
}

// The RTTI type name of an object (".?AVName@@"), or null.
const char* RawTypeName(const void* object)
{
    const auto* vtable = static_cast<const uintptr_t*>(SafeRead(object, 0));
    if (!vtable || !Readable(vtable - 1, sizeof(uintptr_t))) return nullptr;
    const auto* col = reinterpret_cast<const uint32_t*>(vtable[-1]);   // Complete Object Locator
    if (!Readable(col, 16)) return nullptr;
    const char* name = reinterpret_cast<const char*>(col[3] + 8);       // TypeDescriptor: vftable, spare, name
    if (!Readable(name, 2) || name[0] != '.' || name[1] != '?') return nullptr;
    for (size_t n = 2; n < 256; ++n) {                                  // the whole name readable
        if (!Readable(name + n, 1)) return nullptr;
        if (name[n] == 0) return name;
    }
    return nullptr;
}

struct Cached {
    const void* vtable = nullptr;
    uint32_t stamp = 0;
    VisualInfo info;
    // Describe's walk up the frames to a character's attractors (AttachedOwner): its answer, while the attachments
    // (g_attachGeneration) and the visual's parent are as they were.
    uint32_t attachGeneration = ~0u;
    const void* parent = nullptr;
    const void* attachedOwner = nullptr;
};
std::unordered_map<const void*, Cached> g_visuals;
std::unordered_map<const void*, std::string> g_classNames;   // by vtable; never cleared (names are handed out)

const char* ClassName(const void* object)
{
    const void* vtable = SafeRead(object, 0);
    if (!vtable) return "";
    auto found = g_classNames.find(vtable);
    if (found != g_classNames.end())
        return found->second.c_str();
    std::string name;
    if (const char* raw = RawTypeName(object)) {
        name = raw + 4;                                 // ".?AV" / ".?AU"
        size_t end = name.find("@@");
        if (end != std::string::npos) name.resize(end);
    }
    return g_classNames.emplace(vtable, name).first->second.c_str();
}

bool StartsWith(const char* s, const char* prefix)
{
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

Kind KindOf(const char* name)
{
    if (!std::strcmp(name, "RCATMesh_t")) return Kind::Character;
    if (!std::strcmp(name, "AnarchyGround_t")) return Kind::Terrain;
    if (!std::strcmp(name, "VisualRoom_t")) return Kind::Room;
    if (!std::strcmp(name, "VisualLiquid_t")) return Kind::Water;
    if (StartsWith(name, "GfxVisualSimpleShadow")) return Kind::BlobShadow;
    if (StartsWith(name, "GfxVisual")) return Kind::Effect;
    if (std::strstr(name, "Sky")) return Kind::Sky;
    if (!std::strcmp(name, "RTriMesh_t") || !std::strcmp(name, "GenericMeshObject_t") || !std::strcmp(name, "SimpleMesh"))
        return Kind::Static;
    return name[0] ? Kind::Other : Kind::Unknown;
}

// The character (RCATMesh_t frame) somewhere up an object's parents, or null.
const void* CharacterAbove(const void* object)
{
    const void* frame = SafeRead(object, kParent);
    for (int depth = 0; frame && depth < 12; ++depth, frame = SafeRead(frame, kParent))
        if (!std::strcmp(ClassName(frame), "RCATMesh_t"))
            return frame;
    return nullptr;
}

// The character carrying a light: one up its parents, or one next to it under its parent (a character's visual hangs
// its light on its own frame) - only under a small parent (not a zone's root, which may hold characters too).
constexpr uint32_t kFirstChild = 0x1C, kNextSibling = 0x18;   // RRefFrame_t children list

// What hangs on characters' attractors (a held weapon, a shield, a back item): RCATMesh_t::AddAttractorChild keeps
// those frames in the character's own map (std::map<std::string, std::vector<RRefFrame_t*>>, RCATMesh_t +0x1E8;
// cat_life.cpp) and ProcessAttractorChilds moves them to the hand each frame - they are no children of the character
// in the scene's frames (a held weapon was taken for a prop: swayed as a plant, leaves, ao-vk #12). Indexed when a
// character is drawn: frame -> the character's visual (RCATMesh_t's RVisual_t, at +0x3C in it).
constexpr uint32_t kAttachedMap = 0x1E8 - 0x3C;   // the map, from the visual: allocator, head, size
constexpr uint32_t kNodeName = 0x0C, kNodeFrames = 0x28, kNodeIsHead = 0x39;
std::unordered_map<const void*, const void*> g_attachedTo;   // attractor child frame -> character visual
struct AttachedFrames {
    uint32_t stamp = 0;                          // g_renders when indexed
    std::vector<const void*> frames;
};
std::unordered_map<const void*, AttachedFrames> g_attachments;   // character visual -> its frames
uint32_t g_attachGeneration = 0;                // g_attachedTo changed (Describe's cached walks)

void IndexAttachments(const void* character)
{
    AttachedFrames& a = g_attachments[character];
    if (a.stamp == g_renders && !a.frames.empty())
        return;                                  // (once a render)
    a.stamp = g_renders;
    // The frames read again; the index changed only if they differ from last time (most renders: the same).
    static std::vector<const void*> frames;
    frames.clear();
    const auto apply = [&] {
        if (frames == a.frames)
            return;
        for (const void* f : a.frames) {
            auto it = g_attachedTo.find(f);
            if (it != g_attachedTo.end() && it->second == character) g_attachedTo.erase(it);
        }
        for (const void* f : frames) g_attachedTo[f] = character;
        a.frames = frames;
        ++g_attachGeneration;
    };
    const void* head = SafeRead(character, kAttachedMap + 4);
    if (!head) { apply(); return; }
    // In-order walk of the red-black tree (VS2010: head->parent = root; the head node and nil leaves are the head).
    const void* stack[64];
    int depth = 0, nodes = 0;
    const void* n = SafeRead(head, 4);
    static int logged = 0;
    while ((n && n != head) || depth > 0) {
        while (n && n != head && depth < 64 && Readable(static_cast<const uint8_t*>(n) + kNodeIsHead, 1) &&
               !static_cast<const uint8_t*>(n)[kNodeIsHead]) {
            stack[depth++] = n;
            n = SafeRead(n, 0);                  // left
        }
        if (depth == 0 || ++nodes > 64) break;
        n = stack[--depth];
        const auto* first = static_cast<const void* const*>(SafeRead(n, kNodeFrames));
        const auto* last = static_cast<const void* const*>(SafeRead(n, kNodeFrames + 4));
        if (first && last >= first && last - first <= 16 && Readable(first, size_t(last - first) * sizeof(void*)))
            for (const void* const* f = first; f < last; ++f)
                if (*f) {
                    frames.push_back(*f);
                    if (logged < 8) {
                        ++logged;
                        // (the name: a VS2010 std::string - inline below 16 characters)
                        const auto* s = static_cast<const uint8_t*>(n) + kNodeName;
                        const uint32_t cap = Readable(s + 0x14, 4) ? *reinterpret_cast<const uint32_t*>(s + 0x14) : 0;
                        const char* name = cap < 16 ? reinterpret_cast<const char*>(s)
                                                    : static_cast<const char*>(SafeRead(n, kNodeName));
                        Log("visuals: attractor \"%.31s\" holds %s", name && Readable(name, 32) ? name : "?", ClassName(*f));
                    }
                }
        n = SafeRead(n, 8);                      // right
    }
    apply();
    // Characters not drawn for a while: their frames forgotten.
    if ((g_renders & 1023) == 0)
        for (auto it = g_attachments.begin(); it != g_attachments.end();) {
            if (g_renders - it->second.stamp > 2000) {
                for (const void* f : it->second.frames) {
                    auto at = g_attachedTo.find(f);
                    if (at != g_attachedTo.end() && at->second == it->first) g_attachedTo.erase(at);
                }
                it = g_attachments.erase(it);
                ++g_attachGeneration;
            } else {
                ++it;
            }
        }
}

// Frame dump diagnostics (NoteDumpRequested): until this render, visuals' frame chains and the attractor index logged.
uint32_t g_chainLogUntil = 0;
std::unordered_set<const void*> g_chainLogged;

void LogChain(const void* visual)
{
    if (!g_chainLogged.insert(visual).second || g_chainLogged.size() > 4000)
        return;
    std::string line;
    char part[96];
    const void* frame = visual;
    for (int depth = 0; frame && depth < 12; ++depth, frame = SafeRead(frame, kParent)) {
        std::snprintf(part, sizeof(part), "%s%08X %s%s", depth ? " < " : "", unsigned(reinterpret_cast<uintptr_t>(frame)),
                      ClassName(frame), g_attachedTo.count(frame) ? " [attractor child]" : "");
        line += part;
    }
    Log("dump chain: %s", line.c_str());
}

void LogFrameTree(const void* frame, int depth, std::string indent)
{
    for (const void* child = SafeRead(frame, kFirstChild); child && depth < 3; child = SafeRead(child, kNextSibling)) {
        Log("dump attractor tree: %s%08X %s", indent.c_str(), unsigned(reinterpret_cast<uintptr_t>(child)), ClassName(child));
        LogFrameTree(child, depth + 1, indent + "  ");
    }
}

// The character an object hangs on by an attractor: it, or a frame up its parents, is one's attractor child.
const void* AttachedOwner(const void* object)
{
    if (g_attachedTo.empty())
        return nullptr;
    const void* frame = object;
    for (int depth = 0; frame && depth < 12; ++depth, frame = SafeRead(frame, kParent)) {
        auto it = g_attachedTo.find(frame);
        if (it != g_attachedTo.end())
            return it->second;
    }
    return nullptr;
}
const void* CarrierOf(const void* light)
{
    if (const void* character = CharacterAbove(light))
        return character;
    const void* parent = SafeRead(light, kParent);
    if (!parent)
        return nullptr;
    const void* found = nullptr;
    int children = 0;
    for (const void* child = SafeRead(parent, kFirstChild); child && children < 64;
         child = SafeRead(child, kNextSibling), ++children)
        if (!found && !std::strcmp(ClassName(child), "RCATMesh_t"))
            found = child;
    return children <= 8 ? found : nullptr;
}

// ---- lights ----
using ProcessFn = void(__fastcall*)(void* viewport, void*, void* root);
ProcessFn g_process;
LightSinkFn g_lightSink;
const void* const* g_lightsBegin;                  // randy31 0x1017D290: std::vector<RLight_t*> begin, end
constexpr uint32_t kLightListRva = 0x17D290;
constexpr uint32_t kLightData = 0xA4;              // RLight_t: its D3DLIGHT7 (world space after Process)

void Lights();

void __fastcall ProcessHook(void* viewport, void*, void* root)
{
    g_process(viewport, nullptr, root);
    Lights();
}

void Lights()
{
    ForgetPages();
    static uint32_t updates = 0;
    if (++updates == 600) {                      // (the readable checks' cost: RegionCache)
        Log("visuals: %.1f memory queries a scene update (avg of 600)", double(g_memoryQueries) / 600.0);
        updates = 0;
        g_memoryQueries = 0;
    }
    if (!g_lightSink)
        return;
    static std::vector<SceneLight> lights;
    lights.clear();
    const void* const* begin = g_lightsBegin[0] ? static_cast<const void* const*>(g_lightsBegin[0]) : nullptr;
    const void* const* end = static_cast<const void* const*>(g_lightsBegin[1]);
    for (const void* const* it = begin; it && it != end && lights.size() < 1024; ++it) {
        SceneLight l;
        std::memcpy(l.d3dLight, static_cast<const uint8_t*>(*it) + kLightData, sizeof(l.d3dLight));
        l.owner = CarrierOf(*it);
        lights.push_back(l);
    }
    g_lightSink(lights.data(), lights.size());
}

}  // namespace

void SetLightSink(LightSinkFn sink)
{
    g_lightSink = sink;
}

const void* CurrentVisual()
{
    return g_viewport ? At<const void*>(g_viewport, kCurrentVisual) : nullptr;
}

int32_t CurrentList()
{
    return g_viewport && g_renderInstance && *g_renderInstance ? At<int32_t>(*g_renderInstance, 0x288) : -1;
}

const char* ClassChain(const void* visual)
{
    static std::unordered_map<const void*, std::pair<const void*, const char*>> cache;   // visual -> (vtable, chain)
    static std::unordered_set<std::string> chains;                                      // never cleared: handed out
    if (!visual)
        return "";
    const void* vtable = SafeRead(visual, 0);
    auto found = cache.find(visual);
    if (found != cache.end() && found->second.first == vtable)
        return found->second.second;
    std::string chain = ClassName(visual);
    const void* frame = SafeRead(visual, kParent);
    for (int depth = 0; frame && depth < 4; ++depth, frame = SafeRead(frame, kParent)) {
        chain += '<';
        chain += ClassName(frame);
    }
    const char* out = chains.insert(chain).first->c_str();
    if (cache.size() > 20000)
        cache.clear();
    cache[visual] = {vtable, out};
    return out;
}

void NoteDumpRequested()
{
    g_chainLogUntil = g_renders + 60;            // (several renders a frame: past the dumped one)
    g_chainLogged.clear();
    for (const auto& [character, a] : g_attachments)
        for (const void* f : a.frames) {
            const void* parent = SafeRead(f, kParent);
            Log("dump attractor child: %08X %s of character %08X; parent %08X %s", unsigned(reinterpret_cast<uintptr_t>(f)),
                ClassName(f), unsigned(reinterpret_cast<uintptr_t>(character)), unsigned(reinterpret_cast<uintptr_t>(parent)),
                ClassName(parent));
            LogFrameTree(f, 0, "  ");
        }
}

VisualInfo Describe(const void* visual)
{
    if (!visual)
        return {};
    if (g_renders <= g_chainLogUntil)
        LogChain(visual);
    const void* vtable = *static_cast<const void* const*>(visual);
    Cached& c = g_visuals[visual];
    if (c.vtable != vtable || g_renders - c.stamp >= 2000) {
        c.vtable = vtable;
        c.stamp = g_renders;
        c.info.className = ClassName(visual);
        c.info.kind = KindOf(c.info.className);
        // Attached to a character (somewhere up its frames): one of its parts.
        c.info.owner = c.info.kind == Kind::Character ? visual : nullptr;
        if (c.info.kind != Kind::Character && c.info.kind != Kind::BlobShadow)
            if (const void* character = CharacterAbove(visual)) {
                c.info.kind = Kind::CharacterPart;
                c.info.owner = character;
            }
    }
    VisualInfo info = c.info;
    // What hangs on a character's attractors - looked up again whenever the attachments change: a part may be drawn
    // before its character is (heads are), and weapons are put in and out of hands.
    if (info.kind == Kind::Character) {
        IndexAttachments(visual);
    } else if (info.kind != Kind::CharacterPart && info.kind != Kind::BlobShadow) {
        const void* parent = SafeRead(visual, kParent);
        if (c.attachGeneration != g_attachGeneration || c.parent != parent) {
            c.attachGeneration = g_attachGeneration;
            c.parent = parent;
            c.attachedOwner = AttachedOwner(visual);
        }
        if (c.attachedOwner) {
            info.kind = Kind::CharacterPart;
            info.owner = c.attachedOwner;
        }
    }
    if (g_visuals.size() > 20000)                       // visuals come and go: start over now and then
        g_visuals.clear();                              // (c is gone from here on)
    return info;
}

bool Installed()
{
    return g_active;
}

void* EnterRender(void* viewport) { return g_active ? Enter(viewport) : nullptr; }

void LeaveRender(void* previous)
{
    if (g_active) g_viewport = previous;
}

void AfterProcess()
{
    if (g_active) Lights();
}

void Install(HMODULE orig)
{
    if (GetMode("Visuals", Mode::On) != Mode::On)
        return;
    g_renderInstance = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    g_lightsBegin = reinterpret_cast<const void* const*>(reinterpret_cast<uint8_t*>(orig) + kLightListRva);
    if (GetMode("Scene", Mode::On) == Mode::On) {  // viewport.cpp's native Render / Process call in
        g_active = true;
        Log("visuals: on, lights from the scene (through the native viewport)");
        return;
    }
    // RViewPort_t::Render: mov eax, <handler> (absolute); call _EH_prolog (relative).
    static const uint8_t kRender[] = {0xB8, 0xBC, 0x78, 0x08, 0x10, 0xE8, 0xB7, 0xC7, 0x02, 0x00};
    static const size_t kRenderAbs[] = {1}, kRenderRel[] = {6};
    HookFixups fixups;
    fixups.abs32 = kRenderAbs;
    fixups.abs32Count = 1;
    fixups.rel32 = kRenderRel;
    fixups.rel32Count = 1;
    g_render = reinterpret_cast<RenderFn>(HookEntry(orig, 0x4BFFF, kRender, sizeof(kRender),
                                                    reinterpret_cast<void*>(&RenderHook), "RViewPort_t::Render", fixups));
    static const uint8_t kRefraction[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};   // push ebp; mov ebp, esp; sub esp, 10h
    g_renderRefraction = reinterpret_cast<RenderFn>(
        HookEntry(orig, 0x4C4EA, kRefraction, sizeof(kRefraction), reinterpret_cast<void*>(&RenderRefractionHook),
                  "RViewPort_t::RenderRefraction"));
    // RViewPort_t::Process: push ebp; mov ebp, esp; push ecx; fld dword [<constant>] (absolute).
    static const uint8_t kProcess[] = {0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x05, 0x54, 0x3F, 0x09, 0x10};
    static const size_t kProcessAbs[] = {6};
    HookFixups processFixups;
    processFixups.abs32 = kProcessAbs;
    processFixups.abs32Count = 1;
    g_process = reinterpret_cast<ProcessFn>(HookEntry(orig, 0x4BF39, kProcess, sizeof(kProcess),
                                                      reinterpret_cast<void*>(&ProcessHook), "RViewPort_t::Process",
                                                      processFixups));
    g_active = g_render != nullptr;
    Log("visuals: %s%s", g_render && g_renderRefraction ? "on" : "not installed (unknown client build)",
        g_process ? ", lights from the scene" : "");
}

}  // namespace rnative::scene
