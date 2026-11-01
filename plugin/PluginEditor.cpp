#include "PluginEditor.h"

namespace {
const juce::Colour bg(0xff15181d), panel(0xff1f242b), accent(0xff4fc3a1), dim(0xff8a96a3);
}

ZeroDenoiseEditor::ZeroDenoiseEditor(ZeroDenoiseProcessor& p)
    : AudioProcessorEditor(&p), proc(p),
      amountAtt(p.apvts, "amount", amount), outputAtt(p.apvts, "output", output)
{
    amount.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    amount.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 22);
    amount.setColour(juce::Slider::rotarySliderFillColourId, accent);
    amount.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    addAndMakeVisible(amount);

    output.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    output.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
    output.setColour(juce::Slider::rotarySliderFillColourId, dim);
    addAndMakeVisible(output);

    learn.setClickingTogglesState(true);
    learn.setTooltip("Turn on during a noise-only passage, turn off to use the learned noise");
    learn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffd9534f));
    addAndMakeVisible(learn);
    learnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "learn", learn);
    adaptive.setColour(juce::ToggleButton::textColourId, dim);
    adaptive.setColour(juce::ToggleButton::tickColourId, accent);
    addAndMakeVisible(adaptive);
    adaptAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "adaptive", adaptive);

    mode.addItemList(ZeroDenoiseProcessor::modeNames(), 1);
    addAndMakeVisible(mode);
    modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.apvts, "mode", mode);

    for (auto* l : { &amountLabel, &outputLabel, &latencyLabel, &modeLabel }) {
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, dim);
        addAndMakeVisible(l);
    }
    amountLabel.setText("NOISE REDUCTION", juce::dontSendNotification);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    modeLabel.setText("MODE", juce::dontSendNotification);
    latencyLabel.setJustificationType(juce::Justification::centredRight);
    latencyLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    latencyLabel.setColour(juce::Label::textColourId, accent);

    for (auto& s : shown) s = 0.0f;
    setSize(540, 380);
    refresh(true);
    startTimerHz(30);
}

void ZeroDenoiseEditor::timerCallback()
{
    refresh(false);
    if (learn.getToggleState()) {
        blink = (blink + 1) % 30;
        learn.setButtonText(blink < 15 ? "LEARNING..." : "LEARN");
    } else if (learn.getButtonText() != "LEARN") {
        learn.setButtonText("LEARN");
    }
}

void ZeroDenoiseEditor::refresh(bool immediate)
{
    const int lat = proc.latencyForUi.load();
    const float sr = proc.sampleRateForUi.load();
    if (lat != shownLatency || sr != shownRate) {
        shownLatency = lat; shownRate = sr;
        const double ms = 1000.0 * lat / (sr > 0 ? sr : 48000.0f);
        latencyLabel.setText("LATENCY  " + juce::String(ms, 1) + " ms  (" + juce::String(lat) + " smp)",
                             juce::dontSendNotification);
    }
    // Learn/Adaptive drive the DSP noise estimator; the AI network tracks noise itself.
    const bool dspControls = !proc.isAiMode();
    if (learn.isEnabled() != dspControls) {
        learn.setEnabled(dspControls);
        adaptive.setEnabled(dspControls);
        learn.setAlpha(dspControls ? 1.0f : 0.4f);
        adaptive.setAlpha(dspControls ? 1.0f : 0.4f);
    }
    for (size_t b = 0; b < shown.size(); ++b) {
        const float v = proc.meter[b].load(std::memory_order_relaxed);
        shown[b] = (immediate || v < shown[b]) ? v : shown[b] + 0.25f * (v - shown[b]);
    }
    repaint(meterArea);
}

void ZeroDenoiseEditor::paint(juce::Graphics& g)
{
    g.fillAll(bg);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    g.drawText("ZeroDenoise", 20, 12, 300, 26, juce::Justification::left);

    // gain-reduction display: bars hang down from 0 dB
    g.setColour(panel);
    g.fillRoundedRectangle(meterArea.toFloat(), 6.0f);
    const auto r = meterArea.reduced(8).toFloat();
    const float bw = r.getWidth() / (float) shown.size();
    for (size_t b = 0; b < shown.size(); ++b) {
        const float depth = juce::jlimit(0.0f, 1.0f, -shown[b] / 40.0f);
        g.setColour(accent.withAlpha(0.35f + 0.65f * depth));
        g.fillRect(r.getX() + bw * (float) b + 1.0f, r.getY(), bw - 2.0f, r.getHeight() * depth);
    }
    g.setColour(dim);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("gain reduction (0 to -40 dB), 40 Hz - Nyquist", meterArea.reduced(8).removeFromBottom(14),
               juce::Justification::right);
}

void ZeroDenoiseEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(32);
    latencyLabel.setBounds(top.removeFromRight(280));
    meterArea = area.removeFromTop(110);
    area.removeFromTop(10);
    auto left = area.removeFromLeft(area.getWidth() * 2 / 3);
    amountLabel.setBounds(left.removeFromTop(18));
    amount.setBounds(left.reduced(10, 0));
    auto right = area;
    auto row = right.removeFromTop(26);
    learn.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(4, 0));
    adaptive.setBounds(row.reduced(4, 0));
    right.removeFromTop(6);
    modeLabel.setBounds(right.removeFromTop(18));
    mode.setBounds(right.removeFromTop(26).reduced(6, 0));
    right.removeFromTop(8);
    outputLabel.setBounds(right.removeFromTop(18));
    output.setBounds(right.reduced(10, 0));
}
