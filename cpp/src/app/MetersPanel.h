// The Meters slot's display in the mixer: a spectrum (4096-point FFT, Hann window, log frequency axis, slow fall),
// an oscilloscope (triggered on a rising zero crossing) or a tuner (YIN, as the Audio Lab's), above a row of readings
// (momentary / short-term / integrated LUFS, true peak, correlation). Click the readings to restart integrated loudness
// and the peak hold.
#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Theme.h"
#include "engine/Analysis.h"
#include "engine/Effects.h"

namespace ui {

class MetersPanel : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    MetersPanel(const std::string& slotId, std::string view) : tap(us::MeterTap::forSlot(slotId)), mode(std::move(view)) { startTimerHz(30); }
    int idealHeight() const { return 220; }
    void mouseDown(const juce::MouseEvent& e) override { if (e.y > getHeight() - 36) { tap->resetReq++; repaint(); } }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat(); auto row = r.removeFromBottom(34); r.removeFromBottom(4);
        g.setColour(BG); g.fillRoundedRectangle(r, 6); g.setColour(LINE); g.drawRoundedRectangle(r.reduced(0.5f), 6, 1);
        auto a = r.reduced(8, 6);
        if (mode == "scope") scope(g, a); else if (mode == "tuner") tuner(g, a); else spectrum(g, a);
        readings(g, row);
    }
