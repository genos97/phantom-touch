#include "bridge/QrCodeGen.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace {

// ISO/IEC 18004 Version 1..6 parameters for Error Correction Level M
struct QrVerSpec {
    int version;
    int size;
    int dataCodewords;  // Total data codewords across all blocks
    int ecPerBlock;     // EC codewords per block
    int numBlocks;      // Number of RS blocks (all equal size for V1..V6 Level M!)
    int alignCenter;    // Single alignment center coordinate (0 if V1)
};

// Verified exact ISO/IEC 18004 Table 9 values for ECC Level M (Versions 1..6):
//   V1-M: 21x21, 16 data, 10 ec, 1 block  (max 14 bytes)
//   V2-M: 25x25, 28 data, 16 ec, 1 block  (max 26 bytes)
//   V3-M: 29x29, 44 data, 26 ec, 1 block  (max 42 bytes)
//   V4-M: 33x33, 64 data, 18 ec, 2 blocks (max 62 bytes)  <- fits Android ADB WIFI:T:ADB payload!
//   V5-M: 37x37, 86 data, 24 ec, 2 blocks (max 84 bytes)
//   V6-M: 41x41, 108 data, 16 ec, 4 blocks (max 106 bytes)
const QrVerSpec kSpecs[6] = {
    { 1, 21,  16, 10, 1,  0 },
    { 2, 25,  28, 16, 1, 18 },
    { 3, 29,  44, 26, 1, 22 },
    { 4, 33,  64, 18, 2, 26 },
    { 5, 37,  86, 24, 2, 30 },
    { 6, 41, 108, 16, 4, 34 }
};

uint8_t gfMul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        bool hi = (a & 0x80) != 0;
        a = static_cast<uint8_t>(a << 1);
        if (hi) a ^= 0x1D; // Primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D)
        b >>= 1;
    }
    return p;
}

void computeRsGenerator(int degree, std::vector<uint8_t>& outPoly) {
    outPoly.assign(static_cast<size_t>(degree), 0);
    outPoly[static_cast<size_t>(degree - 1)] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            outPoly[static_cast<size_t>(j)] = gfMul(outPoly[static_cast<size_t>(j)], root);
            if (j + 1 < degree) {
                outPoly[static_cast<size_t>(j)] ^= outPoly[static_cast<size_t>(j + 1)];
            }
        }
        root = gfMul(root, 0x02);
    }
}

void computeRsRemainder(const uint8_t* data, int dataLen, const std::vector<uint8_t>& gen,
                        std::vector<uint8_t>& outRem) {
    int deg = static_cast<int>(gen.size());
    outRem.assign(static_cast<size_t>(deg), 0);
    for (int i = 0; i < dataLen; ++i) {
        uint8_t factor = static_cast<uint8_t>(data[i] ^ outRem[0]);
        for (int j = 0; j < deg - 1; ++j) {
            outRem[static_cast<size_t>(j)] = outRem[static_cast<size_t>(j + 1)];
        }
        outRem[static_cast<size_t>(deg - 1)] = 0;
        for (int j = 0; j < deg; ++j) {
            outRem[static_cast<size_t>(j)] ^= gfMul(gen[static_cast<size_t>(j)], factor);
        }
    }
}

void appendBits(std::vector<uint8_t>& bits, uint32_t val, int len) {
    for (int i = len - 1; i >= 0; --i) {
        bits.push_back(static_cast<uint8_t>((val >> i) & 1u));
    }
}

// ISO/IEC 18004 Format Information (ECC Level M = 00, mask 0..7) with BCH(15,5) + XOR 0x5412
uint16_t computeFormatBits(int eccLevelBits, int mask) {
    int data = (eccLevelBits << 3) | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }
    return static_cast<uint16_t>(((data << 10) | rem) ^ 0x5412);
}

bool maskCondition(int mask, int r, int c) {
    switch (mask) {
        case 0: return ((r + c) % 2) == 0;
        case 1: return (r % 2) == 0;
        case 2: return (c % 3) == 0;
        case 3: return ((r + c) % 3) == 0;
        case 4: return (((r / 2) + (c / 3)) % 2) == 0;
        case 5: return ((r * c) % 2) + ((r * c) % 3) == 0;
        case 6: return ((((r * c) % 2) + ((r * c) % 3)) % 2) == 0;
        case 7: return ((((r + c) % 2) + ((r * c) % 3)) % 2) == 0;
        default: return false;
    }
}

