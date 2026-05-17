#include "AudioEngine.h"

#include <cmath>

// On Windows, ASIO drivers can crash with access violations.
// We catch C++ exceptions but can't easily catch SEH in functions with dtors.
// The try/catch blocks around device operations are the best we can do
// without restructuring the code into SEH-safe wrapper functions.

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();

    // Initialize device manager so device types are available for enumeration.
    // This registers ALSA, JACK, CoreAudio, ASIO etc. depending on platform.
    // We don't start audio yet — just make devices queryable.
    auto result = deviceManager.initialiseWithDefaultDevices(2, 2);
    if (result.isNotEmpty())
        std::cerr << "[AudioEngine] init note: " << result.toStdString() << std::endl;

    // Log available device types
    auto& availableTypes = deviceManager.getAvailableDeviceTypes();
    std::cerr << "[AudioEngine] Available device types: " << availableTypes.size() << std::endl;
    for (auto* type : availableTypes)
    {
        type->scanForDevices();
        std::cerr << "[AudioEngine]   " << type->getTypeName().toStdString()
                  << " - inputs: " << type->getDeviceNames(true).size()
                  << ", outputs: " << type->getDeviceNames(false).size() << std::endl;
    }
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

    for (auto* type : deviceManager.getAvailableDeviceTypes())
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
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        for (auto rate : device->getAvailableSampleRates())
            rates.add(rate);
    }
    return rates;
}

juce::Array<int> AudioEngine::getBufferSizes()
{
    juce::Array<int> sizes;
    if (auto* device = deviceManager.getCurrentAudioDevice())
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
    DeviceOptions options;

    juce::AudioIODeviceType* selectedType = nullptr;
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if ((typeName.isNotEmpty() && type->getTypeName() == typeName)
            || (typeName.isEmpty() && selectedType == nullptr))
        {
            selectedType = type;
            if (typeName.isNotEmpty())
                break;
        }
    }

    if (selectedType == nullptr)
    {
        options.error = "Device type not found";
        return options;
    }

    try
    {
        options.type = selectedType->getTypeName();

        auto inputs = selectedType->getDeviceNames(true);
        auto outputs = selectedType->getDeviceNames(false);
        options.input = inputName;
        options.output = outputName;

        if (options.input.isEmpty() && inputs.size() > 0)
            options.input = inputs[0];
        if (options.output.isEmpty() && outputs.size() > 0)
            options.output = outputs[0];

        std::unique_ptr<juce::AudioIODevice> device(
            selectedType->createDevice(options.output, options.input));

        if (!device)
        {
            options.error = "Could not create probe device";
            return options;
        }

        for (auto rate : device->getAvailableSampleRates())
            options.sampleRates.addIfNotAlreadyThere(rate);

        // Buffer sizes are advertised before opening the device, so they are
        // intentionally rate-agnostic. Opening a probe device while audio is
        // running can disrupt some drivers; the real device setup reports the
        // actual accepted block size after Apply.
        const auto advertisedBuffers = device->getAvailableBufferSizes();
        for (auto size : advertisedBuffers)
            options.bufferSizes.addIfNotAlreadyThere(size);

        fprintf(stderr, "[AudioEngine] Probed device options: type='%s' in='%s' out='%s' rates=%d buffers=%d\n",
                options.type.toRawUTF8(), options.input.toRawUTF8(), options.output.toRawUTF8(),
            options.sampleRates.size(), options.bufferSizes.size());
    }
    catch (const std::exception& e)
    {
        options.error = e.what();
    }
    catch (...)
    {
        options.error = "Probe failed";
    }

    return options;
}

juce::String AudioEngine::getCurrentDeviceType()
{
    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
        return type->getTypeName();
    return {};
}

juce::String AudioEngine::getCurrentInputDevice()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        return setup.inputDeviceName;
    }
    return {};
}

