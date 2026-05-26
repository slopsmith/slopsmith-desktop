#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

// Hard ceiling on backing playback speed. This drives input buffer sizing and runtime clamp.
static constexpr double kMaxBackingSpeed = 4.0;
// Transparent full-speed path — skip the stretcher when rate is effectively 1×.
static constexpr double kBackingSpeedBypassEpsilon = 1.0e-4;

// On Windows, ASIO drivers can crash with access violations.
// We catch C++ exceptions but can't easily catch SEH in functions with dtors.
// The try/catch blocks around device operations are the best we can do
// without restructuring the code into SEH-safe wrapper functions.

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();

    auto result = inputDeviceManager.initialiseWithDefaultDevices(2, 2);
    if (result.isNotEmpty())
        std::cerr << "[AudioEngine] input init note: " << result.toStdString() << std::endl;

    auto outResult = outputDeviceManager.initialiseWithDefaultDevices(0, 2);
    if (outResult.isNotEmpty())
        std::cerr << "[AudioEngine] output init note: " << outResult.toStdString() << std::endl;

    // Some backends (WASAPI) bind to a default device on init and would
    // hold it exclusive against the duplex codepath. Idle until split mode.
    outputDeviceManager.closeAudioDevice();

    auto& availableTypes = inputDeviceManager.getAvailableDeviceTypes();
    std::cerr << "[AudioEngine] Available device types: " << availableTypes.size() << std::endl;
    for (auto* type : availableTypes)
    {
        type->scanForDevices();
        std::cerr << "[AudioEngine]   " << type->getTypeName().toStdString()
                  << " - inputs: " << type->getDeviceNames(true).size()
                  << ", outputs: " << type->getDeviceNames(false).size() << std::endl;
    }
    for (auto* type : outputDeviceManager.getAvailableDeviceTypes())
        type->scanForDevices();
}

AudioEngine::~AudioEngine()
{
    stopAudio();
    stopBacking();
}

// ── Device Enumeration ────────────────────────────────────────────────────────

juce::Array<AudioEngine::DeviceTypeInfo> AudioEngine::getDeviceTypes()
{
    juce::Array<DeviceTypeInfo> types;

    for (auto* type : inputDeviceManager.getAvailableDeviceTypes())
    {
        DeviceTypeInfo info;
        info.name = type->getTypeName();

        // Use already-scanned device names (scanForDevices was called during init)
        info.inputDevices = type->getDeviceNames(true);
        info.outputDevices = type->getDeviceNames(false);

        types.add(std::move(info));
    }

    return types;
}

juce::Array<double> AudioEngine::getSampleRates()
{
    juce::Array<double> rates;
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        for (auto rate : device->getAvailableSampleRates())
            rates.add(rate);
    }
    return rates;
}

juce::Array<int> AudioEngine::getBufferSizes()
{
    juce::Array<int> sizes;
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        for (auto size : device->getAvailableBufferSizes())
            sizes.add(size);
    }
    return sizes;
}

AudioEngine::DeviceOptions AudioEngine::probeDeviceOptions(const juce::String& typeName,
                                                           const juce::String& inputName,
                                                           const juce::String& outputName)
{
    return probeDeviceOptionsDual(typeName, inputName, typeName, outputName);
}

AudioEngine::DeviceOptions AudioEngine::probeDeviceOptionsDual(const juce::String& inputTypeName,
                                                               const juce::String& inputName,
                                                               const juce::String& outputTypeName,
                                                               const juce::String& outputName)
{
    DeviceOptions options;
    options.inputType = inputTypeName;
    options.outputType = outputTypeName.isEmpty() ? inputTypeName : outputTypeName;
    options.type = options.inputType;   // legacy alias

    // Resolve each side from its own manager so probe stays consistent with
    // applySplitSetup()/setOutputDeviceType(), which mutate the manager that
    // owns the side they're configuring. Using inputDeviceManager for the
    // output lookup would silently fall back to whatever input has scanned,
    // which can miss output-only backends.
    auto findType = [](juce::AudioDeviceManager& manager,
                       const juce::String& wanted) -> juce::AudioIODeviceType* {
        juce::AudioIODeviceType* match = nullptr;
        for (auto* type : manager.getAvailableDeviceTypes())
        {
            if ((wanted.isNotEmpty() && type->getTypeName() == wanted)
                || (wanted.isEmpty() && match == nullptr))
            {
                match = type;
                if (wanted.isNotEmpty()) break;
            }
        }
        return match;
    };

    auto* inputType  = findType(inputDeviceManager, options.inputType);

    // Match setAudioDevices's resolution: when the caller didn't specify
    // an output type, default it to the SAME type the input side resolved
    // to (using the type's name, looked up in outputDeviceManager). Without
    // this, an empty `options.outputType` would let findType pick whatever
    // outputDeviceManager enumerates first — potentially a different
    // backend than inputDeviceManager picked from the empty string, which
    // then disagrees with the apply path's duplex classification.
    juce::String effectiveOutputTypeName = options.outputType;
    if (effectiveOutputTypeName.isEmpty() && inputType != nullptr)
        effectiveOutputTypeName = inputType->getTypeName();
    auto* outputType = findType(outputDeviceManager, effectiveOutputTypeName);

    if (inputType == nullptr)
    {
        options.error = "Input device type not found";
        options.compatible = false;
        return options;
    }
    if (outputType == nullptr)
    {
        options.error = "Output device type not found";
        options.compatible = false;
        return options;
    }

    try
    {
        options.inputType = inputType->getTypeName();
        options.outputType = outputType->getTypeName();
        options.type = options.inputType;

        options.input = inputName;
        options.output = outputName;

        // userIntendsDuplex matches setAudioDevices's classification:
        // identical names on both sides (typically both empty = OS default)
        // means we'll go duplex regardless of which specific devices the
        // first-enumerated lookup would have produced.
        const bool userIntendsDuplex = (options.inputType == options.outputType
                                        && options.input == options.output);

        // For probing we still need a concrete device to instantiate.
        // Resolve empty names to first-enumerated ONLY for the probe-device
        // creation below — DON'T write back into options.input/options.output;
        // those flow to the UI and the apply path, which treat empty as
        // "OS default" per side.
        auto inputs  = inputType->getDeviceNames(true);
        auto outputs = outputType->getDeviceNames(false);
        const juce::String probeInputName =
            options.input.isEmpty() && inputs.size() > 0 ? inputs[0] : options.input;
        const juce::String probeOutputName =
            options.output.isEmpty() && outputs.size() > 0 ? outputs[0] : options.output;

        const bool isDuplex = userIntendsDuplex
                              || (options.inputType == options.outputType
                                  && options.input == options.output
                                  && options.input.isNotEmpty());

        if (isDuplex)
        {
            std::unique_ptr<juce::AudioIODevice> dev(
                inputType->createDevice(probeOutputName, probeInputName));
            if (!dev) { options.error = "Could not create probe device"; options.compatible = false; return options; }

            options.inputChannels = dev->getInputChannelNames();
            options.outputChannels = dev->getOutputChannelNames();
            for (auto rate : dev->getAvailableSampleRates())
                options.sampleRates.addIfNotAlreadyThere(rate);
            for (auto size : dev->getAvailableBufferSizes())
                options.bufferSizes.addIfNotAlreadyThere(size);
        }
        else
        {
            std::unique_ptr<juce::AudioIODevice> inDev(
                inputType->createDevice({}, probeInputName));
            std::unique_ptr<juce::AudioIODevice> outDev(
                outputType->createDevice(probeOutputName, {}));
            if (!inDev || !outDev)
            {
                options.error = "Could not create dual probe devices";
                options.compatible = false;
                return options;
            }

            options.inputChannels = inDev->getInputChannelNames();
            options.outputChannels = outDev->getOutputChannelNames();

            // Tolerance covers backends that report fractional drift around the nominal rate.
            const auto inRates = inDev->getAvailableSampleRates();
            const auto outRates = outDev->getAvailableSampleRates();
            for (auto r : inRates)
            {
                for (auto r2 : outRates)
                {
                    // <= 0.5 (not <) to match applySplitSetup's rateSupportedBy
                    // check. A backend reporting 47999.5 on both sides has
                    // |r - r2| = 0 (matches anyway) but a backend mixing
                    // 47999.5 in / 48000.0 out has |diff| = 0.5 exactly, which
                    // < 0.5 would reject from the probe even though the
                    // apply-side check accepts it.
                    if (std::abs(r - r2) <= 0.5)
                    {
                        // Round the midpoint to a clean nominal rate
                        // (backends sometimes report fractional near-48000
                        // rates; surfacing the raw value would fail the
                        // apply-side setAudioDeviceSetup, which expects an
                        // exact supported nominal). Re-check the rounded
                        // candidate is within tolerance of BOTH sides — a
                        // matched pair like 48000.4/48000.6 passes the |r-r2|
                        // check but std::round(48000.4)=48000 would fall
                        // outside tolerance of 48000.6 (diff 0.6). Skip
                        // those so the probe stays fail-closed.
                        const double candidate = std::round((r + r2) * 0.5);
                        if (std::abs(r  - candidate) <= 0.5
                         && std::abs(r2 - candidate) <= 0.5)
                        {
                            options.sampleRates.addIfNotAlreadyThere(candidate);
                        }
                        break;
                    }
                }
            }
            if (options.sampleRates.isEmpty())
            {
                options.error = "Input and output devices share no common sample rate";
                options.compatible = false;
            }

            // Split mode opens both sides with the same bufferSize, so the
            // UI should only see sizes the intersection of both devices
            // supports — a union would let the user pick a value that
            // predictably fails at apply time on one side.
            const auto inBufs = inDev->getAvailableBufferSizes();
            const auto outBufs = outDev->getAvailableBufferSizes();
            for (auto b : inBufs)
            {
                for (auto b2 : outBufs)
                {
                    if (b == b2)
                    {
                        options.bufferSizes.addIfNotAlreadyThere(b);
                        break;
                    }
                }
            }
            // An empty intersection means there's no buffer size both sides
            // accept; setting compatible=false stops the UI from re-enabling
            // Apply against a guaranteed-fail config.
            if (options.bufferSizes.isEmpty() && options.error.isEmpty())
            {
                options.error = "Input and output devices share no common buffer size";
                options.compatible = false;
            }
        }

        fprintf(stderr, "[AudioEngine] Probed device options: inType='%s' outType='%s' in='%s' out='%s' "
                "duplex=%d inputs=%d outputs=%d rates=%d buffers=%d compatible=%d\n",
                options.inputType.toRawUTF8(), options.outputType.toRawUTF8(),
                options.input.toRawUTF8(), options.output.toRawUTF8(),
                (int) isDuplex, options.inputChannels.size(), options.outputChannels.size(),
                options.sampleRates.size(), options.bufferSizes.size(), (int) options.compatible);
    }
    catch (const std::exception& e)
    {
        options.error = e.what();
        options.compatible = false;
    }
    catch (...)
    {
        options.error = "Probe failed";
        options.compatible = false;
    }

    return options;
}

