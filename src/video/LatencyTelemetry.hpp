#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FrameTiming {
    int64_t qpcPktRecv = 0;
    int64_t qpcProcIn = 0;
    int64_t qpcDecoded = 0;
    int64_t qpcPresentCall = 0;
    int64_t qpcPresentRet = 0;
    bool present = true;
};

struct TelemetrySummary {
    double rxToPresentP50Ms = 0.0;
    double rxToPresentP95Ms = 0.0;
    double rxToDecodeP50Ms = 0.0;
    double rxToDecodeP95Ms = 0.0;
    double decodeP50Ms = 0.0;
    double decodeP95Ms = 0.0;
    double presentP50Ms = 0.0;
    double measuredFps = 0.0;
    uint64_t decodedCount = 0;
    uint64_t presentedCount = 0;
    uint64_t skippedCount = 0;
    uint32_t sampleCount = 0;
};

struct ProbeResult {
    bool valid = false;
    bool running = false;
    int completed = 0;
    int total = 0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    std::string summaryLine;
};

// Thread-safe 600-frame QPC pipeline telemetry ring buffer (LATENCY_PLAN 0.1 + 1.1).
class LatencyTelemetry {
public:
    static const size_t kRingCapacity = 600;

    LatencyTelemetry();
    ~LatencyTelemetry();

    static int64_t nowQpc();
    static double qpcToMs(int64_t deltaTicks);
    static double computePercentile(std::vector<double>& vals, double p);

    void reset();
    void recordPresented(const FrameTiming& ft);
    void recordSkipped(const FrameTiming& ft);
    TelemetrySummary getSummary() const;
    bool getLastTiming(FrameTiming& outFt) const;

private:
    struct Impl;
    Impl* impl_;
};