juce::String AudioEngine::getCurrentOutputDevice()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        return setup.outputDeviceName;
    }
    return {};
}

double AudioEngine::getLatencyMs() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        int latencySamples = device->getCurrentBufferSizeSamples()
                           + device->getInputLatencyInSamples()
                           + device->getOutputLatencyInSamples();
        return (latencySamples / currentSampleRate.load(std::memory_order_relaxed)) * 1000.0;
    }
    return 0.0;
}

// ── Device Selection ──────────────────────────────────────────────────────────

bool AudioEngine::setDeviceType(const juce::String& typeName)
{
    if (auto* currentType = deviceManager.getCurrentDeviceTypeObject())
    {
        if (currentType->getTypeName() == typeName)
        {
            fprintf(stderr, "[AudioEngine] Device type already selected: %s\n", typeName.toRawUTF8());
            return true;
        }
    }

    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == typeName)
        {
            try {
                fprintf(stderr, "[AudioEngine] Setting device type: %s\n", typeName.toRawUTF8());
                deviceManager.setCurrentAudioDeviceType(typeName, true);
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

bool AudioEngine::setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                                  double sampleRate, int bufferSize)
{
    fprintf(stderr, "[AudioEngine] setAudioDevice: in='%s' out='%s' sr=%.0f bs=%d\n",
            inputName.toRawUTF8(), outputName.toRawUTF8(), sampleRate, bufferSize);

    // Skip if already configured with the same settings
    if (deviceManager.getCurrentAudioDevice() != nullptr)
    {
        juce::AudioDeviceManager::AudioDeviceSetup current;
        deviceManager.getAudioDeviceSetup(current);
        if (current.inputDeviceName == inputName && current.outputDeviceName == outputName
            && current.sampleRate == sampleRate && current.bufferSize == bufferSize)
        {
            fprintf(stderr, "[AudioEngine] Device already configured with same settings, skipping\n");
            return true;
        }
    }

    bool wasRunning = audioRunning.load(std::memory_order_relaxed);
    if (wasRunning) stopAudio();

    // Save current device type name before closing
    juce::String currentTypeName;
    if (auto* currentType = deviceManager.getCurrentDeviceTypeObject())
        currentTypeName = currentType->getTypeName();

    // Close the device completely on Linux to avoid ALSA deadlocks on
    // reconfigure. On Windows, setAudioDeviceSetup can reconfigure in place
    // and closing first makes WASAPI driver changes much slower.
#if JUCE_LINUX
    if (deviceManager.getCurrentAudioDevice() != nullptr)
    {
        try {
            deviceManager.closeAudioDevice();
            fprintf(stderr, "[AudioEngine] Closed device for reconfiguration\n");

            // Re-set the device type so the device list is repopulated
            if (currentTypeName.isNotEmpty())
                deviceManager.setCurrentAudioDeviceType(currentTypeName, true);
        } catch (...) {
            fprintf(stderr, "[AudioEngine] closeAudioDevice crashed, continuing\n");
        }
    }
#endif

    // Initialize if no device type set yet.
    //
    // Linux ordering matters: JUCE typically lists JACK first whenever
    // libjack is installed, even if jackd isn't actually running. Picking
    // JACK in that case makes setCurrentAudioDeviceType block trying to
    // reach a server that doesn't exist. Most home users run PulseAudio /
    // PipeWire over ALSA, so prefer ALSA by default and let pro users
    // who actually run JACK switch in the audio settings UI.
    if (deviceManager.getCurrentDeviceTypeObject() == nullptr)
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

        const auto& available = deviceManager.getAvailableDeviceTypes();
        bool selected = false;

        if (!preferredOrder.isEmpty())
        {
            for (const auto& want : preferredOrder)
            {
                for (auto* type : available)
                {
                    if (type->getTypeName() == want)
                    {
                        deviceManager.setCurrentAudioDeviceType(want, true);
                        selected = true;
                        break;
                    }
                }
                if (selected) break;
            }
        }

        if (!selected && !available.isEmpty())
        {
            // Fallback: take whatever JUCE listed first.
            deviceManager.setCurrentAudioDeviceType(available.getFirst()->getTypeName(), true);
        }
    }

    // If device names are empty, use defaults for the current device type
    juce::String resolvedInput = inputName;
    juce::String resolvedOutput = outputName;
    if (resolvedInput.isEmpty() || resolvedOutput.isEmpty())
    {
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
        {
            auto inputs = type->getDeviceNames(true);
            auto outputs = type->getDeviceNames(false);
            if (resolvedInput.isEmpty() && inputs.size() > 0)
                resolvedInput = inputs[0];
            if (resolvedOutput.isEmpty() && outputs.size() > 0)
                resolvedOutput = outputs[0];
            fprintf(stderr, "[AudioEngine] Resolved empty names: in='%s' out='%s'\n",
                    resolvedInput.toRawUTF8(), resolvedOutput.toRawUTF8());
        }
    }

    // Configure specific devices
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = resolvedInput;
    setup.outputDeviceName = resolvedOutput;
    setup.sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    setup.bufferSize = bufferSize > 0 ? bufferSize : 256;
    setup.inputChannels.setRange(0, 2, true);
    setup.outputChannels.setRange(0, 2, true);
    setup.useDefaultInputChannels = resolvedInput.isEmpty();
    setup.useDefaultOutputChannels = resolvedOutput.isEmpty();

    juce::String result;
    try {
        result = deviceManager.setAudioDeviceSetup(setup, true);
    } catch (...) {
        fprintf(stderr, "[AudioEngine] setAudioDeviceSetup crashed\n");
        return false;
    }
    if (result.isNotEmpty())
    {
        fprintf(stderr, "[AudioEngine] Device setup error: %s\n", result.toRawUTF8());
        try {
            result = deviceManager.initialiseWithDefaultDevices(2, 2);
        } catch (...) {
            fprintf(stderr, "[AudioEngine] Fallback init crashed\n");
            return false;
        }
        if (result.isNotEmpty())
        {
            fprintf(stderr, "[AudioEngine] Fallback init also failed: %s\n", result.toRawUTF8());
            return false;
        }
    }

    if (auto* configuredDevice = deviceManager.getCurrentAudioDevice())
    {
        const double sr = configuredDevice->getCurrentSampleRate();
        const int bs = configuredDevice->getCurrentBufferSizeSamples();
        currentSampleRate.store(sr, std::memory_order_relaxed);
        currentBlockSize.store(bs, std::memory_order_relaxed);

        fprintf(stderr, "[AudioEngine] Device configured OK. Current device: %s\n",
                configuredDevice->getName().toRawUTF8());
        fprintf(stderr, "[AudioEngine] Actual device setup: sr=%.0f bs=%d (requested bs=%d)\n",
                sr, bs, bufferSize);

        signalChain.prepare(sr, bs);
        noiseGate.prepare(sr, bs);
    }
    else
    {
        fprintf(stderr, "[AudioEngine] Device setup completed but no current device is active\n");
        currentSampleRate.store(0.0, std::memory_order_relaxed);
        currentBlockSize.store(0, std::memory_order_relaxed);
        signalChain.releaseResources();
        return false;
    }

    if (wasRunning) startAudio();
    return true;
}

