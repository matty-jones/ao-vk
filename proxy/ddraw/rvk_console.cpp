// The console (docs/console-plan.md): the key left of 1 opens and closes it (by position, any layout; Esc closes it
// too). It sets the renderer's settings by name - applied at once and saved, as from the settings window - and runs
// the hotkeys' actions.
//
// - Input: the game window's messages come here first (ConsoleMessage, from rvk_input.cpp's window procedure, on the
//   window's thread): the open key always, and while open every key press and typed character, queued for the next
//   present. Meanwhile the game gets no keyboard input (InputBlock); the mouse stays the game's.
// - Each present (ConsoleFrame, on the presenting thread with the hotkeys): the queue into the line, commands run,
//   and - when something changed - the console drawn into an image the renderer puts over the next frame
//   (Device::SetConsoleImage). The text: a monospace system font, each character drawn once with GDI into a coverage
//   atlas.
// - History: Up / Down; kept in randy-vk-console.txt next to randy-vk.ini (the last kHistoryMax).
// - Completion: as you type, the best match as grey text after the caret and the candidates under the line; Tab
//   takes the first, Tab again the next.
// randy-vk.ini [Console] Key=<scan code> (default 41 = 0x29; 0: no console).
// Tests: RANDYVK_CONSOLE_TEST=<frame>:<keys> types `keys` from that presented frame on - {open} (the console's key),
// {tab}, {stab} (Shift+Tab), {enter}, {up}, {down}, {left}, {right}, {esc}, {bs}, {pgup}, other characters typed - and
// {shot} saves the console's image as console-test-<n>.bmp next to the log; one piece between {shot}s a frame.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rvk_backend.h"
#include "rvk_settings.h"