int evaluatePenalty(const std::vector<uint8_t>& mod, int sz) {
    int penalty = 0;
    // Rule 1: adjacent runs >= 5 in rows and columns
    for (int r = 0; r < sz; ++r) {
        int run = 1;
        for (int c = 1; c < sz; ++c) {
            if (mod[r * sz + c] == mod[r * sz + c - 1]) {
                ++run;
                if (run == 5) penalty += 3;
                else if (run > 5) penalty += 1;
            } else {
                run = 1;
            }
        }
    }
    for (int c = 0; c < sz; ++c) {
        int run = 1;
        for (int r = 1; r < sz; ++r) {
            if (mod[r * sz + c] == mod[(r - 1) * sz + c]) {
                ++run;
                if (run == 5) penalty += 3;
                else if (run > 5) penalty += 1;
            } else {
                run = 1;
            }
        }
    }
    // Rule 2: 2x2 solid blocks
    for (int r = 0; r < sz - 1; ++r) {
        for (int c = 0; c < sz - 1; ++c) {
            uint8_t v = mod[r * sz + c];
            if (v == mod[r * sz + c + 1] && v == mod[(r + 1) * sz + c] && v == mod[(r + 1) * sz + c + 1]) {
                penalty += 3;
            }
        }
    }
    return penalty;
}

} // namespace

std::string QrCodeGen::buildAndroidAdbPairPayload(const std::string& serviceName, const std::string& password) {
    return "WIFI:T:ADB;S:" + serviceName + ";P:" + password + ";;";
}

std::string QrCodeGen::generateSecurePairingCode(size_t length) {
    if (length == 0) return "";
    static const char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const size_t kAlphaLen = sizeof(kAlphabet) - 1; // 62 chars
    const unsigned int kLimit = (256u / static_cast<unsigned int>(kAlphaLen)) * static_cast<unsigned int>(kAlphaLen); // 248

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
    typedef LONG (WINAPI *PFN_BCryptGenRandom)(void*, unsigned char*, unsigned long, unsigned long);
    HMODULE hBcrypt = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_BCryptGenRandom fnRand = hBcrypt
        ? reinterpret_cast<PFN_BCryptGenRandom>(reinterpret_cast<void*>(GetProcAddress(hBcrypt, "BCryptGenRandom")))
        : nullptr;

    std::string out;
    out.reserve(length);
    unsigned char buf[64];
    while (out.size() < length) {
        bool ok = false;
        if (fnRand) {
            if (fnRand(NULL, buf, sizeof(buf), 0x00000002u /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */) == 0) {
                ok = true;
            }
        }
        if (!ok) {
            // Cryptographically non-ideal fallback only if bcrypt unavailable (should never happen on Win7+)
            LARGE_INTEGER pc;
            for (size_t i = 0; i < sizeof(buf); ++i) {
                QueryPerformanceCounter(&pc);
                buf[i] = static_cast<unsigned char>((pc.QuadPart ^ (GetTickCount() + i * 31u)) & 0xFFu);
            }
        }
        for (size_t i = 0; i < sizeof(buf) && out.size() < length; ++i) {
            if (buf[i] < kLimit) {
                out.push_back(kAlphabet[buf[i] % kAlphaLen]);
            }
        }
    }
    if (hBcrypt) FreeLibrary(hBcrypt);
    return out;
}