// ── Audio Control ─────────────────────────────────────────────────────────────

void AudioEngine::startAudio()
{
    if (audioRunning.load(std::memory_order_relaxed))
    {
        fprintf(stderr, "[AudioEngine] startAudio: already running\n");
        return;
    }
    deviceManager.addAudioCallback(this);
    audioRunning.store(true, std::memory_order_relaxed);
    fprintf(stderr, "[AudioEngine] startAudio: callback added, running=1, device=%s\n",
            deviceManager.getCurrentAudioDevice() ? deviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "none");
}

void AudioEngine::stopAudio()
{
    if (!audioRunning.load(std::memory_order_relaxed)) return;
    deviceManager.removeAudioCallback(this);
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
    backingSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    backingTransport = std::make_unique<juce::AudioTransportSource>();
    backingTransport->setSource(backingSource.get(), 0, nullptr, readerSampleRate);
    backingTransport->prepareToPlay(currentBlockSize.load(std::memory_order_relaxed),
                                    currentSampleRate.load(std::memory_order_relaxed));
    cachedBackingDuration.store(backingTransport->getLengthInSeconds());
    cachedBackingPosition.store(0.0);
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
        // Read back the actual position; the transport may clamp (e.g. negative or past EOF).
        cachedBackingPosition.store(backingTransport->getCurrentPosition());
    }
}

