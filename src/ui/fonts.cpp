#include "ui/fonts.hpp"

#include <spdlog/spdlog.h>

#include <array>

namespace cpzbm::fonts {
namespace {

// "A:" is LVGL's POSIX filesystem driver (LV_FS_POSIX_LETTER, rooted at "/").
constexpr const char* kTtfPath = "A:usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

constexpr std::array<Size, 5> kSizes = {Size::Small, Size::Body, Size::Large, Size::Banner, Size::Code};
std::array<lv_font_t*, kSizes.size()> g_loaded{};

std::size_t indexOf(Size s)
{
    for (std::size_t i = 0; i < kSizes.size(); ++i)
        if (kSizes[i] == s) return i;
    return 0;
}

const lv_font_t* fallback(Size s)
{
    switch (s) {
        case Size::Small: return &lv_font_montserrat_12;
        case Size::Body: return &lv_font_montserrat_14;
        case Size::Large: return &lv_font_montserrat_16;
        case Size::Banner: return &lv_font_montserrat_18;
        case Size::Code: return &lv_font_montserrat_32;
    }
    return &lv_font_montserrat_14;
}

}  // namespace

void init()
{
    for (std::size_t i = 0; i < kSizes.size(); ++i) {
        g_loaded[i] = lv_tiny_ttf_create_file(kTtfPath, static_cast<int32_t>(kSizes[i]));
        if (!g_loaded[i]) {
            spdlog::warn("Fonts: cannot load {}, using built-in Montserrat", kTtfPath);
            deinit();
            return;
        }
    }
    spdlog::info("Fonts: DejaVu Sans loaded");
}

void deinit()
{
    for (auto& f : g_loaded) {
        if (f) lv_tiny_ttf_destroy(f);
        f = nullptr;
    }
}

const lv_font_t* get(Size size)
{
    const lv_font_t* f = g_loaded[indexOf(size)];
    return f ? f : fallback(size);
}

}  // namespace cpzbm::fonts