bool QrCodeGen::encode(const std::string& text, QrMatrix& outMatrix) {
    // Pick smallest Version 1..6 (Level M) that fits [4b mode][8b len][8b*N payload][4b terminator]
    const QrVerSpec* spec = nullptr;
    for (int i = 0; i < 6; ++i) {
        int maxBytes = kSpecs[i].dataCodewords - 2; // 4b mode + 8b count + 4b term = 16 bits = 2 bytes
        if (static_cast<int>(text.size()) <= maxBytes) {
            spec = &kSpecs[i];
            break;
        }
    }
    if (!spec) return false;

    // 1. Encode Byte-Mode bitstream + terminator + pad bytes (0xEC, 0x11)
    std::vector<uint8_t> bits;
    bits.reserve(static_cast<size_t>(spec->dataCodewords) * 8);
    appendBits(bits, 0x4u, 4); // Byte mode indicator
    appendBits(bits, static_cast<uint32_t>(text.size()), 8);
    for (size_t i = 0; i < text.size(); ++i) {
        appendBits(bits, static_cast<uint8_t>(text[i]), 8);
    }
    int totalDataBits = spec->dataCodewords * 8;
    int termLen = totalDataBits - static_cast<int>(bits.size());
    if (termLen > 4) termLen = 4;
    if (termLen > 0) appendBits(bits, 0u, termLen);
    while ((bits.size() % 8) != 0) bits.push_back(0);

    std::vector<uint8_t> dataCw(static_cast<size_t>(spec->dataCodewords), 0);
    size_t byteIdx = 0;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t b = 0;
        for (int k = 0; k < 8; ++k) b = static_cast<uint8_t>((b << 1) | bits[i + k]);
        dataCw[byteIdx++] = b;
    }
    const uint8_t padBytes[2] = { 0xEC, 0x11 };
    int padToggle = 0;
    while (static_cast<int>(byteIdx) < spec->dataCodewords) {
        dataCw[byteIdx++] = padBytes[padToggle];
        padToggle ^= 1;
    }

    // 2. Compute Reed-Solomon EC codewords per block & interleave
    int blockDataLen = spec->dataCodewords / spec->numBlocks;
    std::vector<uint8_t> rsGen;
    computeRsGenerator(spec->ecPerBlock, rsGen);

    std::vector<std::vector<uint8_t>> dataBlocks(static_cast<size_t>(spec->numBlocks));
    std::vector<std::vector<uint8_t>> ecBlocks(static_cast<size_t>(spec->numBlocks));
    for (int b = 0; b < spec->numBlocks; ++b) {
        dataBlocks[b].assign(dataCw.begin() + b * blockDataLen, dataCw.begin() + (b + 1) * blockDataLen);
        computeRsRemainder(dataBlocks[b].data(), blockDataLen, rsGen, ecBlocks[b]);
    }

    std::vector<uint8_t> finalCw;
    finalCw.reserve(static_cast<size_t>(spec->dataCodewords + spec->ecPerBlock * spec->numBlocks));
    for (int i = 0; i < blockDataLen; ++i) {
        for (int b = 0; b < spec->numBlocks; ++b) finalCw.push_back(dataBlocks[b][i]);
    }
    for (int i = 0; i < spec->ecPerBlock; ++i) {
        for (int b = 0; b < spec->numBlocks; ++b) finalCw.push_back(ecBlocks[b][i]);
    }

    // 3. Place function patterns on the sz x sz grid
    const int sz = spec->size;
    std::vector<uint8_t> base(static_cast<size_t>(sz * sz), 0);
    std::vector<uint8_t> isFunc(static_cast<size_t>(sz * sz), 0);

    auto setFunc = [&](int r, int c, bool dark) {
        if (r < 0 || c < 0 || r >= sz || c >= sz) return;
        base[r * sz + c] = dark ? 1u : 0u;
        isFunc[r * sz + c] = 1u;
    };

    // Timing patterns
    for (int i = 0; i < sz; ++i) {
        setFunc(6, i, (i % 2) == 0);
        setFunc(i, 6, (i % 2) == 0);
    }

    // Three 7x7 finder patterns + 1-module white separators
    auto placeFinder = [&](int topR, int topC) {
        for (int dr = -1; dr <= 7; ++dr) {
            for (int dc = -1; dc <= 7; ++dc) {
                int r = topR + dr;
                int c = topC + dc;
                if (r < 0 || c < 0 || r >= sz || c >= sz) continue;
                bool dark = false;
                if (dr >= 0 && dr <= 6 && dc >= 0 && dc <= 6) {
                    if (dr == 0 || dr == 6 || dc == 0 || dc == 6) dark = true;
                    else if (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4) dark = true;
                }
                setFunc(r, c, dark);
            }
        }
    };
    placeFinder(0, 0);
    placeFinder(0, sz - 7);
    placeFinder(sz - 7, 0);

    // Alignment pattern (Version >= 2)
    if (spec->alignCenter > 0) {
        int ac = spec->alignCenter;
        for (int dr = -2; dr <= 2; ++dr) {
            for (int dc = -2; dc <= 2; ++dc) {
                int dist = (dr < 0 ? -dr : dr);
                int dcAbs = (dc < 0 ? -dc : dc);
                if (dcAbs > dist) dist = dcAbs;
                setFunc(ac + dr, ac + dc, dist != 1);
            }
        }
    }

    // Reserve format information strips + mandatory dark module at (sz - 8, 8)
    for (int i = 0; i <= 8; ++i) {
        if (i != 6) {
            setFunc(8, i, false);
            setFunc(i, 8, false);
        }
    }
    for (int i = sz - 8; i < sz; ++i) {
        setFunc(8, i, false);
        setFunc(i, 8, false);
    }
    setFunc(sz - 8, 8, true); // Dark module

    // 4. Zigzag place final data + EC bits
    size_t bitPos = 0;
    size_t totalBits = finalCw.size() * 8;
    bool upward = true;
    for (int rightCol = sz - 1; rightCol >= 1; rightCol -= 2) {
        if (rightCol == 6) rightCol = 5; // Skip vertical timing column
        for (int step = 0; step < sz; ++step) {
            int r = upward ? (sz - 1 - step) : step;
            for (int k = 0; k < 2; ++k) {
                int c = rightCol - k;
                if (!isFunc[r * sz + c]) {
                    bool dark = false;
                    if (bitPos < totalBits) {
                        uint8_t byteVal = finalCw[bitPos >> 3];
                        dark = ((byteVal >> (7 - (bitPos & 7))) & 1u) != 0;
                        ++bitPos;
                    }
                    base[r * sz + c] = dark ? 1u : 0u;
                }
            }
        }
        upward = !upward;
    }

    // 5. Evaluate all 8 standard masks and pick the lowest penalty
    int bestMask = 0;
    int bestPenalty = 0x7FFFFFFF;
    std::vector<uint8_t> bestGrid;

    for (int mask = 0; mask < 8; ++mask) {
        std::vector<uint8_t> cand = base;
        for (int r = 0; r < sz; ++r) {
            for (int c = 0; c < sz; ++c) {
                if (!isFunc[r * sz + c] && maskCondition(mask, r, c)) {
                    cand[r * sz + c] ^= 1u;
                }
            }
        }
        // Write 15-bit format info for ECC Level M (format indicator = 00)
        uint16_t fmt = computeFormatBits(0 /* M */, mask);
        for (int i = 0; i <= 5; ++i) cand[i * sz + 8] = static_cast<uint8_t>((fmt >> i) & 1u);
        cand[7 * sz + 8] = static_cast<uint8_t>((fmt >> 6) & 1u);
        cand[8 * sz + 8] = static_cast<uint8_t>((fmt >> 7) & 1u);
        cand[8 * sz + 7] = static_cast<uint8_t>((fmt >> 8) & 1u);
        for (int i = 9; i < 15; ++i) cand[8 * sz + (14 - i)] = static_cast<uint8_t>((fmt >> i) & 1u);

        for (int i = 0; i < 8; ++i) cand[8 * sz + (sz - 1 - i)] = static_cast<uint8_t>((fmt >> i) & 1u);
        for (int i = 8; i < 15; ++i) cand[(sz - 15 + i) * sz + 8] = static_cast<uint8_t>((fmt >> i) & 1u);
        cand[(sz - 8) * sz + 8] = 1u;

        int pen = evaluatePenalty(cand, sz);
        if (pen < bestPenalty) {
            bestPenalty = pen;
            bestMask = mask;
            bestGrid = cand;
        }
    }
    (void)bestMask;

    outMatrix.size = sz;
    outMatrix.cells.swap(bestGrid);
    return true;
}