namespace rvkproxy {

namespace {

// ---------------------------------------------------------------- input from the window's thread
struct Event {
    enum Kind { Toggle, Key, Char } kind;
    unsigned code;                               // Key: virtual key; Char: the character (the window's code page)
    bool ctrl, shift;
};
std::mutex g_queueLock;
std::vector<Event> g_queue;
std::atomic<bool> g_open{false};                 // (read by the window's thread)
std::atomic<unsigned> g_scan{0x29};              // the open key's scan code; 0: none

void Push(const Event& e)
{
    std::lock_guard<std::mutex> lock(g_queueLock);
    if (g_queue.size() < 256) g_queue.push_back(e);
}

// ---------------------------------------------------------------- the font: a coverage atlas of characters 32..255
struct Font {
    int px = 0;                                  // the size it was made for (the character height asked)
    int cw = 0, ch = 0;                          // cell
    std::vector<uint8_t> cov;                    // 224 cells of cw x ch, one after another
    char face[64] = "";
};

const char* const kFaces[] = {"Consolas", "DejaVu Sans Mono", "Liberation Mono", "Lucida Console", "Courier New"};

bool BuildFont(Font& f, int px)
{
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = nullptr;
    for (const char* name : kFaces) {
        HFONT h = CreateFontA(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, name);
        if (!h) continue;
        HGDIOBJ old = SelectObject(dc, h);
        char got[64] = "";
        GetTextFaceA(dc, sizeof(got), got);
        SelectObject(dc, old);
        if (_stricmp(got, name) == 0) {          // (GDI substitutes a missing face silently)
            font = h;
            break;
        }
        DeleteObject(h);
    }
    if (!font)
        font = CreateFontA(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, nullptr);
    HGDIOBJ oldFont = SelectObject(dc, font);
    GetTextFaceA(dc, sizeof(f.face), f.face);
    TEXTMETRICA tm{};
    GetTextMetricsA(dc, &tm);
    SIZE m{};
    GetTextExtentPoint32A(dc, "M", 1, &m);
    f.cw = std::max<int>(1, m.cx);
    f.ch = std::max<int>(1, tm.tmHeight);
    const int w = f.cw * 16, h = f.ch * 14;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = bmp && bits;
    if (ok) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        std::memset(bits, 0, size_t(w) * h * 4);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < 224; ++i) {
            const char c = char(32 + i);
            TextOutA(dc, (i % 16) * f.cw, (i / 16) * f.ch, &c, 1);
        }
        GdiFlush();
        f.cov.assign(size_t(224) * f.cw * f.ch, 0);
        const uint8_t* src = static_cast<const uint8_t*>(bits);
        for (int i = 0; i < 224; ++i)
            for (int y = 0; y < f.ch; ++y)
                for (int x = 0; x < f.cw; ++x) {
                    const uint8_t* p = src + (size_t((i / 16) * f.ch + y) * w + (i % 16) * f.cw + x) * 4;
                    f.cov[(size_t(i) * f.ch + y) * f.cw + x] = std::max({p[0], p[1], p[2]});
                }
        SelectObject(dc, oldBmp);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    f.px = px;
    return ok;
}

// ---------------------------------------------------------------- colours, and text that carries them
// A byte 1..kColours in a line switches its colour to that one (kColour[n - 1]) for the characters after it.
enum Colour : char { cText = 1, cDim, cValue, cWarn, cError, cName, cGhost, cPrompt };
constexpr int kColours = 8;
const float kColour[kColours][3] = {
    {0.86f, 0.88f, 0.90f},   // text
    {0.52f, 0.55f, 0.60f},   // dim
    {0.44f, 0.77f, 0.82f},   // value (the settings tab's #70C4D0)
    {0.95f, 0.78f, 0.35f},   // warning
    {0.96f, 0.45f, 0.40f},   // error
    {1.00f, 1.00f, 1.00f},   // a name
    {0.40f, 0.43f, 0.48f},   // ghost (the completion's rest)
    {0.44f, 0.77f, 0.82f},   // prompt
};

std::string C(Colour c) { return std::string(1, char(c)); }

size_t Visible(const std::string& s)
{
    size_t n = 0;
    for (char c : s) n += uint8_t(c) > kColours;
    return n;
}

// ---------------------------------------------------------------- state (the presenting thread)
constexpr size_t kHistoryMax = 200, kScrollbackMax = 600;
constexpr int kListMax = 8;

struct Candidate {
    std::string text;                            // what replaces the word
    std::string hint;                            // shown beside it (with colours)
    int rank = 0;
    bool more = false;                           // takes arguments: a space after it when taken
};

struct State {
    bool loaded = false;
    std::string line;
    size_t caret = 0;
    std::vector<std::string> history;
    int historyPos = -1;                         // -1: the line being typed (draft)
    std::string draft;
    std::deque<std::string> out;                 // scrollback, oldest first
    int scroll = 0;                              // lines up from the newest
    int cols = 120;                              // characters across (the last layout)
    // Completion: the current word's candidates (refreshed as the line changes); Tab cycling through them.
    std::vector<Candidate> list;
    std::string hint;                            // one line under the input instead of a list (a number's range)
    bool cycling = false;
    int selected = -1;
    size_t wordStart = 0;                        // where the word being completed starts
    // Drawing.
    bool dirty = true, shown = false;            // shown: an image is with the renderer
    float slide = 0.0f;                          // 0 closed .. 1 open
    LARGE_INTEGER last{};
    bool blinkOn = true;
    ULONGLONG blinkAt = 0;
    Font font;
    std::vector<uint32_t> pixels;
    uint32_t width = 0, height = 0;
    // try: values in effect but not saved (name -> the value tried).
    std::map<std::string, float> tried;
    // bisect: the settings still suspected of a fault RVK_Enhance off takes away, and the half of them at the game's
    // value now (step: which step).
    std::vector<std::string> suspects;
    size_t bisectHalf = 0;
    int bisectStep = 0;
    bool charsSeen = false;                      // a WM_CHAR came (if keys come without: the game doesn't translate)
    int keysWithoutChars = 0;
} g;

void Print(const std::string& text)
{
    // Wrapped at the console's width - at the last space if there is one - the colour carried onto the next piece.
    const size_t cols = size_t(std::max(20, g.cols));
    std::string piece;
    size_t n = 0, space = std::string::npos;     // space: the last one's place in piece
    char colour = 0, colourAtSpace = 0;
    for (char c : text) {
        if (c == '\n') {
            g.out.push_back(piece);
            piece = colour ? std::string(1, colour) : std::string();
            n = 0;
            space = std::string::npos;
            continue;
        }
        if (uint8_t(c) <= kColours) {
            colour = c;
            piece += c;
            continue;
        }
        if (n == cols) {
            std::string rest;
            char restColour = colour;
            if (space != std::string::npos && space > 4) {
                rest = piece.substr(space + 1);
                piece.erase(space);
                restColour = colourAtSpace;
            }
            g.out.push_back(piece);
            piece = (restColour ? std::string(1, restColour) : std::string()) + "  " + rest;
            n = Visible(piece);
            space = std::string::npos;
        }
        if (c == ' ') {
            space = piece.size();
            colourAtSpace = colour;
        }
        piece += c;
        ++n;
    }
    g.out.push_back(piece);
    while (g.out.size() > kScrollbackMax) g.out.pop_front();
    g.scroll = 0;
    g.dirty = true;
}

void Printf(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Print(buf);
}

std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool IEquals(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

bool IStarts(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && _strnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

std::vector<std::string> Words(const std::string& s)
{
    std::vector<std::string> w;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '=') {
            if (!cur.empty()) w.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

// ---------------------------------------------------------------- history file
std::string HistoryPath()
{
    std::string p = rvk_settings::IniPath();
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "randy-vk-console.txt";
}

void LoadHistory()
{
    if (FILE* f = std::fopen(HistoryPath().c_str(), "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) g.history.push_back(s);
        }
        std::fclose(f);
    }
    if (g.history.size() > kHistoryMax) g.history.erase(g.history.begin(), g.history.end() - kHistoryMax);
}

void SaveHistory()
{
    if (FILE* f = std::fopen(HistoryPath().c_str(), "w")) {
        for (const std::string& s : g.history) std::fprintf(f, "%s\n", s.c_str());
        std::fclose(f);
    }
}

// ---------------------------------------------------------------- the settings, by the console's names
struct SettingRef {
    uint32_t index;
    RvkSettingInfo info;
    bool hidden;
    const char* shortName;                       // the name without RVK_
};

const char* Short(const char* name) { return _strnicmp(name, "RVK_", 4) == 0 ? name + 4 : name; }

std::vector<SettingRef> AllSettings()
{
    std::vector<SettingRef> v;
    const uint32_t n = rvk_settings::Count();
    v.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        SettingRef r{i, {}, false, nullptr};
        if (rvk_settings::Info(i, &r.info, &r.hidden)) {
            r.shortName = Short(r.info.name);
            v.push_back(r);
        }
    }
    return v;
}

bool FindSetting(const std::string& word, SettingRef* out)
{
    const char* w = Short(word.c_str());
    for (const SettingRef& r : AllSettings())
        if (_stricmp(r.shortName, w) == 0) {
            *out = r;
            return true;
        }
    return false;
}

std::string FormatValue(const RvkSettingInfo& s, float v)
{
    char buf[48];
    if (s.type == 0) return v != 0.0f ? "on" : "off";
    if (s.type == 2) std::snprintf(buf, sizeof(buf), "%g", double(std::round(v * 1000.0f) / 1000.0f));
    else if (s.type == 4) std::snprintf(buf, sizeof(buf), "%06X", unsigned(std::lround(v)) & 0xFFFFFFu);
    else std::snprintf(buf, sizeof(buf), "%d", int(std::lround(v)));
    return buf;
}

std::string Range(const RvkSettingInfo& s)
{
    if (s.type == 0) return "on / off";
    if (s.type == 3 && s.choices) return std::string("one of ") + s.choices;
    if (s.type == 4) return "a colour, RRGGBB";
    char buf[64];
    if (s.type == 2) std::snprintf(buf, sizeof(buf), "%g .. %g", double(s.min), double(s.max));
    else std::snprintf(buf, sizeof(buf), "%d .. %d", int(s.min), int(s.max));
    return buf;
}

// Why a setting has no effect now (empty: it has): its feature switched off, or every enhancement.
std::string NoEffect(const RvkSettingInfo& s)
{
    if (s.parent && rvk_settings::Get(s.parent) == 0.0f)
        return std::string(Short(s.parent)) + " is off";
    if (std::strcmp(s.section, "General") != 0 && std::strcmp(s.section, "Debug") != 0 &&
        rvk_settings::Get("RVK_Enhance") == 0.0f)
        return "all enhancements are off (Enhance)";
    return "";
}

// A setting's label with its feature's in front ("Bloom - Strength"), where it has one.
std::string Label(const RvkSettingInfo& s)
{
    SettingRef parent;
    if (s.parent && FindSetting(s.parent, &parent) && parent.info.label)
        return std::string(parent.info.label) + " - " + s.label;
    return s.label;
}

// "Bloom = 0.6   0 .. 2, default 0.6   Bloom strength [Effects]   [no cost]"
std::string Describe(const SettingRef& r)
{
    const RvkSettingInfo& s = r.info;
    std::string t = C(cName) + r.shortName + C(cText) + " = " + C(cValue) + FormatValue(s, s.value);
    auto tried = g.tried.find(s.name);
    if (tried != g.tried.end())
        t += C(cWarn) + " (trying " + FormatValue(s, tried->second) + ", not saved)";
    t += C(cDim) + "   " + Range(s) + ", default " + FormatValue(s, s.defaultValue) + "   " + C(cText) + Label(s) + C(cDim) +
         " [" + s.section + (r.hidden ? ", randy-vk.ini only" : "") + "]";
    if (s.cost) t += C(cDim) + "   [" + s.cost + "]";
    const std::string why = NoEffect(s);
    if (!why.empty()) t += C(cWarn) + "   no effect: " + why;
    return t;
}

// The value typed for a setting: a number, on / off (yes / no, true / false), toggle, reset / default.
bool ParseValue(const RvkSettingInfo& s, const std::string& word, float* out)
{
    const std::string w = Lower(word);
    if (w == "on" || w == "true" || w == "yes") { *out = 1.0f; return true; }
    if (w == "off" || w == "false" || w == "no") { *out = s.type == 0 ? 0.0f : s.min; return true; }
    if (w == "toggle") { *out = s.value != 0.0f ? 0.0f : 1.0f; return s.type == 0; }
    if (w == "reset" || w == "default") { *out = s.defaultValue; return true; }
    if (s.type == 4) {                            // a colour: RRGGBB, # or 0x before it allowed
        const char* h = w.c_str();
        if (*h == '#') ++h;
        else if (h[0] == '0' && h[1] == 'x') h += 2;
        char* hexEnd = nullptr;
        const unsigned long v = std::strtoul(h, &hexEnd, 16);
        if (hexEnd == h || *hexEnd || hexEnd - h > 6) return false;
        *out = float(v);
        return true;
    }
    char* end = nullptr;
    const float v = std::strtof(w.c_str(), &end);
    if (end == w.c_str() || *end || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

// ---------------------------------------------------------------- commands
enum Arg { aNone, aSetting, aSection, aPreset, aSkyPreset, aCommand, aText, aAction };
struct Command {
    const char* name;
    const char* args;
    const char* help;
    Arg arg;
};
const Command kCommands[] = {
    {"help", "[command]", "what the console does, or one command", aCommand},
    {"find", "<words>", "the settings whose name or label has the words", aText},
    {"list", "[section]", "a section's settings (none: the sections)", aSection},
    {"changed", "", "the settings that differ from their defaults", aNone},
    {"reset", "<setting>", "a setting back to its default", aSetting},
    {"toggle", "<setting>", "an on / off setting switched", aSetting},
    {"try", "<setting> <value>", "a value in effect but not saved (gone at the next start, or with untry)", aSetting},
    {"bisect", "[gone|still|stop]", "find which setting causes a fault enhance off takes away: half of them at the game's value at a time, nothing saved", aText},
    {"untry", "[setting]", "the saved value back (none: every tried one)", aSetting},
    {"preset", "[name]", "a preset applied (none: the presets)", aPreset},
    {"skypreset", "[name]", "the sky's colours from a preset (none: the presets)", aSkyPreset},
    {"hideui", "", "hide / show the game's interface (Ctrl+Shift+H)", aAction},
    {"freecam", "", "the free camera on / off (Ctrl+Shift+C)", aAction},
    {"dump", "", "the next frame's draws + a screenshot next to the log (Ctrl+Shift+F9)", aAction},
    {"profile", "", "the profiling sweep: what each enhancement costs here (Ctrl+Shift+O)", aAction},
    {"luts", "", "reload the colour look-up tables (Ctrl+Shift+L)", aAction},
    {"lightdebug", "", "the lighting debug view on / off (Ctrl+Shift+F11)", aAction},
    {"clear", "", "empty the console", aNone},
};

const Command* FindCommand(const std::string& word)
{
    for (const Command& c : kCommands)
        if (IEquals(word, c.name)) return &c;
    return nullptr;
}

std::vector<std::string> Sections()
{
    std::vector<std::string> v;
    for (const SettingRef& r : AllSettings())
        if (std::find(v.begin(), v.end(), r.info.section) == v.end()) v.push_back(r.info.section);
    return v;
}

void SetValue(const SettingRef& r, float v)
{
    const RvkSettingInfo& s = r.info;
    if (g.tried.erase(s.name)) rvk_settings::Restore(s.name);
    const float before = s.value;
    rvk_settings::Set(s.name, v);
    SettingRef now;
    if (!FindSetting(s.name, &now)) return;
    std::string t = C(cName) + r.shortName + C(cText) + " = " + C(cValue) + FormatValue(now.info, now.info.value);
    if (now.info.value == before) t += C(cDim) + " (unchanged)";
    else t += C(cDim) + " (was " + FormatValue(s, before) + ")";
    if (std::fabs(now.info.value - v) > 1e-4f && !(s.type == 0 && (v != 0.0f) == (now.info.value != 0.0f)))
        t += C(cWarn) + "   kept to " + Range(s);
    const std::string why = NoEffect(now.info);
    if (!why.empty()) t += C(cWarn) + "   no effect while " + why;
    Print(t);
}

// bisect: a fault RVK_Enhance off takes away - which of the settings it returns to the game's value is it? Half the
// suspects at the game's value (together, nothing saved), the player says whether the fault is gone, the half that
// answers stays suspected - about seven steps for the hundred or so. (Each step the saved values first, then the half.)
void BisectApply()
{
    rvk_settings::RestoreAll();
    g.bisectHalf = (g.suspects.size() + 1) / 2;
    std::vector<std::pair<std::string, float>> values;
    for (size_t i = 0; i < g.bisectHalf; ++i)
        for (uint32_t k = 0; k < rvk_settings::VanillaCount(); ++k)
            if (g.suspects[i] == rvk_settings::VanillaName(k)) values.push_back({g.suspects[i], rvk_settings::VanillaValue(k)});
    rvk_settings::ApplyTemporarySet(values);
    ++g.bisectStep;
    std::string names;
    for (size_t i = 0; i < g.bisectHalf && i < 6; ++i) names += (i ? ", " : "") + g.suspects[i];
    if (g.bisectHalf > 6) names += ", ...";
    Print(C(cText) + "bisect step " + std::to_string(g.bisectStep) + ": " + C(cValue) + std::to_string(g.bisectHalf) +
          C(cText) + " of " + C(cValue) + std::to_string(g.suspects.size()) + C(cText) + " suspects at the game's value (" +
          C(cDim) + names + C(cText) + "). Is the fault there? " + C(cName) + "bisect gone" + C(cText) + " / " + C(cName) +
          "bisect still");
    RvkLog("console: bisect step %d: %u of %u suspects at the game's value", g.bisectStep, unsigned(g.bisectHalf),
           unsigned(g.suspects.size()));
    for (size_t i = 0; i < g.bisectHalf; ++i) RvkLog("console: bisect   %s", g.suspects[i].c_str());
}

void BisectStop(const std::string& why)
{
    g.suspects.clear();
    g.bisectHalf = 0;
    g.bisectStep = 0;
    rvk_settings::RestoreAll();
    Print(C(cText) + why + C(cDim) + " (every saved value back)");
}

void Bisect(const std::string& answer)
{
    if (answer == "stop") {
        BisectStop("bisect stopped");
        return;
    }
    if (answer.empty() || answer == "start") {
        g.suspects.clear();
        g.bisectStep = 0;
        for (uint32_t k = 0; k < rvk_settings::VanillaCount(); ++k)   // (only those not at the game's value already)
            if (std::fabs(rvk_settings::Get(rvk_settings::VanillaName(k)) - rvk_settings::VanillaValue(k)) > 1e-4f)
                g.suspects.push_back(rvk_settings::VanillaName(k));
        if (g.suspects.empty()) {
            Print(C(cDim) + "nothing to bisect: every enhancement is at the game's value already");
            return;
        }
        Print(C(cText) + "bisect: " + C(cValue) + std::to_string(g.suspects.size()) + C(cText) +
              " suspects. Get the fault showing first; after each step look, then answer.");
        BisectApply();
        return;
    }
    if (g.suspects.empty()) {
        Print(C(cError) + "no bisect going: " + C(cName) + "bisect" + C(cText) + " starts one");
        return;
    }
    if (answer == "gone") {
        g.suspects.resize(g.bisectHalf);           // (it's in the half at the game's value)
    } else if (answer == "still") {
        g.suspects.erase(g.suspects.begin(), g.suspects.begin() + std::ptrdiff_t(g.bisectHalf));
        if (g.suspects.empty()) {
            BisectStop("bisect: not one of them alone - two together, or not a setting");
            return;
        }
    } else {
        Print(C(cError) + "bisect gone" + C(cText) + " / " + C(cError) + "bisect still" + C(cText) + " / " + C(cError) +
              "bisect stop");
        return;
    }
    if (g.suspects.size() == 1) {
        const std::string found = g.suspects[0];
        rvk_settings::RestoreAll();
        for (uint32_t k = 0; k < rvk_settings::VanillaCount(); ++k)
            if (found == rvk_settings::VanillaName(k))
                rvk_settings::ApplyTemporarySet({{found, rvk_settings::VanillaValue(k)}});
        Print(C(cText) + "bisect: it is " + C(cName) + found + C(cText) + " - at the game's value now, the rest as saved: "
              "is the fault gone? (" + C(cName) + "bisect stop" + C(cText) + " puts it back)");
        RvkLog("console: bisect found %s", found.c_str());
        g.bisectHalf = 0;
        return;
    }
    BisectApply();
}

void TryValue(const SettingRef& r, float v)
{
    const RvkSettingInfo& s = r.info;
    v = std::clamp(v, s.min, s.max);
    if (s.type != 2) v = std::round(v);
    rvk_settings::ApplyTemporary(s.name, v);
    g.tried[s.name] = v;
    Print(C(cName) + r.shortName + C(cText) + " = " + C(cValue) + FormatValue(s, v) + C(cWarn) +
          " (trying - not saved; saved: " + FormatValue(s, s.value) + ")");
}

std::vector<Candidate> Complete(const std::string& line, size_t* wordStart, std::string* hint);

void Suggest(const std::string& word)
{
    size_t start;
    std::string hint;
    std::vector<Candidate> c = Complete(word, &start, &hint);
    if (c.empty()) return;
    std::string t = C(cDim) + "did you mean: ";
    for (size_t i = 0; i < c.size() && i < 5; ++i) t += (i ? C(cDim) + ", " : "") + C(cName) + c[i].text;
    Print(t);
}

void Help(const std::string& what)
{
    if (const Command* c = what.empty() ? nullptr : FindCommand(what)) {
        Print(C(cName) + c->name + " " + C(cValue) + c->args + C(cText) + "   " + c->help);
        return;
    }
    Print(C(cText) + "Type a setting's name (" + C(cName) + "bloom" + C(cText) + ", " + C(cName) + "RVK_Bloom" + C(cText) +
          ") to see it, and a value after it to set it: " + C(cName) + "bloom 1.2" + C(cText) + ", " + C(cName) +
          "pbr off" + C(cText) + ", " + C(cName) + "pbr toggle" + C(cText) + ", " + C(cName) + "bloom reset" + C(cText) +
          ". Changes apply at once and are saved to randy-vk.ini.");
    Print(C(cDim) + "Tab completes, Up / Down go through what you typed before, PgUp / PgDn scroll, Esc or the key "
                    "left of 1 closes.");
    for (const Command& c : kCommands)
        Print("  " + C(cName) + c.name + " " + C(cValue) + c.args + C(cDim) + "   " + c.help);
}

void List(const std::vector<SettingRef>& v, size_t limit = 80)
{
    for (size_t i = 0; i < v.size() && i < limit; ++i) Print("  " + Describe(v[i]));
    if (v.size() > limit) Printf("%c  ... %u more", cDim, unsigned(v.size() - limit));
}

void Execute(const std::string& text)
{
    Print(C(cPrompt) + "> " + C(cDim) + text);
    std::vector<std::string> w = Words(text);
    if (w.empty()) return;
    const std::string cmd = Lower(w[0]);
    SettingRef r;
    if (cmd == "help" || cmd == "?") {
        Help(w.size() > 1 ? w[1] : "");
    } else if (cmd == "clear") {
        g.out.clear();
        g.scroll = 0;
    } else if (cmd == "find") {
        if (w.size() < 2) { Print(C(cError) + "find what? " + C(cDim) + "find shadow"); return; }
        std::vector<SettingRef> hits;
        for (const SettingRef& s : AllSettings()) {
            const std::string hay = Lower(std::string(s.info.name) + " " + s.info.label + " " + s.info.section);
            bool all = true;
            for (size_t i = 1; i < w.size() && all; ++i) all = hay.find(Lower(w[i])) != std::string::npos;
            if (all) hits.push_back(s);
        }
        if (hits.empty()) Print(C(cDim) + "no setting has that");
        List(hits);
    } else if (cmd == "list") {
        if (w.size() < 2) {
            std::string t = C(cText) + "sections: ";
            bool first = true;
            for (const std::string& s : Sections()) {
                t += (first ? "" : C(cDim) + ", ") + C(cName) + s;
                first = false;
            }
            Print(t);
            return;
        }
        std::vector<SettingRef> hits;
        for (const SettingRef& s : AllSettings())
            if (IStarts(s.info.section, w[1])) hits.push_back(s);
        if (hits.empty()) Print(C(cError) + "no section " + w[1]);
        List(hits, 200);
    } else if (cmd == "changed") {
        std::vector<SettingRef> hits;
        for (const SettingRef& s : AllSettings())
            if (std::fabs(s.info.value - s.info.defaultValue) > 1e-4f) hits.push_back(s);
        if (hits.empty()) Print(C(cDim) + "everything is at its default");
        List(hits, 200);
    } else if (cmd == "reset" || cmd == "toggle") {
        if (w.size() < 2 || !FindSetting(w[1], &r)) {
            Print(C(cError) + (w.size() < 2 ? cmd + " which setting?" : "no setting " + w[1]));
            if (w.size() >= 2) Suggest(w[1]);
            return;
        }
        if (cmd == "toggle" && r.info.type != 0) {
            Print(C(cError) + r.shortName + " isn't on / off: " + Range(r.info));
            return;
        }
        SetValue(r, cmd == "reset" ? r.info.defaultValue : (r.info.value != 0.0f ? 0.0f : 1.0f));
    } else if (cmd == "try") {
        float v;
        if (w.size() < 3 || !FindSetting(w[1], &r) || !ParseValue(r.info, w[2], &v)) {
            Print(C(cError) + "try <setting> <value>" + C(cDim) + "   try bloom 2");
            return;
        }
        TryValue(r, v);
    } else if (cmd == "bisect") {
        Bisect(w.size() >= 2 ? Lower(w[1]) : std::string());
    } else if (cmd == "untry") {
        std::vector<std::string> names;
        if (w.size() >= 2) {
            if (!FindSetting(w[1], &r) || !g.tried.count(r.info.name)) {
                Print(C(cDim) + "nothing tried for " + w[1]);
                return;
            }
            names.push_back(r.info.name);
        } else {
            for (const auto& t : g.tried) names.push_back(t.first);
        }
        if (names.empty()) Print(C(cDim) + "nothing tried");
        for (const std::string& n : names) {
            g.tried.erase(n);
            rvk_settings::Restore(n.c_str());
            if (FindSetting(n, &r)) Print(C(cName) + r.shortName + C(cText) + " = " + C(cValue) +
                                          FormatValue(r.info, r.info.value) + C(cDim) + " (saved value back)");
        }
    } else if (cmd == "preset") {
        const uint32_t n = rvk_settings::PresetCount();
        if (w.size() < 2) {
            const int current = rvk_settings::PresetCurrent();
            std::string t = C(cText) + "presets: ";
            for (uint32_t i = 0; i < n; ++i)
                t += (i ? C(cDim) + ", " : "") + (int(i) == current ? C(cValue) : C(cName)) + rvk_settings::PresetName(i);
            t += C(cDim) + (current < 0 ? "   (now: your own)" : "   (now: " + std::string(rvk_settings::PresetName(current)) + ")");
            Print(t);
            return;
        }
        std::string want = Lower(text.substr(text.find_first_of(" \t") + 1));
        while (!want.empty() && want.back() == ' ') want.pop_back();
        for (uint32_t i = 0; i < n; ++i)
            if (IStarts(rvk_settings::PresetName(i), want)) {
                g.tried.clear();
                rvk_settings::PresetApply(i);
                Print(C(cText) + "preset " + C(cValue) + rvk_settings::PresetName(i) + C(cText) + " applied");
                return;
            }
        Print(C(cError) + "no preset " + want);
    } else if (cmd == "skypreset") {
        const uint32_t n = rvk_settings::SkyPresetCount();
        if (w.size() < 2) {
            const int current = rvk_settings::SkyPresetCurrent();
            std::string t = C(cText) + "sky presets: ";
            for (uint32_t i = 0; i < n; ++i)
                t += (i ? C(cDim) + ", " : "") + (int(i) == current ? C(cValue) : C(cName)) + rvk_settings::SkyPresetName(i);
            t += C(cDim) + (current < 0 ? "   (now: your own colours)" : "   (now: " + std::string(rvk_settings::SkyPresetName(current)) + ")");
            Print(t);
            return;
        }
        const std::string want = Lower(w[1]);
        for (uint32_t i = 0; i < n; ++i)
            if (IStarts(rvk_settings::SkyPresetName(i), want)) {
                rvk_settings::SkyPresetApply(i);
                Print(C(cText) + "sky preset " + C(cValue) + rvk_settings::SkyPresetName(i) + C(cText) +
                      " applied");
                return;
            }
        Print(C(cError) + "no sky preset " + want);
    } else if (const Command* c = FindCommand(cmd); c && c->arg == aAction) {
        RunAction(c->name);
        Print(C(cDim) + c->help);
    } else if (FindSetting(w[0], &r)) {
        if (w.size() == 1) {
            Print(Describe(r));
            return;
        }
        float v;
        if (!ParseValue(r.info, w[1], &v)) {
            Print(C(cError) + "not a value for " + r.shortName + ": " + w[1] + C(cDim) + "   " + Range(r.info));
            return;
        }
        if (Lower(w[1]) == "toggle" && r.info.type != 0) {
            Print(C(cError) + r.shortName + " isn't on / off: " + Range(r.info));
            return;
        }
        // "off" for a strength: its feature's on / off switch off - its lowest value isn't off (gi off left the
        // indirect light at 0.25, ptshadows off the shadows at one - user, 2026-10-19).
        const std::string word = Lower(w[1]);
        SettingRef parent;
        if ((word == "off" || word == "no" || word == "false") && r.info.type != 0 && r.info.parent &&
            FindSetting(r.info.parent, &parent) && parent.info.type == 0) {
            SetValue(parent, 0.0f);
            return;
        }
        if ((word == "off" || word == "no" || word == "false") && r.info.type != 0 && r.info.min > 0.0f)
            Print(C(cWarn) + r.shortName + " has no off: set to its lowest, " + FormatValue(r.info, v));
        SetValue(r, v);
    } else {
        Print(C(cError) + "no setting or command " + w[0] + C(cDim) + "   (help: what there is)");
        Suggest(w[0]);
    }
}

// ---------------------------------------------------------------- completion
// The words of a CamelCase name: "PbrSpec" -> Pbr, Spec; "MBlurObj" -> M, Blur, Obj.
bool WordPrefix(const char* name, const std::string& prefix)
{
    for (size_t i = 1; name[i]; ++i) {
        const bool start = std::isupper(uint8_t(name[i])) || (std::isdigit(uint8_t(name[i])) && !std::isdigit(uint8_t(name[i - 1])));
        if (start && _strnicmp(name + i, prefix.c_str(), prefix.size()) == 0) return true;
    }
    return false;
}

// How well `name` (and its label) match what was typed: 0 its start, 1 a word inside it, 2 anywhere in it, 3 a word
// of the label; -1 no match.
int Rank(const char* name, const char* label, const std::string& typed)
{
    if (typed.empty()) return 0;
    if (_strnicmp(name, typed.c_str(), typed.size()) == 0) return 0;
    if (WordPrefix(name, typed)) return 1;
    if (Lower(name).find(Lower(typed)) != std::string::npos) return 2;
    if (label && typed.size() >= 3) {
        const std::string l = Lower(label), t = Lower(typed);
        for (size_t p = l.find(t); p != std::string::npos; p = l.find(t, p + 1))
            if (p == 0 || !std::isalnum(uint8_t(l[p - 1]))) return 3;
    }
    return -1;
}

std::string SettingHint(const SettingRef& r)
{
    return C(cValue) + FormatValue(r.info, r.info.value) + C(cDim) + "  " + Label(r.info);
}

void AddSettings(std::vector<Candidate>& out, const std::string& typed)
{
    for (const SettingRef& r : AllSettings()) {
        const int rank = Rank(r.shortName, r.info.label, typed);
        if (rank >= 0) out.push_back({r.shortName, SettingHint(r), rank, true});
    }
}

void Sort(std::vector<Candidate>& c)
{
    std::stable_sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.text.size() != b.text.size()) return a.text.size() < b.text.size();
        return _stricmp(a.text.c_str(), b.text.c_str()) < 0;
    });
}

// The candidates for the word the line ends in (wordStart: where it starts); hint: a line to show instead when there
// is nothing to pick from (a number's range).
std::vector<Candidate> Complete(const std::string& line, size_t* wordStart, std::string* hint)
{
    std::vector<Candidate> out;
    hint->clear();
    size_t start = line.find_last_of(" \t=");
    start = start == std::string::npos ? 0 : start + 1;
    *wordStart = start;
    const std::string typed = line.substr(start);
    std::vector<std::string> before = Words(line.substr(0, start));
    if (before.empty()) {
        if (typed.empty()) return out;
        AddSettings(out, typed);
        for (const Command& c : kCommands) {
            const int rank = Rank(c.name, nullptr, typed);
            if (rank >= 0 && rank < 2)
                out.push_back({c.name, C(cDim) + c.args + (c.args[0] ? "  " : "") + c.help, rank, c.arg != aNone && c.arg != aAction});
        }
        Sort(out);
        return out;
    }
    const Command* cmd = FindCommand(before[0]);
    SettingRef r;
    // A setting's value: after "<setting>" or "try <setting>".
    const bool valueOf = (!cmd && before.size() == 1 && FindSetting(before[0], &r)) ||
                         (cmd && std::strcmp(cmd->name, "try") == 0 && before.size() == 2 && FindSetting(before[1], &r));
    if (valueOf) {
        std::vector<std::string> values;
        if (r.info.type == 0) values = {"on", "off", "toggle"};
        if (r.info.type == 3 && r.info.choices)
            values = Words(r.info.choices);
        values.push_back("reset");
        for (const std::string& v : values)
            if (IStarts(v, typed)) {
                float f;
                std::string h;
                if (v == "reset") h = C(cDim) + "default " + FormatValue(r.info, r.info.defaultValue);
                else if (r.info.type == 3 && ParseValue(r.info, v, &f) && std::fabs(f - r.info.value) < 1e-4f) h = C(cDim) + "now";
                out.push_back({v, h, 0, false});
            }
        if (r.info.type == 1 || r.info.type == 2)
            *hint = C(cDim) + Range(r.info) + ", now " + C(cValue) + FormatValue(r.info, r.info.value) + C(cDim) +
                    ", default " + FormatValue(r.info, r.info.defaultValue);
        if (r.info.type == 1 || r.info.type == 2) {
            if (!typed.empty() && !IStarts("reset", typed)) out.clear();
            if (typed.empty()) out.clear();      // (a number: the range says it better)
        }
        return out;
    }
    if (!cmd || before.size() != 1) return out;
    switch (cmd->arg) {
    case aSetting:
        if (std::strcmp(cmd->name, "untry") == 0) {
            for (const auto& t : g.tried) {
                const char* s = Short(t.first.c_str());
                if (IStarts(s, typed)) out.push_back({s, "", 0, false});
            }
        } else {
            AddSettings(out, typed);
            if (std::strcmp(cmd->name, "toggle") == 0)
                out.erase(std::remove_if(out.begin(), out.end(), [](const Candidate& c) {
                              SettingRef s;
                              return !FindSetting(c.text, &s) || s.info.type != 0;
                          }), out.end());
            for (Candidate& c : out) c.more = std::strcmp(cmd->name, "try") == 0;
        }
        break;
    case aSection:
        for (const std::string& s : Sections())
            if (IStarts(s, typed)) out.push_back({s, "", 0, false});
        break;
    case aPreset:
        for (uint32_t i = 0; i < rvk_settings::PresetCount(); ++i)
            if (IStarts(rvk_settings::PresetName(i), typed)) out.push_back({rvk_settings::PresetName(i), "", 0, false});
        break;
    case aSkyPreset:
        for (uint32_t i = 0; i < rvk_settings::SkyPresetCount(); ++i)
            if (IStarts(rvk_settings::SkyPresetName(i), typed)) out.push_back({rvk_settings::SkyPresetName(i), "", 0, false});
        break;
    case aCommand:
        for (const Command& c : kCommands)
            if (IStarts(c.name, typed)) out.push_back({c.name, C(cDim) + c.help, 0, false});
        break;
    default:
        break;
    }
    Sort(out);
    return out;
}

void Refresh()
{
    g.cycling = false;
    g.selected = -1;
    g.list = g.caret == g.line.size() ? Complete(g.line, &g.wordStart, &g.hint) : std::vector<Candidate>();
    if (g.caret != g.line.size()) g.hint.clear();
    // (Only the word itself typed: nothing to add.)
    if (g.list.size() == 1 && _stricmp(g.list[0].text.c_str(), g.line.substr(g.wordStart).c_str()) == 0 && !g.list[0].more)
        g.list.clear();
    g.dirty = true;
}

// The rest of the first candidate when the word is its start (grey after the caret); empty otherwise.
std::string Ghost()
{
    if (g.cycling || g.list.empty() || g.caret != g.line.size()) return "";
    const std::string typed = g.line.substr(g.wordStart);
    const Candidate& c = g.list[0];
    if (typed.empty() || !IStarts(c.text, typed) || c.text.size() == typed.size()) return "";
    return c.text.substr(typed.size());
}

void Take(int i)
{
    const Candidate& c = g.list[size_t(i)];
    g.line = g.line.substr(0, g.wordStart) + c.text + (c.more && !g.cycling ? " " : "");
    g.caret = g.line.size();
}

void TabKey(bool back)
{
    if (g.list.empty()) return;
    if (!g.cycling && g.list.size() == 1) {      // the only one: taken, and on to its arguments
        Take(0);
        Refresh();
        return;
    }
    const int n = int(std::min<size_t>(g.list.size(), kListMax * 4));
    g.selected = !g.cycling ? (back ? n - 1 : 0) : (g.selected + (back ? n - 1 : 1)) % n;
    g.cycling = true;
    Take(g.selected);
    g.dirty = true;
}

// ---------------------------------------------------------------- keys
void Edited()
{
    g.historyPos = -1;
    Refresh();
}

size_t WordLeft(size_t p)
{
    while (p > 0 && g.line[p - 1] == ' ') --p;
    while (p > 0 && g.line[p - 1] != ' ') --p;
    return p;
}

size_t WordRight(size_t p)
{
    while (p < g.line.size() && g.line[p] == ' ') ++p;
    while (p < g.line.size() && g.line[p] != ' ') ++p;
    return p;
}

void Open(bool open);

void KeyDown(const Event& e)
{
    g.blinkOn = true;
    g.blinkAt = GetTickCount64() + 530;
    switch (e.code) {
    case VK_ESCAPE:
        Open(false);
        return;
    case VK_RETURN: {
        const std::string text = g.line;
        g.line.clear();
        g.caret = 0;
        g.draft.clear();
        g.historyPos = -1;
        if (!Words(text).empty()) {
            if (g.history.empty() || g.history.back() != text) {
                g.history.push_back(text);
                if (g.history.size() > kHistoryMax) g.history.erase(g.history.begin());
                SaveHistory();
            }
            Execute(text);
        }
        Refresh();
        return;
    }
    case VK_TAB:
        TabKey(e.shift);
        return;
    case VK_BACK:
        if (g.caret > 0) {
            const size_t from = e.ctrl ? WordLeft(g.caret) : g.caret - 1;
            g.line.erase(from, g.caret - from);
            g.caret = from;
            Edited();
        }
        return;
    case VK_DELETE:
        if (g.caret < g.line.size()) {
            const size_t to = e.ctrl ? WordRight(g.caret) : g.caret + 1;
            g.line.erase(g.caret, to - g.caret);
            Edited();
        }
        return;
    case VK_LEFT:
        g.caret = e.ctrl ? WordLeft(g.caret) : (g.caret ? g.caret - 1 : 0);
        Refresh();
        return;
    case VK_RIGHT: {
        if (g.caret == g.line.size()) {          // at the end: the grey rest taken
            const std::string ghost = Ghost();
            if (!ghost.empty()) {
                g.line += ghost;
                g.caret = g.line.size();
                Edited();
            }
            return;
        }
        g.caret = e.ctrl ? WordRight(g.caret) : g.caret + 1;
        Refresh();
        return;
    }
    case VK_HOME:
        g.caret = 0;
        Refresh();
        return;
    case VK_END:
        g.caret = g.line.size();
        Refresh();
        return;
    case VK_UP:
    case VK_DOWN: {
        if (g.history.empty()) return;
        const int n = int(g.history.size());
        int pos = g.historyPos;
        if (e.code == VK_UP) {
            if (pos == -1) { g.draft = g.line; pos = n - 1; }
            else if (pos > 0) --pos;
        } else {
            if (pos == -1) return;
            pos = pos + 1 < n ? pos + 1 : -1;
        }
        g.historyPos = pos;
        g.line = pos == -1 ? g.draft : g.history[size_t(pos)];
        g.caret = g.line.size();
        const int keep = g.historyPos;
        Refresh();
        g.historyPos = keep;
        g.list.clear();                          // (going through the history: no candidates in the way)
        g.hint.clear();
        return;
    }
    case VK_PRIOR:
    case VK_NEXT: {
        const int page = 8;
        g.scroll = std::clamp(g.scroll + (e.code == VK_PRIOR ? page : -page), 0, std::max(0, int(g.out.size()) - 1));
        g.dirty = true;
        return;
    }
    default:
        break;
    }
    if (!e.ctrl) return;
    switch (e.code) {
    case 'A': g.caret = 0; Refresh(); break;
    case 'E': g.caret = g.line.size(); Refresh(); break;
    case 'U': g.line.erase(0, g.caret); g.caret = 0; Edited(); break;
    case 'K': g.line.erase(g.caret); Edited(); break;
    case 'L': g.out.clear(); g.scroll = 0; g.dirty = true; break;
    case 'V':
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_TEXT)) {
                if (const char* text = static_cast<const char*>(GlobalLock(h))) {
                    std::string t;
                    for (const char* c = text; *c && t.size() < 400; ++c)
                        if (uint8_t(*c) >= 32) t += *c;
                        else if (*c == '\t') t += ' ';
                    g.line.insert(g.caret, t);
                    g.caret += t.size();
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            Edited();
        }
        break;
    default:
        break;
    }
}

void Typed(char c)
{
    if (uint8_t(c) < 32 || c == 127 || g.line.size() >= 400) return;
    if (c == ' ' && g.cycling) g.cycling = false;
    g.line.insert(g.caret, 1, c);
    ++g.caret;
    Edited();
}

void Open(bool open)
{
    if (g_open == open) return;
    g_open = open;
    InputBlock(kInputConsole, open, false);
    if (open) {
        if (!g.loaded) {
            g.loaded = true;
            LoadHistory();
            Print(C(cText) + "randy-vk console. " + C(cDim) + "Type " + C(cName) + "help" + C(cDim) +
                  " for what it does; Tab completes, Up / Down: history.");
        }
        g.blinkOn = true;
        g.blinkAt = GetTickCount64() + 530;
        Refresh();
    }
    g.dirty = true;
}

// ---------------------------------------------------------------- drawing
inline uint32_t Pack(float r, float gr, float b, float a)   // premultiplied, A8R8G8B8
{
    auto u = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (u(a) << 24) | (u(r * a) << 16) | (u(gr * a) << 8) | u(b * a);
}

void Fill(int x0, int y0, int x1, int y1, float r, float gr, float b, float a)
{
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, int(g.width));
    y1 = std::min(y1, int(g.height));
    const uint32_t c = Pack(r, gr, b, a);
    for (int y = y0; y < y1; ++y)
        std::fill(g.pixels.begin() + size_t(y) * g.width + x0, g.pixels.begin() + size_t(y) * g.width + x1, c);
}

