#pragma once

#include "ModernLook.h"

#include <functional>
#include <iterator>

namespace ucshared
{
// An analog skin that stays easy to read: a warm cream faceplate with dark
// lettering, black bakelite knobs with a printed scale and a thin value arc,
// lit push buttons, jewel lamps for switches, cream menus. Values, groups
// and the help line stay where they are in the modern skin.
namespace analog
{
// The faceplate's colours, for the panel chosen in the settings (setPanel).
// The screens, the lamps and the knobs stay the same on every panel.
inline juce::Colour faceplate { 0xffe4dccb };
inline juce::Colour recess { 0xffd6ccb7 };
inline juce::Colour edge { 0xffa99d86 };
inline juce::Colour paper { 0xfff3eee3 };
inline juce::Colour ink { 0xff201b16 };
inline juce::Colour dimInk { 0xff6e6457 };
inline juce::Colour oxblood { 0xffa8322a };
inline juce::Colour brass { 0xffb07a1e };
inline bool darkPanel = false;
const juce::Colour lamp { 0xfff2a93b };
const juce::Colour litText { 0xff201b16 }; // on a lit (amber) button
const juce::Colour screen { 0xff181915 };
const juce::Colour screenGrid { 0xff2f332b };
const juce::Colour phosphor { 0xfff2b34a };
const juce::Colour screenText { 0xffd2c9b6 };
const juce::Colour screenDim { 0xff8c8574 };

// The panels the settings offer, by the name stored there.
struct Panel
{
    const char* id;
    const char* name;
    juce::uint32 faceplate, recess, edge, paper, ink, dimInk, accent, brass;
    bool dark;
};
inline constexpr Panel panels[] = {
    { "cream", "Cream", 0xffe4dccb, 0xffd6ccb7, 0xffa99d86, 0xfff3eee3, 0xff201b16, 0xff6e6457, 0xffa8322a, 0xffb07a1e, false },
    { "silver", "Silver", 0xffc9cbcc, 0xffbabdbf, 0xff8d9195, 0xffe4e6e7, 0xff1b1d20, 0xff5b5f64, 0xffaa3428, 0xff94681c, false },
    { "black", "Black", 0xff22201e, 0xff2c2926, 0xff4d4740, 0xff38342f, 0xffece4d2, 0xffa59c8a, 0xffe0614f, 0xffd9a645, true },
    { "green", "Green", 0xff2c4336, 0xff263a2f, 0xff52705e, 0xff3a5546, 0xffefe7d3, 0xffb5b49c, 0xffeb7c60, 0xffe2b453, true },
    { "navy", "Navy", 0xff243047, 0xff1f2a3e, 0xff4d5f80, 0xff334262, 0xffeee7d6, 0xffaab0bf, 0xffeb7262, 0xffe0b257, true },
};

inline void setPanel (const juce::String& id)
{
    const Panel* p = &panels[0];
    for (const auto& candidate : panels)
        if (id == candidate.id)
            p = &candidate;
    faceplate = juce::Colour (p->faceplate);
    recess = juce::Colour (p->recess);
    edge = juce::Colour (p->edge);
    paper = juce::Colour (p->paper);
    ink = juce::Colour (p->ink);
    dimInk = juce::Colour (p->dimInk);
    oxblood = juce::Colour (p->accent);
    brass = juce::Colour (p->brass);
    darkPanel = p->dark;
}

// The panel menu for the editors: the analog panels, then the flat modern
// skin ("modern"). The choice is stored by id in the plug-in's settings
// file; settings from before the menu stored only whether Analog was on.
inline juce::String panelChoice (juce::PropertiesFile& settings, const juce::String& key)
{
    if (settings.containsKey (key))
        return settings.getValue (key);
    return settings.getBoolValue ("analog", true) ? "cream" : "modern";
}

inline juce::String panelName (const juce::String& id)
{
    for (const auto& p : panels)
        if (id == p.id)
            return p.name;
    return "Modern";
}

inline void showPanelMenu (juce::Component& target, const juce::String& current, std::function<void (const juce::String&)> chosen)
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Panel");
    int itemId = 1;
    for (const auto& p : panels)
        menu.addItem (itemId++, p.name, true, current == p.id);
    menu.addSeparator();
    menu.addItem (100, "Modern (flat)", true, current == "modern");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target), [chosen] (int result)
    {
        if (result == 100)
            chosen ("modern");
        else if (result >= 1 && result <= static_cast<int> (std::size (panels)))
            chosen (panels[result - 1].id);
    });
}

