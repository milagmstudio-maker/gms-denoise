#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

ZeroDenoiseProcessor::ZeroDenoiseProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    amountParam = apvts.getRawParameterValue("amount");
    outputParam = apvts.getRawParameterValue("output");
    modeParam = apvts.getRawParameterValue("mode");
    learnParam = apvts.getRawParameterValue("learn");
    adaptParam = apvts.getRawParameterValue("adaptive");
    voiceParam = apvts.getRawParameterValue("voiceprotect");
    reductionParam = apvts.getRawParameterValue("reduction");
    gateParam = apvts.getRawParameterValue("gate");
    pauseParam = apvts.getRawParameterValue("pauseclean");
    for (auto& m : meter) m.store(0.0f);
}

juce::AudioProcessorValueTreeState::ParameterLayout ZeroDenoiseProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID { "amount", 1 }, "Amount", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 50.0f,
        AudioParameterFloatAttributes().withLabel("%")));
    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID { "output", 1 }, "Output", NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel("dB")));
    p.push_back(std::make_unique<AudioParameterChoice>(
        ParameterID { "mode", 1 }, "Mode", modeNames(), 0));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { "learn", 1 }, "Learn", false));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { "adaptive", 1 }, "Adaptive", true));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { "voiceprotect", 1 }, "Strong Voice Protect", false));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { "pauseclean", 1 }, "Pause Cleanup", false));
    // Reduction: how far noise is pulled down at most (dB). Amount sets how eagerly it is removed.
    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID { "reduction", 1 }, "Reduction", NormalisableRange<float>(0.0f, 80.0f, 0.5f), 40.0f,
        AudioParameterFloatAttributes().withLabel("dB")));
    // Gate: input below this level (dBFS) is muted completely. Bottom of the range = off.
    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID { "gate", 1 }, "Gate Threshold", NormalisableRange<float>(zd::DenoiserParams::kGateOff, -10.0f, 0.5f),
        zd::DenoiserParams::kGateOff,
        AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction([](float v, int) {
            return v <= zd::DenoiserParams::kGateOff ? String("Off") : String(v, 1) + " dB";
        })));
    return { p.begin(), p.end() };
}

bool ZeroDenoiseProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return out == layouts.getMainInputChannelSet();
}

void ZeroDenoiseProcessor::prepareToPlay(double sampleRate, int)
{
    denoiser.prepare(sampleRate, getTotalNumOutputChannels());
    applyMode();
    outGain.reset(sampleRate, 0.02);
    outGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputParam->load()));
    sampleRateForUi.store((float) sampleRate);

    // log-spaced meter bands from 40 Hz to Nyquist over the analysis bins
    const double binHz = sampleRate / denoiser.getFFTSize();
    const double fLo = 40.0, fHi = sampleRate * 0.5;
    for (int b = 0; b <= kMeterBands; ++b) {
        const double f = fLo * std::pow(fHi / fLo, (double) b / kMeterBands);
        meterEdges[(size_t) b] = juce::jlimit(1, denoiser.getNumBins(), (int) std::lround(f / binHz));
    }
}

void ZeroDenoiseProcessor::applyMode()
{
    const int mode = juce::jlimit(0, kNumModes - 1, (int) modeParam->load());
    aiModeForUi.store(mode == kModeAi);
    const int la = kModeLookahead[mode] * denoiser.getScale();
    if (la != denoiser.getLookahead() || getLatencySamples() != la) {
        denoiser.setLookahead(la);
        setLatencySamples(la); // host re-aligns tracks (plugin delay compensation)
    }
    latencyForUi.store(la);
}

void ZeroDenoiseProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numCh = getTotalNumOutputChannels();
    for (int c = getTotalNumInputChannels(); c < numCh; ++c) buffer.clear(c, 0, buffer.getNumSamples());

    applyMode();
    zd::DenoiserParams p = denoiser.getParams();
    p.amount = amountParam->load() * 0.01f;
    p.learn = learnParam->load() > 0.5f;
    p.adaptive = adaptParam->load() > 0.5f;
    p.aiStrongVoice = voiceParam->load() > 0.5f;
    p.reductionDb = reductionParam->load();
    p.gateDb = gateParam->load();
    p.pauseCleanup = pauseParam->load() > 0.5f;
    p.ai = aiModeForUi.load();
    denoiser.setParams(p);
    denoiser.process(buffer.getArrayOfWritePointers(), numCh, buffer.getNumSamples());

    outGain.setTargetValue(juce::Decibels::decibelsToGain(outputParam->load()));
    if (outGain.isSmoothing()) {
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            const float g = outGain.getNextValue();
            for (int c = 0; c < numCh; ++c) buffer.getWritePointer(c)[i] *= g;
        }
    } else {
        buffer.applyGain(outGain.getTargetValue());
    }
    updateMeter();
    inputLevelForUi.store(denoiser.getInputLevelDb(), std::memory_order_relaxed);
    gateOpenForUi.store(denoiser.isGateOpen(), std::memory_order_relaxed);
}

void ZeroDenoiseProcessor::updateMeter()
{
    const auto& g = denoiser.currentGain();
    for (int b = 0; b < kMeterBands; ++b) {
        const int lo = meterEdges[(size_t) b];
        const int hi = juce::jmax(lo + 1, meterEdges[(size_t) b + 1]);
        float s = 0.0f;
        for (int k = lo; k < hi && k < (int) g.size(); ++k) s += g[(size_t) k] * g[(size_t) k];
        const float db = juce::Decibels::gainToDecibels(std::sqrt(s / (float) (hi - lo)), -60.0f);
        meter[(size_t) b].store(db, std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* ZeroDenoiseProcessor::createEditor() { return new ZeroDenoiseEditor(*this); }

void ZeroDenoiseProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, destData);
}

void ZeroDenoiseProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ZeroDenoiseProcessor(); }
