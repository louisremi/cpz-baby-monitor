#pragma once

// 320x170 LVGL UI. Screens are rebuilt on navigation; live values are
// refreshed from the controller snapshot on every tick. Keys are handled
// directly (no LVGL focus groups): the function bar maps number keys 4-8 to
// actions, as recommended by the CardputerZero key guidelines.

#include <cstdint>
#include <string>

#include <lvgl.h>

#include "app/app.hpp"

namespace cpzbm {

enum class Screen { Home, PairShow, PairEnter, Baby, Parent, ConfirmUnpair, RadioError };

class Ui {
public:
    explicit Ui(Controller* ctl);
    void start();
    void tick(uint64_t now_ms);
    // Returns true if the key was consumed.
    bool onKey(uint32_t key, bool pressed, uint64_t now_ms);
    bool quitRequested() const { return quit_; }

private:
    void showScreen(Screen s);
    void build();
    void refresh();
    void buildNavBar(const char* labels[5]);
    lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color, int x, int y);
    void drawWave();
    void handleLinkEvents();

    Controller* ctl_;
    Screen screen_ = Screen::Home;
    bool quit_     = false;
    uint64_t now_  = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* l_title_ = nullptr;
    lv_obj_t* l_status_ = nullptr;
    lv_obj_t* l_main_ = nullptr;
    lv_obj_t* l_aux_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_obj_t* vox_mark_ = nullptr;
    lv_obj_t* banner_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    uint16_t* canvas_buf_ = nullptr;

    // Pair entry
    std::string typed_;
    int peer_index_ = 0;
    uint32_t chosen_peer_ = 0;
    uint64_t last_refresh_ = 0;
};

}  // namespace cpzbm