juce::String AudioEngine::getCurrentDeviceType()
{
    return getCurrentInputDeviceType();
}

juce::String AudioEngine::getCurrentInputDeviceType()
{
    if (auto* type = inputDeviceManager.getCurrentDeviceTypeObject())
        return type->getTypeName();
    return {};
}

juce::String AudioEngine::getCurrentOutputDeviceType()
{
    if (duplexMode.load(std::memory_order_relaxed))
        return getCurrentInputDeviceType();
    if (auto* type = outputDeviceManager.getCurrentDeviceTypeObject())
        return type->getTypeName();
    return {};
}

juce::String AudioEngine::getCurrentInputDevice()
{
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        auto setup = inputDeviceManager.getAudioDeviceSetup();
        return setup.inputDeviceName;
    }
    return {};
}

juce::String AudioEngine::getCurrentOutputDevice()
{
    auto& mgr = duplexMode.load(std::memory_order_relaxed)
        ? inputDeviceManager : outputDeviceManager;
    if (mgr.getCurrentAudioDevice() == nullptr) return {};
    return mgr.getAudioDeviceSetup().outputDeviceName;
}

AudioEngine::DeviceMetrics AudioEngine::getDeviceMetrics() const
{
    DeviceMetrics m;
    m.duplex = duplexMode.load(std::memory_order_relaxed);
    m.inputOverflowCount = inputOverflowCount.load(std::memory_order_relaxed);
    m.outputUnderflowCount = outputUnderflowCount.load(std::memory_order_relaxed);
    // The output ring is only used in split mode. Report 0/0 in duplex so
    // consumers don't think there's a live ring buffer to monitor when
    // there isn't one. Capacity reads-as-zero in duplex matches the
    // "no ring activity" semantic — the ring is structurally inert.
    if (! m.duplex)
    {
        m.outputRingCapacityFrames = kOutputRingFrames;
        // Output device can stop while the input keeps writing, leaving
        // (w - r) larger than capacity. Clamp uint64 → int via the
        // capacity ceiling so the consumer-facing field never overflows
        // or goes negative.
        const uint64_t w = outputRingWriteIndex.load(std::memory_order_acquire);
        const uint64_t r = outputRingReadIndex.load(std::memory_order_acquire);
        const uint64_t fill = (w >= r) ? (w - r) : 0;
        m.outputRingFillFrames = (int) std::min(fill, (uint64_t) kOutputRingFrames);
    }
    return m;
}

double AudioEngine::getLatencyMs() const
{
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    if (sr <= 0.0) return 0.0;

    if (duplexMode.load(std::memory_order_relaxed))
    {
        if (auto* device = inputDeviceManager.getCurrentAudioDevice())
        {
            int latencySamples = device->getCurrentBufferSizeSamples()
                               + device->getInputLatencyInSamples()
                               + device->getOutputLatencyInSamples();
            return (latencySamples / sr) * 1000.0;
        }
        return 0.0;
    }

    int totalSamples = 0;
    if (auto* in = inputDeviceManager.getCurrentAudioDevice())
        totalSamples += in->getCurrentBufferSizeSamples() + in->getInputLatencyInSamples();
    if (auto* out = outputDeviceManager.getCurrentAudioDevice())
        totalSamples += out->getCurrentBufferSizeSamples() + out->getOutputLatencyInSamples();

    // Steady-state ring residency ≈ half capacity once both clocks stabilize.
    totalSamples += kOutputRingFrames / 2;

    return (totalSamples / sr) * 1000.0;
}

// ── Device Selection ──────────────────────────────────────────────────────────

bool AudioEngine::setDeviceType(const juce::String& typeName)
{
    if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
    {
        if (currentType->getTypeName() == typeName)
        {
            fprintf(stderr, "[AudioEngine] Input device type already selected: %s\n", typeName.toRawUTF8());
            return true;
        }
    }

    for (auto* type : inputDeviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == typeName)
        {
            try {
                fprintf(stderr, "[AudioEngine] Setting input device type: %s\n", typeName.toRawUTF8());
                inputDeviceManager.setCurrentAudioDeviceType(typeName, true);
                // Legacy single-type API contract: callers expect both
                // managers to track the same backend so a subsequent
                // duplex configure or probe sees a consistent dual state.
                // setOutputDeviceType() exists for callers that want to
                // diverge the two sides intentionally. Best-effort — if
                // the output side doesn't expose this backend the input
                // change still stands so duplex on the matched backend
                // keeps working.
                if (auto* currentOutputType = outputDeviceManager.getCurrentDeviceTypeObject())
                {
                    if (currentOutputType->getTypeName() != typeName)
                    {
                        for (auto* outType : outputDeviceManager.getAvailableDeviceTypes())
                        {
                            if (outType->getTypeName() == typeName)
                            {
                                try { outputDeviceManager.setCurrentAudioDeviceType(typeName, true); }
                                catch (...) {
                                    fprintf(stderr, "[AudioEngine] setDeviceType: output sync threw (continuing)\n");
                                }
                                break;
                            }
                        }
                    }
                }
                return true;
            } catch (const std::exception& e) {
                fprintf(stderr, "[AudioEngine] setDeviceType crashed: %s\n", e.what());
                return false;
            } catch (...) {
                fprintf(stderr, "[AudioEngine] setDeviceType crashed (unknown)\n");
                return false;
            }
        }
    }
    return false;
}

bool AudioEngine::setOutputDeviceType(const juce::String& typeName)
{
    if (duplexMode.load(std::memory_order_relaxed))
        return setDeviceType(typeName);

    if (auto* currentType = outputDeviceManager.getCurrentDeviceTypeObject())
    {
        if (currentType->getTypeName() == typeName)
            return true;
    }
    for (auto* type : outputDeviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == typeName)
        {
            try {
                fprintf(stderr, "[AudioEngine] Setting output device type: %s\n", typeName.toRawUTF8());
                outputDeviceManager.setCurrentAudioDeviceType(typeName, true);
                return true;
            } catch (...) {
                fprintf(stderr, "[AudioEngine] setOutputDeviceType crashed\n");
                return false;
            }
        }
    }
    return false;
}

bool AudioEngine::setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                                  double sampleRate, int bufferSize)
{
    DeviceConfig c;
    c.inputType  = getCurrentInputDeviceType();
    c.outputType = c.inputType;
    c.inputDevice = inputName;
    c.outputDevice = outputName;
    c.sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    c.bufferSize = bufferSize > 0 ? bufferSize : 256;
    return setAudioDevices(c).ok;
}

AudioEngine::DeviceConfigResult AudioEngine::setAudioDevices(const DeviceConfig& config)
{
    DeviceConfigResult res;

    fprintf(stderr, "[AudioEngine] setAudioDevices: inType='%s' inDev='%s' outType='%s' outDev='%s' sr=%.0f bs=%d\n",
            config.inputType.toRawUTF8(), config.inputDevice.toRawUTF8(),
            config.outputType.toRawUTF8(), config.outputDevice.toRawUTF8(),
            config.sampleRate, config.bufferSize);

    // Platform-preferred backend when unspecified. Linux prefers ALSA over
    // JACK (jackd may be installed but not running).
    juce::String resolvedInputType = config.inputType;
    if (resolvedInputType.isEmpty())
    {
        if (auto* t = inputDeviceManager.getCurrentDeviceTypeObject())
            resolvedInputType = t->getTypeName();
    }
    if (resolvedInputType.isEmpty())
    {
#if JUCE_LINUX
        const juce::StringArray preferredOrder { "ALSA", "JACK" };
#elif JUCE_MAC
        const juce::StringArray preferredOrder { "CoreAudio" };
#elif JUCE_WINDOWS
        const juce::StringArray preferredOrder { "Windows Audio", "ASIO" };
#else
        const juce::StringArray preferredOrder;
#endif
        const auto& available = inputDeviceManager.getAvailableDeviceTypes();
        for (const auto& want : preferredOrder)
        {
            for (auto* type : available)
            {
                if (type->getTypeName() == want)
                {
                    resolvedInputType = want;
                    break;
                }
            }
            if (resolvedInputType.isNotEmpty()) break;
        }
        if (resolvedInputType.isEmpty() && !available.isEmpty())
            resolvedInputType = available.getFirst()->getTypeName();
    }

    juce::String resolvedOutputType = config.outputType.isEmpty()
        ? resolvedInputType : config.outputType;

    // Stop audio BEFORE mutating device-type or device setup. JUCE can
    // tear down and re-scan devices inside setCurrentAudioDeviceType /
    // setAudioDeviceSetup, and doing that while the audio callback is
    // still attached risks crashes/deadlocks on some backends (ASIO is
    // the usual culprit). stopAudio() detaches both callbacks first;
    // we'll re-start at the end if we were running.
    //
    // Unconditional: audioDeviceStopped() can clear audioRunning during a
    // transient input stop while the output callback intentionally stays
    // registered for JUCE's auto-restart. A reconfigure that races that
    // window would otherwise skip the stopAudio() detach and leave the
    // stale output callback attached. stopAudio() is itself idempotent
    // (R9 fix — removeAudioCallback is a no-op when not registered), so
    // running it unconditionally is safe regardless of audioRunning.
    const bool wasRunning = audioRunning.load(std::memory_order_relaxed);
    stopAudio();

    // setCurrentAudioDeviceType can throw from inside JUCE backends (ASIO
    // is the usual culprit). Catch and propagate as a structured error so
    // the N-API caller doesn't see the exception cross the boundary.
    try
    {
        if (auto* current = inputDeviceManager.getCurrentDeviceTypeObject())
        {
            if (current->getTypeName() != resolvedInputType)
                inputDeviceManager.setCurrentAudioDeviceType(resolvedInputType, true);
        }
        else
        {
            inputDeviceManager.setCurrentAudioDeviceType(resolvedInputType, true);
        }
    }
    catch (...)
    {
        res.error = "setCurrentAudioDeviceType threw for input type '" + resolvedInputType + "'";
        return res;
    }

    // User-intent duplex: both sides came in identical (typically both
    // empty = "system default", or both naming the same explicit device).
    // Capture before we resolve names, otherwise the resolve loop below
    // fills empty-input with first-input-device and empty-output with
    // first-output-device — those usually differ (especially on macOS
    // where defaults are separate input/output devices), and the engine
    // would silently route into split mode with ~85ms of ring-buffer
    // latency for a config the user expected to be duplex. Legacy
    // pre-PR settings commonly use empty names; preserve their behavior.
    const bool userIntendsDuplex = (resolvedInputType == resolvedOutputType
                                    && config.inputDevice == config.outputDevice);

    // Don't resolve empty names to first-device-of-each-type. Pre-PR
    // behavior — and Copilot's fail-closed concern — treat empty names
    // as "OS default" per side. Filling them with inputs[0] / outputs[0]
    // is JUCE-enumeration-order dependent and can pick the wrong device
    // (e.g. an audio interface that isn't the system default). Both
    // applyDuplexSetup and applySplitSetup handle empty names by setting
    // useDefault*Channels=true, letting JUCE select the OS default.
    const juce::String& resolvedInput  = config.inputDevice;
    const juce::String& resolvedOutput = config.outputDevice;

    const bool isDuplex = userIntendsDuplex
                          || (resolvedInputType == resolvedOutputType
                              && resolvedInput == resolvedOutput
                              && resolvedInput.isNotEmpty());

    // Normalize before branching — applyDuplexSetup() only checks `> 0` and
    // would otherwise let Infinity (or NaN slipping past N-API) reach JUCE
    // on the legacy positional setDevice() path. Finite-and-positive is the
    // baseline both modes need.
    const double requestedSampleRate =
        (std::isfinite(config.sampleRate) && config.sampleRate > 0.0)
            ? config.sampleRate
            : 48000.0;
    const int requestedBufferSize = config.bufferSize > 0 ? config.bufferSize : 256;

    if (isDuplex)
    {
        teardownSplitMode();

        const juce::String err = applyDuplexSetup(resolvedInput, resolvedOutput,
                                                  requestedSampleRate, requestedBufferSize);
        if (err.isNotEmpty())
        {
            res.error = err;
            res.duplex = true;
            return res;
        }
        duplexMode.store(true, std::memory_order_relaxed);

        if (auto* dev = inputDeviceManager.getCurrentAudioDevice())
        {
            res.sampleRate = dev->getCurrentSampleRate();
            res.inputBlockSize = dev->getCurrentBufferSizeSamples();
            res.outputBlockSize = res.inputBlockSize;
        }
        res.ok = true;
        res.duplex = true;
    }
    else
    {
        DeviceConfig resolved = config;
        resolved.inputType = resolvedInputType;
        resolved.outputType = resolvedOutputType;
        resolved.inputDevice = resolvedInput;
        resolved.outputDevice = resolvedOutput;
        resolved.sampleRate = requestedSampleRate;
        resolved.bufferSize = requestedBufferSize;

        res = applySplitSetup(resolved);
        if (!res.ok)
            return res;
        duplexMode.store(false, std::memory_order_relaxed);
    }

    if (wasRunning) startAudio();
    return res;
}