// Futura on macOS, for titles and group names; Windows has no Futura, so
// its closest standard face there.
inline juce::Font titleFont (float height)
{
   #if JUCE_WINDOWS
    return juce::Font (juce::FontOptions ("Century Gothic", height, juce::Font::bold));
   #else
    return juce::Font (juce::FontOptions ("Futura", height, juce::Font::bold));
   #endif
}

// The unit's front panel, shared by the plug-ins: rack ears down both
// sides, the nameplate, and the maker, model and serial engraved along the
// bottom edge.
inline void drawRackEars (juce::Graphics& g, int width, int height)
{
    for (const float x : { 0.0f, static_cast<float> (width) - 8.0f })
    {
        const juce::Rectangle<float> ear (x, 0.0f, 8.0f, static_cast<float> (height));
        g.setGradientFill (juce::ColourGradient (faceplate.darker (0.06f), ear.getX(), 0.0f, faceplate.darker (0.2f), ear.getRight(), 0.0f, false));
        g.fillRect (ear);
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.drawVerticalLine (static_cast<int> (x == 0.0f ? ear.getRight() : ear.getX()), 0.0f, static_cast<float> (height));
        for (const float y : { 46.0f, static_cast<float> (height) - 46.0f })
        {
            const auto slot = juce::Rectangle<float> (4.0f, 18.0f).withCentre ({ ear.getCentreX(), y });
            g.setColour (juce::Colour (0xff2a2520));
            g.fillRoundedRectangle (slot, 2.0f);
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawRoundedRectangle (slot.expanded (0.5f).translated (0.0f, 0.5f), 2.0f, 0.6f);
        }
    }
}

inline void drawEngraving (juce::Graphics& g, int width, int height, const juce::String& model, const juce::String& serial)
{
    const juce::Rectangle<int> engraving (20, height - 12, width - 40, 11);
    const auto line = "GENOMERSIVE STUDIO   " + model + "   S/N " + serial;
    g.setFont (titleFont (8.5f));
    g.setColour (darkPanel ? juce::Colours::black.withAlpha (0.5f) : juce::Colours::white.withAlpha (0.6f));
    g.drawText (line, engraving.translated (0, 1), juce::Justification::centredRight);
    g.setColour (dimInk);
    g.drawText (line, engraving, juce::Justification::centredRight);
}

// Black anodised, a brass edge and two screws; the model number large, the
// name and the maker beside it.
inline void drawNameplate (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& model, const juce::String& name)
{
    const auto plate = area.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRoundedRectangle (plate.translated (1.0f, 1.5f), 4.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff36312b), plate.getX(), plate.getY(), juce::Colour (0xff141210), plate.getRight(), plate.getBottom(), false));
    g.fillRoundedRectangle (plate, 4.0f);
    g.setColour (brass.withAlpha (0.9f));
    g.drawRoundedRectangle (plate.reduced (2.0f), 3.0f, 1.0f);
    for (const float x : { plate.getX() + 9.0f, plate.getRight() - 9.0f })
    {
        const auto head = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ x, plate.getCentreY() });
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe8dcc0), head.getX(), head.getY(), juce::Colour (0xff7a6a4c), head.getRight(), head.getBottom(), false));
        g.fillEllipse (head);
        g.setColour (juce::Colour (0xff3a3226));
        g.drawLine (juce::Line<float> (head.getX() + 1.0f, head.getCentreY(), head.getRight() - 1.0f, head.getCentreY()), 1.0f);
    }
    auto inner = area.reduced (18, 4);
    g.setColour (juce::Colour (0xfff3ead6));
    g.setFont (titleFont (25.0f));
    const auto modelArea = inner.removeFromLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), model)) + 4);
    g.drawText (model, modelArea, juce::Justification::centredLeft);
    inner.removeFromLeft (8);
    g.setColour (brass.withAlpha (0.8f));
    g.drawVerticalLine (inner.getX(), static_cast<float> (inner.getY() + 3), static_cast<float> (inner.getBottom() - 3));
    inner.removeFromLeft (9);
    g.setColour (juce::Colour (0xfff3ead6));
    g.setFont (titleFont (13.0f));
    g.drawFittedText (name, inner.removeFromTop (inner.getHeight() / 2 + 1), juce::Justification::bottomLeft, 1, 0.8f);
    g.setColour (brass);
    g.setFont (titleFont (9.0f).withExtraKerningFactor (0.2f));
    g.drawText ("GENOMERSIVE STUDIO", inner, juce::Justification::topLeft);
}
} // namespace analog

