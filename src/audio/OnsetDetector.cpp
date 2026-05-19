#include "OnsetDetector.h"

#include <algorithm>
#include <cmath>

namespace
{
// Adaptive-threshold multiplier: a flux frame must exceed the trailing mean
// by this factor to count as an onset.
constexpr float kThresholdK = 1.8f;
// Absolute floor so a near-silent passage (tiny mean) can't let noise through.
constexpr float kThresholdFloor = 1.0e-3f;
// Trailing flux frames feeding the adaptive threshold (~170 ms at 256-hop).
constexpr int kFluxHistFrames = 32;
// Minimum gap between onsets — a guitarist cannot pick faster than this.
constexpr double kDebounceSeconds = 0.040;
// Net onset-timing calibration: the reported attack sits this far before the
// flux-peak frame's end. It absorbs the flux-peak-vs-attack lag (the transient
// takes time to fill the analysis window) plus any residual chain latency.
// Tuned from the live timing-error median — an early build ran ~39 ms late at
// ~11 ms of backdate, so ~50 ms centres it. Stored in seconds and converted to
// samples per device rate so the calibration holds at 44.1 kHz, 96 kHz, etc.
constexpr double kOnsetBackdateSeconds = 0.050;
} // namespace

OnsetDetector::OnsetDetector() { prepare(48000.0); }

void OnsetDetector::prepare(double sr)
{
    sampleRate = (sr > 0.0) ? sr : 48000.0;

    fft = std::make_unique<juce::dsp::FFT>(kFftOrder);
    fftIn.assign((size_t) kWindow, juce::dsp::Complex<float>{ 0.0f, 0.0f });
    fftOut.assign((size_t) kWindow, juce::dsp::Complex<float>{ 0.0f, 0.0f });
    mag.assign((size_t) (kWindow / 2 + 1), 0.0f);
    prevMag.assign((size_t) (kWindow / 2 + 1), 0.0f);
    hist.assign((size_t) kWindow, 0.0f);

    hann.assign((size_t) kWindow, 0.0f);
    for (int i = 0; i < kWindow; ++i)
        hann[(size_t) i] = 0.5f * (1.0f - std::cos(
            2.0 * juce::MathConstants<double>::pi * i / (kWindow - 1)));

    // Spectral-flux band: 70 Hz (below the lowest guitar/bass fundamental's
    // skirt) to 3 kHz — picks carry strong broadband energy here; rumble and
    // hiss outside it only add noise to the flux.
    const double binHz = sampleRate / kWindow;
    loBin = std::max(1, (int) std::floor(70.0 / binHz));
    hiBin = std::min(kWindow / 2, (int) std::ceil(3000.0 / binHz));

    // Derive the onset backdate for this device's sample rate so the timing
    // calibration is rate-independent.
    onsetBackdateSamples = (int) std::lround(kOnsetBackdateSeconds * sampleRate);

    reset();
}

void OnsetDetector::reset()
{
    std::fill(hist.begin(), hist.end(), 0.0f);
    std::fill(prevMag.begin(), prevMag.end(), 0.0f);
    histPos = 0;
    sinceHop = 0;
    primingCount = 0;
    framesSeen = 0;
    fluxA = fluxB = 0.0f;
    frameEndA = frameEndB = 0;
    fluxHist.clear();
    lastOnsetIndex = 0;
    haveOnset = false;
    // nextIndex / indexInit are intentionally left alone — the caller's
    // monotonic index space is unaffected by a history flush.
}

void OnsetDetector::process(const float* samples, size_t n, uint64_t firstSampleIndex,
                            std::vector<OnsetDetector::Onset>& out)
{
    if (samples == nullptr || n == 0) return;

    if (! indexInit)
    {
        nextIndex = firstSampleIndex;
        indexInit = true;
    }
    else if (firstSampleIndex != nextIndex)
    {
        // A gap (samples lost, or a seek): flux across the discontinuity is
        // meaningless — drop history and resync.
        reset();
        nextIndex = firstSampleIndex;
        indexInit = true;
    }

    for (size_t i = 0; i < n; ++i)
    {
        hist[histPos] = samples[i];
        histPos = (histPos + 1) % (size_t) kWindow;
        if (primingCount < kWindow) ++primingCount;
        ++sinceHop;

        const uint64_t idx = nextIndex;  // monotonic index of this sample
        ++nextIndex;

        if (sinceHop >= kHop && primingCount >= kWindow)
        {
            sinceHop = 0;
            processFrame(idx, out);
        }
    }
}

void OnsetDetector::processFrame(uint64_t frameEndIndex,
                                 std::vector<OnsetDetector::Onset>& out)
{
    // Assemble the windowed frame — `histPos` points at the oldest sample.
    for (int j = 0; j < kWindow; ++j)
    {
        const float s = hist[(histPos + (size_t) j) % (size_t) kWindow];
        fftIn[(size_t) j] = juce::dsp::Complex<float>{ s * hann[(size_t) j], 0.0f };
    }
    fft->perform(fftIn.data(), fftOut.data(), false);

    const int halfBins = kWindow / 2 + 1;
    for (int k = 0; k < halfBins; ++k)
    {
        const auto& c = fftOut[(size_t) k];
        mag[(size_t) k] = std::sqrt(c.real() * c.real() + c.imag() * c.imag());
    }

    // Spectral flux: sum of positive magnitude change across the band. The
    // very first frame has no predecessor — seed prevMag and report flux 0
    // so the empty-prevMag spike never enters the history or peak-picker.
    float flux = 0.0f;
    if (framesSeen > 0)
    {
        for (int k = loBin; k <= hiBin; ++k)
        {
            const float d = mag[(size_t) k] - prevMag[(size_t) k];
            if (d > 0.0f) flux += d;
        }
    }
    prevMag = mag;

    // Adaptive threshold from the trailing flux mean.
    float threshold = kThresholdFloor;
    if (! fluxHist.empty())
    {
        float sum = 0.0f;
        for (float f : fluxHist) sum += f;
        threshold = std::max(kThresholdFloor,
                             (sum / (float) fluxHist.size()) * kThresholdK);
    }

    // Confirm the previous frame (B) as a local maximum: B > A and B >= C.
    if (framesSeen >= 2 && fluxB > fluxA && fluxB >= flux && fluxB >= threshold)
    {
        const auto debounce = (uint64_t) (kDebounceSeconds * sampleRate);
        if (! haveOnset || frameEndB > lastOnsetIndex + debounce)
        {
            Onset o;
            o.sampleIndex = (frameEndB > (uint64_t) onsetBackdateSamples)
                          ? frameEndB - (uint64_t) onsetBackdateSamples : 0;
            o.strength = fluxB;
            out.push_back(o);
            lastOnsetIndex = frameEndB;
            haveOnset = true;
        }
    }

    // Roll the flux history and the A/B window forward.
    fluxHist.push_back(flux);
    if ((int) fluxHist.size() > kFluxHistFrames) fluxHist.pop_front();
    fluxA = fluxB;            frameEndA = frameEndB;
    fluxB = flux;             frameEndB = frameEndIndex;
    ++framesSeen;
}
