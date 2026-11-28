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
    strengthParam = apvts.getRawParameterValue("strength");
    hissParam = apvts.getRawParameterValue("hisscut");
    for (auto& m : meter) m.store(0.0f);
    dfn.prepare(48000.0, 2);
    dfnLoader.startThread(juce::Thread::Priority::low);
}

ZeroDenoiseProcessor::~ZeroDenoiseProcessor()
{
    dfnLoader.stopThread(5000);
}

void ZeroDenoiseProcessor::DfnLoader::run()
{
    while (!threadShouldExit()) {
        for (int m = 0; m < zd::DfnRunner::kModels; ++m)
            if (runner.loadWanted(m) && !runner.isLoaded(m)) runner.load(m);
        wait(40);
    }
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
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { "strength", 1 }, "Strength", strengthNames(), 1));
    // Reduction: how far noise is pulled down at most (dB). Amount sets how eagerly it is removed.
    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID { "hisscut", 1 }, "Hiss Cut", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel("%")));
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

void ZeroDenoiseProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int nch = getTotalNumOutputChannels();
    currentRate = sampleRate;
    denoiser.prepare(sampleRate, nch);
    dfn.prepare(sampleRate, nch);
    const int blk = juce::jmax(64, samplesPerBlock);
    inCopy.setSize(nch, blk); pathA.setSize(nch, blk); pathB.setSize(nch, blk);
    // playback starts in a DeepFilterNet mode: load it now, not while the audio runs dry
    const int mode = juce::jlimit(0, kNumModes - 1, (int) modeParam->load());
    path = pathOfMode(mode); prevPath = -1; fadeLeft = 0;
    fadeLen = juce::jmax(1, (int) std::lround(sampleRate * 0.02));
    if (path > 0) dfn.load(path - 1);
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
        dfnMeterEdges[(size_t) b] = juce::jlimit(1, 480, (int) std::lround(juce::jmin(f, 24000.0) / dfn.binHz()));
    }
}

void ZeroDenoiseProcessor::applyMode()
{
    const int mode = juce::jlimit(0, kNumModes - 1, (int) modeParam->load());
    aiModeForUi.store(mode == kModeAi);
    const int pth = pathOfMode(mode);
    dfnModeForUi.store(pth > 0);
    int la;
    if (pth > 0) {
        la = zd::DfnRunner::latencyFor(pth - 1, currentRate);
    } else {
        la = kModeLookahead[mode] * denoiser.getScale();
        if (la != denoiser.getLookahead()) denoiser.setLookahead(la);
    }
    if (getLatencySamples() != la) setLatencySamples(la); // host re-aligns tracks (plugin delay compensation)
    latencyForUi.store(la);
}

void ZeroDenoiseProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numCh = getTotalNumOutputChannels();
    for (int c = getTotalNumInputChannels(); c < numCh; ++c) buffer.clear(c, 0, buffer.getNumSamples());

    applyMode();
    const int n = buffer.getNumSamples();
    const int mode = juce::jlimit(0, kNumModes - 1, (int) modeParam->load());
    const int newPath = pathOfMode(mode);
    if (newPath != path) {
        prevPath = path; path = newPath; fadeLeft = fadeLen;
    }
    if (path > 0 && isNonRealtime() && !dfn.isLoaded(path - 1)) dfn.load(path - 1); // offline render: never dry

    zd::DenoiserParams p = denoiser.getParams();
    p.amount = amountParam->load() * 0.01f;
    p.learn = learnParam->load() > 0.5f;
    p.adaptive = adaptParam->load() > 0.5f;
    p.aiStrongVoice = voiceParam->load() > 0.5f;
    p.reductionDb = reductionParam->load();
    p.gateDb = gateParam->load();
    p.pauseCleanup = pauseParam->load() > 0.5f;
    p.hissCut = hissParam->load() * 0.01f;
    p.maxJumpDb = kStrengthJumpDb[juce::jlimit(0, 2, (int) strengthParam->load())];
    p.ai = aiModeForUi.load();
    denoiser.setParams(p);

    // DeepFilterNet: Amount 0-50 % scales the removal depth up to Reduction, 50-100 % adds the
    // network's post-filter (a little more removal). Reduction at the top = no limit.
    zd::DfnParams dp;
    {
        const float amt = p.amount, red = p.reductionDb;
        dp.attenDb = red >= 79.5f ? 100.0f : red * juce::jmin(1.0f, amt * 2.0f);
        dp.pfBeta = amt > 0.5f ? 0.04f * (amt - 0.5f) * 2.0f : 0.0f;
        dp.guard = p.aiStrongVoice ? 2.0f : 1.0f;
    }

    if (inCopy.getNumSamples() < n || inCopy.getNumChannels() < numCh) { // host block larger than announced
        inCopy.setSize(numCh, n, false, false, true); pathA.setSize(numCh, n, false, false, true); pathB.setSize(numCh, n, false, false, true);
    }
    const bool fading = fadeLeft > 0 && prevPath >= 0;
    for (int c = 0; c < numCh; ++c) inCopy.copyFrom(c, 0, buffer, c, 0, n);

    // the ZeroDenoiser always runs so a switch back to modes 0-3 continues seamlessly
    for (int c = 0; c < numCh; ++c) pathA.copyFrom(c, 0, inCopy, c, 0, n);
    runPath(0, pathA, numCh, n, dp);
    for (int m = 0; m < zd::DfnRunner::kModels; ++m) {
        const bool used = path == m + 1 || (fading && prevPath == m + 1);
        if (!used) dfn.idle(m, inCopy.getArrayOfReadPointers(), numCh, n);
    }
    auto outputOf = [&](int which, juce::AudioBuffer<float>& dst) {
        if (which == 0) { for (int c = 0; c < numCh; ++c) dst.copyFrom(c, 0, pathA, c, 0, n); return; }
        for (int c = 0; c < numCh; ++c) dst.copyFrom(c, 0, inCopy, c, 0, n);
        runPath(which, dst, numCh, n, dp);
    };
    outputOf(path, buffer);
    if (fading) {
        outputOf(prevPath, pathB);
        const int k = juce::jmin(n, fadeLeft);
        for (int i = 0; i < k; ++i) {
            const float g = (float) (fadeLeft - i) / (float) fadeLen; // weight of the old path
            for (int c = 0; c < numCh; ++c) {
                float* o = buffer.getWritePointer(c);
                o[i] = o[i] + g * (pathB.getSample(c, i) - o[i]);
            }
        }
        fadeLeft -= k;
        if (fadeLeft <= 0) prevPath = -1;
    }
    dfnLoadingForUi.store(path > 0 && !dfn.isWet(path - 1));

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

void ZeroDenoiseProcessor::runPath(int which, juce::AudioBuffer<float>& io, int numCh, int n, const zd::DfnParams& dp)
{
    if (which == 0) denoiser.process(io.getArrayOfWritePointers(), numCh, n);
    else dfn.process(which - 1, io.getArrayOfWritePointers(), numCh, n, dp);
}

void ZeroDenoiseProcessor::updateMeter()
{
    const bool d = path > 0;
    const auto& g = d ? dfn.gains() : denoiser.currentGain();
    const auto& edges = d ? dfnMeterEdges : meterEdges;
    for (int b = 0; b < kMeterBands; ++b) {
        const int lo = edges[(size_t) b];
        const int hi = juce::jmax(lo + 1, edges[(size_t) b + 1]);
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
