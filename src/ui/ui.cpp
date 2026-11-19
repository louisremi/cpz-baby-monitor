#include "ui/ui.hpp"

#include "ui/fonts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cpzbm {
namespace {

constexpr int kW = 320, kH = 170;
// Function bar: label centres per the CardputerZero key guideline (x above
// number keys 4-8, y=153), with a short tick pointing down at each key.
constexpr int kNavCenterY = 153;
constexpr int kNavX[5]    = {49, 105, 161, 217, 273};
constexpr int kNavLabelW  = 56;
constexpr int kWaveY = 22, kWaveH = 118;  // waveform canvas area

const lv_color_t kBg     = lv_color_hex(0x101418);
const lv_color_t kFg     = lv_color_hex(0xE8ECEF);
const lv_color_t kDim    = lv_color_hex(0x8A949C);
const lv_color_t kAccent = lv_color_hex(0x4FC3F7);
const lv_color_t kGood   = lv_color_hex(0x66BB6A);
const lv_color_t kWarn   = lv_color_hex(0xFFB74D);
const lv_color_t kBad    = lv_color_hex(0xEF5350);

std::string nameStr(const wire::Name& n) { return std::string(n.data(), strnlen(n.data(), n.size())); }

std::string ago(uint64_t now, uint64_t then)
{
    if (then == 0) return "never";
    const uint64_t s = (now - then) / 1000;
    char buf[32];
    if (s < 60)
        std::snprintf(buf, sizeof buf, "%llus ago", static_cast<unsigned long long>(s));
    else
        std::snprintf(buf, sizeof buf, "%llum ago", static_cast<unsigned long long>(s / 60));
    return buf;
}

Screen screenFor(Role r) { return r == Role::Baby ? Screen::Baby : r == Role::Parent ? Screen::Parent : Screen::Home; }

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

}  // namespace

Ui::Ui(Controller* ctl) : ctl_(ctl) {}

void Ui::start()
{
    if (!ctl_->status().radio_ok) {
        showScreen(Screen::RadioError);
        return;
    }
    // Resume the stored role straight away: a baby monitor that was powered
    // off and on again should go back to monitoring without user input.
    const auto& link = ctl_->link();
    const Role role  = link.paired() ? link.pairing().role : Role::None;
    ctl_->enterRole(role);
    showScreen(screenFor(role));
}

void Ui::showScreen(Screen s)
{
    screen_ = s;
    build();
    refresh();
}

lv_obj_t* Ui::label(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color, int x, int y)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

void Ui::buildNavBar(const char* labels[5])
{
    for (int i = 0; i < 5; ++i) {
        if (!labels[i] || !labels[i][0]) continue;
        // Short vertical tick pointing down at the physical key.
        lv_obj_t* tick = lv_obj_create(root_);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, 2, 5);
        lv_obj_set_style_bg_color(tick, kDim, 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_set_pos(tick, kNavX[i] - 1, kH - 6);
        lv_obj_t* l = lv_label_create(root_);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_font(l, fonts::get(fonts::Size::Small), 0);
        lv_obj_set_style_text_color(l, kFg, 0);
        lv_obj_set_width(l, kNavLabelW);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, kNavX[i] - kNavLabelW / 2, kNavCenterY - lv_font_get_line_height(fonts::get(fonts::Size::Small)) / 2);
    }
}

