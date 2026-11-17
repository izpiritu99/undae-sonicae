// undae-compare: how close two renders of the same project are (the browser engine's export vs the native engine's).
//   undae-compare <a.wav> <b.wav>
// Prints level, stereo balance, the loudness envelope (100 ms windows) and the spectral balance per band.
#include <cmath>
#include <iostream>
#include <iomanip>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

static double db(double v) { return 20 * std::log10(std::max(v, 1e-12)); }

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: undae-compare <a.wav> <b.wav>\n"; return 2; }
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    juce::AudioBuffer<float> buf[2]; double sr = 48000;
    for (int k = 0; k < 2; k++) {
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1 + k])));
        if (!r) { std::cerr << "cannot read " << argv[1 + k] << "\n"; return 1; }
        buf[k].setSize(2, (int)r->lengthInSamples); r->read(&buf[k], 0, (int)r->lengthInSamples, 0, true, true); sr = r->sampleRate;
    }
    const int n = std::min(buf[0].getNumSamples(), buf[1].getNumSamples());
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "length      " << buf[0].getNumSamples() / sr << " s vs " << buf[1].getNumSamples() / sr << " s\n";
    const char* names[] = { "a", "b" };
    for (int k = 0; k < 2; k++) {
        double pk = 0, s = 0, sl = 0, srr = 0;
        for (int i = 0; i < n; i++) { const double l = buf[k].getSample(0, i), r = buf[k].getSample(1, i); pk = std::max({ pk, std::abs(l), std::abs(r) }); s += (l * l + r * r) / 2; sl += l * l; srr += r * r; }
        std::cout << names[k] << "           peak " << db(pk) << " dBFS   RMS " << db(std::sqrt(s / n)) << " dBFS   L/R " << db(std::sqrt(sl / n)) << " / " << db(std::sqrt(srr / n)) << "\n";
    }
    const int win = (int)(sr * 0.1); std::vector<double> e[2];
    for (int k = 0; k < 2; k++) for (int i = 0; i + win <= n; i += win) { double s = 0; for (int j = i; j < i + win; j++) { const double m = 0.5 * (buf[k].getSample(0, j) + buf[k].getSample(1, j)); s += m * m; } e[k].push_back(std::sqrt(s / win)); }
    double ma = 0, mb = 0; const size_t W = e[0].size(); for (size_t i = 0; i < W; i++) { ma += e[0][i]; mb += e[1][i]; } ma /= W; mb /= W;
    double cov = 0, va = 0, vb = 0; std::vector<double> diffs;
    for (size_t i = 0; i < W; i++) { cov += (e[0][i] - ma) * (e[1][i] - mb); va += (e[0][i] - ma) * (e[0][i] - ma); vb += (e[1][i] - mb) * (e[1][i] - mb); if (e[0][i] > 1e-4 || e[1][i] > 1e-4) diffs.push_back(db(e[1][i]) - db(e[0][i])); }
    std::sort(diffs.begin(), diffs.end()); const double med = diffs.empty() ? 0 : diffs[diffs.size() / 2];
    std::vector<double> dev; for (double d : diffs) dev.push_back(std::abs(d - med)); std::sort(dev.begin(), dev.end());
    std::cout << "envelope    correlation " << std::setprecision(3) << cov / std::sqrt(va * vb + 1e-30) << std::setprecision(2) << ", b - a median " << med << " dB, 90% of windows within " << (dev.empty() ? 0 : dev[dev.size() * 9 / 10]) << " dB of that\n";
    // spectral balance: averaged 4096-point spectra
    const int order = 12, N = 1 << order; juce::dsp::FFT fft(order); std::vector<float> w(N); for (int i = 0; i < N; i++) w[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N));
    const double edges[] = { 20, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000, 24000 }; double band[2][10] = {};
    for (int k = 0; k < 2; k++) {
        std::vector<float> fr(2 * N);
        for (int i = 0; i + N <= n; i += N / 2) {
            for (int j = 0; j < N; j++) fr[j] = 0.5f * (buf[k].getSample(0, i + j) + buf[k].getSample(1, i + j)) * w[j];
            std::fill(fr.begin() + N, fr.end(), 0.f); fft.performFrequencyOnlyForwardTransform(fr.data());
            for (int b = 0; b <= N / 2; b++) { const double f = b * sr / N; for (int q = 0; q < 10; q++) if (f >= edges[q] && f < edges[q + 1]) band[k][q] += (double)fr[b] * fr[b]; }
        }
    }
    double ta = 0, tb = 0; for (int q = 0; q < 10; q++) { ta += band[0][q]; tb += band[1][q]; }
    std::cout << "spectrum    band           a share   b share   b vs a\n";
    for (int q = 0; q < 10; q++) std::cout << "            " << std::setw(5) << (int)edges[q] << "-" << std::setw(5) << (int)edges[q + 1] << " Hz  " << std::setw(6) << 100 * band[0][q] / ta << "%   " << std::setw(6) << 100 * band[1][q] / tb << "%   " << std::showpos << db(std::sqrt(band[1][q] / tb)) - db(std::sqrt(band[0][q] / ta)) << std::noshowpos << " dB\n";
    std::cout << "per 2 s     RMS a/b dB:";
    for (int i = 0; i + (int)sr <= n; i += (int)sr * 2) { double s[2] = {}; for (int k = 0; k < 2; k++) for (int j = i; j < i + (int)sr; j++) { const double m = 0.5 * (buf[k].getSample(0, j) + buf[k].getSample(1, j)); s[k] += m * m; } std::cout << " " << (int)std::round(db(std::sqrt(s[0] / sr))) << "/" << (int)std::round(db(std::sqrt(s[1] / sr))); }
    std::cout << "\n";
    return 0;
}