class AnalogLook : public ModernLook
{
public:
    // While the host bypasses the plug-in the editor sets "skinDimmed" on
    // itself, and the lamps and lit buttons dim.
    static bool dimmed (juce::Component& c)
    {
        if (auto* editor = c.findParentComponentOfClass<juce::AudioProcessorEditor>())
            return static_cast<bool> (editor->getProperties().getWithDefault ("skinDimmed", false));
        return false;
    }

    AnalogLook() { refreshColours(); }

    // Takes the current panel's colours (after analog::setPanel).
    void refreshColours()
    {
        using namespace analog;
        setColour (juce::ResizableWindow::backgroundColourId, faceplate);
        setColour (juce::Label::textColourId, ink);
        setColour (juce::Slider::textBoxTextColourId, ink);
        setColour (juce::Slider::textBoxHighlightColourId, lamp.withAlpha (0.5f));
        setColour (juce::Slider::rotarySliderOutlineColourId, edge);
        setColour (juce::Slider::thumbColourId, ink);
        setColour (juce::Slider::backgroundColourId, edge);
        setColour (juce::ComboBox::backgroundColourId, paper);
        setColour (juce::ComboBox::outlineColourId, edge);
        setColour (juce::ComboBox::textColourId, ink);
        setColour (juce::ComboBox::arrowColourId, dimInk);
        setColour (juce::PopupMenu::backgroundColourId, paper);
        setColour (juce::PopupMenu::textColourId, ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, lamp);
        setColour (juce::PopupMenu::highlightedTextColourId, ink);
        setColour (juce::PopupMenu::headerTextColourId, dimInk);
        setColour (juce::TextButton::buttonColourId, paper);
        setColour (juce::TextButton::buttonOnColourId, lamp);
        setColour (juce::TextButton::textColourOffId, ink);
        setColour (juce::TextButton::textColourOnId, litText);
        setColour (juce::ToggleButton::textColourId, ink);
        setColour (juce::ToggleButton::tickColourId, lamp);
        setColour (juce::TextEditor::backgroundColourId, paper);
        setColour (juce::TextEditor::textColourId, ink);
        setColour (juce::TextEditor::outlineColourId, edge);
        setColour (juce::TextEditor::focusedOutlineColourId, oxblood);
        setColour (juce::AlertWindow::backgroundColourId, faceplate);
        setColour (juce::AlertWindow::textColourId, ink);
    }

