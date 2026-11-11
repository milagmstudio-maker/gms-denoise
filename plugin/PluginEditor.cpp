#include "PluginEditor.h"

namespace {
constexpr int kWidth = 640, kHeight = 630;
const juce::String kModel = "De-Noise";

// The editor's own settings (the panel colour and the unit's serial number).
juce::PropertiesFile& uiSettings()
{
    static juce::PropertiesFile file([] {
        juce::PropertiesFile::Options o;
        o.applicationName = "GmSDeNoise";
        o.folderName = "GmS";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        return o;
    }());
    return file;
}

juce::String jp(const char* utf8) { return juce::String::fromUTF8(utf8); }
} // namespace

ZeroDenoiseEditor::ZeroDenoiseEditor(ZeroDenoiseProcessor& p)
    : AudioProcessorEditor(&p), proc(p),
      amountAtt(p.apvts, "amount", amount), outputAtt(p.apvts, "output", output),
      reductionAtt(p.apvts, "reduction", reduction), gateAtt(p.apvts, "gate", gate)
{
    setLookAndFeel(&look);

    auto knob = [this](ucshared::FineSlider& s, const char* id, int boxWidth) {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, boxWidth, 20);
        ucshared::setUpKnob(s, proc.apvts.getParameter(id), ucshared::isBipolar(proc.apvts.getParameter(id)));
        if (auto* prm = proc.apvts.getParameter(id); prm != nullptr && prm->getLabel().isNotEmpty() && juce::String(id) != "gate")
            s.setTextValueSuffix(" " + prm->getLabel());
        addAndMakeVisible(s);
    };
    knob(amount, "amount", 90);
    knob(output, "output", 80);
    knob(reduction, "reduction", 80);
    knob(gate, "gate", 80);
    amount.setTooltip("How much of the noise is removed.");
    output.setTooltip("Output level.");
    reduction.setTooltip("Maximum noise reduction. 0 dB = no reduction.");
    gate.setTooltip("Input quieter than this is muted completely (after noise reduction). Fully left = off.");

    learn.setClickingTogglesState(true);
    learn.setTooltip("Turn on during a noise-only passage, turn off to use the learned noise");
    addAndMakeVisible(learn);
    learnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "learn", learn);
    addAndMakeVisible(adaptive);
    adaptAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "adaptive", adaptive);
    voiceProtect.setTooltip("AI mode: also keep quieter screams and shouts. Background chatter is removed less.");
    addAndMakeVisible(voiceProtect);
    pauseClean.setTooltip("AI mode: where no voice is detected, pull the noise down to the Reduction setting. Very quiet word starts may lose a little.");
    addAndMakeVisible(pauseClean);
    pauseAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "pauseclean", pauseClean);
    voiceAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, "voiceprotect", voiceProtect);

    mode.addItemList(ZeroDenoiseProcessor::modeNames(), 1);
    addAndMakeVisible(mode);
    modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.apvts, "mode", mode);
    strength.addItemList(ZeroDenoiseProcessor::strengthNames(), 1);
    strength.setTooltip("Gentle: background stays steady when the voice starts (less removed between words). Strong: deepest removal, the background can 'breathe' with the voice.");
    addAndMakeVisible(strength);
    strengthAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.apvts, "strength", strength);

    for (auto* l : { &amountLabel, &outputLabel, &modeLabel, &reductionLabel, &gateLabel, &strengthLabel }) {
        l->setJustificationType(juce::Justification::centred);
        l->setFont(ucshared::analog::titleFont(12.0f).withExtraKerningFactor(0.08f));
        addAndMakeVisible(l);
    }
    modeLabel.setJustificationType(juce::Justification::centredLeft);
    strengthLabel.setJustificationType(juce::Justification::centredLeft);
    amountLabel.setText("AMOUNT", juce::dontSendNotification);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    modeLabel.setText("MODE", juce::dontSendNotification);
    reductionLabel.setText("REDUCTION", juce::dontSendNotification);
    strengthLabel.setText("STRENGTH", juce::dontSendNotification);
    gateLabel.setText("GATE", juce::dontSendNotification);
    gateStatus.setJustificationType(juce::Justification::centred);
    gateStatus.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(gateStatus);

    panelButton.setTooltip(jp("前面パネルの色を選びます。音は変わりません。"));
    panelButton.onClick = [this] { showPanelMenu(); };
    addAndMakeVisible(panelButton);

    serial = uiSettings().getValue("serial");
    if (serial.isEmpty()) {
        serial = juce::String(100000 + juce::Random::getSystemRandom().nextInt(900000));
        uiSettings().setValue("serial", serial);
        uiSettings().saveIfNeeded();
    }

    for (auto& s : shown) s = 0.0f;
    setSize(kWidth, kHeight);
    applyPanel(uiSettings().getValue("panel", "cream"));
    refresh(true);
    startTimerHz(30);
}

