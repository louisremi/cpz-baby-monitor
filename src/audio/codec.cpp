#include "audio/codec.hpp"

#include <codec2/codec2.h>

namespace cpzbm::audio {

Codec::Codec(Mode mode) : mode_(mode)
{
    c2_ = codec2_create(static_cast<int>(mode));
    if (c2_) {
        samples_ = codec2_samples_per_frame(c2_);
        bytes_   = codec2_bytes_per_frame(c2_);
    }
}

Codec::~Codec()
{
    if (c2_) codec2_destroy(c2_);
}

void Codec::encode(const int16_t* pcm, std::vector<uint8_t>* out)
{
    const std::size_t at = out->size();
    out->resize(at + static_cast<std::size_t>(bytes_));
    // codec2_encode takes a non-const pointer but does not modify the input.
    codec2_encode(c2_, out->data() + at, const_cast<int16_t*>(pcm));
}

void Codec::decode(const uint8_t* bits, int16_t* pcm) { codec2_decode(c2_, pcm, bits); }

}  // namespace cpzbm::audio
