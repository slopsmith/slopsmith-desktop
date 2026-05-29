#pragma once

#include <cmath>

// Containment for a divergent audio block (issue #403).
//
// The live signal chain (NAM / IR / VST) can emit non-finite samples (NaN/Inf)
// or a runaway level — most likely when the tone chain is rebuilt live on a
// song load. With nothing scrubbing it, that garbage reaches the speakers
// ("buzz, then extremely loud") and poisons persistent downstream state (the
// tonePolish IIR, the output ring), so the engine stays dead until the app is
// restarted. Running this over the guitar bus right after the chain — before
// any IIR/gain/mix — turns a catastrophic, permanent failure into a momentary
// glitch: feed-forward processors (WaveNet NAM, FIR IR) self-heal once they
// stop emitting garbage.
//
// JUCE-free and header-only on purpose so it unit-tests without the audio
// framework. Operates in place on one channel of `n` interleaved-or-planar
// float samples. Returns the number of samples it had to fix (for one-shot
// observability — the caller may count blocks, never log on the RT thread).
//
// `ceiling` is a hard magnitude clamp: real playing sits well under 1.0, so a
// generous ceiling (~+6 dBFS) only ever catches runaway garbage, never musical
// transients.

namespace slopsmith {

inline int sanitizeAudioBlock(float* data, int n, float ceiling = 2.0f) noexcept
{
    int fixed = 0;
    for (int i = 0; i < n; ++i)
    {
        const float s = data[i];
        if (!std::isfinite(s))
        {
            data[i] = 0.0f;
            ++fixed;
        }
        else if (s > ceiling)
        {
            data[i] = ceiling;
            ++fixed;
        }
        else if (s < -ceiling)
        {
            data[i] = -ceiling;
            ++fixed;
        }
    }
    return fixed;
}

} // namespace slopsmith