ZeroDenoiseEditor::~ZeroDenoiseEditor()
{
    setLookAndFeel(nullptr);
}

void ZeroDenoiseEditor::applyPanel(const juce::String& id)
{
    panelId = id;
    ucshared::analog::setPanel(id);
    look.refreshColours();
    panelButton.setButtonText(ucshared::analog::panelName(id));
    for (auto* s : { &amount, &reduction, &gate, &output })
        s->setColour(juce::Slider::rotarySliderFillColourId, ucshared::analog::oxblood);
    // Components keep colours copied from the look: let them take the new ones.
    sendLookAndFeelChange();
    repaint();
}

void ZeroDenoiseEditor::showPanelMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader("Panel");
    int itemId = 1;
    for (const auto& p : ucshared::analog::panels)
        menu.addItem(itemId++, p.name, true, panelId == p.id);
    juce::Component::SafePointer<ZeroDenoiseEditor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&panelButton), [safe](int result) {
        if (safe == nullptr || result < 1 || result > (int) std::size(ucshared::analog::panels))
            return;
        const juce::String id = ucshared::analog::panels[result - 1].id;
        uiSettings().setValue("panel", id);
        uiSettings().saveIfNeeded();
        safe->applyPanel(id);
    });
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
        latencyText = juce::String(ms, 1) + " ms";
        latencyTip = juce::String(lat) + " samples";
        repaint(latencyArea);
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
        gateStatus.setColour(juce::Label::textColourId, on && !open ? ucshared::analog::oxblood : ucshared::analog::dimInk);
    }
    for (size_t b = 0; b < shown.size(); ++b) {
        const float v = proc.meter[b].load(std::memory_order_relaxed);
        shown[b] = (immediate || v < shown[b]) ? v : shown[b] + 0.25f * (v - shown[b]);
    }
    repaint(meterArea);
}

