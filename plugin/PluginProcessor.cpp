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
    setLatencySamples(0);
    outGain.reset(sampleRate, 0.02);
    outGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputParam->load()));
    sampleRateForUi.store((float) sampleRate);

    // log-spaced meter bands from 40 Hz to Nyquist over the analysis bins
    const double binHz = sampleRate / zd::ZeroDenoiser::kFFT;
    const double fLo = 40.0, fHi = sampleRate * 0.5;
    for (int b = 0; b <= kMeterBands; ++b) {
        const double f = fLo * std::pow(fHi / fLo, (double) b / kMeterBands);
        meterEdges[(size_t) b] = juce::jlimit(1, zd::ZeroDenoiser::kBins, (int) std::lround(f / binHz));
    }
}

void ZeroDenoiseProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numCh = getTotalNumOutputChannels();
    for (int c = getTotalNumInputChannels(); c < numCh; ++c) buffer.clear(c, 0, buffer.getNumSamples());

    zd::DenoiserParams p = denoiser.getParams();
    p.amount = amountParam->load() * 0.01f;
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