void Glyph(int x, int y, uint8_t c, const float rgb[3])
{
    if (c < 32) return;
    const Font& f = g.font;
    const uint8_t* cov = &f.cov[size_t(c - 32) * f.cw * f.ch];
    for (int j = 0; j < f.ch; ++j) {
        const int py = y + j;
        if (py < 0 || py >= int(g.height)) continue;
        uint32_t* row = &g.pixels[size_t(py) * g.width];
        for (int i = 0; i < f.cw; ++i) {
            const int px = x + i;
            const uint32_t k = cov[j * f.cw + i];
            if (!k || px < 0 || px >= int(g.width)) continue;
            const uint32_t d = row[px], inv = 255 - k;
            auto ch = [&](int shift, float v) {
                const uint32_t dst = (d >> shift) & 255;
                return (std::min<uint32_t>(255, (uint32_t(v * 255.0f) * k + dst * inv + 127) / 255)) << shift;
            };
            const uint32_t da = d >> 24;
            row[px] = (std::min<uint32_t>(255, k + (da * inv + 127) / 255) << 24) | ch(16, rgb[0]) | ch(8, rgb[1]) |
                      ch(0, rgb[2]);
        }
    }
}

// Text with colour bytes from (x, y), at most `cols` characters; returns the x after it.
int Text(int x, int y, const std::string& s, Colour start = cText, int cols = 1 << 20)
{
    const float* rgb = kColour[start - 1];
    int n = 0;
    for (char c : s) {
        if (uint8_t(c) <= kColours) {
            if (c) rgb = kColour[uint8_t(c) - 1];
            continue;
        }
        if (n++ >= cols) break;
        Glyph(x, y, uint8_t(c), rgb);
        x += g.font.cw;
    }
    return x;
}