void ZeroDenoiseEditor::paint(juce::Graphics& g)
{
    using namespace ucshared::analog;
    g.fillAll(faceplate);
    drawRackEars(g, getWidth(), getHeight());
    drawNameplate(g, { 24, 16, 300, 44 }, kModel, "VOICE NOISE REDUCER");

    // A recessed black screen with a thin bezel, like the plug-ins' displays.
    auto screenAt = [&g](juce::Rectangle<int> area) {
        const auto r = area.toFloat();
        g.setColour(juce::Colours::white.withAlpha(darkPanel ? 0.08f : 0.55f));
        g.drawRoundedRectangle(r.expanded(1.5f).translated(0.0f, 1.0f), 5.0f, 1.0f);
        g.setColour(edge.darker(0.3f));
        g.fillRoundedRectangle(r.expanded(1.5f), 5.0f);
        g.setGradientFill(juce::ColourGradient(screen.brighter(0.08f), r.getX(), r.getY(), screen, r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawHorizontalLine(area.getY() + 1, r.getX() + 3.0f, r.getRight() - 3.0f);
    };

    // Latency readout.
    screenAt(latencyArea);
    {
        auto r = latencyArea.reduced(10, 0);
        g.setFont(titleFont(9.5f).withExtraKerningFactor(0.18f));
        g.setColour(screenDim);
        g.drawText("LATENCY", r.removeFromLeft(62), juce::Justification::centredLeft);
        g.setColour(phosphor);
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText(latencyText, r, juce::Justification::centredRight);
    }

    // Gain-reduction display: bars hang down from 0 dB.
    screenAt(meterArea);
    {
        const auto inner = meterArea.reduced(12, 10).withTrimmedTop(16).withTrimmedBottom(14).withTrimmedLeft(30).toFloat();
        g.setFont(titleFont(9.5f).withExtraKerningFactor(0.18f));
        g.setColour(screenDim);
        g.drawText("GAIN REDUCTION", meterArea.reduced(12, 6).removeFromTop(14), juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("0 to -40 dB  /  40 Hz - Nyquist", meterArea.reduced(12, 5).removeFromBottom(12), juce::Justification::centredRight);
        for (int db = 10; db <= 30; db += 10) {
            const float y = inner.getY() + inner.getHeight() * (float) db / 40.0f;
            g.setColour(screenGrid);
            g.drawHorizontalLine(juce::roundToInt(y), inner.getX(), inner.getRight());
            g.setColour(screenDim.withAlpha(0.8f));
            g.drawText("-" + juce::String(db), juce::Rectangle<float>(inner.getX() - 32.0f, y - 6.0f, 26.0f, 12.0f), juce::Justification::centredRight);
        }
        const float bw = inner.getWidth() / (float) shown.size();
        for (size_t b = 0; b < shown.size(); ++b) {
            const float depth = juce::jlimit(0.0f, 1.0f, -shown[b] / 40.0f);
            if (depth <= 0.002f) continue;
            const juce::Rectangle<float> bar(inner.getX() + bw * (float) b + 1.0f, inner.getY(), bw - 2.0f, inner.getHeight() * depth);
            g.setGradientFill(juce::ColourGradient(phosphor.withAlpha(0.95f), bar.getX(), bar.getY(), phosphor.withAlpha(0.35f), bar.getX(), bar.getBottom(), false));
            g.fillRect(bar);
        }
        g.setColour(phosphor.withAlpha(0.5f));
        g.drawHorizontalLine(juce::roundToInt(inner.getY()), inner.getX(), inner.getRight());
    }

    // Group captions and the rules between the groups.
    g.setFont(titleFont(11.0f).withExtraKerningFactor(0.15f));
    g.setColour(dimInk);
    g.drawText("NOISE", 24, 214, 200, 16, juce::Justification::centredLeft);
    g.drawText("DETECTION", 336, 214, 240, 16, juce::Justification::centredLeft);
    g.drawText("RANGE  /  GATE  /  OUTPUT", 24, 452, 400, 16, juce::Justification::centredLeft);
    g.setColour(edge);
    g.drawHorizontalLine(232, 24.0f, (float) getWidth() - 24.0f);
    g.drawVerticalLine(318, 240.0f, 438.0f);
    g.drawHorizontalLine(470, 24.0f, (float) getWidth() - 24.0f);
    g.drawVerticalLine(208, 478.0f, 600.0f);
    g.drawVerticalLine(392, 478.0f, 600.0f);

    drawEngraving(g, getWidth(), getHeight(), kModel, serial);
}

void ZeroDenoiseEditor::resized()
{
    const int w = getWidth();
    panelButton.setBounds(w - 24 - 88, 24, 88, 28);
    latencyArea = { w - 24 - 88 - 12 - 176, 22, 176, 32 };
    meterArea = { 24, 74, w - 48, 126 };

    amountLabel.setBounds(24, 242, 280, 18);
    amount.setBounds(52, 262, 224, 168);

    const int cx = 336, cw = w - 24 - cx;
    learn.setBounds(cx, 244, 118, 28);
    adaptive.setBounds(cx + 128, 244, cw - 128, 28);
    modeLabel.setBounds(cx, 282, cw, 16);
    mode.setBounds(cx, 300, cw, 28);
    voiceProtect.setBounds(cx, 334, cw, 24);
    pauseClean.setBounds(cx, 360, cw, 24);
    strengthLabel.setBounds(cx, 390, cw, 16);
    strength.setBounds(cx, 408, cw, 28);

    const int colW = (w - 48) / 3, top = 478;
    auto column = [&](juce::Label& label, juce::Slider& s, int i) {
        const int x = 24 + i * colW;
        label.setBounds(x, top, colW, 18);
        s.setBounds(x + 22, top + 18, colW - 44, 104);
    };
    column(reductionLabel, reduction, 0);
    column(gateLabel, gate, 1);
    column(outputLabel, output, 2);
    gateStatus.setBounds(24 + colW, top + 124, colW, 16);
}
