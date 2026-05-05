#include "AudioEngine.h"

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
        return (latencySamples / currentSampleRate) * 1000.0;
    }
    return 0.0;
}

// ── Device Selection ──────────────────────────────────────────────────────────

bool AudioEngine::setDeviceType(const juce::String& typeName)
{
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

    bool wasRunning = audioRunning;
    if (wasRunning) stopAudio();

    // Save current device type name before closing
    juce::String currentTypeName;
    if (auto* currentType = deviceManager.getCurrentDeviceTypeObject())
        currentTypeName = currentType->getTypeName();

    // Close the device completely to avoid ALSA deadlocks on reconfigure
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

    fprintf(stderr, "[AudioEngine] Device configured OK. Current device: %s\n",
            deviceManager.getCurrentAudioDevice() ? deviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "none");

    signalChain.prepare(
        deviceManager.getCurrentAudioDevice()->getCurrentSampleRate(),
        deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples());

    noiseGate.prepare(deviceManager.getCurrentAudioDevice()->getCurrentSampleRate(),
                      deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples());

    if (wasRunning) startAudio();
    return true;
}

// ── Audio Control ─────────────────────────────────────────────────────────────

void AudioEngine::startAudio()
{
    if (audioRunning) { fprintf(stderr, "[AudioEngine] startAudio: already running\n"); return; }
    deviceManager.addAudioCallback(this);
    audioRunning = true;
    fprintf(stderr, "[AudioEngine] startAudio: callback added, running=%d, device=%s\n",
            audioRunning, deviceManager.getCurrentAudioDevice() ? deviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "none");
}

void AudioEngine::stopAudio()
{
    if (!audioRunning) return;
    deviceManager.removeAudioCallback(this);
    audioRunning = false;
}

// ── Backing Track ─────────────────────────────────────────────────────────────

bool AudioEngine::loadBackingTrack(const juce::File& file)
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
    backingTransport.reset();
    backingSource.reset();

    auto* reader = formatManager.createReaderFor(file);
    if (!reader)
    {
        // Transport/source already reset above; clear cached state so the renderer
        // doesn't keep displaying the previous track's position/duration.
        cachedBackingPosition.store(0.0);
        cachedBackingDuration.store(0.0);
        return false;
    }

    backingSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    backingTransport = std::make_unique<juce::AudioTransportSource>();
    backingTransport->setSource(backingSource.get(), 0, nullptr, reader->sampleRate);
    backingTransport->prepareToPlay(currentBlockSize, currentSampleRate);
    cachedBackingDuration.store(backingTransport->getLengthInSeconds());
    cachedBackingPosition.store(0.0);
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

void AudioEngine::setNoiseGate(bool enabled,
                               float thresholdLinear,
                               int holdSamples,
                               float attack,
                               float release)
{
    noiseGate.setParameters(enabled, thresholdLinear, holdSamples, attack, release);
}

// ── Audio Callback ────────────────────────────────────────────────────────────

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    currentSampleRate = device->getCurrentSampleRate();
    currentBlockSize = device->getCurrentBufferSizeSamples();

    signalChain.prepare(currentSampleRate, currentBlockSize);
    pitchDetector.prepare(currentSampleRate, currentBlockSize);
    noiseGate.prepare(currentSampleRate, currentBlockSize);

    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
        backingTransport->prepareToPlay(currentBlockSize, currentSampleRate);
}

void AudioEngine::audioDeviceStopped()
{
    signalChain.releaseResources();
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

    // Copy input with gain, handling channel selection
    if (numInputChannels >= 2 && selectedCh >= 0 && selectedCh < numInputChannels)
    {
        // Single channel mode (e.g., dry from Valeton GP-5 left channel)
        for (int outCh = 0; outCh < numOutputChannels; ++outCh)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(outCh, i, inputData[selectedCh][i] * inGain);
    }
    else
    {
        // Normal stereo or mono mix
        for (int ch = 0; ch < juce::jmin(numInputChannels, numOutputChannels); ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(ch, i, inputData[ch][i] * inGain);
    }

    // Zero extra output channels
    for (int ch = numInputChannels; ch < numOutputChannels; ++ch)
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

    // Feed pitch detector (before processing so we detect the dry guitar signal)
    if (numOutputChannels > 0)
        pitchDetector.pushSamples(buffer.getReadPointer(0), numSamples);

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