// The font for a main target of w x h, and how many characters go across.
void Metrics(uint32_t w, uint32_t h)
{
    const int px = std::clamp(int(std::lround(h / 56.0)), 14, 34);
    if (g.font.px != px) BuildFont(g.font, px);
    g.cols = std::max(20, (int(w) - std::max(4, g.font.ch / 2) * 2) / std::max(1, g.font.cw));
}

// The console's image for a main target of w x h (strip + room for the candidates under it), slid in by g.slide.
void Draw(uint32_t w, uint32_t h)
{
    Metrics(w, h);
    const int cw = g.font.cw, ch = g.font.ch, pad = std::max(4, ch / 2);
    const int strip = std::max(ch * 8, int(h * 0.36f));
    const int listRows = std::min<int>(kListMax, int(g.list.size())) + (g.list.size() > size_t(kListMax) ? 1 : 0);
    const int below = (listRows || !g.hint.empty()) ? (std::max(listRows, 1) * ch + pad * 2) : 0;
    g.width = w;
    g.height = std::min<uint32_t>(h, uint32_t(strip + below + 2));
    g.pixels.assign(size_t(g.width) * g.height, 0);
    const float ease = 1.0f - (1.0f - g.slide) * (1.0f - g.slide);
    const int top = -int(std::lround((1.0f - ease) * float(strip + below)));   // slid up off the screen
    // The strip.
    Fill(0, top, int(w), top + strip, 0.035f, 0.045f, 0.06f, 0.86f);
    Fill(0, top + strip - 2, int(w), top + strip, 0.44f, 0.77f, 0.82f, 0.55f);
    const int inputY = top + strip - pad - ch;
    Fill(pad, inputY - pad / 2 - 1, int(w) - pad, inputY - pad / 2, 0.5f, 0.55f, 0.6f, 0.25f);
    // The scrollback, newest at the bottom.
    const int rows = std::max(1, (inputY - pad / 2 - pad - top) / ch);
    const int newest = int(g.out.size()) - 1 - g.scroll;
    for (int i = 0; i < rows && newest - i >= 0; ++i)
        Text(pad, inputY - pad - (i + 1) * ch, g.out[size_t(newest - i)], cText, g.cols);
    if (g.scroll > 0) {
        const std::string more = "-- " + std::to_string(g.scroll) + " more below (PgDn) --";
        Text(int(w) - pad - int(more.size()) * cw, inputY - pad - ch, C(cDim) + more);
    }
    // The input line: "> " and the text (its end shown if longer than the strip), grey completion, the caret.
    const int x0 = Text(pad, inputY, C(cPrompt) + "> ");
    const int fit = std::max(4, g.cols - 3);
    size_t first = g.caret > size_t(fit) - 1 ? g.caret - size_t(fit) + 1 : 0;
    const std::string shown = g.line.substr(first, size_t(fit));
    int x = Text(x0, inputY, shown, cText);
    const std::string ghost = Ghost();
    if (!ghost.empty()) Text(x, inputY, ghost, cGhost, std::max(0, fit - int(shown.size())));
    if (g.blinkOn) {
        const int cx = x0 + int(g.caret - first) * cw;
        Fill(cx, inputY + 1, cx + std::max(2, cw / 6), inputY + ch - 1, 0.9f, 0.95f, 1.0f, 0.9f);
    }
    // Under the strip: the candidates (the picked one marked) or the hint.
    if (below) {
        const int y0 = top + strip, bx = x0 + int(g.wordStart >= first ? g.wordStart - first : 0) * cw;
        size_t widest = Visible(g.hint);
        for (int i = 0; i < listRows && i < int(g.list.size()); ++i)
            widest = std::max(widest, g.list[size_t(i)].text.size() + 2 + Visible(g.list[size_t(i)].hint));
        widest = std::min<size_t>(widest, size_t(std::max(10, (int(w) - bx) / cw - 2)));
        const int bw = int(widest) * cw + pad * 2;
        Fill(bx - pad, y0, bx - pad + bw, y0 + below - pad, 0.035f, 0.045f, 0.06f, 0.92f);
        if (!g.hint.empty() && g.list.empty()) {
            Text(bx, y0 + pad / 2, g.hint, cDim, int(widest));
        } else {
            // A window of kListMax round the picked one.
            const int n = int(g.list.size());
            const int from = g.selected >= kListMax ? g.selected - kListMax + 1 : 0;
            for (int i = 0; i < kListMax && from + i < n; ++i) {
                const Candidate& c = g.list[size_t(from + i)];
                const int y = y0 + pad / 2 + i * ch;
                if (from + i == g.selected) Fill(bx - pad / 2, y, bx - pad - pad / 2 + bw, y + ch, 0.18f, 0.27f, 0.34f, 0.95f);
                const int hx = Text(bx, y, C(from + i == g.selected ? cName : cText) + c.text, cText, int(widest));
                if (!c.hint.empty()) Text(hx + cw * 2, y, c.hint, cDim, int(widest) - int(c.text.size()) - 2);
            }
            if (n > kListMax)
                Text(bx, y0 + pad / 2 + kListMax * ch, C(cDim) + "... " + std::to_string(n - kListMax) + " more (Tab)");
        }
    }
}

