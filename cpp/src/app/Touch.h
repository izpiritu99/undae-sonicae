// Touch screens (Android, touch laptops): a long press acts as a right-click (menus, deleting a note or a clip), two
// fingers pinch to zoom the Piano Roll and the playlist. Installed once as a global mouse listener; mouse input is left alone.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ui {

// Buttons that open a menu on right-click check this too, so a long press opens the same menu.
inline bool& touchMenu() { static bool f = false; return f; }
inline bool wantsMenu() { return juce::ModifierKeys::currentModifiers.isPopupMenu() || touchMenu(); }
inline std::function<bool(juce::Button*)>& longPressHook() { static std::function<bool(juce::Button*)> f; return f; }   // buttons with their own right-click action

class TouchHelper : public juce::MouseListener, private juce::Timer {
public:
    TouchHelper() { juce::Desktop::getInstance().addGlobalMouseListener(this); }
    ~TouchHelper() override { juce::Desktop::getInstance().removeGlobalMouseListener(this); }

    void mouseDown(const juce::MouseEvent& e) override {
        if (!e.source.isTouch()) return;
        fingers[e.source.getIndex()] = e.getScreenPosition().toFloat();
        if (fingers.size() == 2) { stopTimer(); press = {}; pinchStart = spread(); pinchLast = pinchStart; pinchComp = e.eventComponent; return; }
        press = { e.eventComponent, e.getScreenPosition(), e.source.getIndex(), false }; startTimer(550);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!e.source.isTouch()) return;
        fingers[e.source.getIndex()] = e.getScreenPosition().toFloat();
        if (press.comp && e.source.getIndex() == press.finger && e.getScreenPosition().getDistanceFrom(press.at) > 12) { stopTimer(); press.comp = nullptr; }
        if (fingers.size() == 2 && pinchComp) {   // pinch: zoom by how much the fingers spread, around their midpoint
            const float d = spread(); if (pinchLast > 20 && std::abs(d / pinchLast - 1) > 0.02f) {
                auto mid = (fingers.begin()->second + std::next(fingers.begin())->second) * 0.5f;
                for (juce::Component* c = pinchComp; c; c = c->getParentComponent()) {   // the first component up the tree that zooms
                    const auto local = c->getLocalPoint(nullptr, mid.toInt());
                    juce::MouseEvent me(e.source, local.toFloat(), {}, 1, 0, 0, 0, 0, c, c, juce::Time::getCurrentTime(), local.toFloat(), juce::Time::getCurrentTime(), 1, false);
                    c->mouseMagnify(me, d / pinchLast);
                    if (zooms(c)) break;
                }
                pinchLast = d;
            }
        }
    }
    void mouseUp(const juce::MouseEvent& e) override {
        if (!e.source.isTouch()) return;
        fingers.erase(e.source.getIndex()); if (fingers.size() < 2) pinchComp = nullptr;
        if (e.source.getIndex() == press.finger) { stopTimer(); press = {}; }
    }

private:
    struct Press { juce::Component::SafePointer<juce::Component> comp; juce::Point<int> at; int finger = -1; bool fired = false; } press;
    std::map<int, juce::Point<float>> fingers; float pinchStart = 0, pinchLast = 0; juce::Component::SafePointer<juce::Component> pinchComp;
    float spread() const { if (fingers.size() < 2) return 0; return fingers.begin()->second.getDistanceFrom(std::next(fingers.begin())->second); }
    static bool zooms(juce::Component* c) { return c->getProperties().getWithDefault("pinchZoom", false); }
    void timerCallback() override {
        stopTimer(); auto* c = press.comp.getComponent(); if (!c) return;
        press.fired = true;
        if (auto* b = dynamic_cast<juce::Button*>(c)) {
            // a button: run its click as a menu request, and keep the finger's release from clicking it again
            touchMenu() = true; if (!(longPressHook() && longPressHook()(b)) && b->onClick) b->onClick(); touchMenu() = false;
            b->setEnabled(false); juce::Component::SafePointer<juce::Button> sb(b);
            juce::Timer::callAfterDelay(400, [sb] { if (sb) sb->setEnabled(true); });
            return;
        }
        // anything else: the same as a right-click where the finger is
        const auto local = c->getLocalPoint(nullptr, press.at).toFloat(); auto src = juce::Desktop::getInstance().getMainMouseSource();
        const juce::ModifierKeys rb(juce::ModifierKeys::rightButtonModifier);
        juce::MouseEvent me(src, local, rb, 1, 0, 0, 0, 0, c, c, juce::Time::getCurrentTime(), local, juce::Time::getCurrentTime(), 1, false);
        c->mouseDown(me); c->mouseUp(me.withNewPosition(local));
        if (auto* v = juce::Desktop::getInstance().getComponent(0)) (void)v;
    }
};

}   // namespace ui
