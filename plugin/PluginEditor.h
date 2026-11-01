#pragma once
#include "PluginProcessor.h"

class ZeroDenoiseEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit ZeroDenoiseEditor(ZeroDenoiseProcessor&);
    ~ZeroDenoiseEditor() override = default;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    ZeroDenoiseProcessor& proc;
    juce::Slider amount, output;
    juce::Label amountLabel, outputLabel, latencyLabel;
    juce::AudioProcessorValueTreeState::SliderAttachment amountAtt, outputAtt;
    std::array<float, ZeroDenoiseProcessor::kMeterBands> shown {};
    juce::Rectangle<int> meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroDenoiseEditor)
};