// RVK_PerfHud: a small readout in the top left, redrawn four times a second into the console's image while the
// console is closed (it has the image while open). Each thread's time is its frame less its waiting - the game thread
// for the render thread, the render thread for the GPU - so the largest of the three is what holds the frame back.
// The GPU's share of the time is its frame times the frame rate: how busy ao-vk keeps it (a 32-bit process can't ask
// the driver - NVIDIA's NVML is 64-bit only, and under Wine not there at all - so other programs' GPU work isn't in
// it). The CPU's: the whole machine's busy time (GetSystemTimes) and the game's own, both over all its cores.
struct Hud {
    bool shown = false;                          // the readout's image is with the renderer
    uint32_t frames = 0;
    LARGE_INTEGER since{};
    uint64_t presented = 0;                      // frame generation's frames shown, at `since`
    uint64_t idle = 0, kernel = 0, user = 0;     // the machine's CPU times at `since` (100 ns; kernel includes idle)
    uint64_t game = 0;                           // the game process's (kernel + user)
} g_hud;

uint64_t Ticks(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// The CPU's busy share since the last call, the machine's and the game's (0..100; -1: not known yet).
void CpuUse(double elapsed, double* machine, double* game)
{
    *machine = *game = -1.0;
    FILETIME idle, kernel, user, created, exited, pKernel, pUser;
    if (!GetSystemTimes(&idle, &kernel, &user) ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &pKernel, &pUser))
        return;
    const uint64_t i = Ticks(idle), k = Ticks(kernel), u = Ticks(user), gm = Ticks(pKernel) + Ticks(pUser);
    if (g_hud.kernel) {
        const uint64_t total = (k - g_hud.kernel) + (u - g_hud.user);
        if (total > 0) *machine = 100.0 * (1.0 - double(i - g_hud.idle) / double(total));
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const double wall = elapsed * 1e7 * std::max<DWORD>(si.dwNumberOfProcessors, 1);
        if (wall > 0.0) *game = 100.0 * double(gm - g_hud.game) / wall;
    }
    g_hud.idle = i;
    g_hud.kernel = k;
    g_hud.user = u;
    g_hud.game = gm;
}

