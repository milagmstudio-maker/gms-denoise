#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "ZeroDenoiser.h"
#include "DfnRunner.h"

class ZeroDenoiseProcessor : public juce::AudioProcessor {
public:
    static constexpr int kMeterBands = 48;

    ZeroDenoiseProcessor();
    ~ZeroDenoiseProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    // gain reduction per log-spaced band in dB (written on audio thread, read by UI)
    std::array<std::atomic<float>, kMeterBands> meter {};
    std::atomic<float> sampleRateForUi { 48000.0f };
    std::atomic<int> latencyForUi { 0 };
    std::atomic<float> inputLevelForUi { -120.0f };
    std::atomic<bool> gateOpenForUi { true };

    // Mode -> lookahead in samples (signal delay reported to the host)
    // AI: RNNoise gains need one 480-sample network frame of lookahead (10 ms @48k).
    // AI Deep / AI Deep HQ (1.3): DeepFilterNet3 low-latency / standard model, 20 / 40 ms
    // at every rate (latency from zd::DfnRunner::latencyFor). New modes go at the end so
    // saved projects keep their mode.
    static constexpr int kNumModes = 6;
    static constexpr int kModeAi = 3;
    static constexpr int kModeDfn = 4;   // DFN model 0
    static constexpr int kModeDfnHq = 5; // DFN model 1
    static constexpr int kModeLookahead[kNumModes] = { 0, 128, 512, 480, -1, -1 };
    static juce::StringArray modeNames() { return { "Zero latency", "Low latency", "High quality", "AI (10 ms)", "AI Deep (20 ms)", "AI Deep HQ (40 ms)" }; }
    bool isAiMode() const { return aiModeForUi.load(); }
    bool isDfnMode() const { return dfnModeForUi.load(); }
    bool isDfnLoading() const { return dfnLoadingForUi.load(); }
    std::atomic<bool> aiModeForUi { false };
    std::atomic<bool> dfnModeForUi { false };
    std::atomic<bool> dfnLoadingForUi { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void updateMeter();

    zd::ZeroDenoiser denoiser;
    std::atomic<float>* amountParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* learnParam = nullptr;
    std::atomic<float>* adaptParam = nullptr;
    std::atomic<float>* voiceParam = nullptr;
    std::atomic<float>* reductionParam = nullptr;
    std::atomic<float>* gateParam = nullptr;
    std::atomic<float>* pauseParam = nullptr;
    std::atomic<float>* strengthParam = nullptr;
    std::atomic<float>* hissParam = nullptr;
public:
    // Strength: how far the background may drop between words compared with under the voice
    // (Gentle 6 dB / Medium 12 dB / Strong: no limit). Smaller = less 'breathing'.
    static juce::StringArray strengthNames() { return { "Gentle", "Medium", "Strong" }; }
    static constexpr float kStrengthJumpDb[3] = { 6.0f, 12.0f, 0.0f };
private:
    void applyMode();
    juce::SmoothedValue<float> outGain;
    std::array<int, kMeterBands + 1> meterEdges {};
    std::array<int, kMeterBands + 1> dfnMeterEdges {};

    // DeepFilterNet modes: networks load on this thread when a mode needs them
    zd::DfnRunner dfn;
    struct DfnLoader : juce::Thread {
        explicit DfnLoader(zd::DfnRunner& r) : juce::Thread("Shizumaru DFN loader"), runner(r) {}
        void run() override;
        zd::DfnRunner& runner;
    } dfnLoader { dfn };
    // signal path: 0 = ZeroDenoiser (modes 0-3), 1/2 = DFN model 0/1. A change of path is
    // crossfaded over kPathFade samples (both paths run meanwhile).
    int path = 0, prevPath = -1, fadeLeft = 0, fadeLen = 1;
    double currentRate = 48000.0;
    juce::AudioBuffer<float> inCopy, pathA, pathB;
    static int pathOfMode(int mode) { return mode == kModeDfn ? 1 : mode == kModeDfnHq ? 2 : 0; }
    void runPath(int which, juce::AudioBuffer<float>& io, int numCh, int n, const zd::DfnParams& dp);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroDenoiseProcessor)
};
