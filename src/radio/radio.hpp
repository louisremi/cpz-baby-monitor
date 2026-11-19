#pragma once

// Thread-owned SX1262 link. The radio thread is the only code that touches the
// SX1262 (SPI/GPIO are not thread-safe), polling it every millisecond; other
// threads exchange packets through bounded queues.

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "radio/radio_config.hpp"

namespace cpzbm {

struct RxPacket {
    std::vector<uint8_t> bytes;
    float rssi = 0.0f;
    float snr  = 0.0f;
};

struct RadioStats {
    uint64_t tx_ok      = 0;
    uint64_t tx_dropped = 0;
    uint64_t rx         = 0;
    float last_rssi     = 0.0f;
    float last_snr      = 0.0f;
};

class Radio {
public:
    Radio();
    ~Radio();
    Radio(const Radio&)            = delete;
    Radio& operator=(const Radio&) = delete;

    // Powers the Cap and starts the radio thread. Returns false (with
    // error()) if the hardware could not be initialized.
    bool start(const LinkProfile& profile);
    void stop();
    bool running() const { return running_.load(); }
    std::string error() const;

    // Queue a packet (<=255 bytes). Oldest queued packets are dropped when
    // the queue is full, so stale audio never delays fresh control traffic.
    void send(std::vector<uint8_t> bytes);
    bool receive(RxPacket* out);
    RadioStats stats() const;
    // Microseconds the transmitter is busy for a packet of this length.
    uint32_t airtimeUs(std::size_t length) const;
    // True while the radio has queued or in-flight transmissions.
    bool txBusy() const;

private:
    struct Impl;
    void threadMain();

    Impl* impl_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::deque<std::vector<uint8_t>> tx_queue_;
    std::deque<RxPacket> rx_queue_;
    RadioStats stats_;
    std::string error_;
    LinkProfile profile_;
    std::atomic<bool> tx_inflight_{false};
};

}  // namespace cpzbm