void AudioEngine::startBacking()
{
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingTransport->start();
        backingPlaying.store(true);
    }
}

void AudioEngine::stopBackingNoLock()
{
    if (backingTransport)
    {
        backingTransport->stop();
        backingPlaying.store(false);
    }
}

void AudioEngine::stopBacking()
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
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

// ── Audio Callback ────────────────────────────────────────────────────────────

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    currentSampleRate.store(sr, std::memory_order_relaxed);
    currentBlockSize.store(bs, std::memory_order_relaxed);

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

    signalChain.prepare(sr, bs);
    pitchDetector.prepare(sr, bs);
    noiseGate.prepare(sr, bs);

    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
        backingTransport->prepareToPlay(bs, sr);
}

void AudioEngine::audioDeviceStopped()
{
    signalChain.releaseResources();
    // Flatten the input ring index on stop so a getInputFrame() call
    // made between stopAudio() and the next startAudio() returns the
    // cold-start zero-padded frame rather than stale samples from the
    // just-finished session.
    inputFrameRingWriteIndex.store(0, std::memory_order_relaxed);
    // Track the actual device lifecycle, not just our intent. JUCE
    // can stop the device externally (hot-unplug, format change, OS
    // sleep), which fires this callback without going through our
    // stopAudio() path — leaving audioRunning stuck at true would
    // keep the audio-bridge's idle gate burning IPC on a dead engine
    // until the user actually clicked Stop.
    audioRunning.store(false, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputData, int numInputChannels,
    float* const* outputData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Work directly in output buffer
    juce::AudioBuffer<float> buffer(outputData, numOutputChannels, numSamples);

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
        for (int outCh = 0; outCh < numOutputChannels; ++outCh)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(outCh, i, inputData[selectedCh][i] * inGain);
        filledOutputChannels = numOutputChannels;
    }
    else if (selectedCh < 0 && numInputChannels > 1)
    {
        // "Both (Mono Mix)": average all input channels and broadcast
        // the result to every output channel, so the signal chain,
        // pitch detector, input-frame ring, and the user's monitoring
        // all see the same mono signal. Previously the else-branch
        // copied channels through 1:1, which delivered stereo to the
        // signal chain and to the user even though the UI label
        // promised a mix; that mismatch surfaced as the renderer's
        // chord scorer and the engine pitch detector seeing different
        // audio from what the signal chain/output produced.
        const float invCh = 1.0f / (float) numInputChannels;
        for (int i = 0; i < numSamples; ++i)
        {
            float mix = 0.0f;
            for (int ch = 0; ch < numInputChannels; ++ch)
                mix += inputData[ch][i];
            const float gained = mix * invCh * inGain;
            for (int outCh = 0; outCh < numOutputChannels; ++outCh)
                buffer.setSample(outCh, i, gained);
        }
        filledOutputChannels = numOutputChannels;
    }
    else
    {
        // Pass-through: single-input device, or stereo in/out with no
        // explicit channel selection and no need to mix.
        const int passThroughChannels = juce::jmin(numInputChannels, numOutputChannels);
        for (int ch = 0; ch < passThroughChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(ch, i, inputData[ch][i] * inGain);
        filledOutputChannels = passThroughChannels;
    }

    // Zero anything we didn't fill. Previously this was hard-coded to
    // start at numInputChannels, which on a 2-in/4-out broadcast
    // config would have wiped the upper two channels we just wrote.
    for (int ch = filledOutputChannels; ch < numOutputChannels; ++ch)
        buffer.clear(ch, 0, numSamples);

    // Metering: input level (pre-processing)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numOutputChannels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
        currentInputLevel.store(peak);
        float prevPeak = inputPeak.load();
        if (peak > prevPeak) inputPeak.store(peak);
    }

    // Feed pitch detector and the input-frame ring (before signal-
    // chain processing, so we detect the dry guitar). In the common
    // case (numOutputChannels > 0) the channel-copy block above
    // guarantees buffer channel 0 holds the right post-gain mono
    // signal for every selection mode, and we read it directly. For
    // input-only configurations (numOutputChannels == 0, rare but
    // legal on some ASIO/JACK setups) the output buffer is empty, so
    // we materialize the same post-gain mono signal from `inputData`
    // into a pre-sized scratch vector and feed both consumers from
    // there.
    const float* monoSource = nullptr;
    if (numOutputChannels > 0)
    {
        monoSource = buffer.getReadPointer(0);
    }
    else if (numInputChannels > 0 && (int) inputCaptureScratch.size() >= numSamples)
    {
        // Build the mono source mirroring the channel-copy semantics:
        // explicit channel select picks one input; -1 with multi-input
        // averages; otherwise input channel 0.
        if (selectedCh >= 0 && selectedCh < numInputChannels)
        {
            for (int i = 0; i < numSamples; ++i)
                inputCaptureScratch[(size_t) i] = inputData[selectedCh][i] * inGain;
        }
        else if (selectedCh < 0 && numInputChannels > 1)
        {
            const float invCh = 1.0f / (float) numInputChannels;
            for (int i = 0; i < numSamples; ++i)
            {
                float mix = 0.0f;
                for (int ch = 0; ch < numInputChannels; ++ch)
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
    // Backing track still plays through.
    if (monitorMuted.load() && !hasProcessors)
        buffer.clear();

    // Chain output gain — the amp/tone's output level. Applied to the guitar
    // signal ONLY, before the backing track is mixed in, so switching tone
    // presets changes the guitar level without touching the song volume.
    buffer.applyGain(chainOutputGain.load());

    // Mix backing track
    {
        const juce::ScopedTryLock sl(backingLock);
        if (sl.isLocked() && backingTransport && backingPlaying.load())
        {
            backingBuffer.setSize(numOutputChannels, numSamples, false, false, true);
            backingBuffer.clear();
            juce::AudioSourceChannelInfo info(&backingBuffer, 0, numSamples);
            backingTransport->getNextAudioBlock(info);

            // Keep cached position and playing state up to date for lock-free polling.
            // backingTransport is non-null (checked above) and backingLock is held for
            // this entire block via ScopedTryLock, so these reads are safe.
            cachedBackingPosition.store(backingTransport->getCurrentPosition());

            // Sync the flag if transport stopped at EOF
            if (!backingTransport->isPlaying())
                backingPlaying.store(false);

            float bVol = backingVolume.load();
            for (int ch = 0; ch < numOutputChannels; ++ch)
                buffer.addFrom(ch, 0, backingBuffer,
                               juce::jmin(ch, backingBuffer.getNumChannels() - 1),
                               0, numSamples, bVol);
        }
    }

    // Apply output gain
    float outGain = outputGain.load();
    buffer.applyGain(outGain);

    // Metering: output level (post-processing)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numOutputChannels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
        currentOutputLevel.store(peak);
        float prevPeak = outputPeak.load();
        if (peak > prevPeak) outputPeak.store(peak);
    }
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
