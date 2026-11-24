#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

class GrokBotPlayer {
public:
    struct FrameInfo {
        const uint8_t *data;
        uint32_t compressed_size;
        bool delta;
    };

    GrokBotPlayer() = default;
    ~GrokBotPlayer();

    GrokBotPlayer(const GrokBotPlayer &) = delete;
    GrokBotPlayer &operator=(const GrokBotPlayer &) = delete;

    bool init(const char *path);
    void release();

    int clip_count() const { return clip_count_; }
    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    uint8_t fps() const { return fps_; }

    bool select(int index);
    bool seek(int frame_index);
    bool advance();
    bool retreat();
    bool decode_current();
    bool decode_backward();
    bool rebuild_current();

    int current_index() const { return current_index_; }
    int selected_index() const { return selected_; }
    int selected_frame_count() const;
    int start_frame() const;
    bool rebuild_pending() const { return rebuild_pending_; }
    const char *clip_name(int index) const;
    const char *selected_name() const;
    uint32_t packed_frame_size() const { return packed_frame_size_; }
    const uint8_t *packed_frame() const { return packed_; }

private:
    struct ClipInfo {
        char name[17] = {};
        uint16_t frames = 0;
        uint16_t start_frame = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t packed_frame_size = 0;
    };

    bool frame_info(int frame_index, FrameInfo *frame) const;
    bool decode_frame(const FrameInfo &frame, bool delta);

    FILE *file_ = nullptr;
    const uint8_t *data_ = nullptr;
    size_t data_size_ = 0;
    uint8_t *clip_data_ = nullptr;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint8_t fps_ = 0;
    int clip_count_ = 0;
    int selected_ = -1;
    int current_index_ = 0;
    uint32_t packed_frame_size_ = 0;
    bool rebuild_pending_ = false;
    ClipInfo clips_[64] = {};
    uint8_t *packed_ = nullptr;
    uint8_t *delta_ = nullptr;
};
