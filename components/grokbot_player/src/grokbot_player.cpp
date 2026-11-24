#include "grokbot_player.h"

#include <cstring>
#include <zlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "grokbot_player";

namespace {
constexpr size_t kGlobalHeaderSize = 16;
constexpr size_t kClipHeaderSize = 32;
constexpr size_t kFrameIndexSize = 8;
constexpr size_t kFrameHeaderSize = 8;

uint16_t read_u16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t *alloc_frame_buffer(size_t size)
{
    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buffer) {
        buffer = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return buffer;
}
}

GrokBotPlayer::~GrokBotPlayer()
{
    release();
}

void GrokBotPlayer::release()
{
    free(packed_);
    free(delta_);
    packed_ = nullptr;
    delta_ = nullptr;
    data_ = nullptr;
    data_size_ = 0;
    width_ = 0;
    height_ = 0;
    fps_ = 0;
    clip_count_ = 0;
    selected_ = -1;
    current_index_ = 0;
    packed_frame_size_ = 0;
    rebuild_pending_ = false;
    memset(clips_, 0, sizeof(clips_));
}

bool GrokBotPlayer::init(const uint8_t *data, size_t size)
{
    release();
    if (!data || size < kGlobalHeaderSize || memcmp(data, "GBZ1", 4) != 0 || read_u16(data + 4) != 1) {
        ESP_LOGE(TAG, "invalid atlas header");
        return false;
    }

    data_ = data;
    data_size_ = size;
    width_ = read_u16(data + 6);
    height_ = read_u16(data + 8);
    fps_ = data[10];
    clip_count_ = data[11];
    if (!width_ || !height_ || !fps_ || clip_count_ <= 0 ||
        clip_count_ > static_cast<int>(sizeof(clips_) / sizeof(clips_[0]))) {
        ESP_LOGE(TAG, "invalid atlas metadata");
        release();
        return false;
    }

    const size_t table_end = kGlobalHeaderSize + static_cast<size_t>(clip_count_) * kClipHeaderSize;
    if (table_end > size) {
        ESP_LOGE(TAG, "clip table truncated");
        release();
        return false;
    }

    for (int i = 0; i < clip_count_; ++i) {
        const uint8_t *header = data + kGlobalHeaderSize + static_cast<size_t>(i) * kClipHeaderSize;
        ClipInfo &clip = clips_[i];
        clip.name = reinterpret_cast<const char *>(header);
        clip.frames = read_u16(header + 16);
        clip.start_frame = read_u16(header + 18);
        clip.offset = read_u32(header + 20);
        clip.size = read_u32(header + 24);
        clip.packed_frame_size = read_u32(header + 28);
        if (!clip.frames || clip.start_frame >= clip.frames || clip.offset < table_end ||
            clip.offset > size || clip.size > size - clip.offset ||
            !clip.packed_frame_size ||
            static_cast<size_t>(clip.frames) * kFrameIndexSize > clip.size) {
            ESP_LOGE(TAG, "invalid clip %d", i);
            release();
            return false;
        }
        if (clip.packed_frame_size != static_cast<uint32_t>(width_) * height_ / 2) {
            ESP_LOGE(TAG, "unexpected packed frame size in clip %d", i);
            release();
            return false;
        }
    }

    packed_frame_size_ = clips_[0].packed_frame_size;
    packed_ = alloc_frame_buffer(packed_frame_size_);
    delta_ = alloc_frame_buffer(packed_frame_size_);
    if (!packed_ || !delta_) {
        ESP_LOGE(TAG, "frame buffer allocation failed");
        release();
        return false;
    }
    return select(0);
}

bool GrokBotPlayer::select(int index)
{
    if (!data_ || index < 0 || index >= clip_count_) {
        return false;
    }
    selected_ = index;
    current_index_ = clips_[index].start_frame;
    rebuild_pending_ = true;
    return true;
}

bool GrokBotPlayer::seek(int frame_index)
{
    if (selected_ < 0 || frame_index < 0 || frame_index >= clips_[selected_].frames) {
        return false;
    }
    current_index_ = frame_index;
    return true;
}

