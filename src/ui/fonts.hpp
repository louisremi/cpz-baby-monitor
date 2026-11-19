#pragma once

// UI fonts. The CardputerZero guidelines recommend DejaVu Sans (installed by
// fonts-dejavu-core on the device) at >=12 px; it is rasterised at runtime
// with LVGL's tiny_ttf. If the TTF cannot be loaded, the built-in Montserrat
// fonts of the same sizes are used so the UI still renders.

#include <lvgl.h>

namespace cpzbm::fonts {

enum class Size { Small = 12, Body = 14, Large = 16, Banner = 18, Code = 32 };

void init();
void deinit();
const lv_font_t* get(Size size);

}  // namespace cpzbm::fonts
