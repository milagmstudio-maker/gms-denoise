#pragma once
#include "PluginProcessor.h"
#include "shared/AnalogLook.h"

// The editor wears the Genomersive Studio front panel shared with Ultimate
// Compressor and GmSverb (shared/AnalogLook.h): a faceplate in the panel
// colour chosen with the Panel button, black knobs, lit buttons, jewel lamps,
// and the gain-reduction display on a black screen.
class ZeroDenoiseEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit ZeroDenoiseEditor(ZeroDenoiseProcessor&);
    ~ZeroDenoiseEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refresh(bool immediate);
    void applyPanel(const juce::String& id);
    void showPanelMenu();

    ZeroDenoiseProcessor& proc;
    ucshared::AnalogLook look;
    juce::String panelId, serial;
    ucshared::FineSlider amount, output, reduction, gate;
    juce::Label amountLabel, outputLabel, modeLabel, reductionLabel, gateLabel, gateStatus;
    juce::ComboBox mode, strength;
    juce::Label strengthLabel;
    juce::TextButton learn { "LEARN" };
    juce::TextButton panelButton;
    juce::ToggleButton adaptive { "Adaptive" };
    juce::ToggleButton voiceProtect { "Strong voice protect" };
    juce::ToggleButton pauseClean { "Pause cleanup" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> learnAtt, adaptAtt, voiceAtt, pauseAtt;
    int blink = 0;
    juce::AudioProcessorValueTreeState::SliderAttachment amountAtt, outputAtt, reductionAtt, gateAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt, strengthAtt;
    int shownLatency = -1; float shownRate = 0.0f;
    juce::String latencyText, latencyTip;
    std::array<float, ZeroDenoiseProcessor::kMeterBands> shown {};
    juce::Rectangle<int> meterArea, latencyArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroDenoiseEditor)
};
