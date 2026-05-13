// Non-Windows fallback: sandboxing is a no-op. The caller falls back to the
// existing in-process loader (today's behaviour).
//
// macOS and Linux sandbox implementations are tracked as follow-up PRs; see
// SANDBOX-DESIGN.md §9.

#include "SandboxedProcessor.h"

namespace slopsmith::sandbox {

std::unique_ptr<juce::AudioProcessor> tryLoadSandboxed(
    const juce::PluginDescription& /*desc*/,
    double /*sampleRate*/, int /*blockSize*/,
    juce::String& /*errorOut*/)
{
    return nullptr;
}

bool shouldSandbox(const juce::PluginDescription& /*desc*/)
{
    return false;
}

} // namespace slopsmith::sandbox
