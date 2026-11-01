#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "ZeroDenoiser.h"

class ZeroDenoiseProcessor : public juce::AudioProcessor {
public:
    static constexpr int kMeterBands = 48;

    ZeroDenoiseProcessor();
    ~ZeroDenoiseProcessor() override = default;

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

    // Mode -> lookahead in samples (signal delay reported to the host)
    // AI: RNNoise gains need one 480-sample network frame of lookahead (10 ms @48k).
    static constexpr int kNumModes = 4;
    static constexpr int kModeAi = 3;
    static constexpr int kModeLookahead[kNumModes] = { 0, 128, 512, 480 };
    static juce::StringArray modeNames() { return { "Zero latency", "Low latency", "High quality", "AI (10 ms)" }; }
    bool isAiMode() const { return aiModeForUi.load(); }
    std::atomic<bool> aiModeForUi { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void updateMeter();

    zd::ZeroDenoiser denoiser;
    std::atomic<float>* amountParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* learnParam = nullptr;
    std::atomic<float>* adaptParam = nullptr;
    void applyMode();
    juce::SmoothedValue<float> outGain;
    std::array<int, kMeterBands + 1> meterEdges {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroDenoiseProcessor)
};