void HudFrame(rvk::ThreadedDevice* device)
{
    if (rvk_settings::Get("RVK_PerfHud") == 0.0f) {
        if (g_hud.shown) device->SetConsoleImage(0, 0, nullptr);
        g_hud = Hud{};
        return;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    ++g_hud.frames;
    if (!g_hud.since.QuadPart) {
        g_hud.since = now;
        return;
    }
    const double elapsed = double(now.QuadPart - g_hud.since.QuadPart) / double(freq.QuadPart);
    if (elapsed < 0.25 && g_hud.shown) return;
    const double fps = (g_hud.frames - 1) / std::max(elapsed, 1e-3);
    double cpu, cpuGame;
    CpuUse(elapsed, &cpu, &cpuGame);
    g_hud.frames = 1;
    g_hud.since = now;
    const uint32_t w = device->Width(), h = device->Height();
    if (!w || !h) return;
    const rvk::ThreadedDevice::LiveTimes t = device->Live();
    // With frame generation (RVK_FrameGen): the frames shown, the generated ones with the rendered.
    const double shownFps = t.shown > g_hud.presented && g_hud.presented ? (t.shown - g_hud.presented) / std::max(elapsed, 1e-3) : 0.0;
    g_hud.presented = t.shown;
    const float game = std::max(0.0f, t.frameMs - t.gameWaitMs), render = std::max(0.0f, t.renderMs - t.gpuWaitMs);
    const char* limit = t.gpuMs > 0.0f && t.gpuMs >= game && t.gpuMs >= render ? "GPU" : game >= render ? "game" : "render";
    // (A time of 0: not measured - the renderer's profiler off.)
    const auto ms = [](char* out, size_t size, const char* name, float v) {
        if (v > 0.0f) std::snprintf(out, size, "%-6s %5.2f ms", name, v);
        else std::snprintf(out, size, "%-6s     - ms", name);
    };
    char lines[8][48];
    std::snprintf(lines[0], sizeof(lines[0]), "%4.0f fps  %5.2f ms", fps, fps > 0.0 ? 1000.0 / fps : 0.0);
    ms(lines[1], sizeof(lines[1]), "game", game);
    ms(lines[2], sizeof(lines[2]), "render", t.renderMs > 0.0f ? render : 0.0f);
    ms(lines[3], sizeof(lines[3]), "GPU", t.gpuMs);
    if (t.gpuMs > 0.0f && fps > 0.0) {           // (its share of the time: busy ms a second)
        const size_t at = std::strlen(lines[3]);
        std::snprintf(lines[3] + at, sizeof(lines[3]) - at, " %3.0f%%", std::min(100.0, t.gpuMs * fps / 10.0));
    }
    if (cpu >= 0.0)
        std::snprintf(lines[4], sizeof(lines[4]), "CPU    %3.0f%%  game %3.0f%%", cpu, std::max(0.0, cpuGame));
    else
        std::snprintf(lines[4], sizeof(lines[4]), "CPU      -");
    // Client memory: how much of the game's address space is taken (a 32-bit program: 4 GB at most, and a zone's load
    // needs a large free block - long sessions that ran it out ended the game). Video memory: against the driver's budget.
    // (From the regions, rvk LatestAddressSpace: under Wine GlobalMemoryStatusEx said 93% with a gigabyte still free.)
    const rvk::AddressSpace space = rvk::LatestAddressSpace();
    const bool memKnown = space.total > 0 && space.free <= space.total;
    const uint64_t memFree = memKnown ? space.free : 0;
    const bool memLow = memKnown && memFree < (512ull << 20);
    if (memKnown)
        std::snprintf(lines[5], sizeof(lines[5]), "client mem %3.0f%%  %4u MB free",
                      100.0 * double(space.total - memFree) / double(space.total), unsigned(memFree >> 20));
    else
        std::snprintf(lines[5], sizeof(lines[5]), "client mem   -");
    uint64_t vramUsed, vramBudget;
    device->Vram(&vramUsed, &vramBudget);
    if (vramBudget)
        std::snprintf(lines[6], sizeof(lines[6]), "VRAM   %3.0f%%  %.1f of %.1f GB", 100.0 * double(vramUsed) / double(vramBudget),
                      double(vramUsed) / double(1ull << 30), double(vramBudget) / double(1ull << 30));
    else
        std::snprintf(lines[6], sizeof(lines[6]), "VRAM     -");
    int count = 7;
    if (shownFps > 0.0) std::snprintf(lines[count++], sizeof(lines[7]), "%4.0f shown (frame gen)", shownFps);
    Metrics(w, h);
    const int cw = g.font.cw, ch = g.font.ch, pad = std::max(3, ch / 3);
    size_t widest = 0;
    for (int i = 0; i < count; ++i) widest = std::max(widest, std::strlen(lines[i]));
    const int bw = int(widest + 2) * cw + pad * 2, bh = count * ch + pad * 2;
    g.width = w;
    g.height = uint32_t(std::min<int>(int(h), bh));
    g.pixels.assign(size_t(g.width) * g.height, 0);
    Fill(0, 0, bw, bh, 0.035f, 0.045f, 0.06f, 0.72f);
    Text(pad, pad, C(cValue) + lines[0]);
    for (int i = 1; i < 4; ++i) {
        const bool limits = std::strncmp(lines[i], limit, std::strlen(limit)) == 0;   // (the one holding the frame back)
        Text(pad, pad + i * ch, C(limits ? cWarn : cText) + lines[i]);
    }
    Text(pad, pad + 4 * ch, C(cText) + lines[4]);
    Text(pad, pad + 5 * ch, C(memLow ? cWarn : cText) + lines[5]);
    Text(pad, pad + 6 * ch, C(cText) + lines[6]);
    if (count == 8) Text(pad, pad + 7 * ch, C(cValue) + lines[7]);
    device->SetConsoleImage(g.width, g.height, g.pixels.data());
    g_hud.shown = true;
    g.shown = false;                             // (the console's own image: none now)
}

}  // namespace