juce::String AudioEngine::applyDuplexSetup(const juce::String& inputName,
                                           const juce::String& outputName,
                                           double sampleRate, int bufferSize)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputName;
    setup.outputDeviceName = outputName;
    setup.sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    setup.bufferSize = bufferSize > 0 ? bufferSize : 256;
    setup.useDefaultInputChannels = inputName.isEmpty();
    setup.useDefaultOutputChannels = outputName.isEmpty();

    // Channel masks must match too — high-numbered selectedInputChannel needs
    // the expanded mask that an older session may not have opened.
    if (auto* currentDevice = inputDeviceManager.getCurrentAudioDevice())
    {
        try
        {
            juce::AudioDeviceManager::AudioDeviceSetup current;
            inputDeviceManager.getAudioDeviceSetup(current);

            const int advertisedInputs = currentDevice->getInputChannelNames().size();
            juce::BigInteger expectedInputs;
            expectedInputs.setRange(0, advertisedInputs > 0 ? advertisedInputs : 2, true);

            const int advertisedOutputs = currentDevice->getOutputChannelNames().size();
            juce::BigInteger expectedOutputs;
            expectedOutputs.setRange(0, juce::jmin(advertisedOutputs > 0 ? advertisedOutputs : 2, 2), true);

            if (current.inputDeviceName == setup.inputDeviceName
                && current.outputDeviceName == setup.outputDeviceName
                && current.sampleRate == setup.sampleRate
                && current.bufferSize == setup.bufferSize
                && current.useDefaultInputChannels == setup.useDefaultInputChannels
                && current.useDefaultOutputChannels == setup.useDefaultOutputChannels
                && current.inputChannels == expectedInputs
                && current.outputChannels == expectedOutputs
                && duplexMode.load(std::memory_order_relaxed))
            {
                fprintf(stderr, "[AudioEngine] Duplex device already configured with same settings, skipping\n");
                return {};
            }
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[AudioEngine] Current device channel check failed: %s\n", e.what());
        }
        catch (...)
        {
            fprintf(stderr, "[AudioEngine] Current device channel check failed (unknown)\n");
        }
    }

    // ALSA deadlocks on reconfigure unless we fully close first. WASAPI
    // reconfigures in place and is much slower if closed.
#if JUCE_LINUX
    juce::String currentTypeName;
    if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
        currentTypeName = currentType->getTypeName();
    if (inputDeviceManager.getCurrentAudioDevice() != nullptr)
    {
        try {
            inputDeviceManager.closeAudioDevice();
            fprintf(stderr, "[AudioEngine] Closed device for reconfiguration\n");
            if (currentTypeName.isNotEmpty())
                inputDeviceManager.setCurrentAudioDeviceType(currentTypeName, true);
        } catch (...) {
            fprintf(stderr, "[AudioEngine] closeAudioDevice crashed, continuing\n");
        }
    }
#endif

    int inputChannelCount = 0;
    int outputChannelCount = 0;
    if (auto* type = inputDeviceManager.getCurrentDeviceTypeObject())
    {
        try
        {
            if (auto probe = std::unique_ptr<juce::AudioIODevice>(type->createDevice(outputName, inputName)))
            {
                inputChannelCount = probe->getInputChannelNames().size();
                outputChannelCount = probe->getOutputChannelNames().size();
            }
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[AudioEngine] Channel probe failed: %s\n", e.what());
        }
        catch (...)
        {
            fprintf(stderr, "[AudioEngine] Channel probe failed (unknown)\n");
        }
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    if (outputChannelCount <= 0) outputChannelCount = 2;

    setup.inputChannels.setRange(0, inputChannelCount, true);
    setup.outputChannels.setRange(0, juce::jmin(outputChannelCount, 2), true);

    juce::String result;
    try {
        result = inputDeviceManager.setAudioDeviceSetup(setup, true);
    } catch (...) {
        return "setAudioDeviceSetup threw";
    }
    if (result.isNotEmpty())
    {
        fprintf(stderr, "[AudioEngine] Device setup error: %s\n", result.toRawUTF8());
        try {
            result = inputDeviceManager.initialiseWithDefaultDevices(2, 2);
        } catch (...) {
            return "fallback initialiseWithDefaultDevices threw";
        }
        if (result.isNotEmpty())
            return "device setup failed: " + result;
    }

    if (auto* configuredDevice = inputDeviceManager.getCurrentAudioDevice())
    {
        const double sr = configuredDevice->getCurrentSampleRate();
        const int bs = configuredDevice->getCurrentBufferSizeSamples();
        currentSampleRate.store(sr, std::memory_order_relaxed);
        inputBlockSize.store(bs, std::memory_order_relaxed);
        outputBlockSize.store(bs, std::memory_order_relaxed);

        fprintf(stderr, "[AudioEngine] Duplex device configured OK. Current device: %s\n",
                configuredDevice->getName().toRawUTF8());
        fprintf(stderr, "[AudioEngine] Actual device setup: sr=%.0f bs=%d (requested bs=%d)\n",
                sr, bs, bufferSize);

        signalChain.prepare(sr, bs);
        noiseGate.prepare(sr, bs);
        tonePolish.prepare(sr);
        return {};
    }
    currentSampleRate.store(0.0, std::memory_order_relaxed);
    inputBlockSize.store(0, std::memory_order_relaxed);
    outputBlockSize.store(0, std::memory_order_relaxed);
    signalChain.releaseResources();
    return "no current device after setup";
}

