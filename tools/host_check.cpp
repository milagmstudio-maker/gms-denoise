// Loads the built VST3 through JUCE's hosting code (like a DAW would) and checks
// reported latency, parameter list, processing and state round-trip.
#include <juce_audio_processors/juce_audio_processors.h>
#include <random>
#include <algorithm>
#include <vector>
#include <cstdlib>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::printf("usage: host_check path/to/Plugin.vst3\n"); return 2; }
    juce::AudioPluginFormatManager fm;
    fm.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> types;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(types, argv[1]);
    if (types.isEmpty()) { std::printf("FAIL: no plugin found\n"); return 1; }
    juce::String err;
    auto inst = fm.createPluginInstance(*types[0], 48000.0, 256, err);
    if (!inst) { std::printf("FAIL: %s\n", err.toRawUTF8()); return 1; }
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };

    check(inst->getName() == "ZeroDenoise", "name = " + inst->getName());
    inst->setPlayConfigDetails(2, 2, 48000.0, 1024);
    inst->prepareToPlay(48000.0, 1024); // max block; the checks use 37..1024
    check(inst->getLatencySamples() == 0, "reported latency = " + juce::String(inst->getLatencySamples()));
    auto params = inst->getParameters();
    juce::StringArray names;
    for (auto* p : params) names.add(p->getName(32));
    check(names.contains("Amount") && names.contains("Output") && names.contains("Mode"), "parameters: " + names.joinIntoString(", "));

    juce::AudioProcessorParameter* mp = nullptr;
    for (auto* p : params) if (p->getName(32) == "Mode") mp = p;
    const int nModes = mp ? mp->getNumSteps() : 0;
    check(nModes == 4, "Mode has " + juce::String(nModes) + " choices");
    auto setMode = [&](int idx) { mp->setValueNotifyingHost((float) idx / (float) (nModes - 1)); };

    // every mode: host must see the new latency
    if (mp != nullptr && nModes == 4) {
        const int expect[4] = { 0, 128, 512, 480 };
        juce::AudioBuffer<float> b(2, 256); juce::MidiBuffer m; b.clear();
        for (int idx : { 1, 2, 3, 0 }) {
            setMode(idx);
            inst->processBlock(b, m);
            check(inst->getLatencySamples() == expect[idx],
                  mp->getCurrentValueAsText() + " reports latency " + juce::String(inst->getLatencySamples()));
        }
    }
    // amount -> 100 %
    params[0]->setValueNotifyingHost(1.0f);
    auto noiseRun = [&](int block, double& redOut) {
        std::mt19937 rng(1);
        std::normal_distribution<float> nd(0.f, 0.05f);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        double inE = 0, outE = 0;
        const int total = 48000 * 3, from = 48000 * 2;
        for (int pos = 0; pos < total; pos += block) {
            for (int i = 0; i < block; ++i)   // sample-major so the signal is the same for every block size
                for (int c = 0; c < 2; ++c) buf.setSample(c, i, nd(rng));
            double e0 = 0;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < block; ++i) e0 += buf.getSample(c, i) * buf.getSample(c, i);
            inst->processBlock(buf, midi);
            if (pos > from) {
                inE += e0;
                for (int c = 0; c < 2; ++c) for (int i = 0; i < block; ++i) outE += buf.getSample(c, i) * buf.getSample(c, i);
            }
        }
        redOut = 10.0 * std::log10(outE / inE);
    };
    double red = 0;
    if (mp) setMode(0);
    noiseRun(256, red);
    check(red < -25.0, "Zero latency: white noise reduction through host: " + juce::String(red, 1) + " dB");
    if (mp && nModes == 4) {
        // each run restarts the network (mode off -> on), so all block sizes must agree
        std::vector<double> reds;
        for (int block : { 256, 37, 1024 }) {
            setMode(0);
            { juce::AudioBuffer<float> b(2, 64); juce::MidiBuffer m; b.clear(); inst->processBlock(b, m); }
            setMode(3);
            noiseRun(block, red);
            reds.push_back(red);
            check(red < -25.0, "AI mode (hiss assist), block " + juce::String(block) + ": white noise " + juce::String(red, 1) + " dB");
        }
        const auto mm = std::minmax_element(reds.begin(), reds.end());
        check(*mm.second - *mm.first < 0.5, "AI mode result independent of host block size (spread " +
              juce::String(*mm.second - *mm.first, 2) + " dB)");
        // AI at other sample rates: lookahead scales with the rate
        for (double sr : { 44100.0, 96000.0 }) {
            inst->releaseResources();
            inst->setPlayConfigDetails(2, 2, sr, 1024);
            inst->prepareToPlay(sr, 1024);
            juce::AudioBuffer<float> b(2, 256); juce::MidiBuffer m; b.clear();
            inst->processBlock(b, m);
            const int lat = inst->getLatencySamples();
            const int expect = sr > 50000 ? 960 : 480;
            check(lat == expect, "AI mode @" + juce::String(sr / 1000.0, 1) + " kHz latency " + juce::String(lat) +
                  " smp = " + juce::String(1000.0 * lat / sr, 2) + " ms");
        }
        inst->releaseResources();
        inst->setPlayConfigDetails(2, 2, 48000.0, 1024);
        inst->prepareToPlay(48000.0, 1024); // max block; the checks use 37..1024
    }

    juce::MemoryBlock state;
    inst->getStateInformation(state);
    params[0]->setValueNotifyingHost(0.0f);
    inst->setStateInformation(state.getData(), (int) state.getSize());
    check(std::abs(params[0]->getValue() - 1.0f) < 1e-4f, "state round trip restores Amount");
    if (mp) {
        setMode(0);
        inst->setStateInformation(state.getData(), (int) state.getSize());
        check(mp->getCurrentValueAsText() == "AI (10 ms)", "state round trip restores Mode = " + mp->getCurrentValueAsText());
    }
    inst->releaseResources();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
