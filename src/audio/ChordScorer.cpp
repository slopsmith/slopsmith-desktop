#include "ChordScorer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
    // Standard-tuning MIDI base tables — verbatim from screen.js so
    // open-string MIDI values stay identical between the browser path
    // and the native port. Comments mirror the JS pitch labels for
    // sanity at a glance.
    const std::vector<int> kTuningBass4{ 28, 33, 38, 43 };                    // E1 A1 D2 G2
    const std::vector<int> kTuningBass5{ 23, 28, 33, 38, 43 };                // B0 E1 A1 D2 G2
    const std::vector<int> kTuningGuitar6{ 40, 45, 50, 55, 59, 64 };          // E2 A2 D3 G3 B3 E4
    const std::vector<int> kTuningGuitar7{ 35, 40, 45, 50, 55, 59, 64 };      // B1 E2 A2 D3 G3 B3 E4
    const std::vector<int> kTuningGuitar8{ 30, 35, 40, 45, 50, 55, 59, 64 };  // F#1 B1 E2 A2 D3 G3 B3 E4

    // Energy threshold default and the hammer-on / pull-off relaxation,
    // both from `_ndScoreChord`. Pulled out as constants so the
    // technique-adjustment block reads the same as the JS.
    constexpr float kEnergyThresholdDefault = 0.03f;
    constexpr float kEnergyThresholdSoftAttack = 0.015f;
    // Bend / slide pitch window — pitch is in motion, so the JS widens
    // the cents tolerance to at least 100. We mirror that floor exactly.
    constexpr float kBendSlideCentsFloor = 100.0f;
    // Target FFT bin width in Hz. Picks an fftSize that keeps the
    // low-B fundamental (5-string bass) resolvable across any device
    // sample rate. JS uses the same constant for the same reason.
    constexpr double kTargetBinHz = 3.0;

    int nextPow2(int n) noexcept
    {
        int p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    // Same parabolic-peak refinement the JS uses. Clamps to ±1 so a
    // near-zero denominator can't push the corrected peak into a
    // neighbour bin.
    float parabolicOffset(float yPrev, float yPeak, float yNext) noexcept
    {
        const float denom = yPrev - 2.0f * yPeak + yNext;
        if (std::abs(denom) < 1e-12f) return 0.0f;
        const float delta = 0.5f * (yPrev - yNext) / denom;
        if (delta > 1.0f) return 1.0f;
        if (delta < -1.0f) return -1.0f;
        return delta;
    }

    // Octave-fold cents deviation into (-600, +600]. Used by the per-
    // string pitch check so a detected octave-mismatched fundamental
    // (very common on guitar — strong 2nd harmonic in DI tones) still
    // counts as the right note. Mirrors `_ndFoldOctaveCents`.
    //
    // Range note: `std::round` ties away from zero, so an input of
    // exactly +600 folds to -600 while an input of -600 stays at -600 +
    // 1200 = +600. The asymmetry doesn't affect the hit/miss decision
    // because the caller compares `std::abs(centsError) <= tolerance`,
    // which collapses both endpoints to magnitude 600.
    float foldOctaveCents(float cents) noexcept
    {
        if (! std::isfinite(cents)) return std::numeric_limits<float>::infinity();
        return cents - (std::round(cents / 1200.0f) * 1200.0f);
    }

    int midiFromStringFret(int stringIdx, int fret, const std::vector<int>& base,
                           const std::vector<int>& offsets, int capo) noexcept
    {
        const int off = (stringIdx >= 0 && stringIdx < (int) offsets.size()) ? offsets[(size_t) stringIdx] : 0;
        const int b = (stringIdx >= 0 && stringIdx < (int) base.size()) ? base[(size_t) stringIdx] : 0;
        return b + off + capo + fret;
    }

    // Frequency band [loHz, hiHz] covering frets 0..24 for the given
    // string at the supplied tuning, with ±10% headroom so non-standard
    // tunings / capo / offsets still land inside the band. Same shape
    // as `_ndStringBandHz`.
    std::pair<double, double> stringBandHz(int stringIdx, const std::vector<int>& base,
                                           const std::vector<int>& offsets, int capo) noexcept
    {
        const int openMidi = midiFromStringFret(stringIdx, 0, base, offsets, capo);
        const int fret24Midi = openMidi + 24;
        const double loHz = 440.0 * std::pow(2.0, (openMidi - 69) / 12.0) * 0.90;
        const double hiHz = 440.0 * std::pow(2.0, (fret24Midi - 69) / 12.0) * 1.10;
        return { loHz, hiHz };
    }
}

const std::vector<int>* ChordScorer::standardMidiFor(const std::string& arrangement, int stringCount)
{
    if (arrangement == "bass")
    {
        if (stringCount == 4) return &kTuningBass4;
        if (stringCount == 5) return &kTuningBass5;
        return nullptr;
    }
    if (arrangement == "guitar")
    {
        if (stringCount == 6) return &kTuningGuitar6;
        if (stringCount == 7) return &kTuningGuitar7;
        if (stringCount == 8) return &kTuningGuitar8;
        return nullptr;
    }
    return nullptr;
}

