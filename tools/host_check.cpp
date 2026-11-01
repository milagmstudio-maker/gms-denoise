// Loads the built VST3 through JUCE's hosting code (like a DAW would) and checks
// reported latency, parameter list, processing and state round-trip.
#include <juce_audio_processors/juce_audio_processors.h>
#include <random>
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
    inst->setPlayConfigDetails(2, 2, 48000.0, 256);
    inst->prepareToPlay(48000.0, 256);
    check(inst->getLatencySamples() == 0, "reported latency = " + juce::String(inst->getLatencySamples()));
    auto params = inst->getParameters();
    juce::StringArray names;
    for (auto* p : params) names.add(p->getName(32));
    check(names.contains("Amount") && names.contains("Output") && names.contains("Mode"), "parameters: " + names.joinIntoString(", "));

    // mode -> High quality: host must see the new latency
    juce::AudioProcessorParameter* mp = nullptr;
    for (auto* p : params) if (p->getName(32) == "Mode") mp = p;
    if (mp != nullptr) {
        mp->setValueNotifyingHost(1.0f);
        juce::AudioBuffer<float> b(2, 256); juce::MidiBuffer m; b.clear();
        inst->processBlock(b, m);
        check(inst->getLatencySamples() == 512, "High quality mode reports latency " + juce::String(inst->getLatencySamples()));
        mp->setValueNotifyingHost(0.0f);
        inst->processBlock(b, m);
        check(inst->getLatencySamples() == 0, "back to Zero latency mode reports " + juce::String(inst->getLatencySamples()));
    }
    // amount -> 100 %
    params[0]->setValueNotifyingHost(1.0f);
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0.f, 0.05f);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    double inE = 0, outE = 0;
    for (int b = 0; b < 48000 * 3 / 256; ++b) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buf.setSample(c, i, nd(rng));
        double e0 = 0;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 256; ++i) e0 += buf.getSample(c, i) * buf.getSample(c, i);
        inst->processBlock(buf, midi);
        if (b > 48000 * 2 / 256) {
            inE += e0;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 256; ++i) outE += buf.getSample(c, i) * buf.getSample(c, i);
        }
    }
    const double red = 10.0 * std::log10(outE / inE);
    check(red < -25.0, "white noise reduction through host: " + juce::String(red, 1) + " dB");

    juce::MemoryBlock state;
    inst->getStateInformation(state);
    params[0]->setValueNotifyingHost(0.0f);
    inst->setStateInformation(state.getData(), (int) state.getSize());
    check(std::abs(params[0]->getValue() - 1.0f) < 1e-4f, "state round trip restores Amount");
    inst->releaseResources();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