bool ConsoleOpen() { return g_open; }

bool ConsoleMessage(HWND, UINT msg, WPARAM wp, LPARAM lp)
{
    const unsigned scan = unsigned(lp >> 16) & 0xFF, key = g_scan;
    const bool extended = (lp & (1 << 24)) != 0, repeat = (lp & (1 << 30)) != 0;
    const bool ours = key && scan == key && !extended;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && ours) {
        if (!repeat && !(GetKeyState(VK_MENU) & 0x8000)) {
            Push({Event::Toggle, 0, false, false});
            // A dead key there (^ on German layouts): its waiting accent cleared, so the next letter comes plain.
            BYTE keys[256] = {};
            WCHAR buf[4];
            ToUnicode(VK_SPACE, MapVirtualKeyA(VK_SPACE, MAPVK_VK_TO_VSC), keys, buf, 4, 0);
        }
        return true;
    }
    if ((msg == WM_CHAR || msg == WM_DEADCHAR || msg == WM_SYSCHAR || msg == WM_SYSDEADCHAR) && ours)
        return true;
    if (!g_open) return false;
    // (AltGr is Ctrl+Alt: a character, not a Ctrl shortcut.)
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (wp == VK_F4) return false;           // (Alt+F4 still closes the game)
        return true;
    case WM_KEYDOWN:
        Push({Event::Key, unsigned(wp), ctrl, shift});
        return true;
    case WM_CHAR:
        Push({Event::Char, unsigned(wp), ctrl, shift});
        return true;
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return true;
    default:
        return false;
    }
}

