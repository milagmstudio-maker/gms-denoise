#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

namespace ucshared
{
// The look shared by the three plug-ins: flat dark surfaces, thin arc
// knobs with the value arc in the control's accent colour (from the
// centre for bipolar controls), pill switches, rounded flat buttons and
// menus, one type scale.
namespace palette
{
const juce::Colour background { 0xff111318 };
const juce::Colour panel { 0xff191c22 };
const juce::Colour raised { 0xff22262e };
const juce::Colour outline { 0xff30353f };
const juce::Colour text { 0xffe8eaee };
const juce::Colour dimText { 0xff8b919c };
const juce::Colour accent { 0xff3cc6c0 };
} // namespace palette

class ModernLook : public juce::LookAndFeel_V4
{
public:
    ModernLook()
    {
        using namespace palette;
       #if JUCE_WINDOWS
        // The default Windows face has no Japanese; this one has both.
        setDefaultSansSerifTypefaceName ("Yu Gothic UI");
       #endif
        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Label::textColourId, text);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
        setColour (juce::Slider::rotarySliderOutlineColourId, outline);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::backgroundColourId, outline);
        setColour (juce::ComboBox::backgroundColourId, raised);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, dimText);
        setColour (juce::PopupMenu::backgroundColourId, raised);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.3f));
        setColour (juce::PopupMenu::highlightedTextColourId, text);
        setColour (juce::PopupMenu::headerTextColourId, dimText);
        setColour (juce::TextButton::buttonColourId, raised);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::ToggleButton::tickDisabledColourId, outline);
        setColour (juce::TextEditor::backgroundColourId, raised);
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::outlineColourId, outline);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::AlertWindow::backgroundColourId, panel);
        setColour (juce::AlertWindow::textColourId, text);
    }

    // Knobs: a thin track, the value arc from the start (or from the
    // centre when the slider's "bipolar" property is set), a dot at the
    // value and a short pointer in the middle. Brighter while the mouse is
    // over it or dragging.
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y), static_cast<float> (width), static_cast<float> (height)).reduced (3.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float lineWidth = juce::jmax (2.5f, radius * 0.13f);
        const float arcRadius = radius - lineWidth * 0.5f;
        const bool hot = slider.isMouseOverOrDragging() && slider.isEnabled();
        auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
        if (! slider.isEnabled())
            accent = accent.withSaturation (0.0f).withAlpha (0.5f);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (palette::outline);
        g.strokePath (track, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const bool bipolar = static_cast<bool> (slider.getProperties().getWithDefault ("bipolar", false));
        const float valueAngle = startAngle + position * (endAngle - startAngle);
        const float fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        if (std::abs (valueAngle - fromAngle) > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (fromAngle, valueAngle), juce::jmax (fromAngle, valueAngle), true);
            g.setColour (hot ? accent.brighter (0.25f) : accent);
            g.strokePath (value, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // The value in use, when an automatic control moved it off the knob.
        if (const auto live = slider.getProperties().getVarPointer ("live"); live != nullptr)
        {
            const float liveAngle = startAngle + static_cast<float> (static_cast<double> (*live)) * (endAngle - startAngle);
            const auto at = centre.getPointOnCircumference (arcRadius, liveAngle);
            g.setColour (palette::text);
            g.fillEllipse (juce::Rectangle<float> (lineWidth * 1.9f, lineWidth * 1.9f).withCentre (at));
            g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId).brighter (0.4f));
            g.fillEllipse (juce::Rectangle<float> (lineWidth * 1.2f, lineWidth * 1.2f).withCentre (at));
        }

        // Body and pointer.
        const float body = arcRadius - lineWidth * 1.4f;
        g.setColour (hot ? palette::raised.brighter (0.12f) : palette::raised);
        g.fillEllipse (juce::Rectangle<float> (body * 2.0f, body * 2.0f).withCentre (centre));
        const auto tip = centre.getPointOnCircumference (body * 0.85f, valueAngle);
        const auto base = centre.getPointOnCircumference (body * 0.35f, valueAngle);
        g.setColour (slider.isEnabled() ? palette::text : palette::dimText);
        g.drawLine ({ base, tip }, juce::jmax (2.0f, lineWidth * 0.7f));
    }

    // Switches: a pill on the left, the label beside it.
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool) override
    {
        auto area = button.getLocalBounds().toFloat();
        const float h = juce::jmin (16.0f, area.getHeight() - 4.0f);
        const auto pill = area.removeFromLeft (h * 1.8f).withSizeKeepingCentre (h * 1.8f, h);
        const bool on = button.getToggleState();
        auto accent = button.findColour (juce::ToggleButton::tickColourId);
        if (! button.isEnabled())
            accent = accent.withSaturation (0.0f).withAlpha (0.5f);
        g.setColour (on ? accent : (highlighted ? palette::outline.brighter (0.2f) : palette::outline));
        g.fillRoundedRectangle (pill, h * 0.5f);
        const float knob = h - 4.0f;
        const float knobX = on ? pill.getRight() - knob - 2.0f : pill.getX() + 2.0f;
        g.setColour (on ? juce::Colours::black.withAlpha (0.75f) : palette::text.withAlpha (button.isEnabled() ? 0.9f : 0.4f));
        g.fillEllipse (knobX, pill.getY() + 2.0f, knob, knob);

        area.removeFromLeft (8.0f);
        g.setColour (button.isEnabled() ? palette::text : palette::dimText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (button.getButtonText(), area.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                               bool highlighted, bool down) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto colour = background;
        if (down)
            colour = colour.brighter (0.15f);
        else if (highlighted)
            colour = colour.brighter (0.08f);
        if (! button.isEnabled())
            colour = colour.withMultipliedAlpha (0.5f);
        g.setColour (colour);
        const bool left = button.isConnectedOnLeft(), right = button.isConnectedOnRight();
        juce::Path shape;
        shape.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), 5.0f, 5.0f,
                                   ! left, ! right, ! left, ! right);
        g.fillPath (shape);
        if (! button.getToggleState())
        {
            g.setColour (palette::outline);
            g.strokePath (shape, juce::PathStrokeType (1.0f));
        }
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (13.0f, static_cast<float> (buttonHeight) * 0.55f)));
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (box.isMouseOver (true) ? 0.08f : 0.0f));
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (palette::outline);
        g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
        const float cx = static_cast<float> (width) - 12.0f, cy = static_cast<float> (height) * 0.5f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (box.isEnabled() ? palette::dimText : palette::outline);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (14.0f)); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (6, 1, box.getWidth() - 26, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }
};

