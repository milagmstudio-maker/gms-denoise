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
    void refresh(bool immediate);

    ZeroDenoiseProcessor& proc;
    juce::Slider amount, output;
    juce::Label amountLabel, outputLabel, latencyLabel, modeLabel;
    juce::ComboBox mode;
    juce::AudioProcessorValueTreeState::SliderAttachment amountAtt, outputAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt;
    int shownLatency = -1; float shownRate = 0.0f;
    std::array<float, ZeroDenoiseProcessor::kMeterBands> shown {};
    juce::Rectangle<int> meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroDenoiseEditor)
};