bool GrokBotPlayer::advance()
{
    if (selected_ < 0) {
        return false;
    }
    ++current_index_;
    if (current_index_ >= clips_[selected_].frames) {
        current_index_ = clips_[selected_].start_frame;
        return true;
    }
    return false;
}

bool GrokBotPlayer::retreat()
{
    if (selected_ < 0 || current_index_ <= 0) {
        return false;
    }
    --current_index_;
    return true;
}

bool GrokBotPlayer::frame_info(int frame_index, FrameInfo *frame) const
{
    if (!frame || !data_ || selected_ < 0 || frame_index < 0 || frame_index >= clips_[selected_].frames) {
        return false;
    }
    const ClipInfo &clip = clips_[selected_];
    const size_t index_pos = clip.offset + static_cast<size_t>(frame_index) * kFrameIndexSize;
    const uint32_t record_offset = read_u32(data_ + index_pos);
    const uint32_t size_flags = read_u32(data_ + index_pos + 4);
    const uint32_t compressed_size = size_flags & 0x7fffffffU;
    if (record_offset > clip.size || clip.size - record_offset < kFrameHeaderSize) {
        return false;
    }
    const size_t record_pos = clip.offset + record_offset;
    if (compressed_size > clip.size - record_offset - kFrameHeaderSize) {
        return false;
    }
    if (read_u32(data_ + record_pos) != compressed_size || data_[record_pos + 4] != (size_flags >> 31)) {
        return false;
    }
    frame->data = data_ + record_pos + kFrameHeaderSize;
    frame->compressed_size = compressed_size;
    frame->delta = (size_flags >> 31) != 0;
    return true;
}

bool GrokBotPlayer::decode_frame(const FrameInfo &frame, bool delta)
{
    if (!packed_ || !delta_ || !frame.data || !frame.compressed_size || frame.delta != delta) {
        return false;
    }
    uLongf unpacked = packed_frame_size_;
    const int result = uncompress(delta_, &unpacked, frame.data, frame.compressed_size);
    if (result != Z_OK || unpacked != packed_frame_size_) {
        ESP_LOGE(TAG, "inflate failed: rc=%d size=%lu/%u", result,
                 static_cast<unsigned long>(unpacked), static_cast<unsigned>(packed_frame_size_));
        return false;
    }
    if (delta) {
        for (size_t i = 0; i < packed_frame_size_; ++i) {
            packed_[i] ^= delta_[i];
        }
    } else {
        memcpy(packed_, delta_, packed_frame_size_);
    }
    return true;
}

bool GrokBotPlayer::decode_current()
{
    FrameInfo frame = {};
    return frame_info(current_index_, &frame) && decode_frame(frame, frame.delta);
}

bool GrokBotPlayer::decode_backward()
{
    FrameInfo frame = {};
    if (!frame_info(current_index_, &frame) || !frame.delta || !decode_frame(frame, true)) {
        return false;
    }
    return retreat();
}

bool GrokBotPlayer::rebuild_current()
{
    if (selected_ < 0) {
        return false;
    }
    const int target = current_index_;
    if (!seek(0)) {
        return false;
    }
    for (int i = 0; i <= target; ++i) {
        FrameInfo frame = {};
        if (!frame_info(i, &frame) || !decode_frame(frame, frame.delta)) {
            seek(target);
            return false;
        }
    }
    if (!seek(target)) {
        return false;
    }
    rebuild_pending_ = false;
    return true;
}

int GrokBotPlayer::selected_frame_count() const
{
    return selected_ < 0 ? 0 : clips_[selected_].frames;
}

int GrokBotPlayer::start_frame() const
{
    return selected_ < 0 ? 0 : clips_[selected_].start_frame;
}

const char *GrokBotPlayer::clip_name(int index) const
{
    return index >= 0 && index < clip_count_ ? clips_[index].name : "";
}

const char *GrokBotPlayer::selected_name() const
{
    return clip_name(selected_);
}