AudioEngine::DeviceConfigResult AudioEngine::applySplitSetup(const DeviceConfig& config)
{
    DeviceConfigResult res;
    res.duplex = false;

    // The split-mode output ring is fixed at kOutputRingFrames samples
    // (~85ms @ 48kHz). A single callback at bufferSize > kOutputRingFrames
    // would overrun the ring in one go, guaranteeing immediate
    // overwrite/wrap and audible glitches. Reject those configurations up
    // front — duplex still works fine since it bypasses the ring entirely.
    if (config.bufferSize > kOutputRingFrames)
    {
        res.error = "Buffer size " + juce::String(config.bufferSize)
                  + " exceeds split-mode ring capacity ("
                  + juce::String(kOutputRingFrames) + "). Pick a smaller buffer size or use duplex.";
        return res;
    }

    // setCurrentAudioDeviceType can throw from JUCE backends (ASIO).
    // Catch so the failure surfaces as a structured error rather than an
    // exception crossing the N-API boundary.
    try
    {
        if (auto* current = outputDeviceManager.getCurrentDeviceTypeObject())
        {
            if (current->getTypeName() != config.outputType)
                outputDeviceManager.setCurrentAudioDeviceType(config.outputType, true);
        }
        else
        {
            outputDeviceManager.setCurrentAudioDeviceType(config.outputType, true);
        }
    }
    catch (...)
    {
        res.error = "setCurrentAudioDeviceType threw for output type '" + config.outputType + "'";
        return res;
    }

    // v1 forces matching nominal SR — no adaptive resampler yet.
    // Resolve empty name to first-enumerated for the createDevice probe
    // call (matches probeDeviceOptionsDual's strategy). createDevice("")
    // is implementation-defined per backend — some return the default,
    // some return null. Using first-enumerated keeps probe and apply
    // checking the SAME concrete device, so an empty-name config can't
    // pass the UI probe and then fail this check.
    auto rateSupportedBy = [&](juce::AudioIODeviceType* t,
                               const juce::String& dev, bool isInput, double sr) {
        if (!t) return false;
        juce::String resolved = dev;
        if (resolved.isEmpty())
        {
            auto names = t->getDeviceNames(isInput);
            if (names.size() > 0) resolved = names[0];
        }
        std::unique_ptr<juce::AudioIODevice> probe(
            isInput ? t->createDevice({}, resolved) : t->createDevice(resolved, {}));
        if (!probe) return false;
        // Tolerance matches the probe-side rounding: probeDeviceOptionsDual
        // rounds the matched rate to the nearest integer (see :208), so a
        // backend reporting e.g. 47999.5 surfaces 48000 in the UI. If we
        // kept `< 0.5` here, the round-trip would fail at apply time because
        // |47999.5 - 48000.0| is exactly 0.5. Use `<= 0.5` so the boundary
        // case the probe accepted is also accepted at apply.
        for (auto r : probe->getAvailableSampleRates())
            if (std::abs(r - sr) <= 0.5) return true;
        return false;
    };
    juce::AudioIODeviceType* inputType = nullptr;
    juce::AudioIODeviceType* outputType = nullptr;
    for (auto* t : inputDeviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == config.inputType) { inputType = t; break; }
    for (auto* t : outputDeviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == config.outputType) { outputType = t; break; }
    if (!inputType || !outputType)
    {
        res.error = "Device type not found";
        return res;
    }
    if (!rateSupportedBy(inputType, config.inputDevice, true, config.sampleRate)
     || !rateSupportedBy(outputType, config.outputDevice, false, config.sampleRate))
    {
        res.error = "Sample rate not supported by both input and output devices";
        return res;
    }

    juce::AudioDeviceManager::AudioDeviceSetup inSetup;
    // Resolve empty name to first-enumerated input device — matches the
    // rateSupportedBy preflight above AND probeDeviceOptionsDual. Using
    // empty + useDefault*Channels here would make JUCE open the OS
    // default, which can differ from inputs[0] on platforms where the
    // OS-default differs from JUCE's enumeration order. The probe + SR
    // preflight + actual open all need to agree on the same concrete
    // device for the apply path to behave consistently with what the UI
    // showed the user.
    juce::String resolvedInputName = config.inputDevice;
    if (resolvedInputName.isEmpty())
    {
        auto names = inputType->getDeviceNames(true);
        if (names.size() > 0) resolvedInputName = names[0];
    }

    inSetup.inputDeviceName  = resolvedInputName;
    inSetup.outputDeviceName = "";
    inSetup.sampleRate = config.sampleRate;
    inSetup.bufferSize = config.bufferSize;
    inSetup.useDefaultInputChannels = false;
    inSetup.useDefaultOutputChannels = false;

    int inputChannelCount = 0;
    {
        try {
            std::unique_ptr<juce::AudioIODevice> probe(inputType->createDevice({}, resolvedInputName));
            if (probe) inputChannelCount = probe->getInputChannelNames().size();
        } catch (...) {}
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    inSetup.inputChannels.setRange(0, inputChannelCount, true);
    inSetup.outputChannels.clear();

    // Rollback helper: on any failure path after a side has been opened,
    // close both managers' devices so we don't leave the OS audio resource
    // held (sometimes exclusively, e.g. ASIO) while setDevice reports a
    // failure. closeAudioDevice is idempotent so unconditional calls are
    // safe even when only the input or neither side opened.
    auto rollbackOpenedDevices = [&]() {
        // Drop any callback we already attached to the output manager —
        // closeAudioDevice() does not invoke removeAudioCallback, and leaving
        // outputCallbackRegistered=true would cause the next startAudio()
        // to skip the re-attach (it gates on !outputCallbackRegistered),
        // leaving split-mode output silent after a partial-open failure.
        if (outputCallbackRegistered)
        {
            try { outputDeviceManager.removeAudioCallback(&outputCallback); } catch (...) {}
            outputCallbackRegistered = false;
        }
        try { inputDeviceManager.closeAudioDevice(); } catch (...) {}
        try { outputDeviceManager.closeAudioDevice(); } catch (...) {}
    };

    // Mirror applyDuplexSetup's JUCE_LINUX close-before-reconfigure pattern:
    // ALSA deadlocks if we let setAudioDeviceSetup mutate a live device. The
    // device type is re-asserted afterwards so the close doesn't drop us back
    // to whatever JUCE picked at startup. closeAudioDevice/setCurrentAudioDeviceType
    // throwing is non-fatal — we still try the setup below and surface its error.
#if JUCE_LINUX
    {
        juce::String currentInputTypeName;
        if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
            currentInputTypeName = currentType->getTypeName();
        if (inputDeviceManager.getCurrentAudioDevice() != nullptr)
        {
            try {
                inputDeviceManager.closeAudioDevice();
                if (currentInputTypeName.isNotEmpty())
                    inputDeviceManager.setCurrentAudioDeviceType(currentInputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode input close threw, continuing\n");
            }
        }
    }
#endif

    juce::String inErr;
    try { inErr = inputDeviceManager.setAudioDeviceSetup(inSetup, true); }
    catch (...) { res.error = "input setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (inErr.isNotEmpty()) { res.error = "input setup: " + inErr; rollbackOpenedDevices(); return res; }

    auto* inDev = inputDeviceManager.getCurrentAudioDevice();
    if (!inDev) { res.error = "no input device after setup"; rollbackOpenedDevices(); return res; }
    const double inSr = inDev->getCurrentSampleRate();
    const int    inBs = inDev->getCurrentBufferSizeSamples();

    // Same first-enumerated resolution on the output side — see input note
    // above for why this matches the probe + SR preflight strategy.
    juce::String resolvedOutputName = config.outputDevice;
    if (resolvedOutputName.isEmpty())
    {
        auto names = outputType->getDeviceNames(false);
        if (names.size() > 0) resolvedOutputName = names[0];
    }

    juce::AudioDeviceManager::AudioDeviceSetup outSetup;
    outSetup.inputDeviceName  = "";
    outSetup.outputDeviceName = resolvedOutputName;
    outSetup.sampleRate = config.sampleRate;
    outSetup.bufferSize = config.bufferSize;
    outSetup.useDefaultInputChannels = false;
    outSetup.useDefaultOutputChannels = false;

    int outputChannelCount = 0;
    {
        try {
            std::unique_ptr<juce::AudioIODevice> probe(outputType->createDevice(resolvedOutputName, {}));
            if (probe) outputChannelCount = probe->getOutputChannelNames().size();
        } catch (...) {}
    }
    if (outputChannelCount <= 0) outputChannelCount = 2;
    outSetup.inputChannels.clear();
    outSetup.outputChannels.setRange(0, juce::jmin(outputChannelCount, 2), true);

    // Same JUCE_LINUX close-before-reconfigure as the input side above — also
    // protects when split mode is re-applied with a different output device.
#if JUCE_LINUX
    {
        juce::String currentOutputTypeName;
        if (auto* currentType = outputDeviceManager.getCurrentDeviceTypeObject())
            currentOutputTypeName = currentType->getTypeName();
        if (outputDeviceManager.getCurrentAudioDevice() != nullptr)
        {
            try {
                outputDeviceManager.closeAudioDevice();
                if (currentOutputTypeName.isNotEmpty())
                    outputDeviceManager.setCurrentAudioDeviceType(currentOutputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode output close threw, continuing\n");
            }
        }
    }
#endif

    juce::String outErr;
    try { outErr = outputDeviceManager.setAudioDeviceSetup(outSetup, true); }
    catch (...) { res.error = "output setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (outErr.isNotEmpty()) { res.error = "output setup: " + outErr; rollbackOpenedDevices(); return res; }

    auto* outDev = outputDeviceManager.getCurrentAudioDevice();
    if (!outDev) { res.error = "no output device after setup"; rollbackOpenedDevices(); return res; }
    const double outSr = outDev->getCurrentSampleRate();
    const int    outBs = outDev->getCurrentBufferSizeSamples();

    if (std::abs(inSr - outSr) > 0.5)
    {
        res.error = "Input and output devices opened at different sample rates";
        rollbackOpenedDevices();
        return res;
    }

    currentSampleRate.store(inSr, std::memory_order_relaxed);
    inputBlockSize.store(inBs, std::memory_order_relaxed);
    outputBlockSize.store(outBs, std::memory_order_relaxed);

    fprintf(stderr, "[AudioEngine] Split mode configured: inSr=%.0f inBs=%d outSr=%.0f outBs=%d\n",
            inSr, inBs, outSr, outBs);

    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    outputUnderflowCount.store(0, std::memory_order_relaxed);
    inputOverflowCount.store(0, std::memory_order_relaxed);
    for (auto& slot : outputPendingRing)
        slot.store(0u, std::memory_order_relaxed);

    signalChain.prepare(inSr, inBs);
    noiseGate.prepare(inSr, inBs);
    tonePolish.prepare(inSr);

    res.ok = true;
    res.sampleRate = inSr;
    res.inputBlockSize = inBs;
    res.outputBlockSize = outBs;
    return res;
}

void AudioEngine::teardownSplitMode()
{
    // Unconditional remove — JUCE's removeAudioCallback is idempotent
    // (no-op if the callback isn't registered), so we don't need the
    // outputCallbackRegistered guard here. This makes teardown robust
    // against a stale flag left over from a previous failed split setup.
    outputDeviceManager.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
    try { outputDeviceManager.closeAudioDevice(); }
    catch (...) { fprintf(stderr, "[AudioEngine] teardownSplitMode: output close threw\n"); }

    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    for (auto& slot : outputPendingRing)
        slot.store(0u, std::memory_order_relaxed);
}

// ── Audio Control ─────────────────────────────────────────────────────────────

void AudioEngine::startAudio()
{
    if (audioRunning.load(std::memory_order_relaxed))
    {
        fprintf(stderr, "[AudioEngine] startAudio: already running\n");
        return;
    }

    // Input first so it has time to prefill the ring before the output
    // callback pulls — otherwise split mode underflows once at start.
    inputDeviceManager.addAudioCallback(this);

    // Guard against double-registration: audioRunning can be cleared by
    // audioDeviceStopped() on a transient input unplug while the output
    // callback intentionally stays registered (JUCE auto-restart relies on
    // that). A later startAudio() would then add the same callback again
    // and JUCE would dispatch it twice per block.
    if (!duplexMode.load(std::memory_order_relaxed) && !outputCallbackRegistered)
    {
        outputDeviceManager.addAudioCallback(&outputCallback);
        outputCallbackRegistered = true;
    }

    audioRunning.store(true, std::memory_order_relaxed);
    fprintf(stderr, "[AudioEngine] startAudio: duplex=%d input='%s' output='%s'\n",
            (int) duplexMode.load(std::memory_order_relaxed),
            inputDeviceManager.getCurrentAudioDevice()
                ? inputDeviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "none",
            outputDeviceManager.getCurrentAudioDevice()
                ? outputDeviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "(duplex)");
}

void AudioEngine::stopAudio()
{
    // Always attempt to detach both callbacks — removeAudioCallback is
    // idempotent. We don't gate on audioRunning here because that flag can
    // be cleared externally by audioDeviceStopped() (input device
    // hot-unplug); in split mode the output callback may still be registered
    // even after the input side has reported itself stopped, and a guarded
    // stopAudio() would no-op while leaving the output device producing.
    // Output first so it doesn't pull from a stalling ring during detach.
    outputDeviceManager.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
    inputDeviceManager.removeAudioCallback(this);
    audioRunning.store(false, std::memory_order_relaxed);
}

// ── Backing Track ─────────────────────────────────────────────────────────────

bool AudioEngine::loadBackingTrack(const juce::File& file)
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
    backingTransport.reset();
    backingSource.reset();

    const bool exists = file.existsAsFile();
    std::cerr << "[AudioEngine] loadBackingTrack path="
              << file.getFullPathName().toStdString()
              << " exists=" << exists
              << " size=" << (exists ? (long long)file.getSize() : -1)
              << std::endl;

    auto* reader = formatManager.createReaderFor(file);
    if (!reader)
    {
        std::cerr << "[AudioEngine] loadBackingTrack: no reader for ext='"
                  << file.getFileExtension().toStdString()
                  << "' (registered formats=" << formatManager.getNumKnownFormats()
                  << ")" << std::endl;
        // Transport/source already reset above; clear cached state so the renderer
        // doesn't keep displaying the previous track's position/duration.
        cachedBackingPosition.store(0.0);
        cachedBackingDuration.store(0.0);
        return false;
    }

    const double readerSampleRate = reader->sampleRate;
    const juce::int64 readerLengthInSamples = reader->lengthInSamples;
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    // Backing audio plays through the output device in both modes, so size
    // against outputBlockSize. In duplex mode outputBlockSize == inputBlockSize;
    // in split mode the output device's clock drives the backing pull.
    const int    bs = outputBlockSize.load(std::memory_order_relaxed);

    backingSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    backingTransport = std::make_unique<juce::AudioTransportSource>();
    // The 4th arg makes AudioTransportSource SRC the file to device rate.
    // Stretch always sees device-rate audio so that its presetDefault parameters match.
    backingTransport->setSource(backingSource.get(), 0, nullptr, readerSampleRate);

    // Loading a backing track before the audio device has started leaves
    // sr/bs at zero. presetDefault(2, 0.0f) would seed the stretcher with
    // undefined internal timing, and prepareToPlay(0, 0) is similarly
    // ill-defined. Defer the stretcher + buffer setup; the relevant
    // audio*AboutToStart() re-runs the same block once a real sample
    // rate / block size are known (audioDeviceAboutToStart for duplex,
    // audioOutputAboutToStart for split).
    if (sr > 0.0 && bs > 0)
    {
        // prepareToPlay's first arg is an upper bound on subsequent
        // getNextAudioBlock requests, per the juce::AudioSource contract.
        // The RT callback can pull ceil(bs * kMaxBackingSpeed) frames in a
        // single block when the speed is above 1×, so prepare for that
        // worst case — preparing with just `bs` would risk JUCE internal
        // buffer overruns/asserts on the first faster-than-1× block.
        const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
        backingTransport->prepareToPlay(maxInputFrames, sr);

        backingStretch.presetDefault(2, (float) sr);
        backingStretch.reset();
        backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);

        backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
        backingBuffer.setSize(2, bs, false, false, true);
    }

    cachedBackingDuration.store(backingTransport->getLengthInSeconds());
    cachedBackingPosition.store(0.0);
    backingHeardPositionSec.store(0.0, std::memory_order_relaxed);
    std::cerr << "[AudioEngine] loadBackingTrack OK sr=" << readerSampleRate
              << " len=" << readerLengthInSamples
              << std::endl;
    return true;
}

void AudioEngine::setBackingPosition(double seconds)
{
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingTransport->setPosition(seconds);
        backingStretch.reset();
        // Read back the actual position; the transport may clamp (e.g. negative or past EOF).
        const double pos = backingTransport->getCurrentPosition();
        cachedBackingPosition.store(pos);
        backingHeardPositionSec.store(pos, std::memory_order_relaxed);
    }
}

void AudioEngine::startBacking()
{
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingTransport->start();
        backingPlaying.store(true);
        backingHeardPositionSec.store(backingTransport->getCurrentPosition(),
                                      std::memory_order_relaxed);
    }
}

void AudioEngine::stopBackingNoLock()
{
    if (backingTransport)
    {
        backingTransport->stop();
        backingStretch.reset();
        backingPlaying.store(false);
    }
}

void AudioEngine::stopBacking()
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
}

