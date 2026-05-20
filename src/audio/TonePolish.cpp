#include "TonePolish.h"

#include <cmath>

void TonePolish::prepare(double sr)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;

    // juce::dsp::IIR::Filter is non-copy-assignable (its HeapBlock member is
    // non-copyable), so vector::assign(count, value) won't compile. Clear and
    // resize default-constructs each slot in place instead.
    filters.clear();
    filters.resize((size_t) kMaxChannels);

    updateCoefficients();
    reset();

    // Signal that DSP state is fully initialised. Release-store so the audio
    // thread's acquire-load in processBlock() sees all preceding writes.
    paramPrepared.store(true, std::memory_order_release);
}

void TonePolish::reset()
{
    for (auto& trio : filters)
        for (auto& f : trio)
            f.reset();
}

void TonePolish::setEnabled(bool enabled)
{
    // When re-enabling, mark that filter state needs to be cleared before the
    // next block so stale IIR delay lines don't produce a click at the bypass
    // boundary.
    //
    // Ordering: paramNeedsReset is set *before* paramEnabled using seq_cst
    // stores, and both reads in processBlock() use seq_cst loads. This gives
    // a total store order visible to the audio thread, preventing the race
    // where paramEnabled is seen as true while paramNeedsReset is still false
    // (which would skip the reset and allow a click on re-enable).
    if (enabled)
        paramNeedsReset.store(true, std::memory_order_seq_cst);
    paramEnabled.store(enabled, std::memory_order_seq_cst);
}

void TonePolish::updateCoefficients()
{
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    const auto sr = sampleRate;

    // Reference-counted Coefficients objects shared across the per-channel
    // Filter instances — one allocation per filter type, not per channel.
    const auto hp = Coeffs::makeHighPass(sr, TonePolishSpecs::kHighPassHz);

    // makeLowShelf signature: (sampleRate, cutoffFrequency, Q, gain).
    // gain is a linear amplitude factor: 10^(dB/20).
    const auto shelfGain = std::pow(10.0f, TonePolishSpecs::kLowShelfDb / 20.0f);
    const auto ls = Coeffs::makeLowShelf(sr, TonePolishSpecs::kLowShelfHz,
                                          1.0f / juce::MathConstants<float>::sqrt2, // S = 1 → Q ≈ 0.707
                                          shelfGain);

    const auto peakGain = std::pow(10.0f, TonePolishSpecs::kPeakDb / 20.0f);
    const auto pk = Coeffs::makePeakFilter(sr, TonePolishSpecs::kPeakHz,
                                            TonePolishSpecs::kPeakQ, peakGain);

    for (auto& trio : filters)
    {
        trio[0].coefficients = hp;
        trio[1].coefficients = ls;
        trio[2].coefficients = pk;
    }
}

void TonePolish::processBlock(juce::AudioBuffer<float>& buffer)
{
    // Guard against calls before prepare() has populated filter coefficients.
    if (! paramPrepared.load(std::memory_order_acquire))
        return;

    // seq_cst load pairs with the seq_cst store in setEnabled() so the
    // paramNeedsReset flag written before paramEnabled is always visible
    // before we observe paramEnabled == true.
    if (! paramEnabled.load(std::memory_order_seq_cst))
        return;

    // Clear stale IIR delay-line state on re-enable so the first active block
    // starts clean and does not produce a click at the bypass boundary.
    if (paramNeedsReset.exchange(false, std::memory_order_seq_cst))
        reset();

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    const int available = (int) filters.size();
    const int chans = juce::jmin(numChannels, available);
    if (chans <= 0)
        return;

    float* const* channelData = buffer.getArrayOfWritePointers();

    for (int ch = 0; ch < chans; ++ch)
    {
        auto* data = channelData[ch];
        auto& trio = filters[(size_t) ch];

        for (auto& f : trio)
        {
            juce::dsp::AudioBlock<float> block(&data, 1, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing<float> ctx(block);
            f.process(ctx);
        }
    }
}
