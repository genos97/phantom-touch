#pragma once

#include <cstddef>
#include <cstdint>

class SpsParser {
public:
    // Parses an H.264 Annex-B SPS NAL unit to extract display resolution (handles auto-rotation)
    static bool parseH264Resolution(const uint8_t* data, size_t size,
                                    uint32_t& outWidth, uint32_t& outHeight);
};
