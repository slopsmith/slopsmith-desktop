#pragma once

#include <cmath>

/// SoundTouch preserve-pitch slowdown helpers (backing track only).
/// Compiled when SLOPSMITH_SOUNDTOUCH_SUPPORT is enabled.

enum class SlowdownQualityPreset
{
    LowLatency,
    Balanced,
    HighQuality,
    ExtremeSlowPractice,
};

struct SlowdownPresetParams
{
    int sequenceMs = 82;
    int seekWindowMs = 22;
    int overlapMs = 10;
    int quickSeek = 0;
    int useAaFilter = 1;
    int aaFilterLength = 64;
};

/// Fixed SoundTouch parameters for each quality tier.
SlowdownPresetParams slowdownPresetParams(SlowdownQualityPreset preset);

/// Pick a preset from playback tempo (SoundTouch units: 1.0 = normal, 0.5 = half speed).
/// Lower tempos use longer windows — see SoundTouch docs on SEQUENCE_MS / SEEKWINDOW_MS.
SlowdownQualityPreset selectPresetForTempo(double tempo);

#if defined(SLOPSMITH_SOUNDTOUCH_SUPPORT) && SLOPSMITH_SOUNDTOUCH_SUPPORT

#include <SoundTouch.h>

void applySlowdownPreset(soundtouch::SoundTouch& st, SlowdownQualityPreset preset);

#endif

/// Exponential ramp toward target tempo over ~120 ms to avoid clicks when the user moves the speed slider.
double smoothTempoChange(double& smoothedTempo, double targetTempo, int numSamples, double sampleRate);

/// Returns false if any invariant check fails (for manual / CI validation).
bool runBackingSlowdownStretchSelfChecks();
