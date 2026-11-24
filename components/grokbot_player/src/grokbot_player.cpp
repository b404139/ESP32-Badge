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
    if (file_) {
        fclose(file_);
    }
    free(packed_);
    free(delta_);
    free(clip_data_);
    file_ = nullptr;
    packed_ = nullptr;
    delta_ = nullptr;
    clip_data_ = nullptr;
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

bool GrokBotPlayer::init(const char *path)
{
    release();
    if (!path) {
        return false;
    }
    file_ = fopen(path, "rb");
    if (!file_) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }
    if (fseek(file_, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "atlas seek failed");
        release();
        return false;
    }
    const long file_size = ftell(file_);
    if (file_size < static_cast<long>(kGlobalHeaderSize) || fseek(file_, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "invalid atlas size");
        release();
        return false;
    }

    uint8_t header[kGlobalHeaderSize];
    if (fread(header, 1, sizeof(header), file_) != sizeof(header) ||
        memcmp(header, "GBZ1", 4) != 0 || read_u16(header + 4) != 1) {
        ESP_LOGE(TAG, "invalid atlas header");
        release();
        return false;
    }

    width_ = read_u16(header + 6);
    height_ = read_u16(header + 8);
    fps_ = header[10];
    clip_count_ = header[11];
    if (!width_ || !height_ || !fps_ || clip_count_ <= 0 ||
        clip_count_ > static_cast<int>(sizeof(clips_) / sizeof(clips_[0])) || header[15] != 0) {
        ESP_LOGE(TAG, "invalid atlas metadata");
        release();
        return false;
    }

    const size_t table_end = kGlobalHeaderSize + static_cast<size_t>(clip_count_) * kClipHeaderSize;
    if (static_cast<size_t>(file_size) < table_end) {
        ESP_LOGE(TAG, "clip table truncated");
        release();
        return false;
    }

    uint32_t max_clip_size = 0;
    for (int i = 0; i < clip_count_; ++i) {
        uint8_t raw[kClipHeaderSize];
        if (fread(raw, 1, sizeof(raw), file_) != sizeof(raw)) {
            ESP_LOGE(TAG, "clip table read failed");
            release();
            return false;
        }
        ClipInfo &clip = clips_[i];
        memcpy(clip.name, raw, 16);
        clip.name[16] = '\0';
        clip.frames = read_u16(raw + 16);
        clip.start_frame = read_u16(raw + 18);
        clip.offset = read_u32(raw + 20);
        clip.size = read_u32(raw + 24);
        clip.packed_frame_size = read_u32(raw + 28);
        if (!clip.name[0] || !clip.frames || clip.start_frame >= clip.frames || clip.offset < table_end ||
            clip.offset > static_cast<uint32_t>(file_size) ||
            clip.size > static_cast<uint32_t>(file_size) - clip.offset ||
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
        if (clip.size > max_clip_size) {
            max_clip_size = clip.size;
        }
    }

    packed_frame_size_ = clips_[0].packed_frame_size;
    clip_data_ = alloc_frame_buffer(max_clip_size);
    packed_ = alloc_frame_buffer(packed_frame_size_);
    delta_ = alloc_frame_buffer(packed_frame_size_);
    if (!clip_data_ || !packed_ || !delta_) {
        ESP_LOGE(TAG, "buffer allocation failed");
        release();
        return false;
    }
    if (!select(0)) {
        release();
        return false;
    }
    ESP_LOGI(TAG, "atlas ready clips=%d max_clip=%lu", clip_count_,
             static_cast<unsigned long>(max_clip_size));
    return true;
}

bool GrokBotPlayer::select(int index)
{
    if (!file_ || !clip_data_ || index < 0 || index >= clip_count_) {
        return false;
    }
    const ClipInfo &clip = clips_[index];
    if (fseek(file_, static_cast<long>(clip.offset), SEEK_SET) != 0 ||
        fread(clip_data_, 1, clip.size, file_) != clip.size) {
        ESP_LOGE(TAG, "load clip %s failed", clip.name);
        selected_ = -1;
        current_index_ = 0;
        rebuild_pending_ = false;
        return false;
    }
    data_ = clip_data_;
    data_size_ = clip.size;
    selected_ = index;
    current_index_ = clip.start_frame;
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
    const size_t index_pos = static_cast<size_t>(frame_index) * kFrameIndexSize;
    if (index_pos > data_size_ || data_size_ - index_pos < kFrameIndexSize) {
        return false;
    }
    const uint32_t record_offset = read_u32(data_ + index_pos);
    const uint32_t size_flags = read_u32(data_ + index_pos + 4);
    const uint32_t compressed_size = size_flags & 0x7fffffffU;
    if (record_offset > data_size_ || data_size_ - record_offset < kFrameHeaderSize) {
        return false;
    }
    const size_t record_pos = record_offset;
    if (compressed_size > data_size_ - record_pos - kFrameHeaderSize) {
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
                 static_cast<unsigned long>(unpacked),
                 static_cast<unsigned>(packed_frame_size_));
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
