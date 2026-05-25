#include "BackingSlowdownStretch.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace
{
constexpr double kTempoRampSeconds = 0.12;

SlowdownPresetParams paramsFor(SlowdownQualityPreset preset)
{
    SlowdownPresetParams p;
    switch (preset)
    {
        case SlowdownQualityPreset::LowLatency:
            // Shorter windows — lower CPU/latency; fine near normal speed.
            p.sequenceMs = 40;
            p.seekWindowMs = 15;
            p.overlapMs = 8;
            p.quickSeek = 1;
            p.useAaFilter = 1;
            p.aaFilterLength = 32;
            break;
        case SlowdownQualityPreset::Balanced:
            p.sequenceMs = 82;
            p.seekWindowMs = 22;
            p.overlapMs = 10;
            p.quickSeek = 0;
            p.useAaFilter = 1;
            p.aaFilterLength = 48;
            break;
        case SlowdownQualityPreset::HighQuality:
            // Longer sequence/seek — less echo when stretching below ~0.85×.
            p.sequenceMs = 100;
            p.seekWindowMs = 28;
            p.overlapMs = 12;
            p.quickSeek = 0;
            p.useAaFilter = 1;
            p.aaFilterLength = 64;
            break;
        case SlowdownQualityPreset::ExtremeSlowPractice:
            // Maximum quality for very slow practice (< 35% speed).
            p.sequenceMs = 120;
            p.seekWindowMs = 35;
            p.overlapMs = 14;
            p.quickSeek = 0;
            p.useAaFilter = 1;
            p.aaFilterLength = 96;
            break;
    }
    return p;
}

} // namespace

SlowdownPresetParams slowdownPresetParams(SlowdownQualityPreset preset)
{
    return paramsFor(preset);
}

SlowdownQualityPreset selectPresetForTempo(double tempo)
{
    if (!std::isfinite(tempo) || tempo <= 0.0)
        return SlowdownQualityPreset::Balanced;

    if (tempo >= 0.85)
        return SlowdownQualityPreset::LowLatency;
    if (tempo >= 0.60)
        return SlowdownQualityPreset::Balanced;
    if (tempo >= 0.35)
        return SlowdownQualityPreset::HighQuality;
    return SlowdownQualityPreset::ExtremeSlowPractice;
}

#if defined(SLOPSMITH_SOUNDTOUCH_SUPPORT) && SLOPSMITH_SOUNDTOUCH_SUPPORT

void applySlowdownPreset(soundtouch::SoundTouch& st, SlowdownQualityPreset preset)
{
    const SlowdownPresetParams p = paramsFor(preset);
    st.setSetting(SETTING_USE_QUICKSEEK, p.quickSeek);
    st.setSetting(SETTING_USE_AA_FILTER, p.useAaFilter);
    st.setSetting(SETTING_AA_FILTER_LENGTH, p.aaFilterLength);
    st.setSetting(SETTING_SEQUENCE_MS, p.sequenceMs);
    st.setSetting(SETTING_SEEKWINDOW_MS, p.seekWindowMs);
    st.setSetting(SETTING_OVERLAP_MS, p.overlapMs);
}

#endif

double smoothTempoChange(double& smoothedTempo, double targetTempo, int numSamples, double sampleRate)
{
    if (!std::isfinite(targetTempo) || targetTempo <= 0.0)
        return smoothedTempo;

    if (numSamples <= 0 || sampleRate <= 0.0)
        return smoothedTempo;

    if (std::abs(smoothedTempo - targetTempo) < 1.0e-5)
    {
        smoothedTempo = targetTempo;
        return smoothedTempo;
    }

    const double tauSamples = kTempoRampSeconds * sampleRate;
    const double alpha = 1.0 - std::exp(-(double) numSamples / tauSamples);
    smoothedTempo += alpha * (targetTempo - smoothedTempo);
    return smoothedTempo;
}

bool runBackingSlowdownStretchSelfChecks()
{
    if (selectPresetForTempo(1.0) != SlowdownQualityPreset::LowLatency)
        return false;
    if (selectPresetForTempo(0.90) != SlowdownQualityPreset::LowLatency)
        return false;
    if (selectPresetForTempo(0.85) != SlowdownQualityPreset::LowLatency)
        return false;
    if (selectPresetForTempo(0.84) != SlowdownQualityPreset::Balanced)
        return false;
    if (selectPresetForTempo(0.60) != SlowdownQualityPreset::Balanced)
        return false;
    if (selectPresetForTempo(0.59) != SlowdownQualityPreset::HighQuality)
        return false;
    if (selectPresetForTempo(0.35) != SlowdownQualityPreset::HighQuality)
        return false;
    if (selectPresetForTempo(0.34) != SlowdownQualityPreset::ExtremeSlowPractice)
        return false;

    const auto extreme = slowdownPresetParams(SlowdownQualityPreset::ExtremeSlowPractice);
    if (extreme.sequenceMs < slowdownPresetParams(SlowdownQualityPreset::HighQuality).sequenceMs)
        return false;

    double smoothed = 1.0;
    for (int i = 0; i < 200; ++i)
        smoothTempoChange(smoothed, 0.5, 256, 48000.0);
    if (std::abs(smoothed - 0.5) > 0.02)
        return false;

    smoothed = 0.5;
    for (int i = 0; i < 200; ++i)
        smoothTempoChange(smoothed, 1.0, 256, 48000.0);
    if (std::abs(smoothed - 1.0) > 0.02)
        return false;

    return true;
}
