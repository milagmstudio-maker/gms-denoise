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

    for (auto* l : { &amountLabel, &outputLabel, &latencyLabel }) {
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, dim);
        addAndMakeVisible(l);
    }
    amountLabel.setText("NOISE REDUCTION", juce::dontSendNotification);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    latencyLabel.setText("latency: 0 samples", juce::dontSendNotification);

    for (auto& s : shown) s = 0.0f;
    setSize(520, 340);
    startTimerHz(30);
}

void ZeroDenoiseEditor::timerCallback()
{
    for (size_t b = 0; b < shown.size(); ++b) {
        const float v = proc.meter[b].load(std::memory_order_relaxed);
        shown[b] = v < shown[b] ? v : shown[b] + 0.25f * (v - shown[b]);
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
    area.removeFromTop(32);
    meterArea = area.removeFromTop(110);
    area.removeFromTop(10);
    auto left = area.removeFromLeft(area.getWidth() * 2 / 3);
    amountLabel.setBounds(left.removeFromTop(18));
    amount.setBounds(left.reduced(10, 0));
    auto right = area;
    outputLabel.setBounds(right.removeFromTop(18));
    latencyLabel.setBounds(right.removeFromBottom(18));
    output.setBounds(right.reduced(20, 10));
}