// A knob whose drag is handled here, so that holding Cmd, Ctrl or Shift
// switches to fine steps (a tenth) at any moment without the value jumping:
// 250 pixels of movement (up or right) cover the whole range. The position
// is accumulated, so fine steps smaller than the parameter's interval still
// add up. Alt-click and double-click return to the default as before.
class FineSlider : public juce::Slider
{
public:
    // When an automatic control (Auto Threshold, Intelligence) runs with a
    // value other than the knob's, the knob shows it: a dot on the scale
    // (the "live" property, as a proportion of the range, drawn by the
    // looks) and the number in the text box, except while it is dragged,
    // when the box shows the value being set. std::nullopt hides both.
    void setLive (std::optional<double> value)
    {
        if (value.has_value())
            value = juce::jlimit (getMinimum(), getMaximum(), *value);
        if (value == live)
            return;
        live = value;
        if (live.has_value())
            getProperties().set ("live", valueToProportionOfLength (*live));
        else
            getProperties().remove ("live");
        updateText();
        repaint();
    }

    juce::String getTextFromValue (double value) override
    {
        if (live.has_value() && ! isMouseButtonDown())
            return juce::Slider::getTextFromValue (*live);
        return juce::Slider::getTextFromValue (value);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::Slider::mouseUp (e);
        updateText();
    }

    static constexpr double kPixelsForFullRange = 250.0, kFineFactor = 0.1;

    void mouseDown (const juce::MouseEvent& e) override
    {
        juce::Slider::mouseDown (e);
        last = e.position;
        proportion = valueToProportionOfLength (getValue());
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! isRotary() || ! isEnabled() || e.mods.isAltDown() || e.mods.isPopupMenu())
        {
            juce::Slider::mouseDrag (e);
            return;
        }
        const auto delta = e.position - last;
        last = e.position;
        const bool fine = e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isShiftDown();
        proportion = juce::jlimit (0.0, 1.0, proportion + static_cast<double> (delta.x - delta.y) / kPixelsForFullRange * (fine ? kFineFactor : 1.0));
        setValue (proportionOfLengthToValue (proportion), juce::sendNotificationSync);
    }

private:
    juce::Point<float> last;
    double proportion = 0.0;
    std::optional<double> live;
};

// Knob behaviour shared by the plug-ins: double-click returns to the
// parameter's default, Cmd / Ctrl / Shift while dragging a FineSlider moves
// it in fine steps, the value can be typed into the text below, and the
// mouse wheel works. `bipolar` draws the arc from the centre.
inline void setUpKnob (juce::Slider& slider, juce::RangedAudioParameter* parameter, bool bipolar = false)
{
    if (parameter != nullptr)
        slider.setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
    slider.setVelocityBasedMode (false);
    slider.setVelocityModeParameters (0.6, 1, 0.0, false);
    slider.setScrollWheelEnabled (true);
    slider.setTextBoxIsEditable (true);
    slider.getProperties().set ("bipolar", bipolar);
}

// Whether a parameter's range is centred on zero (gains, tilts, +-amounts).
inline bool isBipolar (const juce::RangedAudioParameter* parameter)
{
    if (auto* f = dynamic_cast<const juce::AudioParameterFloat*> (parameter))
        return f->range.start < 0.0f && f->range.end > 0.0f && std::abs (f->range.start + f->range.end) < 1.0e-3f * (f->range.end - f->range.start);
    return false;
}
} // namespace ucshared