void Ui::build()
{
    if (root_) lv_obj_delete(root_);
    root_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kW, kH);
    lv_obj_set_style_bg_color(root_, kBg, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    l_title_ = l_status_ = l_main_ = l_aux_ = bar_ = vox_mark_ = banner_ = canvas_ = nullptr;
    if (canvas_buf_) {
        lv_free(canvas_buf_);
        canvas_buf_ = nullptr;
    }

    l_title_  = label(root_, "", fonts::get(fonts::Size::Body), kAccent, 6, 3);
    l_status_ = label(root_, "", fonts::get(fonts::Size::Small), kDim, 120, 4);
    lv_obj_set_width(l_status_, 194);
    lv_obj_set_style_text_align(l_status_, LV_TEXT_ALIGN_RIGHT, 0);

    switch (screen_) {
        case Screen::RadioError: {
            lv_label_set_text(l_title_, "BabyMon");
            l_main_ = label(root_, "LoRa radio unavailable", fonts::get(fonts::Size::Large), kBad, 10, 40);
            l_aux_  = label(root_, "", fonts::get(fonts::Size::Small), kFg, 10, 70);
            lv_obj_set_width(l_aux_, 300);
            lv_label_set_long_mode(l_aux_, LV_LABEL_LONG_WRAP);
            const char* nav[5] = {"", "", "", "", "Quit"};
            buildNavBar(nav);
            break;
        }
        case Screen::Home: {
            lv_label_set_text(l_title_, "BabyMon");
            l_main_ = label(root_, "", fonts::get(fonts::Size::Large), kFg, 10, 34);
            l_aux_  = label(root_, "", fonts::get(fonts::Size::Small), kDim, 10, 62);
            lv_obj_set_width(l_aux_, 300);
            lv_label_set_long_mode(l_aux_, LV_LABEL_LONG_WRAP);
            if (ctl_->link().paired()) {
                const char* nav[5] = {"Baby", "Parent", "Region", "", "Unpair"};
                buildNavBar(nav);
            } else {
                const char* nav[5] = {"Show", "Enter", "Region", "", "Quit"};
                buildNavBar(nav);
            }
            break;
        }
        case Screen::PairShow: {
            lv_label_set_text(l_title_, "Pair: show code");
            label(root_, "Type this code on the other device:", fonts::get(fonts::Size::Small), kDim, 10, 28);
            l_main_ = label(root_, "", fonts::get(fonts::Size::Code), kFg, 0, 52);
            lv_obj_set_width(l_main_, kW);
            lv_obj_set_style_text_align(l_main_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_letter_space(l_main_, 6, 0);
            l_aux_ = label(root_, "", fonts::get(fonts::Size::Small), kDim, 10, 100);
            lv_obj_set_width(l_aux_, 300);
            const char* nav[5] = {"", "", "", "", "Cancel"};
            buildNavBar(nav);
            break;
        }
        case Screen::PairEnter: {
            lv_label_set_text(l_title_, "Pair: enter code");
            l_aux_  = label(root_, "", fonts::get(fonts::Size::Small), kFg, 10, 26);
            lv_obj_set_width(l_aux_, 300);
            l_main_ = label(root_, "", fonts::get(fonts::Size::Code), kFg, 0, 70);
            lv_obj_set_width(l_main_, kW);
            lv_obj_set_style_text_align(l_main_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_letter_space(l_main_, 6, 0);
            label(root_, "F/X: device  0-9: code  OK: pair  ESC: back", fonts::get(fonts::Size::Small), kDim, 6,
                  kNavCenterY - 7);
            break;
        }
        case Screen::Baby: {
            lv_label_set_text(l_title_, "Baby unit");
            label(root_, "Mic level", fonts::get(fonts::Size::Small), kDim, 10, 28);
            bar_ = lv_bar_create(root_);
            lv_obj_set_size(bar_, 300, 18);
            lv_obj_set_pos(bar_, 10, 46);
            lv_bar_set_range(bar_, -80, 0);
            lv_obj_set_style_bg_color(bar_, lv_color_hex(0x263238), LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar_, kGood, LV_PART_INDICATOR);
            vox_mark_ = lv_obj_create(root_);
            lv_obj_remove_style_all(vox_mark_);
            lv_obj_set_size(vox_mark_, 2, 26);
            lv_obj_set_style_bg_color(vox_mark_, kWarn, 0);
            lv_obj_set_style_bg_opa(vox_mark_, LV_OPA_COVER, 0);
            l_main_ = label(root_, "", fonts::get(fonts::Size::Large), kFg, 10, 74);
            l_aux_  = label(root_, "", fonts::get(fonts::Size::Small), kDim, 10, 100);
            lv_obj_set_width(l_aux_, 300);
            const char* nav[5] = {"VOX-", "VOX+", "Always", "Codec", "Home"};
            buildNavBar(nav);
            break;
        }
        case Screen::Parent: {
            lv_label_set_text(l_title_, "Parent");
            canvas_buf_ = static_cast<uint16_t*>(lv_malloc(kW * kWaveH * sizeof(uint16_t)));
            canvas_     = lv_canvas_create(root_);
            lv_canvas_set_buffer(canvas_, canvas_buf_, kW, kWaveH, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(canvas_, 0, kWaveY);
            banner_ = lv_label_create(root_);
            lv_obj_set_style_text_font(banner_, fonts::get(fonts::Size::Banner), 0);
            lv_obj_set_style_text_color(banner_, lv_color_white(), 0);
            lv_obj_set_style_bg_color(banner_, kBad, 0);
            lv_obj_set_style_bg_opa(banner_, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_hor(banner_, 10, 0);
            lv_obj_set_style_pad_ver(banner_, 4, 0);
            lv_label_set_text(banner_, "LINK LOST");
            lv_obj_align(banner_, LV_ALIGN_TOP_MID, 0, 62);
            lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
            const char* nav[5] = {"", "", "", "Mute", "Home"};
            buildNavBar(nav);
            break;
        }
        case Screen::ConfirmUnpair: {
            lv_label_set_text(l_title_, "Unpair?");
            l_main_ = label(root_, "Forget the paired device?", fonts::get(fonts::Size::Large), kFg, 10, 40);
            label(root_, "You will need to pair again with a new code.", fonts::get(fonts::Size::Small), kDim, 10, 68);
            const char* nav[5] = {"Yes", "", "", "", "No"};
            buildNavBar(nav);
            break;
        }
    }
}

void Ui::drawWave()
{
    if (!canvas_ || !canvas_buf_) return;
    const auto& st   = ctl_->status();
    const uint16_t bg   = rgb565(0x10, 0x14, 0x18);
    const uint16_t axis = rgb565(0x30, 0x38, 0x40);
    const int mid       = kWaveH / 2;
    // Map peak amplitude logarithmically: quiet sounds are still visible,
    // crying (near full scale) fills the height.
    for (int x = 0; x < kW; ++x) {
        const int col_index = kW - 1 - x;  // newest column on the right
        float v             = 0;
        if (col_index < kWaveColumns) {
            const int idx = ((st.wave_head - 1 - col_index) % kWaveColumns + kWaveColumns) % kWaveColumns;
            v             = st.wave[idx];
        }
        float h = 0;
        if (v > 0.003f) h = std::clamp((20.0f * std::log10(v) + 50.0f) / 50.0f, 0.0f, 1.0f);
        const int half    = static_cast<int>(h * (mid - 2));
        const uint16_t c  = h > 0.8f ? rgb565(0xEF, 0x53, 0x50) : h > 0.5f ? rgb565(0xFF, 0xB7, 0x4D)
                                                                          : rgb565(0x4F, 0xC3, 0xF7);
        for (int y = 0; y < kWaveH; ++y) {
            const int d = y - mid;
            canvas_buf_[y * kW + x] = (d >= -half && d <= half && half > 0) ? c : (d == 0 ? axis : bg);
        }
    }
    lv_obj_invalidate(canvas_);
}

void Ui::refresh()
{
    const auto& st   = ctl_->status();
    const auto& link = ctl_->link();
    char buf[160];

    // Status line: battery + link quality.
    if (l_status_) {
        std::string s = st.region == Region::EU868 ? "EU  " : "US  ";
        if (st.region_switching) s = "band... ";
        if (link.paired() && (screen_ == Screen::Baby || screen_ == Screen::Parent || screen_ == Screen::Home)) {
            if (st.last_rx_ms && st.now_ms - st.last_rx_ms < 15000)
                std::snprintf(buf, sizeof buf, "%.0fdBm %+.0fdB  ", st.rssi, st.snr);
            else
                std::snprintf(buf, sizeof buf, "no link  ");
            s += buf;
        }
        if (st.battery <= 100) {
            std::snprintf(buf, sizeof buf, "bat %u%%", st.battery);
            s += buf;
        }
        lv_label_set_text(l_status_, s.c_str());
    }

    switch (screen_) {
        case Screen::RadioError:
            std::snprintf(buf, sizeof buf, "%s\n\nClose the stock LoRa app and check that the Cap LoRa-1262 is attached.",
                          st.radio_error.c_str());
            lv_label_set_text(l_aux_, buf);
            break;
        case Screen::Home:
            if (link.paired()) {
                std::snprintf(buf, sizeof buf, "Paired with %s", nameStr(link.pairing().peer_name).c_str());
                lv_label_set_text(l_main_, buf);
                std::snprintf(buf, sizeof buf,
                              "Last heard %s.  Band: %s\nChoose this device's role; the other device switches automatically.",
                              ago(st.now_ms, st.last_rx_ms).c_str(), regionName(st.region));
                lv_label_set_text(l_aux_, buf);
            } else {
                lv_label_set_text(l_main_, "Not paired");
                std::snprintf(buf, sizeof buf,
                              "This device: %s  Band: %s\nShow a code here, then enter it on the other device "
                              "(both on the same band).",
                              nameStr(ctl_->identity().name).c_str(), regionName(st.region));
                lv_label_set_text(l_aux_, buf);
            }
            break;
        case Screen::PairShow: {
            const std::string& code = link.code();
            std::snprintf(buf, sizeof buf, "%.3s %.3s", code.c_str(), code.size() > 3 ? code.c_str() + 3 : "");
            lv_label_set_text(l_main_, buf);
            const char* phase = link.phase() == PairPhase::AwaitConfirm ? "Verifying..." : "Waiting for the other device";
            std::snprintf(buf, sizeof buf, "%s\nThis device: %s", phase, nameStr(ctl_->identity().name).c_str());
            lv_label_set_text(l_aux_, buf);
            break;
        }
        case Screen::PairEnter: {
            const auto peers = link.peers(st.now_ms);
            if (peer_index_ >= static_cast<int>(peers.size())) peer_index_ = 0;
            std::string s;
            if (peers.empty()) {
                s = "Searching for a device showing a code...";
            } else {
                for (std::size_t i = 0; i < peers.size() && i < 2; ++i) {
                    std::snprintf(buf, sizeof buf, "%s %s  (%.0f dBm)\n", static_cast<int>(i) == peer_index_ ? ">" : "  ",
                                  nameStr(peers[i].name).c_str(), peers[i].rssi);
                    s += buf;
                }
                chosen_peer_ = peers[static_cast<std::size_t>(peer_index_)].dev_id;
            }
            const auto phase = link.phase();
            if (phase == PairPhase::Requesting || phase == PairPhase::Confirming)
                s += "Pairing...";
            else if (phase == PairPhase::Failed)
                s += std::string(pairErrorText(link.lastError())) + " - try again";
            lv_label_set_text(l_aux_, s.c_str());
            std::string shown = typed_;
            while (shown.size() < 6) shown += '_';
            std::snprintf(buf, sizeof buf, "%.3s %.3s", shown.c_str(), shown.c_str() + 3);
            lv_label_set_text(l_main_, buf);
            break;
        }
        case Screen::Baby: {
            lv_bar_set_value(bar_, static_cast<int32_t>(std::max(-80.0f, st.mic_dbfs)), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_, st.gate_open ? kBad : kGood, LV_PART_INDICATOR);
            const int mx = 10 + static_cast<int>((st.vox_dbfs + 80.0f) / 80.0f * 300.0f);
            lv_obj_set_pos(vox_mark_, mx, 42);
            if (!st.audio_error.empty())
                std::snprintf(buf, sizeof buf, "Mic error");
            else if (st.always_stream)
                std::snprintf(buf, sizeof buf, "Streaming (always on)");
            else
                std::snprintf(buf, sizeof buf, "%s", st.gate_open ? "Sound - streaming" : "Quiet - listening");
            lv_label_set_text(l_main_, buf);
            lv_obj_set_style_text_color(l_main_, st.gate_open ? kBad : kFg, 0);
            std::snprintf(buf, sizeof buf, "%s\nVOX %.0f dB  gain %+.0f dB  codec %s\nsent %llu  parent heard %s",
                          st.audio_error.empty() ? "" : st.audio_error.c_str(), st.vox_dbfs, st.agc_db,
                          audio::modeName(st.codec), static_cast<unsigned long long>(st.audio_packets),
                          ago(st.now_ms, st.last_rx_ms).c_str());
            lv_label_set_text(l_aux_, buf + (st.audio_error.empty() ? 1 : 0));
            break;
        }
        case Screen::Parent: {
            drawWave();
            const bool lost = linkLost(st.now_ms, st.last_rx_ms);
            if (lost)
                lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
            std::snprintf(buf, sizeof buf, "Parent%s%s", st.peer_sound ? "  - sound" : "",
                          st.speaker_muted ? "  [muted]" : "");
            lv_label_set_text(l_title_, buf);
            lv_obj_set_style_text_color(l_title_, st.peer_sound ? kBad : kAccent, 0);
            break;
        }
        case Screen::ConfirmUnpair:
            break;
    }
}

void Ui::handleLinkEvents()
{
    LinkEvent e;
    while (ctl_->pollUiEvent(&e)) {
        switch (e.kind) {
            case LinkEvent::Kind::Paired:
                typed_.clear();
                ctl_->leaveRole();
                showScreen(Screen::Home);
                break;
            case LinkEvent::Kind::RoleChanged: {
                const Role r = ctl_->link().pairing().role;
                ctl_->enterRole(r);
                showScreen(screenFor(r));
                break;
            }
            case LinkEvent::Kind::Unpaired:
                ctl_->leaveRole();
                showScreen(Screen::Home);
                break;
            case LinkEvent::Kind::PairFailed:
                if (screen_ == Screen::PairEnter) typed_.clear();
                break;
            default:
                break;
        }
    }
}

void Ui::tick(uint64_t now_ms)
{
    now_ = now_ms;
    handleLinkEvents();
    // 25 fps on the waveform screen (one column per 40 ms), 5 fps elsewhere.
    const uint64_t period = screen_ == Screen::Parent ? 40 : 200;
    if (now_ms - last_refresh_ >= period) {
        last_refresh_ = now_ms;
        refresh();
    }
}

bool Ui::onKey(uint32_t key, bool pressed, uint64_t now_ms)
{
    if (!pressed) return true;
    auto& link = ctl_->link();
    const bool esc = key == LV_KEY_ESC;
    switch (screen_) {
        case Screen::RadioError:
            if (esc || key == '8') quit_ = true;
            break;
        case Screen::Home:
            if (key == '6') {
                ctl_->toggleRegion(now_ms);  // same key, paired or not
                refresh();
            } else if (link.paired()) {
                if (key == '4') {
                    link.setRole(Role::Baby, now_ms);  // emits RoleChanged -> screen switch
                } else if (key == '5') {
                    link.setRole(Role::Parent, now_ms);
                } else if (key == '8') {
                    showScreen(Screen::ConfirmUnpair);
                } else if (esc) {
                    quit_ = true;
                }
            } else {
                if (key == '4') {
                    link.startShowing(now_ms);
                    showScreen(Screen::PairShow);
                } else if (key == '5') {
                    typed_.clear();
                    peer_index_ = 0;
                    link.startScanning(now_ms);
                    showScreen(Screen::PairEnter);
                } else if (esc || key == '8') {
                    quit_ = true;
                }
            }
            break;
        case Screen::PairShow:
            if (esc || key == '8') {
                link.cancelPairing();
                showScreen(Screen::Home);
            }
            break;
        case Screen::PairEnter: {
            const bool busy = link.phase() == PairPhase::Requesting || link.phase() == PairPhase::Confirming;
            if (esc) {
                link.cancelPairing();
                showScreen(Screen::Home);
            } else if (busy) {
                break;
            } else if (key >= '0' && key <= '9' && typed_.size() < 6) {
                if (link.phase() == PairPhase::Failed) link.startScanning(now_ms);
                typed_ += static_cast<char>(key);
            } else if (key == LV_KEY_BACKSPACE && !typed_.empty()) {
                typed_.pop_back();
            } else if (key == 'f' || key == LV_KEY_UP) {
                peer_index_ = std::max(0, peer_index_ - 1);
            } else if (key == 'x' || key == LV_KEY_DOWN) {
                ++peer_index_;
            } else if (key == LV_KEY_ENTER && typed_.size() == 6 && chosen_peer_ != 0) {
                if (!link.submitCode(chosen_peer_, typed_, now_ms)) {
                    link.startScanning(now_ms);  // peer went stale; rescan
                }
                typed_.clear();
            }
            refresh();
            break;
        }
        case Screen::Baby:
            if (key == '4') ctl_->adjustVox(-3.0f);
            else if (key == '5') ctl_->adjustVox(3.0f);
            else if (key == '6') ctl_->toggleAlwaysStream();
            else if (key == '7') ctl_->toggleCodecMode();
            else if (esc || key == '8') {
                ctl_->leaveRole();
                showScreen(Screen::Home);
            }
            refresh();
            break;
        case Screen::Parent:
            if (key == '7') ctl_->toggleMute();
            else if (esc || key == '8') {
                ctl_->leaveRole();
                showScreen(Screen::Home);
            }
            break;
        case Screen::ConfirmUnpair:
            if (key == '4') {
                link.unpair();  // emits Unpaired -> Home
            } else if (esc || key == '8') {
                showScreen(Screen::Home);
            }
            break;
    }
    return true;
}

}  // namespace cpzbm
