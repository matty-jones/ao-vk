// The renderer's plain settings (rvk.h RVK_FLOAT_KNOBS, RVK_BOOL_KNOBS; docs/audit-2026-10.md §5): one value each that
// a feature reads as it is, set here through one table instead of a setter apiece.
#include "internal.h"

#include <cstring>

namespace rvk {

namespace {

const char* const kKnobNames[] = {
#define RVK_KNOB_NAME(name, member, refresh) "RVK_" #name,
    RVK_FLOAT_KNOBS(RVK_KNOB_NAME) RVK_BOOL_KNOBS(RVK_KNOB_NAME)
#undef RVK_KNOB_NAME
};
static_assert(sizeof(kKnobNames) / sizeof(kKnobNames[0]) == size_t(Knob::Count), "a name for every knob");

}  // namespace

int KnobByName(const char* setting)
{
    for (size_t i = 0; i < size_t(Knob::Count); ++i)
        if (std::strcmp(kKnobNames[i], setting) == 0) return int(i);
    return -1;
}

void Device::SetKnob(Knob knob, float value)
{
    // What a change must refresh: the draws' constants, the frame lights, the ground grass's tiles (baked).
    enum : uint32_t { C = 1, L = 2, G = 4 };
    struct FloatKnob { float Device::*member; uint32_t refresh; };
    struct BoolKnob { bool Device::*member; uint32_t refresh; };
    static const FloatKnob kFloats[] = {
#define RVK_KNOB_ROW(name, member, refresh) {&Device::member, refresh},
        RVK_FLOAT_KNOBS(RVK_KNOB_ROW)
    };
    static const BoolKnob kBools[] = {
        RVK_BOOL_KNOBS(RVK_KNOB_ROW)
#undef RVK_KNOB_ROW
    };
    constexpr size_t floats = sizeof(kFloats) / sizeof(kFloats[0]);
    static_assert(floats + sizeof(kBools) / sizeof(kBools[0]) == size_t(Knob::Count), "a row for every knob");
    const size_t i = size_t(knob);
    uint32_t refresh;
    if (i < floats) {
        float& v = this->*kFloats[i].member;
        if (v == value) return;
        v = value;
        refresh = kFloats[i].refresh;
    } else if (i < size_t(Knob::Count)) {
        bool& v = this->*kBools[i - floats].member;
        if (v == (value != 0.0f)) return;
        v = value != 0.0f;
        refresh = kBools[i - floats].refresh;
    } else {
        return;
    }
    if (refresh & C) m_constantsDirty = true;
    if (refresh & L) m_frameLightsDirty = true;
    if (refresh & G) m_grassDirty = true;
}

}  // namespace rvk
