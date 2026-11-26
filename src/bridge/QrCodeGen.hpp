#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct QrMatrix {
    int size = 0;               // Module dimension (e.g. 29x29 for V3, 33x33 for V4)
    std::vector<uint8_t> cells; // size * size, 1 = dark module, 0 = light module

    bool at(int x, int y) const {
        if (x < 0 || y < 0 || x >= size || y >= size) return false;
        return cells[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] != 0;
    }
};

// Zero-dependency ISO/IEC 18004 QR Code Encoder (Byte mode, ECC Level M, Versions 1..6).
// Generates real scannable Android 11+ Wireless ADB Pairing QR codes:
//   "WIFI:T:ADB;S:<serviceName>;P:<password>;;"
class QrCodeGen {
public:
    static bool encode(const std::string& text, QrMatrix& outMatrix);
    static std::string buildAndroidAdbPairPayload(const std::string& serviceName, const std::string& password);
};