namespace {

// RANDYVK_CONSOLE_TEST (see the top): the next piece of the script into the queue; true: save the image this frame.
bool g_test = false;

bool TestScript()
{
    static int frame = 0, at = -1;
    static std::string script;
    static bool read = false;
    if (!read) {
        read = true;
        char v[2048] = "";
        if (GetEnvironmentVariableA("RANDYVK_CONSOLE_TEST", v, sizeof(v)) && std::strchr(v, ':')) {
            at = std::atoi(v);
            script = std::strchr(v, ':') + 1;
            g_test = true;
        }
    }
    if (at < 0 || ++frame < at || script.empty()) return false;
    static const struct { const char* token; unsigned vk; bool shift; } kKeys[] = {
        {"tab", VK_TAB, false}, {"stab", VK_TAB, true}, {"enter", VK_RETURN, false}, {"up", VK_UP, false},
        {"down", VK_DOWN, false}, {"left", VK_LEFT, false}, {"right", VK_RIGHT, false}, {"esc", VK_ESCAPE, false},
        {"bs", VK_BACK, false}, {"pgup", VK_PRIOR, false},
    };
    while (!script.empty()) {
        if (script[0] == '{') {
            const size_t close = script.find('}');
            if (close == std::string::npos) { script.clear(); break; }
            const std::string token = script.substr(1, close - 1);
            script.erase(0, close + 1);
            if (token == "shot") return true;
            if (token == "open") Push({Event::Toggle, 0, false, false});
            for (const auto& k : kKeys)
                if (token == k.token) Push({Event::Key, k.vk, false, k.shift});
            continue;
        }
        Push({Event::Char, uint8_t(script[0]), false, false});
        script.erase(0, 1);
    }
    return false;
}

void SaveTestShot()
{
    static int n = 0;
    char dir[MAX_PATH] = "";
    GetEnvironmentVariableA("RANDYVK_LOG", dir, sizeof(dir));
    if (char* slash = std::strrchr(dir, '\\')) slash[1] = 0; else dir[0] = 0;
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%sconsole-test-%d.bmp", dir, n++);
    FILE* f = std::fopen(path, "wb");
    if (!f || !g.width) { if (f) std::fclose(f); return; }
    // Over a mid grey (what it would look like over a frame), bottom-up 24-bit rows.
    const uint32_t w = g.width, h = g.height, row = (w * 3 + 3) & ~3u;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = LONG(w);
    ih.biHeight = LONG(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* src = &g.pixels[size_t(h - 1 - y) * w];
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t p = src[x], inv = 255 - (p >> 24);
            for (int c = 0; c < 3; ++c) line[x * 3 + c] = uint8_t(std::min<uint32_t>(255, ((p >> (8 * c)) & 255) + 110 * inv / 255));
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
    RvkLog("console: test image %s", path);
}

}  // namespace

void ConsoleFrame(rvk::ThreadedDevice* device, HWND window)
{
    InputAttach(window);
    const bool shot = TestScript();
    static bool configured = false;
    if (!configured) {
        configured = true;
        g_scan = unsigned(GetPrivateProfileIntA("Console", "Key", 0x29, rvk_settings::IniPath())) & 0xFF;
        RvkLog(g_scan ? "console: key scan code 0x%02x" : "console: off (randy-vk.ini [Console] Key=0)", g_scan.load());
    }
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(g_queueLock);
        events.swap(g_queue);
    }
    if (!events.empty() && device->Width() && device->Height())
        Metrics(device->Width(), device->Height());   // (what is printed wraps at the width)
    for (const Event& e : events) {
        if (e.kind == Event::Toggle) Open(!g_open);
        else if (!g_open) continue;
        else if (e.kind == Event::Key) {
            KeyDown(e);
            if (!g.charsSeen && e.code >= 'A' && e.code <= 'Z' && !e.ctrl && ++g.keysWithoutChars == 20)
                RvkLog("console: letters pressed but no characters typed (the game's window doesn't translate keys)");
        } else if (e.kind == Event::Char) {
            g.charsSeen = true;
            if (!e.ctrl || uint8_t(e.code) >= 32) Typed(char(e.code));
        }
    }
    const bool hud = !g_open && !g.shown && g.slide == 0.0f && rvk_settings::Get("RVK_PerfHud") != 0.0f;
    if (!g_open && !g.shown && g.slide == 0.0f && (!shot || hud)) {
        HudFrame(device);                        // (RVK_PerfHud: with the console closed; {shot}: its image)
        if (shot && g_hud.shown) SaveTestShot();
        return;
    }
    g_hud.shown = false;                         // (the console has the image)
    // The slide in and out (~0.12 s), the caret's blink.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = g.last.QuadPart ? float(double(now.QuadPart - g.last.QuadPart) / double(freq.QuadPart)) : 0.0f;
    g.last = now;
    const float target = g_open ? 1.0f : 0.0f;
    if (g_test) g.slide = target;                 // (tests: no sliding)
    if (g.slide != target) {
        g.slide = g_open ? std::min(1.0f, g.slide + std::min(dt, 0.05f) / 0.12f) : std::max(0.0f, g.slide - std::min(dt, 0.05f) / 0.12f);
        g.dirty = true;
    }
    if (g_open && GetTickCount64() >= g.blinkAt) {
        g.blinkOn = !g.blinkOn;
        g.blinkAt = GetTickCount64() + 530;
        g.dirty = true;
    }
    if (!g_open && g.slide == 0.0f) {
        if (g.shown) device->SetConsoleImage(0, 0, nullptr);
        g.shown = false;
        g.last.QuadPart = 0;
        return;
    }
    const uint32_t w = device->Width(), h = device->Height();
    if (!w || !h) return;
    if (!g.dirty && g.width == w && g.shown && !shot) return;
    Draw(w, h);
    if (shot) SaveTestShot();
    device->SetConsoleImage(g.width, g.height, g.pixels.data());
    g.shown = true;
    g.dirty = false;
}

}  // namespace rvkproxy
