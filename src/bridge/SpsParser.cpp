#include "bridge/SpsParser.hpp"

#include <vector>

namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), bitPos_(0) {}

    bool ok() const { return !overflow_; }

    uint32_t readBits(int count) {
        if (count <= 0) return 0;
        if (count > 32) {
            overflow_ = true;
            return 0;
        }
        uint32_t val = 0;
        for (int i = 0; i < count; ++i) {
            size_t byteIdx = bitPos_ >> 3;
            if (byteIdx >= size_) {
                overflow_ = true;
                return 0;
            }
            int bitIdx = 7 - static_cast<int>(bitPos_ & 7);
            val = (val << 1) | ((data_[byteIdx] >> bitIdx) & 1u);
            ++bitPos_;
        }
        return val;
    }

    uint32_t readUE() {
        int leadingZeros = 0;
        while (ok() && readBits(1) == 0) {
            ++leadingZeros;
            if (leadingZeros > 31) {
                overflow_ = true;
                return 0;
            }
        }
        if (leadingZeros == 0) return 0;
        uint32_t suffix = readBits(leadingZeros);
        return ((1u << leadingZeros) - 1u) + suffix;
    }

    int32_t readSE() {
        uint32_t code = readUE();
        if (code & 1u) {
            return static_cast<int32_t>((code + 1u) >> 1);
        }
        return -static_cast<int32_t>(code >> 1);
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t bitPos_;
    bool overflow_ = false;
};

void skipScalingList(BitReader& br, int sizeOfScalingList) {
    int lastScale = 8;
    int nextScale = 8;
    for (int j = 0; j < sizeOfScalingList && br.ok(); ++j) {
        if (nextScale != 0) {
            int deltaScale = br.readSE();
            nextScale = (lastScale + deltaScale + 256) % 256;
        }
        lastScale = (nextScale == 0) ? lastScale : nextScale;
    }
}

bool parseSpsNal(const uint8_t* nal, size_t nalLen, uint32_t& outWidth, uint32_t& outHeight) {
    if (nalLen < 4) return false;

    std::vector<uint8_t> rbsp;
    rbsp.reserve(nalLen);
    for (size_t i = 1; i < nalLen; ++i) {
        if (i + 2 < nalLen && nal[i] == 0x00 && nal[i + 1] == 0x00 && nal[i + 2] == 0x03) {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
        } else {
            rbsp.push_back(nal[i]);
        }
    }

    BitReader br(rbsp.data(), rbsp.size());
    uint32_t profileIdc = br.readBits(8);
    br.readBits(8);
    br.readBits(8);
    br.readUE();

    uint32_t chromaFormatIdc = 1;
    uint32_t separateColourPlaneFlag = 0;

    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
        profileIdc == 244 || profileIdc == 44  || profileIdc == 83  ||
        profileIdc == 86  || profileIdc == 118 || profileIdc == 128 ||
        profileIdc == 138 || profileIdc == 139 || profileIdc == 134) {
        chromaFormatIdc = br.readUE();
        if (chromaFormatIdc == 3) {
            separateColourPlaneFlag = br.readBits(1);
        }
        br.readUE();
        br.readUE();
        br.readBits(1);
        if (br.readBits(1)) {
            int count = (chromaFormatIdc != 3) ? 8 : 12;
            for (int i = 0; i < count && br.ok(); ++i) {
                if (br.readBits(1)) {
                    skipScalingList(br, (i < 6) ? 16 : 64);
                }
            }
        }
    }

    br.readUE();
    uint32_t picOrderCntType = br.readUE();
    if (picOrderCntType == 0) {
        br.readUE();
    } else if (picOrderCntType == 1) {
        br.readBits(1);
        br.readSE();
        br.readSE();
        uint32_t numRef = br.readUE();
        if (numRef > 256) return false;
        for (uint32_t i = 0; i < numRef && br.ok(); ++i) {
            br.readSE();
        }
    }

    br.readUE();
    br.readBits(1);

    uint32_t picWidthInMbsMinus1 = br.readUE();
    uint32_t picHeightInMapUnitsMinus1 = br.readUE();
    uint32_t frameMbsOnlyFlag = br.readBits(1);
    if (!frameMbsOnlyFlag) {
        br.readBits(1);
    }
    br.readBits(1);

    uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (br.readBits(1)) {
        cropLeft   = br.readUE();
        cropRight  = br.readUE();
        cropTop    = br.readUE();
        cropBottom = br.readUE();
    }

    if (!br.ok()) return false;

    uint32_t width  = (picWidthInMbsMinus1 + 1u) * 16u;
    uint32_t height = (2u - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1u) * 16u;

    uint32_t cropUnitX = 1;
    uint32_t cropUnitY = 2u - frameMbsOnlyFlag;
    if (separateColourPlaneFlag == 0) {
        if (chromaFormatIdc == 1) {
            cropUnitX = 2;
            cropUnitY = 2u * (2u - frameMbsOnlyFlag);
        } else if (chromaFormatIdc == 2) {
            cropUnitX = 2;
            cropUnitY = 2u - frameMbsOnlyFlag;
        }
    }

    uint32_t subW = (cropLeft + cropRight) * cropUnitX;
    uint32_t subH = (cropTop + cropBottom) * cropUnitY;
    if (subW >= width || subH >= height) return false;

    width  -= subW;
    height -= subH;

    if (width < 64 || height < 64 || width > 8192 || height > 8192) return false;
    outWidth  = width;
    outHeight = height;
    return true;
}

} // namespace

bool SpsParser::parseH264Resolution(const uint8_t* data, size_t len,
                                    uint32_t& outWidth, uint32_t& outHeight) {
    if (!data || len < 5) return false;

    size_t i = 0;
    while (i + 4 < len) {
        size_t startCodeLen = 0;
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            startCodeLen = 3;
        } else if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            startCodeLen = 4;
        }

        if (startCodeLen == 0) {
            ++i;
            continue;
        }

        size_t nalStart = i + startCodeLen;
        if (nalStart >= len) break;

        size_t nalEnd = len;
        for (size_t j = nalStart + 1; j + 3 < len; ++j) {
            if (data[j] == 0x00 && data[j + 1] == 0x00 &&
                (data[j + 2] == 0x01 || (data[j + 2] == 0x00 && data[j + 3] == 0x01))) {
                nalEnd = j;
                break;
            }
        }

        uint8_t nalType = data[nalStart] & 0x1Fu;
        if (nalType == 7) {
            if (parseSpsNal(data + nalStart, nalEnd - nalStart, outWidth, outHeight)) {
                return true;
            }
        }
        i = nalEnd;
    }

    return false;
}
