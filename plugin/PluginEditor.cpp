#include "PluginEditor.h"

namespace {
const juce::Colour bg(0xff15181d), panel(0xff1f242b), accent(0xff4fc3a1), dim(0xff8a96a3);
}

ZeroDenoiseEditor::ZeroDenoiseEditor(ZeroDenoiseProcessor& p)
    : AudioProcessorEditor(&p), proc(p),
      amountAtt(p.apvts, "amount", amount), outputAtt(p.apvts, "output", output),
      reductionAtt(p.apvts, "reduction", reduction), gateAtt(p.apvts, "gate", gate)
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

    for (auto* s : { &reduction, &gate }) {
        s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 20);
        s->setColour(juce::Slider::rotarySliderFillColourId, accent);
        addAndMakeVisible(s);
    }
    reduction.setTooltip("Maximum noise reduction. 0 dB = no reduction.");
    gate.setTooltip("Input quieter than this is muted completely (after noise reduction). Fully left = off.");

    learn.setClickingTogglesState(true);
    learn.setTooltip("Turn on during a noise-only passage, turn off to use the learned noise");
    learn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffd9534f));
    addAndMakeVisible(learn);
    learnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "learn", learn);
    adaptive.setColour(juce::ToggleButton::textColourId, dim);
    adaptive.setColour(juce::ToggleButton::tickColourId, accent);
    addAndMakeVisible(adaptive);
    adaptAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "adaptive", adaptive);
    voiceProtect.setColour(juce::ToggleButton::textColourId, dim);
    voiceProtect.setColour(juce::ToggleButton::tickColourId, accent);
    voiceProtect.setTooltip("AI mode: also keep quieter screams and shouts. Background chatter is removed less.");
    addAndMakeVisible(voiceProtect);
    pauseClean.setColour(juce::ToggleButton::textColourId, dim);
    pauseClean.setColour(juce::ToggleButton::tickColourId, accent);
    pauseClean.setTooltip("AI mode: where no voice is detected, pull the noise down to the Reduction setting. Very quiet word starts may lose a little.");
    addAndMakeVisible(pauseClean);
    pauseAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "pauseclean", pauseClean);
    voiceAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "voiceprotect", voiceProtect);

    mode.addItemList(ZeroDenoiseProcessor::modeNames(), 1);
    addAndMakeVisible(mode);
    modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.apvts, "mode", mode);

    for (auto* l : { &amountLabel, &outputLabel, &latencyLabel, &modeLabel, &reductionLabel, &gateLabel, &gateStatus }) {
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, dim);
        addAndMakeVisible(l);
    }
    amountLabel.setText("AMOUNT", juce::dontSendNotification);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    modeLabel.setText("MODE", juce::dontSendNotification);
    reductionLabel.setText("REDUCTION", juce::dontSendNotification);
    gateLabel.setText("GATE", juce::dontSendNotification);
    gateStatus.setFont(juce::FontOptions(12.0f));
    latencyLabel.setJustificationType(juce::Justification::centredRight);
    latencyLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    latencyLabel.setColour(juce::Label::textColourId, accent);

    for (auto& s : shown) s = 0.0f;
    setSize(540, 500);
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
    const bool ai = proc.isAiMode(); // the switch only acts in AI mode
    for (auto* b : { &voiceProtect, &pauseClean })
        if (b->isEnabled() != ai) { b->setEnabled(ai); b->setAlpha(ai ? 1.0f : 0.4f); }
    {
        const float lvl = proc.inputLevelForUi.load(std::memory_order_relaxed);
        const bool on = proc.apvts.getRawParameterValue("gate")->load() > zd::DenoiserParams::kGateOff;
        const bool open = proc.gateOpenForUi.load(std::memory_order_relaxed);
        const juce::String t = "in " + (lvl < -99.0f ? juce::String("-inf") : juce::String(juce::roundToInt(lvl))) + " dB"
                               + (on ? (open ? "  |  open" : "  |  MUTED") : "");
        if (gateStatus.getText() != t) gateStatus.setText(t, juce::dontSendNotification);
        gateStatus.setColour(juce::Label::textColourId, on && !open ? juce::Colour(0xffd9534f) : dim);
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
    auto bottom = area.removeFromBottom(130);
    {
        const int w = bottom.getWidth() / 3;
        auto rb = bottom.removeFromLeft(w), gb = bottom.removeFromLeft(w), ob = bottom;
        reductionLabel.setBounds(rb.removeFromTop(18));
        reduction.setBounds(rb.reduced(14, 0));
        gateLabel.setBounds(gb.removeFromTop(18));
        gateStatus.setBounds(gb.removeFromBottom(16));
        gate.setBounds(gb.reduced(14, 0));
        outputLabel.setBounds(ob.removeFromTop(18));
        output.setBounds(ob.reduced(14, 0));
    }
    area.removeFromBottom(6);
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
    voiceProtect.setBounds(right.removeFromTop(24).reduced(4, 0));
    pauseClean.setBounds(right.removeFromTop(24).reduced(4, 0));
}