void AudioEngine::setBackingSpeed(double speed)
{
    if (!std::isfinite(speed) || speed <= 0.0)
    {
        return;
    }

    const double clamped = juce::jlimit(0.01, kMaxBackingSpeed, speed);
    const double prev = backingSpeed.load(std::memory_order_relaxed);
    if (std::abs(clamped - prev) < 0.001)
    {
        return;
    }

    // Reset stretch and re-anchor heard position before publishing the new rate.
    // RT reads backingSpeed under tryLock; publishing early could process one block
    // at the new rate with stale stretch state (CodeRabbit PR #237).
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingStretch.reset();
        const double pos = backingTransport->getCurrentPosition();
        backingHeardPositionSec.store(pos, std::memory_order_relaxed);
    }

    backingSpeed.store(clamped, std::memory_order_release);

    std::cerr << "[AudioEngine] setBackingSpeed(" << clamped << ") stretch reset"
              << std::endl;
}

void AudioEngine::resetPeaks()
{
    inputPeak.store(0.0f);
    outputPeak.store(0.0f);
}

void AudioEngine::setNoiseGate(bool enabled, float thresholdDb, float releaseMs, float depthDb)
{
    noiseGate.setParameters(enabled, thresholdDb, releaseMs, depthDb);
}

void AudioEngine::setTonePolishEnabled(bool enabled)
{
    tonePolish.setEnabled(enabled);
}

// ── Audio Callback ────────────────────────────────────────────────────────────

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // Fires on the input manager — duplex serves output here too; split has
    // audioOutputAboutToStart on the second manager.
    //
    // Also fires when JUCE auto-restarts the input after a transient stop
    // (hot-replug, OS resume). audioDeviceStopped() cleared audioRunning;
    // restore it here so scoreChord() / getActiveDetection() come back
    // online without requiring a manual stopAudio()/startAudio() cycle,
    // and so a subsequent setAudioDevices() sees the engine as running
    // and runs its defensive stopAudio() before mutating device state.
    audioRunning.store(true, std::memory_order_relaxed);
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    currentSampleRate.store(sr, std::memory_order_relaxed);
    inputBlockSize.store(bs, std::memory_order_relaxed);
    if (duplexMode.load(std::memory_order_relaxed))
        outputBlockSize.store(bs, std::memory_order_relaxed);

    // Reset the input ring buffer so a stop→start cycle delivers a
    // clean zero-padded cold-start frame instead of mixing in stale
    // samples from the previous run. Plain relaxed stores are fine —
    // the audio thread isn't running yet (this is the device-start
    // hook), so there's no race to worry about here.
    inputFrameRingWriteIndex.store(0, std::memory_order_relaxed);
    for (auto& slot : inputFrameRing)
        slot.store(0.0f, std::memory_order_relaxed);

    // Pre-size the zero-output capture scratch to this device's block
    // size so the audio thread doesn't allocate when we hit that path.
    // For the common output > 0 case this storage stays unused.
    if ((int) inputCaptureScratch.size() < bs)
        inputCaptureScratch.assign((size_t) bs, 0.0f);

    // Input callback uses outputBackingBuffer as DSP scratch in split mode.
    // Pre-size against input block size so the hot path can't allocate when
    // inputBlockSize > outputBlockSize (audioOutputAboutToStart only sizes
    // against output).
    if (!duplexMode.load(std::memory_order_relaxed)
        && outputBackingBuffer.getNumSamples() < bs)
    {
        outputBackingBuffer.setSize(2, bs, false, false, true);
    }

    signalChain.prepare(sr, bs);
    pitchDetector.prepare(sr, bs);
    mlNoteDetector.prepare(sr, bs);
    noteVerifier.prepare(sr, bs);
    noiseGate.prepare(sr, bs);
    tonePolish.prepare(sr);

    // Split mode preps the backing stretcher in audioOutputAboutToStart
    // instead — that callback owns the device the backing audio actually
    // plays on, and pulls from backingTransport at the output device's
    // block size.
    if (duplexMode.load(std::memory_order_relaxed))
    {
        const juce::ScopedLock sl(backingLock);
        if (backingTransport)
        {
            // See loadBackingTrack() for why prepareToPlay uses maxInputFrames
            // rather than bs: the RT callback can pull ceil(bs * kMaxBackingSpeed)
            // frames in a single block at faster-than-1× speeds.
            const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
            backingTransport->prepareToPlay(maxInputFrames, sr);
            backingStretch.presetDefault(2, (float) sr);
            backingStretch.reset();
            backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);
            backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
            backingBuffer.setSize(2, bs, false, false, true);
        }
    }
}

void AudioEngine::audioDeviceStopped()
{
    signalChain.releaseResources();
    mlNoteDetector.stop();
    noteVerifier.stop();
    inputFrameRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    audioRunning.store(false, std::memory_order_relaxed);

    // Note on split-mode lifecycle: we deliberately do NOT detach the
    // output callback here. JUCE auto-restarts a transiently-stopped input
    // device by re-firing audioDeviceAboutToStart() on its own; that path
    // doesn't re-add the output callback, so detaching would break
    // automatic recovery (output stays silent until a manual reconfigure).
    // While input is down, the guitar/DSP side of the output goes silent
    // (no producer feeding outputPendingRing, so the consumer's underflow
    // branch zero-fills), but the backing track keeps playing — the output
    // callback mixes backingTransport independently of ring state. That's
    // intentional UX: a user unplugging their interface mid-song doesn't
    // lose their place. Real teardown belongs to stopAudio() / teardownSplitMode().
}

