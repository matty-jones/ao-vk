// The renderer's settings: one table of every option, persisted in randy-vk.ini next to randy31.dll, applied to the
// rvk device, changed by the in-game hotkeys and - through the C interface exported from randy31.dll (below) - by a
// settings window such as AOReloaded's options tab.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rvk {
class ThreadedDevice;
}

// ---- Exported C interface (randy31.dll), version 2 ----
// RvkSettings_Count() settings, described by RvkSettings_Get(index); RvkSettings_Set(name, value) changes one
// (clamped to its range, applied at once, saved). Bool settings are 0 / 1, int ones whole numbers, choice ones one
// of `choices`. Version 2 added type 3 (choice) and the fields from `parent` on (a version 1 caller's smaller size
// gets the others).
extern "C" {
struct RvkSettingInfo {
    uint32_t size;              // sizeof(RvkSettingInfo), set by the caller
    const char* name;           // at most 15 characters, starts with "RVK_"
    const char* label;
    const char* section;        // grouping for display
    uint32_t type;              // 0 bool, 1 int, 2 float, 3 choice (an int from `choices`)
    float min, max, step;       // step: the slider's resolution (float settings)
    float value, defaultValue;
    const char* parent;         // the bool switching the feature this belongs to (null: none)
    const char* choices;        // choice: the allowed values separated by spaces
};
uint32_t RvkSettings_Version();
uint32_t RvkSettings_Count();
int RvkSettings_Get(uint32_t index, RvkSettingInfo* out);
int RvkSettings_Set(const char* name, float value);
}

namespace rvk_settings {

void Load();                                        // once: randy-vk.ini (legacy RANDYVK_* env vars where absent)
float Get(const char* name);                        // as set (hotkeys, the settings window)
float GetEffective(const char* name);               // as in effect (RVK_Enhance off: the game's own look)
void ApplyTemporary(const char* name, float value);  // in effect until Restore, not saved (profiling sweep)
void Restore(const char* name);
void Set(const char* name, float value);            // clamped, applied to the device if there is one, saved
void ApplyAll(rvk::ThreadedDevice* device);         // a new device gets every setting (and the colour tables)
void LoadLuts(rvk::ThreadedDevice* device);         // randy-vk-day/night.cube next to the ini (Ctrl+Shift+L)
void PollIni(rvk::ThreadedDevice* device);          // once a frame: randy-vk.ini edited by hand -> its changes applied
void LogAll();
const char* IniPath();                              // randy-vk.ini (other sections: [Particles])
// Presets: 0 Original, 1 Classic+, 2 Modern, 3 Ultra; Current: the one the settings match, -1 custom.
uint32_t PresetCount();
const char* PresetName(uint32_t i);
bool PresetApply(uint32_t i);
int PresetCurrent();
// The settings RVK_Enhance off returns to the game's look, with those values (the console's bisect).
uint32_t VanillaCount();
const char* VanillaName(uint32_t i);
float VanillaValue(uint32_t i);
// These values in effect together, not saved (the console's bisect: applied one at a time, a setting applied with its
// partners would take back an earlier one's); RestoreAll: every saved value in effect again.
void ApplyTemporarySet(const std::vector<std::pair<std::string, float>>& values);
void RestoreAll();
// The sky's colour presets (docs/sky-plan.md §1; the console's skypreset): each sets the nine RVK_Sky* colours.
uint32_t SkyPresetCount();
const char* SkyPresetName(uint32_t i);
bool SkyPresetApply(uint32_t i);
int SkyPresetCurrent();                             // -1: colours of your own
// The console (rvk_console.cpp): every setting by index (table order), the hidden ones too (`hidden`: the settings
// window leaves it out); `parent` null where the feature can't be switched off.
uint32_t Count();
bool Info(uint32_t index, RvkSettingInfo* out, bool* hidden);

}  // namespace rvk_settings