void ChordScorer::ensureFft(int fftSize)
{
    if (fftSize == currentFftSize) return;
    int order = 0;
    while ((1 << order) < fftSize) ++order;
    fft = std::make_unique<juce::dsp::FFT>(order);
    currentFftSize = fftSize;
    currentFftOrder = order;
    fftScratch.assign((size_t) fftSize, juce::dsp::Complex<float>{0.0f, 0.0f});
    fftOutScratch.assign((size_t) fftSize, juce::dsp::Complex<float>{0.0f, 0.0f});
    magnitudes.assign((size_t) ((fftSize >> 1) + 1), 0.0f);
}

void ChordScorer::computeMagnitudes(const float* buffer, int numSamples)
{
    // Zero the scratch — the FFT reads all fftSize complex slots; the
    // windowed input fills only the first `numSamples` of them.
    std::fill(fftScratch.begin(), fftScratch.end(),
              juce::dsp::Complex<float>{0.0f, 0.0f});

    // Hann-window the real part, leave imag at zero. Identical to the
    // JS implementation, including the `numSamples - 1` divisor (NOT
    // `numSamples`) which is the closed-form Hann.
    const float invDen = (numSamples > 1)
                       ? static_cast<float>(2.0 * juce::MathConstants<double>::pi / (numSamples - 1))
                       : 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float w = 0.5f * (1.0f - std::cos(invDen * (float) i));
        fftScratch[(size_t) i] = juce::dsp::Complex<float>{ buffer[i] * w, 0.0f };
    }

    // Forward FFT — out-of-place. JUCE's FFT::perform contract is that
    // input and output must be distinct buffers ("Performs an out-of-place
    // FFT" — juce_FFT.h). The Ooura fallback engine that Linux + Windows
    // desktop builds default to recurses into a radix decomposition that
    // reads input positions and writes output positions in overlapping
    // iteration patterns; aliasing the two buffers produces cascading
    // numerical corruption (observed: ~1e27-magnitude bins from sub-1.0
    // input samples). The corrupted magnitudes propagate into the per-
    // string band energy as Inf/NaN downstream, and every chord scores
    // as all-miss. macOS desktop builds use vDSP (Apple Accelerate),
    // which tolerates input==output aliasing in practice — which is
    // why this bug only surfaces on the Linux/Windows desktop bridge.
    fft->perform(fftScratch.data(), fftOutScratch.data(), false);

    const int halfBins = (currentFftSize >> 1) + 1;
    for (int k = 0; k < halfBins; ++k)
    {
        const auto& c = fftOutScratch[(size_t) k];
        magnitudes[(size_t) k] = std::sqrt(c.real() * c.real() + c.imag() * c.imag());
    }
}

