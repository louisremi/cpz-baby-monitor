#include "radio/radio.hpp"

#include <spdlog/spdlog.h>
#include <unistd.h>

#include <chrono>

#include "radio/hw/cap_lora_1262.hpp"

namespace cpzbm {

namespace {
constexpr std::size_t kMaxTxQueue = 4;
constexpr std::size_t kMaxRxQueue = 32;
}  // namespace

struct Radio::Impl {
    cap_lora::CapLoRa1262 lora;
};

Radio::Radio() = default;
Radio::~Radio() { stop(); }

bool Radio::start(const LinkProfile& profile)
{
    if (running_) return true;
    profile_ = profile;
    impl_    = new Impl();
    cap_lora::RadioConfig cfg;
    cfg.frequency_mhz    = profile.frequency_mhz;
    cfg.bandwidth_khz    = profile.bandwidth_khz;
    cfg.spreading_factor = profile.spreading_factor;
    cfg.coding_rate      = profile.coding_rate;
    cfg.sync_word        = profile.sync_word;
    cfg.output_power_dbm = profile.power_dbm;
    cfg.preamble         = profile.preamble;
    impl_->lora.set_config(cfg);
    if (!impl_->lora.initialize()) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "SX1262 init failed (Cap LoRa attached? another LoRa app running?)";
        spdlog::error("Radio: {}", error_);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    spdlog::info("Radio: up, profile={} {:.1f} MHz BW{:.0f} SF{} CR4/{} {} dBm", profile.name, profile.frequency_mhz,
                 profile.bandwidth_khz, profile.spreading_factor, profile.coding_rate, profile.power_dbm);
    stop_    = false;
    running_ = true;
    thread_  = std::thread([this] { threadMain(); });
    return true;
}

void Radio::stop()
{
    if (!impl_) return;
    stop_ = true;
    impl_->lora.request_stop();
    if (thread_.joinable()) thread_.join();
    impl_->lora.clear_stop();
    impl_->lora.shutdown();
    delete impl_;
    impl_    = nullptr;
    running_ = false;
}

std::string Radio::error() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void Radio::send(std::vector<uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > 255) return;
    std::lock_guard<std::mutex> lock(mutex_);
    while (tx_queue_.size() >= kMaxTxQueue) {
        tx_queue_.pop_front();
        ++stats_.tx_dropped;
    }
    tx_queue_.push_back(std::move(bytes));
}

bool Radio::receive(RxPacket* out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (rx_queue_.empty()) return false;
    *out = std::move(rx_queue_.front());
    rx_queue_.pop_front();
    return true;
}

RadioStats Radio::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

uint32_t Radio::airtimeUs(std::size_t length) const { return loraTimeOnAirUs(profile_, length); }

bool Radio::txBusy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tx_inflight_.load() || !tx_queue_.empty();
}

void Radio::threadMain()
{
    auto& lora     = impl_->lora;
    bool was_tx    = false;
    auto tx_start  = std::chrono::steady_clock::now();
    while (!stop_) {
        auto msg = lora.msg_poll();  // services IRQs (RX done / TX done)
        if (msg.ok()) {
            RxPacket pkt;
            pkt.bytes.assign(msg->begin(), msg->end());
            pkt.rssi = lora.last_rssi();
            pkt.snr  = lora.last_snr();
            std::lock_guard<std::mutex> lock(mutex_);
            if (rx_queue_.size() >= kMaxRxQueue) rx_queue_.pop_front();
            ++stats_.rx;
            stats_.last_rssi = pkt.rssi;
            stats_.last_snr  = pkt.snr;
            rx_queue_.push_back(std::move(pkt));
        }
        const bool in_tx = lora.tx_in_progress();
        if (was_tx && !in_tx) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.tx_ok;
        }
        // Recover from a TX_DONE IRQ that never arrived (should not happen,
        // but a stuck transmitter would silently kill the link).
        if (in_tx && std::chrono::steady_clock::now() - tx_start > std::chrono::seconds(2)) {
            spdlog::warn("Radio: TX timeout, re-initializing");
            lora.shutdown();
            lora.clear_stop();
            if (!lora.initialize()) {
                spdlog::error("Radio: re-init failed");
                running_ = false;
                return;
            }
        }
        was_tx = lora.tx_in_progress();
        tx_inflight_ = was_tx;
        if (!was_tx) {
            std::vector<uint8_t> next;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!tx_queue_.empty()) {
                    next = std::move(tx_queue_.front());
                    tx_queue_.pop_front();
                }
            }
            if (!next.empty()) {
                auto r = lora.msg_send(std::string(next.begin(), next.end()));
                if (r.ok()) {
                    was_tx       = true;
                    tx_inflight_ = true;
                    tx_start     = std::chrono::steady_clock::now();
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.tx_dropped;
                }
            }
        }
        usleep(1000);
    }
}

}  // namespace cpzbm
