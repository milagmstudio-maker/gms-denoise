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
    juce::Slider amount, output, reduction, gate;
    juce::Label amountLabel, outputLabel, latencyLabel, modeLabel, reductionLabel, gateLabel, gateStatus;
    juce::ComboBox mode, strength;
    juce::Label strengthLabel;
    juce::TextButton learn { "LEARN" };
    juce::ToggleButton adaptive { "Adaptive" };
    juce::ToggleButton voiceProtect { "Strong voice protect" };
    juce::ToggleButton pauseClean { "Pause cleanup" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> learnAtt, adaptAtt, voiceAtt, pauseAtt;
    int blink = 0;
    juce::AudioProcessorValueTreeState::SliderAttachment amountAtt, outputAtt, reductionAtt, gateAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt, strengthAtt;
    int shownLatency = -1; float shownRate = 0.0f;
    std::array<float, ZeroDenoiseProcessor::kMeterBands> shown {};
    juce::Rectangle<int> meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroDenoiseEditor)
};