ChordScorer::Result ChordScorer::scoreChord(const float* buffer, int numSamples,
                                            double sampleRate, const Request& req)
{
    Result out{};
    out.totalStrings = (int) req.notes.size();

    // Build the all-miss shape every validation-failure path returns.
    // The caller's contract is one result entry per requested note
    // (matches AudioEngine::scoreChord's audio-not-running fast path)
    // — without this, an out-of-range or mismatched request would
    // yield totalStrings > 0 with results.length == 0 and break
    // renderers that iterate results[] one-to-one with the chord-note
    // list.
    auto fillMissResults = [&out, &req]() {
        out.results.clear();
        out.results.reserve(req.notes.size());
        for (const auto& n : req.notes)
        {
            NoteResult r{};
            r.string = n.string;
            r.fret = n.fret;
            out.results.push_back(r);
        }
    };

    // Bail with the per-note all-miss shape when the audio inputs are
    // unusable (zero/negative samples, zero sample rate, or a null
    // buffer the caller forgot to populate). Setting totalStrings = 0
    // here would diverge from the other failure paths; instead emit
    // the same shape every other early-exit produces.
    if (numSamples <= 0 || sampleRate <= 0.0 || buffer == nullptr)
    {
        fillMissResults();
        return out;
    }
    if (out.totalStrings == 0) return out;

    // Validate request shape. Unsupported (arrangement, stringCount)
    // pairs and undersized/mismatched tuningOffsets used to silently
    // fall back to bass-4 / guitar-6 with zero offsets, producing
    // plausible-looking but wrong scores. Fail closed instead — emit
    // an all-miss result set so the renderer sees score=0 / isHit=false
    // with the expected per-note entries.
    const auto* basePtr = standardMidiFor(req.arrangement, req.stringCount);
    if (basePtr == nullptr) { fillMissResults(); return out; }
    const auto& base = *basePtr;
    if ((int) req.tuningOffsets.size() != req.stringCount)
    {
        fillMissResults();
        return out;
    }
    for (const auto& n : req.notes)
    {
        if (n.string < 0 || n.string >= req.stringCount)
        {
            fillMissResults();
            return out;
        }
    }

    // Size the FFT exactly the way JS does: at least nextPow2(numSamples),
    // but never finer than the bin-width floor derived from sampleRate so
    // the low-B fundamental on 5-string bass remains resolvable across
    // device rates. Clamp the final size to kMaxFftSize so a caller-
    // controlled `numSamples` (or a pathological sampleRate) can't force
    // an oversized FFT-plan/scratch allocation across the IPC boundary.
    const int clampedSamples = std::min(numSamples, kMaxFftSize);
    const int resolutionFloor = std::min(
        nextPow2((int) std::ceil(sampleRate / kTargetBinHz)),
        kMaxFftSize);
    const int fftSize = std::max(nextPow2(clampedSamples), resolutionFloor);
    ensureFft(fftSize);
    computeMagnitudes(buffer, clampedSamples);

    const double binHz = sampleRate / fftSize;
    lastBinHz = binHz;

    // Total spectrum energy — one full pass, shared across every per-
    // string `bandEnergy` call below. Same optimisation `_ndScoreChord`
    // does in JS.
    double totalEnergy = 0.0;
    for (float m : magnitudes)
        totalEnergy += (double) m * m;

    out.results.reserve(req.notes.size());
    int hits = 0;
    for (const auto& note : req.notes)
    {
        // Per-technique threshold adjustments, mirroring screen.js.
        float energyThreshold = kEnergyThresholdDefault;
        float cents = req.pitchCheckCents;
        if (note.hammerOn || note.pullOff)
            energyThreshold = kEnergyThresholdSoftAttack;
        if (note.bend || note.slide)
            cents = std::max(cents, kBendSlideCentsFloor);
        if (note.harmonic)
            cents = 0.0f; // energy-only

        const auto [loHz, hiHz] = stringBandHz(note.string, base, req.tuningOffsets, req.capo);

        // Band energy fraction. Identical bin-clamp + summation as JS
        // `_ndBandEnergy`. hiBin < loBin only happens for a fully-out-of-range
        // band (e.g. clamped below zero), so we shortcut to 0.
        const int nBins = (int) magnitudes.size();
        const int loBin = std::max(0, (int) std::floor(loHz / binHz));
        const int hiBin = std::min(nBins - 1, (int) std::ceil(hiHz / binHz));
        double bandEnergy = 0.0;
        if (hiBin >= loBin)
        {
            for (int k = loBin; k <= hiBin; ++k)
                bandEnergy += (double) magnitudes[(size_t) k] * magnitudes[(size_t) k];
        }
        const float bandEnergyFraction = (totalEnergy < 1e-12)
            ? 0.0f
            : (float) (bandEnergy / totalEnergy);

        NoteResult nr{};
        nr.string = note.string;
        nr.fret = note.fret;
        nr.bandEnergy = bandEnergyFraction;

        if (bandEnergyFraction < energyThreshold)
        {
            nr.hit = false;
            nr.hasCents = false;
        }
        else if (cents <= 0.0f)
        {
            // Energy-only path (harmonic flag, or caller asked for it).
            nr.hit = true;
            nr.hasCents = false;
        }
        else
        {
            // Peak-pick within the band, parabolic refine, fold to nearest
            // octave for the cents comparison. Same shape as JS
            // `_ndConstraintCheckString`'s pitch branch.
            int peakBin = loBin;
            float peakVal = -std::numeric_limits<float>::infinity();
            for (int k = loBin; k <= hiBin; ++k)
            {
                if (magnitudes[(size_t) k] > peakVal)
                {
                    peakVal = magnitudes[(size_t) k];
                    peakBin = k;
                }
            }
            const float delta = (peakBin > loBin && peakBin < hiBin)
                ? parabolicOffset(magnitudes[(size_t) (peakBin - 1)],
                                  magnitudes[(size_t) peakBin],
                                  magnitudes[(size_t) (peakBin + 1)])
                : 0.0f;
            const double detectedHz = (peakBin + delta) * binHz;

            const int expectedMidi = midiFromStringFret(note.string, note.fret, base, req.tuningOffsets, req.capo);
            const double expectedHz = 440.0 * std::pow(2.0, (expectedMidi - 69) / 12.0);
            const float rawCentsError = (float) (1200.0 * std::log2(detectedHz / expectedHz));
            const float centsError = foldOctaveCents(rawCentsError);
            const float centsDiff = std::abs(centsError);

            nr.hit = centsDiff <= cents;
            nr.hasCents = true;
            nr.centsDiff = centsDiff;
            nr.centsError = centsError;
        }

        if (nr.hit) ++hits;
        out.results.push_back(nr);
    }

    out.hitStrings = hits;
    out.score = out.totalStrings > 0 ? (float) hits / (float) out.totalStrings : 0.0f;
    out.isHit = out.score >= req.minHitRatio;
    return out;
}