void AudioEngine::audioOutputAboutToStart(juce::AudioIODevice* device)
{
    const int bs = device->getCurrentBufferSizeSamples();
    outputBlockSize.store(bs, std::memory_order_relaxed);

    if ((int) outputPullScratchL.size() < bs) outputPullScratchL.assign((size_t) bs, 0.0f);
    if ((int) outputPullScratchR.size() < bs) outputPullScratchR.assign((size_t) bs, 0.0f);
    // NOTE: outputBackingBuffer is sized by audioDeviceAboutToStart() from the
    // INPUT device's block size — it's the split-input DSP scratch, not an
    // output-side buffer. Don't touch it here: resizing from the output
    // thread races with the input callback and can shrink the scratch below
    // its expected size. The output side uses backingBuffer / backingInputBuffer,
    // sized below.

    // Prefer the output device's reported rate as authoritative. The
    // stored currentSampleRate (set from the input side) was the right
    // fallback during initial setup, but if the OS or driver reopened
    // the output device at a different rate (format change, sleep/resume,
    // user-changed default), trusting the stored value would seed the
    // stretcher with a mismatched rate. Compare and warn so the
    // discrepancy is visible in logs; the device-reported rate wins.
    const double srStored = currentSampleRate.load(std::memory_order_relaxed);
    const double srDev    = device->getCurrentSampleRate();
    const double sr = srDev > 0.0 ? srDev : srStored;
    if (srStored > 0.0 && srDev > 0.0 && std::abs(srStored - srDev) > 0.5)
    {
        fprintf(stderr, "[AudioEngine] audioOutputAboutToStart: stored sr=%.0f differs from device sr=%.0f — using device\n",
                srStored, srDev);
    }
    // Propagate the authoritative rate to currentSampleRate so downstream
    // consumers (audioOutputCallback's latency comp, getLatencyMs(),
    // scoreChord's fallback) all see the same value. Without this, a
    // device-side rate change (sleep/resume, format change) would leave
    // currentSampleRate stuck at the input-side seed value.
    if (sr > 0.0) currentSampleRate.store(sr, std::memory_order_relaxed);
    {
        const juce::ScopedLock sl(backingLock);
        if (backingTransport && sr > 0.0 && bs > 0)
        {
            // Mirror loadBackingTrack() / audioDeviceAboutToStart() — the
            // output device drives backing playback in split mode, so this
            // is where the stretcher gets sized for that side. prepareToPlay
            // upper-bounds future getNextAudioBlock requests, and the
            // RT callback can pull ceil(bs * kMaxBackingSpeed) at faster
            // speeds.
            const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
            backingTransport->prepareToPlay(maxInputFrames, sr);
            backingStretch.presetDefault(2, (float) sr);
            backingStretch.reset();
            backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);
            backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
            backingBuffer.setSize(2, bs, false, false, true);
        }
    }
}

