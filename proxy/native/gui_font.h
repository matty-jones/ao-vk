// Display-resolution GUI text caches; native layout and world fonts remain unchanged.
#pragma once
#include "native/native.h"

namespace rnative::gui {

void InstallFontScaling(HMODULE gui);
bool FontScalingReady();
bool DumpFontCaches(const char* path);
void* CreateTextBitmap(void* storage, int width, int height, float factor);
void ForgetTextBitmap(void* bitmap);

}  // namespace rnative::gui
