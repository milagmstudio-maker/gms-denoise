#pragma once
// DfnRunner: the two DeepFilterNet3 modes ("AI Deep", "AI Deep HQ") around the
// Rust bridge in third_party/dfbridge.
//
// The networks are big (about 0.4-0.6 s to load, ~100 MB each), so they are
// loaded only when a mode needs them, off the audio thread (load()). Until a
// model is loaded and warmed up, process() outputs the input delayed by the
// mode's latency, then crossfades to the network output, so switching never
// clicks and the reported latency is always right.
//
// Thread use: prepare() and load() on non-audio threads (they lock each other
// out); process()/idle() on the audio thread only; the audio thread never
// waits for a lock.

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

struct Dfb;

namespace zd {

struct DfnParams {
    float attenDb = 100.0f; // max attenuation, >= 100 = no limit
    float guard = 1.0f;     // voice guard: 0 off, 1 default, up to 2 = stronger
    float pfBeta = 0.0f;    // DeepFilterNet post-filter (0 = off)
};

class DfnRunner {
public:
    static constexpr int kModels = 2; // 0 = low latency (20 ms), 1 = HQ (40 ms)
    ~DfnRunner();

    static bool available();                         // built with the bridge
    static int latencyFor(int model, double sampleRate);

    void prepare(double sampleRate, int numChannels); // frees the engines if the config changed, resets
    bool load(int model);                              // blocking; true when the engine exists
    bool isLoaded(int model) const { return eng[(size_t) model].load(std::memory_order_acquire) != nullptr; }
    bool loadWanted(int model) const { return want[(size_t) model].load(std::memory_order_relaxed); }
    bool loadFailed(int model) const { return failed[(size_t) model].load(std::memory_order_relaxed); }

    // Audio thread: in-place, any block size, numChannels as prepared.
    void process(int model, float* const* io, int numChannels, int numSamples, const DfnParams& p);
    // Audio thread, every block in which the model is not processed: feeds its dry delay line
    // only; the next process() restarts the network (warm-up, crossfade from the delayed input).
    void idle(int model, const float* const* in, int numChannels, int numSamples);
    bool isWet(int model) const { return slot[(size_t) model].ramp >= 1.0f; }

    // Per-bin gain 0..1 of the last processed frame (spacing binHz()), for the meter.
    const std::vector<float>& gains() const { return gainBuf; }
    float binHz() const { return 50.0f; }

private:
    struct Slot {
        std::vector<std::vector<float>> delay; // dry path, length = latency
        int pos = 0;
        long fed = 0;     // samples given to the engine since its reset
        float ramp = 0.0f; // 0 dry .. 1 network
        bool needReset = true;
        int latency = 0;
    };
    std::array<std::atomic<Dfb*>, kModels> eng {};
    std::array<std::atomic<bool>, kModels> want {};
    std::array<std::atomic<bool>, kModels> failed {};
    std::array<Slot, kModels> slot;
    std::mutex cfgLock;
    double sr = 48000.0;
    int nch = 2;
    std::vector<std::vector<float>> inBuf, wetBuf;
    std::vector<float> gainBuf;
    void freeAll();
};

} // namespace zd
