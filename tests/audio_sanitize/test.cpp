// Unit test for slopsmith::sanitizeAudioBlock (issue #403 containment).
// JUCE-free and platform-independent — exit 0 = pass.
#include "../../src/audio/AudioSanitize.h"

#include <cassert>
#include <cstdio>
#include <limits>

using slopsmith::sanitizeAudioBlock;

static void test_clean_signal_untouched()
{
    float buf[] = { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.9f, -1.9f };
    const int n = (int) (sizeof(buf) / sizeof(buf[0]));
    const int fixed = sanitizeAudioBlock(buf, n);
    assert(fixed == 0);
    assert(buf[1] == 0.5f && buf[3] == 1.0f && buf[5] == 1.9f);
}

static void test_nan_and_inf_become_zero()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    float buf[] = { nan, inf, -inf, 0.25f };
    const int fixed = sanitizeAudioBlock(buf, 4);
    assert(fixed == 3);
    assert(buf[0] == 0.0f && buf[1] == 0.0f && buf[2] == 0.0f);
    assert(buf[3] == 0.25f); // finite, in range — untouched
}

static void test_runaway_clamped_to_ceiling()
{
    float buf[] = { 1e30f, -1e30f, 5.0f, -5.0f };
    const int fixed = sanitizeAudioBlock(buf, 4, 2.0f);
    assert(fixed == 4);
    assert(buf[0] == 2.0f && buf[1] == -2.0f);
    assert(buf[2] == 2.0f && buf[3] == -2.0f);
}

static void test_ceiling_boundary_inclusive()
{
    // Exactly at the ceiling is in range (not > ceiling) — must be untouched.
    float buf[] = { 2.0f, -2.0f };
    const int fixed = sanitizeAudioBlock(buf, 2, 2.0f);
    assert(fixed == 0);
    assert(buf[0] == 2.0f && buf[1] == -2.0f);
}

static void test_empty_block_safe()
{
    float* p = nullptr;
    const int fixed = sanitizeAudioBlock(p, 0);
    assert(fixed == 0);
}

int main()
{
    test_clean_signal_untouched();
    test_nan_and_inf_become_zero();
    test_runaway_clamped_to_ceiling();
    test_ceiling_boundary_inclusive();
    test_empty_block_safe();
    std::printf("audio_sanitize: all tests passed\n");
    return 0;
}
