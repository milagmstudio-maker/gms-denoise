// Renders the plugin editor to a PNG for docs/review (links the plugin's shared code directly).
#include "../plugin/PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    ZeroDenoiseProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 256);
    proc.prepareToPlay(48000.0, 256);
    if (argc > 2) proc.apvts.getParameter("mode")->setValueNotifyingHost((float) std::atoi(argv[2]) / 2.0f);
    proc.apvts.getParameter("amount")->setValueNotifyingHost(0.7f);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    juce::Random r(1);
    for (int b = 0; b < 400; ++b) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buf.setSample(c, i, (r.nextFloat() - 0.5f) * 0.1f * (1.0f + (float) std::sin(i * 0.02)));
        proc.processBlock(buf, midi);
    }
    std::unique_ptr<juce::AudioProcessorEditor> ed { proc.createEditor() };
    for (int i = 0; i < 40; ++i) { ed->repaint(); juce::Thread::sleep(1); }
    // let the meter timer settle by calling the paint path directly
    auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
    juce::File f(argc > 1 ? argv[1] : "ui.png");
    f.deleteFile();
    juce::FileOutputStream os(f);
    juce::PNGImageFormat().writeImageToStream(img, os);
    std::printf("wrote %s\n", f.getFullPathName().toRawUTF8());
    return 0;
}