void AudioEngine::audioOutputStopped()
{
    // No-op by design. The consumer's catch-up branch in audioOutputCallback
    // handles both (w - r) > cap (producer lapped during the stop) and
    // w < r (a future reset race) on the next output start, so we don't
    // need to reset readIndex here. Resetting r to 0 while the producer
    // keeps advancing w is equivalent — both end up in the catch-up branch
    // — but it leaves a transient window where r appears reset without the
    // ring invariants being re-established, which is harder to reason about.
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputData, int numInputChannels,
    float* const* outputData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const bool duplex = duplexMode.load(std::memory_order_relaxed);

    // Duplex writes outputData directly. Split runs DSP into a private 2-channel
    // scratch and pushes the result to outputPendingRing for OutputCallback.
    juce::AudioBuffer<float> buffer;
    if (duplex)
    {
        buffer.setDataToReferTo(outputData, numOutputChannels, numSamples);
    }
    else
    {
        // outputBackingBuffer is pre-sized to inputBlockSize in
        // audioDeviceAboutToStart(); never realloc on the RT thread. A
        // reconfig race could transiently deliver a larger numSamples —
        // clamp it (drop the tail) so the rest of the callback operates
        // strictly within the allocated scratch.
        const int scratchCap = outputBackingBuffer.getNumSamples();
        if (numSamples > scratchCap)
            numSamples = scratchCap;
        outputBackingBuffer.clear(0, 0, numSamples);
        outputBackingBuffer.clear(1, 0, numSamples);
        buffer.setDataToReferTo(outputBackingBuffer.getArrayOfWritePointers(), 2, numSamples);
    }

    const int effectiveOutputChannels = duplex ? numOutputChannels : 2;

    float inGain = inputGain.load();
    int selectedCh = selectedInputChannel.load();

    // Copy input with gain, handling channel selection. Track how
    // many output channels we've filled so the "zero extras" pass
    // below can clip to the right range — the broadcast branches
    // fill all of them, the pass-through branch only fills the
    // overlap.
    int filledOutputChannels = 0;
    if (numInputChannels >= 2 && selectedCh >= 0 && selectedCh < numInputChannels)
    {
        // Single-channel mode (e.g. dry from Valeton GP-5 left channel).
        // Broadcast the selected input across all output channels.
        for (int outCh = 0; outCh < effectiveOutputChannels; ++outCh)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(outCh, i, inputData[selectedCh][i] * inGain);
        filledOutputChannels = effectiveOutputChannels;
    }
    else if (selectedCh < 0 && numInputChannels > 1)
    {
        // Default pair mono mix: average the first two input channels
        // and broadcast the result to every output channel, so the
        // signal chain, pitch detector, input-frame ring, and the
        // user's monitoring all see the same mono signal. We open all
        // advertised hardware inputs so explicit higher channel picks
        // work, but the default keeps the old first-pair semantics
        // instead of attenuating the signal by averaging every input.
        const int mixChannels = juce::jmin(numInputChannels, 2);
        const float invCh = 1.0f / (float) mixChannels;
        for (int i = 0; i < numSamples; ++i)
        {
            float mix = 0.0f;
            for (int ch = 0; ch < mixChannels; ++ch)
                mix += inputData[ch][i];
            const float gained = mix * invCh * inGain;
            for (int outCh = 0; outCh < effectiveOutputChannels; ++outCh)
                buffer.setSample(outCh, i, gained);
        }
        filledOutputChannels = effectiveOutputChannels;
    }
    else
    {
        // Pass-through: single-input device, or stereo in/out with no
        // explicit channel selection and no need to mix.
        const int passThroughChannels = juce::jmin(numInputChannels, effectiveOutputChannels);
        for (int ch = 0; ch < passThroughChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(ch, i, inputData[ch][i] * inGain);
        filledOutputChannels = passThroughChannels;
    }

    // Zero anything we didn't fill. Previously this was hard-coded to
    // start at numInputChannels, which on a 2-in/4-out broadcast
    // config would have wiped the upper two channels we just wrote.
    for (int ch = filledOutputChannels; ch < effectiveOutputChannels; ++ch)
        buffer.clear(ch, 0, numSamples);

    // Metering: input level (pre-processing)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < effectiveOutputChannels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
        currentInputLevel.store(peak);
        float prevPeak = inputPeak.load();
        if (peak > prevPeak) inputPeak.store(peak);
    }

    // Feed pitch detector + ring with the pre-FX dry guitar. Buffer ch 0 holds
    // the post-gain mono signal in both duplex and split paths. Zero-output
    // duplex setups (input-only ASIO/JACK) need the scratch fallback.
    const float* monoSource = nullptr;
    if (effectiveOutputChannels > 0)
    {
        monoSource = buffer.getReadPointer(0);
    }
    else if (numInputChannels > 0 && (int) inputCaptureScratch.size() >= numSamples)
    {
        // Build the mono source mirroring the channel-copy semantics:
        // explicit channel select picks one input; -1 with multi-input
        // averages the first pair; otherwise input channel 0.
        if (selectedCh >= 0 && selectedCh < numInputChannels)
        {
            for (int i = 0; i < numSamples; ++i)
                inputCaptureScratch[(size_t) i] = inputData[selectedCh][i] * inGain;
        }
        else if (selectedCh < 0 && numInputChannels > 1)
        {
            const int mixChannels = juce::jmin(numInputChannels, 2);
            const float invCh = 1.0f / (float) mixChannels;
            for (int i = 0; i < numSamples; ++i)
            {
                float mix = 0.0f;
                for (int ch = 0; ch < mixChannels; ++ch)
                    mix += inputData[ch][i];
                inputCaptureScratch[(size_t) i] = mix * invCh * inGain;
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
                inputCaptureScratch[(size_t) i] = inputData[0][i] * inGain;
        }
        monoSource = inputCaptureScratch.data();
    }

    if (monoSource != nullptr)
    {
        pitchDetector.pushSamples(monoSource, numSamples);
        // Feed the polyphonic ML detector the same dry mono signal. Lock-free
        // and a no-op when ONNX support isn't compiled in.
        mlNoteDetector.pushSamples(monoSource, numSamples);

        // Mirror the same signal into the lock-free ring buffer that
        // backs getInputFrame(). The release-store on the write index
        // pairs with the main-thread reader's acquire load so every
        // sample written below is visible before the index update.
        // Per-slot stores are atomic-relaxed so the concurrent read
        // by getInputFrame() isn't a data race (UB) when the writer
        // laps mid-snapshot.
        const uint64_t w = inputFrameRingWriteIndex.load(std::memory_order_relaxed);
        constexpr int kMask = kInputFrameRingCapacity - 1;
        for (int i = 0; i < numSamples; ++i)
            inputFrameRing[(w + (uint64_t) i) & (uint64_t) kMask]
                .store(monoSource[i], std::memory_order_relaxed);
        inputFrameRingWriteIndex.store(w + (uint64_t) numSamples, std::memory_order_release);
    }

    noiseGate.processBlock(buffer);

    // Process through signal chain (VSTs, NAM, IR)
    bool hasProcessors = signalChain.getNumSlots() > 0;
    juce::MidiBuffer midi;
    signalChain.process(buffer, midi);

    // Monitor mute: silence the guitar pass-through when no processors are loaded.
    // This prevents hearing raw/amp-processed input when the user hasn't set up a chain yet.
    // Backing track still plays through. Suppressed during a song-load chain
    // rebuild so the brief (or failed) empty-chain window doesn't silence the guitar.
    if (monitorMuted.load() && !hasProcessors && !monitorMuteSuppressed.load())
        buffer.clear();

    // Chain output gain — the amp/tone's output level. Applied to the guitar
    // signal ONLY, before the backing track is mixed in, so switching tone
    // presets changes the guitar level without touching the song volume.
    buffer.applyGain(chainOutputGain.load());

    // Tone Polish — fixed 3-band mastering EQ (HPF 80 Hz, low shelf -3 dB
    // @ 180 Hz, peak -0.5 dB @ 200 Hz Q=1). Sits on the guitar bus only,
    // between chain output gain and the backing-track mix, so the backing
    // track and master output gain stay bit-untouched. Bypassed at a
    // single atomic load when disabled.
    tonePolish.processBlock(buffer);

    // Duplex mixes backing + applies output gain + meters here.
    // Split defers all three to OutputCallback (output device's clock).
    if (duplex)
    {
        const juce::ScopedTryLock sl(backingLock);
        if (sl.isLocked() && backingTransport && backingPlaying.load())
        {
            const double rate = juce::jlimit(0.01, kMaxBackingSpeed, backingSpeed.load(std::memory_order_relaxed));

            // Defensive clamp: the buffers are sized in audioDeviceAboutToStart()
            // from the device's nominal block size, but the callback can deliver
            // a larger numSamples on a device-reconfig race. Drop the excess
            // frames silently rather than reading/writing past the allocated
            // span. This avoids realloc on the RT thread; the next callback
            // after reconfig will arrive at the new nominal size.
            const int outCap = backingBuffer.getNumSamples();
            const int inCap  = backingInputBuffer.getNumSamples();
            const int outSamples = juce::jmin(numSamples, outCap);
            const double sr = currentSampleRate.load(std::memory_order_relaxed);
            const bool bypassStretch = std::abs(rate - 1.0) < kBackingSpeedBypassEpsilon;

            int sourceFramesPulled = 0;

            if (bypassStretch)
            {
                // 1× — bit-transparent transport read, no phase-vocoder path.
                backingBuffer.clear(0, outSamples);
                juce::AudioSourceChannelInfo info(&backingBuffer, 0, outSamples);
                backingTransport->getNextAudioBlock(info);
                sourceFramesPulled = outSamples;
            }
            else
            {
                // Slow/fast path — pull only the source frames needed for this output
                // block (output * rate), then stretch in-process to fill outSamples.
                const int inputFrames = juce::jmin((int) std::ceil(outSamples * rate), inCap);

                backingInputBuffer.clear(0, inputFrames);
                juce::AudioSourceChannelInfo info(&backingInputBuffer, 0, inputFrames);
                backingTransport->getNextAudioBlock(info);
                sourceFramesPulled = inputFrames;

                backingBuffer.clear(0, outSamples);

                const float* const* inPtrs  = backingInputBuffer.getArrayOfReadPointers();
                float* const* outPtrs = backingBuffer.getArrayOfWritePointers();
                backingStretch.process(inPtrs, inputFrames, outPtrs, outSamples);
            }

            if (sr > 0.0 && sourceFramesPulled > 0)
            {
                const double heard = backingHeardPositionSec.load(std::memory_order_relaxed)
                                     + static_cast<double>(sourceFramesPulled) / sr;
                backingHeardPositionSec.store(heard, std::memory_order_relaxed);

                const double latencyInputSec = bypassStretch
                    ? static_cast<double>(backingStretchLatencySamples.load(std::memory_order_relaxed)) / sr
                    : (backingStretchLatencySamples.load(std::memory_order_relaxed) * rate) / sr;
                cachedBackingPosition.store(juce::jmax(0.0, heard - latencyInputSec));
            }

            // Sync the flag if transport stopped at EOF
            if (!backingTransport->isPlaying())
                backingPlaying.store(false);

            float bVol = backingVolume.load();
            const int mixChannels = juce::jmin(numOutputChannels, 2);
            for (int ch = 0; ch < mixChannels; ++ch)
                buffer.addFrom(ch, 0, backingBuffer, ch, 0, outSamples, bVol);
        }

        // Apply output gain
        buffer.applyGain(outputGain.load());

        // Output metering
        float peak = 0.0f;
        for (int ch = 0; ch < numOutputChannels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
        currentOutputLevel.store(peak);
        float prevPeak = outputPeak.load();
        if (peak > prevPeak) outputPeak.store(peak);
    }
    else
    {
        // Split: push processed stereo (pre-backing, pre-output-gain) into the
        // ring. OutputCallback adds backing + output gain on its own clock.
        //
        // Strict SPSC: producer (this callback) only ever writes
        // outputRingWriteIndex; consumer (audioOutputCallback) is the sole
        // writer of outputRingReadIndex. Drop-oldest is achieved by letting
        // writeIndex lap the buffer — old slots get overwritten in place,
        // and the consumer catches up by advancing its own readIndex when
        // it observes (w - r) > cap. Counting the overflow at the consumer
        // side is what surfaces it in DeviceMetrics.
        constexpr uint64_t kMask = (uint64_t) kOutputRingFrames - 1;
        const uint64_t w = outputRingWriteIndex.load(std::memory_order_relaxed);

        const float* L = buffer.getReadPointer(0);
        const float* R = buffer.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const uint64_t slot = (w + (uint64_t) i) & kMask;
            // Single atomic store packs both channels — prevents the L/R tear
            // a consumer could otherwise observe when the producer wraps
            // mid-callback (relaxed because ordering is established by the
            // release on outputRingWriteIndex below).
            outputPendingRing[slot].store(packLR(L[i], R[i]), std::memory_order_relaxed);
        }
        outputRingWriteIndex.store(w + (uint64_t) numSamples, std::memory_order_release);
    }
}

void AudioEngine::audioOutputCallback(const float* const* /*inputData*/,
                                      int /*numInputChannels*/,
                                      float* const* outputData,
                                      int numOutputChannels,
                                      int numSamples)
{
    juce::AudioBuffer<float> buffer(outputData, numOutputChannels, numSamples);
    if (numOutputChannels <= 0)
        return;

    constexpr uint64_t kMask = (uint64_t) kOutputRingFrames - 1;
    constexpr uint64_t kCap  = (uint64_t) kOutputRingFrames;

    // Clamp the working size to the scratch capacity pre-allocated in
    // audioOutputAboutToStart() so the .assign() calls below never realloc
    // on the RT thread when a transient oversized block arrives (mirrors
    // the backing path's clamp in audioDeviceIOCallbackWithContext).
    const int scratchCap = (int) outputPullScratchL.size();
    const int outSamples = juce::jmin(numSamples, scratchCap);

    uint64_t r = outputRingReadIndex.load(std::memory_order_relaxed);
    const uint64_t w = outputRingWriteIndex.load(std::memory_order_acquire);

    // If audioDeviceStopped() raced between our two loads and reset both
    // indices to 0, we can observe w < r. Treat that as an empty ring
    // and resync — without this, the unsigned (w - r) wraps into a huge
    // positive value and falls into the catch-up branch reading stale slots.
    if (w < r)
    {
        r = w;
        outputRingReadIndex.store(r, std::memory_order_relaxed);
    }

    // Catch up if the producer has lapped (drop-oldest is achieved via this
    // single-writer consumer-side advance, not a producer-side write to r).
    if ((w - r) > kCap)
    {
        r = w - kCap;
        outputRingReadIndex.store(r, std::memory_order_relaxed);
        inputOverflowCount.fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t available = w - r;
    const int      pullCount = juce::jmin(outSamples, (int) available);

    // When scratchCap clamped outSamples below numSamples (device-reconfig
    // race), we still need to consume the ring frames that match the
    // device callback's full block size — otherwise those extras stay
    // queued and play back late, accumulating ring/output-clock skew
    // until the latency is audible. Drop them from the ring without
    // copying into scratch.
    const int      consumeCount = juce::jmin(numSamples, (int) available);

    for (int i = 0; i < pullCount; ++i)
    {
        const uint64_t slot = (r + (uint64_t) i) & kMask;
        // Single atomic load → atomic unpack of both channels (matches the
        // producer's packed store) so L and R always belong to the same frame.
        float l, rr;
        unpackLR(outputPendingRing[slot].load(std::memory_order_relaxed), l, rr);
        outputPullScratchL[(size_t) i] = l;
        outputPullScratchR[(size_t) i] = rr;
    }
    if (pullCount < outSamples)
    {
        for (int i = pullCount; i < outSamples; ++i)
        {
            outputPullScratchL[(size_t) i] = 0.0f;
            outputPullScratchR[(size_t) i] = 0.0f;
        }
        outputUnderflowCount.fetch_add(1, std::memory_order_relaxed);
    }
    outputRingReadIndex.store(r + (uint64_t) consumeCount, std::memory_order_release);

    buffer.clear();
    const int copyChannels = juce::jmin(numOutputChannels, 2);
    for (int i = 0; i < outSamples; ++i)
    {
        buffer.setSample(0, i, outputPullScratchL[(size_t) i]);
        if (copyChannels > 1)
            buffer.setSample(1, i, outputPullScratchR[(size_t) i]);
    }

    {
        const juce::ScopedTryLock sl(backingLock);
        if (sl.isLocked() && backingTransport && backingPlaying.load())
        {
            // Mirror the duplex callback's stretch path. Buffers are sized in
            // audioOutputAboutToStart() against the output device's nominal bs;
            // clamp here in case a reconfig race delivers a larger numSamples.
            const double rate = juce::jlimit(0.01, kMaxBackingSpeed, backingSpeed.load(std::memory_order_relaxed));
            const int outCap = backingBuffer.getNumSamples();
            const int inCap  = backingInputBuffer.getNumSamples();
            const int backingOut = juce::jmin(numSamples, outCap);
            const int inputFrames = juce::jmin((int) std::ceil(backingOut * rate), inCap);

            backingInputBuffer.clear(0, inputFrames);
            juce::AudioSourceChannelInfo info(&backingInputBuffer, 0, inputFrames);
            backingTransport->getNextAudioBlock(info);

            backingBuffer.clear(0, backingOut);
            const float* const* inPtrs  = backingInputBuffer.getArrayOfReadPointers();
            float* const*       outPtrs = backingBuffer.getArrayOfWritePointers();
            backingStretch.process(inPtrs, inputFrames, outPtrs, backingOut);

            const double srNow = currentSampleRate.load(std::memory_order_relaxed);
            const double latencyInputSec = (srNow > 0.0)
                ? (backingStretchLatencySamples.load(std::memory_order_relaxed) * rate) / srNow
                : 0.0;
            cachedBackingPosition.store(juce::jmax(0.0, backingTransport->getCurrentPosition() - latencyInputSec));

            if (!backingTransport->isPlaying())
                backingPlaying.store(false);

            float bVol = backingVolume.load();
            for (int ch = 0; ch < copyChannels; ++ch)
                buffer.addFrom(ch, 0, backingBuffer, ch, 0, backingOut, bVol);
        }
    }

    buffer.applyGain(outputGain.load());

    float peak = 0.0f;
    for (int ch = 0; ch < numOutputChannels; ++ch)
        peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
    currentOutputLevel.store(peak);
    float prevPeak = outputPeak.load();
    if (peak > prevPeak) outputPeak.store(peak);
}

ChordScorer::Result AudioEngine::scoreChord(const ChordScorer::Request& req)
{
    // Fast-path when the device isn't running — the input ring is
    // zeroed in audioDeviceStopped() (and stays at zero between init
    // and the first device start), so any FFT we ran here would just
    // produce an all-miss score against a silence buffer. Skip the
    // ring snapshot + FFT and synthesize the same shape directly.
    // Keeps a renderer that polls scoreChord during a transport stop
    // from spending main-thread CPU on a known-no-op call.
    if (! audioRunning.load(std::memory_order_relaxed))
    {
        ChordScorer::Result out{};
        out.totalStrings = (int) req.notes.size();
        out.results.reserve(req.notes.size());
        for (const auto& n : req.notes)
        {
            ChordScorer::NoteResult r{};
            r.string = n.string;
            r.fret = n.fret;
            out.results.push_back(r);
        }
        return out;
    }

    // When a Basic Pitch model is loaded, judge the chord against the ML
    // detector's active-pitch set — genuine polyphonic transcription rather
    // than the per-string energy/constraint check. Returns the identical
    // Result shape so the N-API wrapper and the plugin need no change.
    //
    // `req.bypassMl` overrides this: the ML path is onset-driven and drops
    // notes the detector never onsets, so the renderer can force the DSP
    // band-energy scorer to verify a chart note from spectral energy alone.
    if (! req.bypassMl && mlNoteDetector.isReady())
        return scoreChordWithMl(req);

    // Snapshot the input ring at the requested window size and forward
    // to the scorer. The renderer never sees audio data — only the
    // result object — which is the whole point of running the scoring
    // here rather than over an audio-frame IPC.
    const int numSamples = (req.numSamples > 0) ? req.numSamples : 4096;
    auto frame = getInputFrame(numSamples);
    // currentSampleRate is 0 between init() and the first
    // audioDeviceAboutToStart, and can also drop to 0 after a device
    // teardown. ChordScorer would still return a well-shaped all-miss
    // result in that case (see its `numSamples <= 0 || sampleRate <= 0`
    // fail-closed path), so this fallback isn't load-bearing for the
    // result shape — but a 48 kHz floor lets the scorer actually run
    // the FFT against whatever stale audio is still in the ring,
    // giving the renderer a real score during the small window between
    // device-stop and the next audioRunning=false observation.
    // Mirrors NodeAddon::GetSampleRate's 48 kHz floor.
    double sr = currentSampleRate.load(std::memory_order_relaxed);
    if (! std::isfinite(sr) || sr <= 0.0) sr = 48000.0;
    return chordScorer.scoreChord(frame.data(), (int) frame.size(), sr, req);
}

std::vector<float> AudioEngine::getInputFrame(int numSamples) const
{
    if (numSamples <= 0) return {};
    if (numSamples > kInputFrameRingCapacity)
        numSamples = kInputFrameRingCapacity;

    // Acquire pairs with the audio thread's release store of the write
    // index: every sample written into the ring before that index is
    // visible to us here.
    const uint64_t w = inputFrameRingWriteIndex.load(std::memory_order_acquire);
    std::vector<float> out((size_t) numSamples, 0.0f);

    // Cold-start: audio thread hasn't filled `numSamples` yet. Return
    // what we have, zero-padded on the *left* so the most-recent
    // samples land at the end of the buffer (the YIN/HPS algorithms
    // expect time-aligned data).
    if (w < (uint64_t) numSamples)
    {
        const size_t available = (size_t) w;
        for (size_t i = 0; i < available; ++i)
            out[(size_t) numSamples - available + i]
                = inputFrameRing[i].load(std::memory_order_relaxed);
        return out;
    }

    constexpr uint64_t kMask = (uint64_t) kInputFrameRingCapacity - 1;
    const uint64_t start = w - (uint64_t) numSamples;
    for (int i = 0; i < numSamples; ++i)
        out[(size_t) i]
            = inputFrameRing[(start + (uint64_t) i) & kMask].load(std::memory_order_relaxed);
    return out;
}

uint64_t AudioEngine::getInputSince(uint64_t fromIndex, std::vector<float>& out) const
{
    out.clear();
    // Acquire pairs with the audio thread's release store — every sample
    // written before `w` is visible here.
    const uint64_t w = inputFrameRingWriteIndex.load(std::memory_order_acquire);
    if (fromIndex >= w) return w;  // nothing new

    constexpr uint64_t kCap  = (uint64_t) kInputFrameRingCapacity;
    constexpr uint64_t kMask = kCap - 1;

    // If the caller fell more than a ring behind, the oldest samples were
    // overwritten — start at the oldest still-live sample. The worker drives
    // this every ~10 ms (~480 samples) so a kCap-sample (170 ms) gap never
    // happens in practice; the clamp just keeps the copy in-bounds.
    uint64_t start = fromIndex;
    if (w - start > kCap) start = w - kCap;

    const size_t n = (size_t) (w - start);
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = inputFrameRing[(start + (uint64_t) i) & kMask].load(std::memory_order_relaxed);
    return w;
}

// ── ML note detection ─────────────────────────────────────────────────────────

namespace
{
juce::String midiNoteName(int midi)
{
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (midi < 0 || midi > 127) return "?";
    return juce::String(names[midi % 12]) + juce::String(midi / 12 - 1);
}
} // namespace

PitchDetector::Detection AudioEngine::getActiveDetection() const
{
    // Audio stopped — neither detector has live data. Return no detection
    // rather than letting the YIN fallback path surface its last stale note
    // for the whole stopped / cold-start window.
    if (! audioRunning.load(std::memory_order_relaxed))
        return {};

    // Prefer the polyphonic ML detector's dominant pitch when a model is
    // loaded; otherwise fall back to the YIN detector. The shape is identical
    // so getPitchDetection's consumers can't tell which detector answered.
    if (mlNoteDetector.isReady())
    {
        const auto note = mlNoteDetector.getDominantNote();
        PitchDetector::Detection d;
        if (note.midi >= 0)
        {
            d.midiNote = note.midi;
            d.confidence = note.confidence;
            d.frequency = 440.0f * std::pow(2.0f, (float) (note.midi - 69) / 12.0f);
            d.cents = 0.0f;  // ML detection is discrete-pitch — no cents estimate
            d.noteName = midiNoteName(note.midi);
        }
        return d;
    }
    return pitchDetector.getLatestDetection();
}

ChordScorer::Result AudioEngine::scoreChordWithMl(const ChordScorer::Request& req) const
{
    ChordScorer::Result out{};
    out.totalStrings = (int) req.notes.size();
    out.results.reserve(req.notes.size());

    // Standard-tuning MIDI base for this (arrangement, stringCount). nullptr
    // for unsupported pairs — every note then fails closed, mirroring the
    // constraint scorer's fail-closed contract.
    const std::vector<int>* base = ChordScorer::standardMidiFor(req.arrangement, req.stringCount);

    // Mirror ChordScorer's request-shape validation (ChordScorer.cpp): a
    // tuningOffsets vector whose length doesn't match stringCount is a
    // malformed request — fail closed (every note a miss) instead of
    // silently substituting zero offsets, which could score real hits
    // where the constraint scorer would have returned all-miss.
    const bool validRequest = base != nullptr
        && (int) req.tuningOffsets.size() == req.stringCount;

    // Mirror ChordScorer exactly (ChordScorer.cpp): a malformed request, or
    // any single out-of-range note, fails the WHOLE chord closed (all-miss) —
    // do not score the sibling notes as hits when one note is invalid.
    bool allValid = validRequest;
    if (allValid)
        for (const auto& n : req.notes)
        {
            if (n.string < 0 || n.string >= req.stringCount || n.fret < 0)
            {
                allValid = false;
                break;
            }
            // Reject a request whose synthesized MIDI falls outside the valid
            // 0..127 range — the detector can't represent it, so it must fail
            // closed like any other malformed note, not be probed downstream.
            const int off = req.tuningOffsets[(size_t) n.string];
            // Sum in 64-bit: base/off/capo/fret arrive from IPC as 32-bit
            // ints, so an int sum could overflow before the range check.
            const long long expectedMidi =
                (long long) (*base)[(size_t) n.string] + off + req.capo + n.fret;
            if (expectedMidi < 0 || expectedMidi > 127)
            {
                allValid = false;
                break;
            }
        }

    if (! allValid)
    {
        for (const auto& n : req.notes)
        {
            ChordScorer::NoteResult r{};
            r.string = n.string;
            r.fret = n.fret;
            r.hasCents = false;
            out.results.push_back(r);  // r.hit defaults to false
        }
        out.hitStrings = 0;
        out.score = 0.0f;
        out.isHit = false;
        return out;
    }

    int hits = 0;
    for (const auto& n : req.notes)
    {
        ChordScorer::NoteResult r{};
        r.string = n.string;
        r.fret = n.fret;
        r.hasCents = false;  // ML judges by pitch-class membership, not cents

        if (validRequest && n.string >= 0
            && n.string < (int) base->size() && n.fret >= 0)
        {
            // Expected MIDI exactly as ChordScorer computes it:
            // base + per-string tuning offset + capo + fret.
            const int off = (n.string < (int) req.tuningOffsets.size())
                                ? req.tuningOffsets[(size_t) n.string] : 0;
            // 64-bit sum to avoid int overflow on malformed IPC values; the
            // value is already validated to 0..127 by the allValid pass above.
            const int expectedMidi = (int) (
                (long long) (*base)[(size_t) n.string] + off + req.capo + n.fret);

            float conf = 0.0f;
            bool active = mlNoteDetector.isPitchActive(expectedMidi, &conf);

            // Bend / slide: the sounding pitch is moving — accept a ±2
            // semitone window around the expected note.
            if (! active && (n.bend || n.slide))
            {
                for (int d = -2; d <= 2 && ! active; ++d)
                    if (d != 0)
                        active = mlNoteDetector.isPitchActive(expectedMidi + d, &conf);
            }
            // Harmonic: the fretted fundamental is suppressed and an overtone
            // sounds — accept the octave or octave+fifth above.
            if (! active && n.harmonic)
                active = mlNoteDetector.isPitchActive(expectedMidi + 12, &conf)
                      || mlNoteDetector.isPitchActive(expectedMidi + 19, &conf);

            r.hit = active;
            r.bandEnergy = conf;  // posteriorgram confidence, 0..1 (energy proxy)
        }

        if (r.hit) ++hits;
        out.results.push_back(r);
    }

    out.hitStrings = hits;
    out.score = out.totalStrings > 0 ? (float) hits / (float) out.totalStrings : 0.0f;
    out.isHit = out.totalStrings > 0 && out.score >= req.minHitRatio;
    return out;
}