    // A black knob with a skirt and a white line, a printed scale of 11
    // ticks round it, and the value as a thin arc in the control's colour
    // just inside the scale (from the centre for bipolar controls).
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using namespace analog;
        const auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y), static_cast<float> (width), static_cast<float> (height)).reduced (2.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const bool enabled = slider.isEnabled();
        const bool hot = slider.isMouseOverOrDragging() && enabled;
        auto colour = slider.findColour (juce::Slider::rotarySliderFillColourId);
        if (! enabled)
            colour = colour.withSaturation (0.0f).withAlpha (0.5f);

        // Scale.
        g.setColour (enabled ? ink.withAlpha (0.75f) : dimInk.withAlpha (0.5f));
        for (int i = 0; i <= 10; ++i)
        {
            const float angle = startAngle + (endAngle - startAngle) * static_cast<float> (i) / 10.0f;
            const bool major = i % 5 == 0;
            g.drawLine ({ centre.getPointOnCircumference (radius * (major ? 0.80f : 0.86f), angle), centre.getPointOnCircumference (radius, angle) },
                        major ? 1.6f : 1.0f);
        }

        // Value arc.
        const float arcRadius = radius * 0.74f;
        const float lineWidth = juce::jmax (2.0f, radius * 0.08f);
        const bool bipolar = static_cast<bool> (slider.getProperties().getWithDefault ("bipolar", false));
        const float valueAngle = startAngle + position * (endAngle - startAngle);
        const float fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (edge.withAlpha (0.6f));
        g.strokePath (track, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        if (std::abs (valueAngle - fromAngle) > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (fromAngle, valueAngle), juce::jmax (fromAngle, valueAngle), true);
            g.setColour (hot ? colour.brighter (0.2f) : colour);
            g.strokePath (value, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        }

        // The value in use, when an automatic control moved it off the knob:
        // a lit red lamp on the scale ring.
        if (const auto live = slider.getProperties().getVarPointer ("live"); live != nullptr)
        {
            const float liveAngle = startAngle + static_cast<float> (static_cast<double> (*live)) * (endAngle - startAngle);
            const auto at = centre.getPointOnCircumference (arcRadius, liveAngle);
            const float size = juce::jmax (6.0f, lineWidth * 2.6f);
            g.setColour (juce::Colour (0xff2a0c08));
            g.fillEllipse (juce::Rectangle<float> (size + 2.0f, size + 2.0f).withCentre (at));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffffb08a), at.x - size * 0.25f, at.y - size * 0.25f,
                                                     juce::Colour (0xffd8321e), at.x + size * 0.5f, at.y + size * 0.5f, true));
            g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (at));
        }

        // A black skirt, then a machined aluminium cap lit from the top left
        // (concentric turning marks, a bevel), a shadow beneath.
        const float skirt = arcRadius - lineWidth * 0.5f - 2.0f;
        const float cap = skirt * 0.70f;
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (skirt * 2.0f, skirt * 2.0f).withCentre (centre.translated (1.0f, 2.0f)));
        const auto skirtArea = juce::Rectangle<float> (skirt * 2.0f, skirt * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (hot ? 0xff3e3a36 : 0xff34302c), skirtArea.getX(), skirtArea.getY(),
                                                 juce::Colour (0xff12100e), skirtArea.getRight(), skirtArea.getBottom(), false));
        g.fillEllipse (skirtArea);
        const auto capArea = juce::Rectangle<float> (cap * 2.0f, cap * 2.0f).withCentre (centre);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (capArea.expanded (1.2f).translated (0.6f, 0.8f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (enabled ? 0xfff1eee8 : 0xffc9c6c0), capArea.getX(), capArea.getY(),
                                                 juce::Colour (enabled ? 0xff8b877f : 0xff8a8781), capArea.getRight(), capArea.getBottom(), false));
        g.fillEllipse (capArea);
        for (float r = cap * 0.92f; r > cap * 0.12f; r -= 1.6f)
        {
            g.setColour ((static_cast<int> (r * 10.0f) % 2 == 0 ? juce::Colours::white : juce::Colours::black).withAlpha (0.07f));
            g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 0.8f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawEllipse (capArea.reduced (0.6f), 0.8f);

        // Pointer: engraved into the cap, a cream notch on the skirt.
        const float pointerWidth = juce::jmax (1.8f, radius * 0.06f);
        g.setColour (juce::Colour (0xff1c1915));
        g.drawLine ({ centre.getPointOnCircumference (cap * 0.18f, valueAngle), centre.getPointOnCircumference (cap * 0.95f, valueAngle) }, pointerWidth);
        g.setColour (enabled ? juce::Colour (0xfff4efe4) : juce::Colour (0xff8c867b));
        g.drawLine ({ centre.getPointOnCircumference (cap * 1.08f, valueAngle), centre.getPointOnCircumference (skirt * 0.97f, valueAngle) }, pointerWidth);
    }

    // Switches: a jewel lamp that glows when on, the label beside it.
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool) override
    {
        using namespace analog;
        auto area = button.getLocalBounds().toFloat();
        const float d = juce::jmin (14.0f, area.getHeight() - 6.0f);
        const auto bezel = area.removeFromLeft (d + 8.0f).withSizeKeepingCentre (d + 4.0f, d + 4.0f);
        const auto jewel = bezel.reduced (2.0f);
        const bool on = button.getToggleState();
        const bool enabled = button.isEnabled();
        auto colour = button.findColour (juce::ToggleButton::tickColourId);
        if (! enabled)
            colour = colour.withSaturation (0.0f);
        const bool dim = dimmed (button);
        if (dim)
            colour = colour.darker (0.6f);

        g.setColour (juce::Colour (0xff8f8676));
        g.fillEllipse (bezel);
        g.setColour (juce::Colour (0xfff7f2e8));
        g.drawEllipse (bezel.reduced (0.5f), 1.0f);
        if (on)
        {
            g.setColour (colour.withAlpha (dim ? 0.0f : 0.35f));
            g.fillEllipse (bezel.expanded (3.0f));
            g.setGradientFill (juce::ColourGradient (colour.brighter (0.6f), jewel.getCentreX(), jewel.getCentreY() - d * 0.2f,
                                                     colour.darker (0.2f), jewel.getCentreX(), jewel.getBottom() + d * 0.3f, true));
        }
        else
            g.setGradientFill (juce::ColourGradient (juce::Colour (highlighted ? 0xff6b4a2c : 0xff553a24), jewel.getCentreX(), jewel.getY(),
                                                     juce::Colour (0xff2a1d14), jewel.getCentreX(), jewel.getBottom(), false));
        g.fillEllipse (jewel);
        g.setColour (juce::Colours::white.withAlpha (on ? 0.6f : 0.25f));
        g.fillEllipse (jewel.getX() + d * 0.25f, jewel.getY() + d * 0.15f, d * 0.3f, d * 0.22f);

        area.removeFromLeft (4.0f);
        g.setColour (enabled ? ink : dimInk.withAlpha (0.6f));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (button.getButtonText(), area.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    // Push buttons: raised cream keys; a selected one is lit amber.
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                               bool highlighted, bool down) override
    {
        using namespace analog;
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f, 1.0f);
        const bool lit = button.getToggleState();
        auto colour = background;
        if (lit && dimmed (button))
            colour = colour.darker (0.35f);
        if (down)
            colour = colour.darker (0.08f);
        else if (highlighted)
            colour = colour.brighter (0.06f);
        if (! button.isEnabled())
            colour = colour.withMultipliedAlpha (0.5f);
        const bool left = button.isConnectedOnLeft(), right = button.isConnectedOnRight();
        auto shapeOf = [left, right] (juce::Rectangle<float> r)
        {
            juce::Path p;
            p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), 3.5f, 3.5f, ! left, ! right, ! left, ! right);
            return p;
        };
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.fillPath (shapeOf (bounds.translated (0.0f, down ? 0.5f : 1.5f)));
        const auto shape = shapeOf (bounds);
        g.setGradientFill (juce::ColourGradient (colour.brighter (lit ? 0.25f : 0.08f), 0.0f, bounds.getY(),
                                                 colour.darker (lit ? 0.12f : 0.06f), 0.0f, bounds.getBottom(), false));
        g.fillPath (shape);
        g.setColour (lit ? colour.darker (0.35f) : edge);
        g.strokePath (shape, juce::PathStrokeType (1.0f));
        if (! lit)
        {
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawHorizontalLine (static_cast<int> (bounds.getY()) + 1, bounds.getX() + 3.0f, bounds.getRight() - 3.0f);
        }
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (13.0f, static_cast<float> (buttonHeight) * 0.55f), juce::Font::bold));
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        using namespace analog;
        const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)).reduced (0.5f, 1.0f);
        g.setColour (paper.brighter (box.isMouseOver (true) ? 0.04f : 0.0f));
        g.fillRoundedRectangle (bounds, 3.5f);
        g.setColour (juce::Colours::black.withAlpha (0.12f));
        g.drawHorizontalLine (static_cast<int> (bounds.getY()) + 1, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
        g.setColour (edge);
        g.drawRoundedRectangle (bounds, 3.5f, 1.0f);
        const float cx = static_cast<float> (width) - 12.0f, cy = static_cast<float> (height) * 0.5f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (box.isEnabled() ? ink : edge);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};
} // namespace ucshared
