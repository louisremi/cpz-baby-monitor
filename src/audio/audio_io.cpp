#include "audio/audio_io.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_NULL
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

#include "audio/codec.hpp"

namespace cpzbm::audio {
namespace {

bool initContext(ma_context* ctx, std::string* error)
{
    // Prefer pulse (pipewire-pulse) so we coexist with other audio users and
    // the system volume keys; fall back to ALSA for headless debugging.
    ma_backend backends[] = {ma_backend_pulseaudio, ma_backend_alsa};
    ma_context_config cfg = ma_context_config_init();
    cfg.pulse.pApplicationName = "cpz-baby-monitor";
    const ma_result r = ma_context_init(backends, 2, &cfg, ctx);
    if (r != MA_SUCCESS) {
        *error = std::string("audio context: ") + ma_result_description(r);
        return false;
    }
    spdlog::info("Audio: backend {}", ma_get_backend_name(ctx->backend));
    return true;
}

}  // namespace

struct Capture::Impl {
    ma_context ctx{};
    ma_device dev{};
    bool ctx_ok = false, dev_ok = false;
    int block = 320;
    std::mutex mutex;
    std::vector<int16_t> partial;
    std::deque<std::vector<int16_t>> blocks;
};

namespace {
void captureCallback(ma_device* dev, void*, const void* input, ma_uint32 frames)
{
    auto* impl     = static_cast<Capture::Impl*>(dev->pUserData);
    const auto* in = static_cast<const int16_t*>(input);
    std::lock_guard<std::mutex> lock(impl->mutex);
    for (ma_uint32 i = 0; i < frames; ++i) {
        impl->partial.push_back(in[i]);
        if (static_cast<int>(impl->partial.size()) == impl->block) {
            if (impl->blocks.size() > 50) impl->blocks.pop_front();  // consumer stalled: drop oldest
            impl->blocks.push_back(std::move(impl->partial));
            impl->partial.clear();
            impl->partial.reserve(static_cast<std::size_t>(impl->block));
        }
    }
}
}  // namespace

Capture::Capture() = default;
Capture::~Capture() { stop(); }

bool Capture::start(int block_samples)
{
    if (impl_) return true;
    impl_        = new Impl();
    impl_->block = block_samples;
    if (!initContext(&impl_->ctx, &error_)) {
        stop();
        return false;
    }
    impl_->ctx_ok              = true;
    ma_device_config cfg       = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format         = ma_format_s16;
    cfg.capture.channels       = 1;
    cfg.sampleRate             = kSampleRate;
    cfg.periodSizeInFrames     = static_cast<ma_uint32>(block_samples);
    cfg.dataCallback           = captureCallback;
    cfg.pUserData              = impl_;
    ma_result r                = ma_device_init(&impl_->ctx, &cfg, &impl_->dev);
    if (r != MA_SUCCESS) {
        error_ = std::string("capture init: ") + ma_result_description(r);
        stop();
        return false;
    }
    impl_->dev_ok = true;
    r             = ma_device_start(&impl_->dev);
    if (r != MA_SUCCESS) {
        error_ = std::string("capture start: ") + ma_result_description(r);
        stop();
        return false;
    }
    spdlog::info("Audio: capture started ({} Hz, block {})", kSampleRate, block_samples);
    return true;
}

void Capture::stop()
{
    if (!impl_) return;
    if (impl_->dev_ok) ma_device_uninit(&impl_->dev);
    if (impl_->ctx_ok) ma_context_uninit(&impl_->ctx);
    delete impl_;
    impl_ = nullptr;
}

bool Capture::read(std::vector<int16_t>* block)
{
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->blocks.empty()) return false;
    *block = std::move(impl_->blocks.front());
    impl_->blocks.pop_front();
    return true;
}

struct Playback::Impl {
    ma_context ctx{};
    ma_device dev{};
    bool ctx_ok = false, dev_ok = false;
    mutable std::mutex mutex;
    std::deque<int16_t> queue;
    bool muted = false;
};

namespace {
void playbackCallback(ma_device* dev, void* output, const void*, ma_uint32 frames)
{
    auto* impl = static_cast<Playback::Impl*>(dev->pUserData);
    auto* out  = static_cast<int16_t*>(output);
    std::lock_guard<std::mutex> lock(impl->mutex);
    for (ma_uint32 i = 0; i < frames; ++i) {
        int16_t s = 0;
        if (!impl->queue.empty()) {
            s = impl->queue.front();
            impl->queue.pop_front();
        }
        out[i] = impl->muted ? 0 : s;
    }
}
}  // namespace

Playback::Playback() = default;
Playback::~Playback() { stop(); }

bool Playback::start()
{
    if (impl_) return true;
    impl_ = new Impl();
    if (!initContext(&impl_->ctx, &error_)) {
        stop();
        return false;
    }
    impl_->ctx_ok          = true;
    ma_device_config cfg   = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format    = ma_format_s16;
    cfg.playback.channels  = 1;
    cfg.sampleRate         = kSampleRate;
    cfg.periodSizeInFrames = 320;
    cfg.dataCallback       = playbackCallback;
    cfg.pUserData          = impl_;
    ma_result r            = ma_device_init(&impl_->ctx, &cfg, &impl_->dev);
    if (r != MA_SUCCESS) {
        error_ = std::string("playback init: ") + ma_result_description(r);
        stop();
        return false;
    }
    impl_->dev_ok = true;
    r             = ma_device_start(&impl_->dev);
    if (r != MA_SUCCESS) {
        error_ = std::string("playback start: ") + ma_result_description(r);
        stop();
        return false;
    }
    spdlog::info("Audio: playback started");
    return true;
}

void Playback::stop()
{
    if (!impl_) return;
    if (impl_->dev_ok) ma_device_uninit(&impl_->dev);
    if (impl_->ctx_ok) ma_context_uninit(&impl_->ctx);
    delete impl_;
    impl_ = nullptr;
}

void Playback::write(const int16_t* pcm, std::size_t n)
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    // Cap latency at 2 s: if the queue grows (clock drift), drop the oldest.
    constexpr std::size_t kMax = kSampleRate * 2;
    impl_->queue.insert(impl_->queue.end(), pcm, pcm + n);
    while (impl_->queue.size() > kMax) impl_->queue.pop_front();
}

void Playback::setMuted(bool muted)
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->muted = muted;
}

std::size_t Playback::queued() const
{
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->queue.size();
}

}  // namespace cpzbm::audio
