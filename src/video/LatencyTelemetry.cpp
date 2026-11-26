#include "video/LatencyTelemetry.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

struct LatencyTelemetry::Impl {
    CRITICAL_SECTION lock;
    FrameTiming ring[kRingCapacity];
    size_t ringHead = 0;
    size_t ringCount = 0;
    uint64_t decodedCount = 0;
    uint64_t presentedCount = 0;
    uint64_t skippedCount = 0;

    Impl() {
        InitializeCriticalSection(&lock);
    }
    ~Impl() {
        DeleteCriticalSection(&lock);
    }
};

namespace {

int64_t queryQpcFreq() {
    static int64_t freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (f.QuadPart > 0) ? f.QuadPart : 10000000LL;
    }
    return freq;
}

} // namespace

LatencyTelemetry::LatencyTelemetry() : impl_(new Impl()) {}

LatencyTelemetry::~LatencyTelemetry() {
    delete impl_;
    impl_ = nullptr;
}

int64_t LatencyTelemetry::nowQpc() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<int64_t>(now.QuadPart);
}

double LatencyTelemetry::qpcToMs(int64_t deltaTicks) {
    if (deltaTicks <= 0) return 0.0;
    return (static_cast<double>(deltaTicks) * 1000.0) / static_cast<double>(queryQpcFreq());
}

double LatencyTelemetry::computePercentile(std::vector<double>& vals, double p) {
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    if (p <= 0.0) return vals.front();
    if (p >= 1.0) return vals.back();
    double pos = p * static_cast<double>(vals.size() - 1);
    size_t idx = static_cast<size_t>(std::floor(pos));
    size_t nxt = (idx + 1 < vals.size()) ? (idx + 1) : idx;
    double frac = pos - static_cast<double>(idx);
    return vals[idx] * (1.0 - frac) + vals[nxt] * frac;
}

void LatencyTelemetry::reset() {
    EnterCriticalSection(&impl_->lock);
    impl_->ringHead = 0;
    impl_->ringCount = 0;
    impl_->decodedCount = 0;
    impl_->presentedCount = 0;
    impl_->skippedCount = 0;
    LeaveCriticalSection(&impl_->lock);
}

void LatencyTelemetry::recordPresented(const FrameTiming& ft) {
    EnterCriticalSection(&impl_->lock);
    ++impl_->decodedCount;
    ++impl_->presentedCount;
    impl_->ring[impl_->ringHead] = ft;
    impl_->ringHead = (impl_->ringHead + 1) % kRingCapacity;
    if (impl_->ringCount < kRingCapacity) ++impl_->ringCount;
    LeaveCriticalSection(&impl_->lock);
}

void LatencyTelemetry::recordSkipped(const FrameTiming& ft) {
    (void)ft;
    EnterCriticalSection(&impl_->lock);
    ++impl_->decodedCount;
    ++impl_->skippedCount;
    LeaveCriticalSection(&impl_->lock);
}

TelemetrySummary LatencyTelemetry::getSummary() const {
    std::vector<double> rxToPres;
    std::vector<double> rxToDec;
    std::vector<double> decodeMs;
    std::vector<double> presMs;
    TelemetrySummary sum;
    int64_t minDecQpc = 0;
    int64_t maxDecQpc = 0;
    size_t decQpcCount = 0;

    EnterCriticalSection(&impl_->lock);
    sum.decodedCount = impl_->decodedCount;
    sum.presentedCount = impl_->presentedCount;
    sum.skippedCount = impl_->skippedCount;
    const size_t n = impl_->ringCount;
    rxToPres.reserve(n);
    rxToDec.reserve(n);
    decodeMs.reserve(n);
    presMs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const FrameTiming& ft = impl_->ring[i];
        if (ft.qpcPktRecv > 0 && ft.qpcPresentRet >= ft.qpcPktRecv) {
            rxToPres.push_back(qpcToMs(ft.qpcPresentRet - ft.qpcPktRecv));
        }
        if (ft.qpcPktRecv > 0 && ft.qpcDecoded >= ft.qpcPktRecv) {
            rxToDec.push_back(qpcToMs(ft.qpcDecoded - ft.qpcPktRecv));
        }
        if (ft.qpcDecoded > 0) {
            if (decQpcCount == 0 || ft.qpcDecoded < minDecQpc) minDecQpc = ft.qpcDecoded;
            if (decQpcCount == 0 || ft.qpcDecoded > maxDecQpc) maxDecQpc = ft.qpcDecoded;
            ++decQpcCount;
        }
        if (ft.qpcProcIn > 0 && ft.qpcDecoded >= ft.qpcProcIn) {
            decodeMs.push_back(qpcToMs(ft.qpcDecoded - ft.qpcProcIn));
        }
        if (ft.qpcPresentCall > 0 && ft.qpcPresentRet >= ft.qpcPresentCall) {
            presMs.push_back(qpcToMs(ft.qpcPresentRet - ft.qpcPresentCall));
        }
    }
    LeaveCriticalSection(&impl_->lock);

    sum.sampleCount = static_cast<uint32_t>(rxToPres.size());
    sum.rxToPresentP50Ms = computePercentile(rxToPres, 0.50);
    sum.rxToPresentP95Ms = computePercentile(rxToPres, 0.95);
    sum.rxToDecodeP50Ms = computePercentile(rxToDec, 0.50);
    sum.rxToDecodeP95Ms = computePercentile(rxToDec, 0.95);
    sum.decodeP50Ms = computePercentile(decodeMs, 0.50);
    sum.decodeP95Ms = computePercentile(decodeMs, 0.95);
    sum.presentP50Ms = computePercentile(presMs, 0.50);
    if (decQpcCount >= 2 && maxDecQpc > minDecQpc) {
        double spanSec = qpcToMs(maxDecQpc - minDecQpc) / 1000.0;
        if (spanSec > 0.01) {
            sum.measuredFps = static_cast<double>(decQpcCount - 1) / spanSec;
        }
    }
    return sum;
}

bool LatencyTelemetry::getLastTiming(FrameTiming& outFt) const {
    EnterCriticalSection(&impl_->lock);
    bool ok = (impl_->ringCount > 0);
    if (ok) {
        size_t lastIdx = (impl_->ringHead + kRingCapacity - 1) % kRingCapacity;
        outFt = impl_->ring[lastIdx];
    }
    LeaveCriticalSection(&impl_->lock);
    return ok;
}