private:
    std::shared_ptr<us::MeterTap> tap; std::string mode; juce::dsp::FFT fft{ 12 }; std::vector<float> L, R, work, shown; double pitch = -1; float zoom = 1;
    static constexpr int N = 4096;

    void timerCallback() override {
        if (!isShowing()) return;
        tap->last(N, L, R);
        if (mode == "spectrum" || mode.empty()) {
            work.assign(2 * N, 0.f);
            for (int i = 0; i < N; i++) work[(size_t)i] = 0.5f * (L[(size_t)i] + R[(size_t)i]) * (float)(0.5 - 0.5 * std::cos(2 * us::PI * i / (N - 1)));
            fft.performFrequencyOnlyForwardTransform(work.data(), true);
            if (shown.size() != N / 2) shown.assign(N / 2, -120.f);
            for (int b = 0; b < N / 2; b++) { const float db = 20 * std::log10(work[(size_t)b] * 4.f / N + 1e-9f); shown[(size_t)b] = std::max(db, shown[(size_t)b] - 1.5f); }
        } else if (mode == "tuner") {
            std::vector<float> m(2048); for (int i = 0; i < 2048; i++) m[(size_t)i] = 0.5f * (L[(size_t)(N - 2048 + i)] + R[(size_t)(N - 2048 + i)]);
            pitch = us::pitchOf(m, tap->rate.load());
        }
        repaint();
    }
    void grid(juce::Graphics& g, juce::Rectangle<float> a, double lo, double hi) {
        g.setFont(font(FS * 0.75f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 }) {
            const float x = a.getX() + (float)(std::log(f / lo) / std::log(hi / lo)) * a.getWidth();
            g.setColour(LINE.withAlpha(0.6f)); g.fillRect(x, a.getY(), 1.f, a.getHeight());
            g.setColour(DIM); g.drawText(f >= 1000 ? juce::String((int)(f / 1000)) + "k" : juce::String((int)f), (int)x + 2, (int)a.getBottom() - 14, 30, 12, juce::Justification::left);
        }
        for (int db = -12; db >= -84; db -= 24) { const float y = a.getY() + (float)(-db / 96.0) * a.getHeight(); g.setColour(LINE.withAlpha(0.4f)); g.fillRect(a.getX(), y, a.getWidth(), 1.f); g.setColour(DIM); g.drawText(juce::String(db), (int)a.getX() + 2, (int)y + 1, 30, 12, juce::Justification::left); }
    }
    void spectrum(juce::Graphics& g, juce::Rectangle<float> a) {
        const double lo = 20, hi = 20000, sr = tap->rate.load(); grid(g, a, lo, hi);
        if (shown.empty()) return;
        juce::Path p; bool first = true;
        for (int px = 0; px <= (int)a.getWidth(); px += 2) {
            const double f0 = lo * std::pow(hi / lo, (double)px / a.getWidth()), f1 = lo * std::pow(hi / lo, (double)(px + 2) / a.getWidth());
            int b0 = (int)(f0 / sr * N), b1 = std::max(b0 + 1, (int)(f1 / sr * N)); b0 = juce::jlimit(1, N / 2 - 1, b0); b1 = juce::jlimit(b0 + 1, N / 2, b1);
            float v = -120; for (int b = b0; b < b1; b++) v = std::max(v, shown[(size_t)b]);
            const float y = a.getY() + juce::jlimit(0.f, 1.f, -v / 96.f) * a.getHeight();
            if (first) { p.startNewSubPath(a.getX() + px, y); first = false; } else p.lineTo(a.getX() + px, y);
        }
        juce::Path fill = p; fill.lineTo(a.getRight(), a.getBottom()); fill.lineTo(a.getX(), a.getBottom()); fill.closeSubPath();
        g.setColour(ACCENT.withAlpha(0.18f)); g.fillPath(fill); g.setColour(ACCENT); g.strokePath(p, juce::PathStrokeType(1.5f));
    }
    void scope(juce::Graphics& g, juce::Rectangle<float> a) {
        g.setColour(LINE.withAlpha(0.6f)); g.fillRect(a.getX(), a.getCentreY(), a.getWidth(), 1.f);
        if (L.size() < (size_t)N) return;
        const int span = (int)std::min(2048.0, tap->rate.load() * 0.02);   // about 20 ms
        int s = N - span - 1; for (int i = N - span - 2; i > N - span - 1800 && i > 1; i--) { const float m0 = L[(size_t)i - 1] + R[(size_t)i - 1], m1 = L[(size_t)i] + R[(size_t)i]; if (m0 < 0 && m1 >= 0) { s = i; break; } }
        float pk = 0.02f; for (int i = 0; i < span; i++) pk = std::max({ pk, std::abs(L[(size_t)(s + i)]), std::abs(R[(size_t)(s + i)]) });
        zoom = pk > zoom ? pk : zoom * 0.8f + pk * 0.2f;   // auto zoom: jumps up for loud parts, eases back down
        const float k = a.getHeight() * 0.46f / zoom;
        g.setColour(DIM); g.setFont(font(FS * 0.75f)); g.drawText("x" + juce::String(1.f / zoom, 1), a.toNearestInt().removeFromTop(14), juce::Justification::topRight);
        for (int ch = 0; ch < 2; ch++) {
            const auto& v = ch ? R : L; juce::Path p;
            for (int i = 0; i < span; i++) { const float x = a.getX() + a.getWidth() * i / (float)(span - 1), y = a.getCentreY() - juce::jlimit(-a.getHeight() * 0.5f, a.getHeight() * 0.5f, v[(size_t)(s + i)] * k); if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y); }
            g.setColour(ch ? ACCENT2.withAlpha(0.8f) : ACCENT); g.strokePath(p, juce::PathStrokeType(1.4f));
        }
    }
    void tuner(juce::Graphics& g, juce::Rectangle<float> a) {
        const int n = pitch < 0 ? -1 : (int)std::round(pitch); const float off = n < 0 ? 0.f : (float)(pitch - n);
        g.setColour(n < 0 ? DIM : ACCENT); g.setFont(fontB(FS * 2.6f));
        g.drawText(n < 0 ? juce::String::charToString((juce::juce_wchar)0x2013) : juce::MidiMessage::getMidiNoteName(n, true, true, 4), a.removeFromTop(a.getHeight() * 0.55f), juce::Justification::centred);
        auto bar = a.withSizeKeepingCentre(std::min(360.f, a.getWidth() - 20), 10); g.setColour(BG3); g.fillRoundedRectangle(bar, 5);
        g.setColour(DIM); g.fillRect(bar.getCentreX(), bar.getY() - 4, 1.f, bar.getHeight() + 8);
        if (n >= 0) { g.setColour(std::abs(off) < 0.05f ? GOOD : ACCENT2); g.fillRoundedRectangle(bar.getCentreX() - 6 + off * bar.getWidth(), bar.getY() - 3, 12, 16, 3);
            g.setColour(DIM); g.setFont(font(FS * 0.9f)); g.drawText((off >= 0 ? "+" : "") + juce::String((int)std::round(off * 100)) + " cents", bar.translated(0, 16).withHeight(18).toNearestInt(), juce::Justification::centred); }
    }
    void readings(juce::Graphics& g, juce::Rectangle<float> row) {
        auto f = [](float v, const char* unit) { return v < -150 ? juce::String::charToString((juce::juce_wchar)0x2013) : juce::String(v, 1) + " " + unit; };
        const char* labels[] = { "Momentary", "Short-term", "Integrated", "True peak", "Correlation" };
        const juce::String vals[] = { f(tap->momentary, "LUFS"), f(tap->shortTerm, "LUFS"), f(tap->integrated, "LUFS"), f(tap->truePeak, "dBTP"), juce::String(tap->corr.load(), 2) };
        const float w = row.getWidth() / 5;
        for (int i = 0; i < 5; i++) {
            auto c = row.removeFromLeft(w).toNearestInt();
            g.setColour(DIM); g.setFont(font(FS * 0.72f)); g.drawFittedText(labels[i], c.removeFromTop(c.getHeight() / 2), juce::Justification::centredBottom, 1);
            g.setColour(i == 3 && tap->truePeak > -1.0f ? BAD : i == 4 && tap->corr < 0 ? WARN : TEXT); g.setFont(fontB(FS * 0.86f)); g.drawFittedText(vals[i], c, juce::Justification::centredTop, 1);
        }
    }
};

}   // namespace ui
