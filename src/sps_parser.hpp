#pragma once

#include <cstddef>
#include <cstdint>

// Parses an Annex-B H.264 config packet (containing an SPS NAL unit)
// and extracts the exact cropped display width and height.
// Returns true if an SPS NAL unit was found and parsed successfully.
bool parseH264SpsResolution(const uint8_t* data, size_t len, uint32_t& outWidth, uint32_t& outHeight);
