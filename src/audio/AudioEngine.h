#pragma once
#include "NoiseGate.h"
#include "TonePolish.h"
#include "SignalChain.h"
#include "PitchDetector.h"
#include "ChordScorer.h"
#include "MlNoteDetector.h"
#include "NoteVerifier.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    SignalChain& getSignalChain() { return signalChain; }
    PitchDetector& getPitchDetector() { return pitchDetector; }
    MlNoteDetector& getMlNoteDetector() { return mlNoteDetector; }

    // Load the Basic Pitch ONNX model for the polyphonic ML detector. When a
    // model is loaded, getActiveDetection() / scoreChord() route through it;
    // otherwise they fall back to the YIN PitchDetector / ChordScorer.
    bool loadNoteModel(const juce::File& modelFile) { return mlNoteDetector.loadModel(modelFile); }
    bool hasMlNoteDetector() const { return mlNoteDetector.isAvailable(); }

    // Best current single-note detection: the ML detector's dominant pitch
    // when a model is loaded, else the YIN detector's latest result. Shape is
    // identical either way so the getPitchDetection bridge is detector-agnostic.
    PitchDetector::Detection getActiveDetection() const;

    // Device enumeration
    struct DeviceTypeInfo
    {
        juce::String name;
        juce::StringArray inputDevices;
        juce::StringArray outputDevices;
    };
    struct DeviceOptions
    {
        juce::String type;
        juce::String input;
        juce::String output;
        juce::Array<double> sampleRates;
        juce::Array<int> bufferSizes;
        juce::String error;
    };
    juce::Array<DeviceTypeInfo> getDeviceTypes();
    juce::Array<double> getSampleRates();
    juce::Array<int> getBufferSizes();
    DeviceOptions probeDeviceOptions(const juce::String& typeName,
                                     const juce::String& inputName,
                                     const juce::String& outputName);
    juce::String getCurrentDeviceType();
    juce::String getCurrentInputDevice();
    juce::String getCurrentOutputDevice();
    double getCurrentSampleRate() const { return currentSampleRate.load(std::memory_order_relaxed); }
    int getCurrentBlockSize() const { return currentBlockSize.load(std::memory_order_relaxed); }

    // Device selection
    bool setDeviceType(const juce::String& typeName);
    bool setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                        double sampleRate = 48000.0, int bufferSize = 256);

    // Audio start/stop
    void startAudio();
    void stopAudio();
    bool isAudioRunning() const { return audioRunning.load(std::memory_order_relaxed); }

    // Gain controls
    void setInputGain(float gain) { inputGain.store(gain); }
    void setOutputGain(float gain) { outputGain.store(gain); }
    float getInputGain() const { return inputGain.load(); }
    float getOutputGain() const { return outputGain.load(); }

    // Chain output gain — the amp/tone's output level, applied to the guitar
    // signal before the backing track is mixed. Distinct from outputGain (the
    // post-mix master) so a tone-preset switch doesn't move the song volume.
    void setChainOutputGain(float gain) { chainOutputGain.store(gain); }
    float getChainOutputGain() const { return chainOutputGain.load(); }

    // Input channel selection (for multi-channel interfaces like Valeton GP-5)
    // 0=left (dry), 1=right (wet), -1=both (mono mix)
    void setInputChannel(int channel) { selectedInputChannel.store(channel); }
    int getInputChannel() const { return selectedInputChannel.load(); }

    // Monitor mute — when true, input is still processed (pitch detection, metering)
    // but output is silenced unless there are processors in the signal chain
    void setMonitorMute(bool mute) { monitorMuted.store(mute); }
    bool isMonitorMuted() const { return monitorMuted.load(); }

    // Monitor-mute suppression — when true, the monitor mute is temporarily
    // overridden so the dry guitar stays audible even with an empty chain.
    // The renderer sets this around a song-load chain rebuild (clear + reload),
    // so the brief empty-chain window doesn't silence the player's guitar.
    void setMonitorMuteSuppressed(bool suppressed) { monitorMuteSuppressed.store(suppressed); }
    bool isMonitorMuteSuppressed() const { return monitorMuteSuppressed.load(); }

    // Noise gate (post-input-gain, pre FX chain; pitch detector sees ungated signal)
    void setNoiseGate(bool enabled, float thresholdDb, float releaseMs, float depthDb);

    // Tone Polish — fixed 3-band mastering EQ (HPF 80 Hz, low shelf -3 dB
    // @ 180 Hz, peak -0.5 dB @ 200 Hz Q=1). Applied on the guitar bus only,
    // between chainOutputGain and the backing-track mix, so the backing
    // track and master output gain stay bit-untouched. Defaults on;
    // renderer exposes a per-preset toggle.
    void setTonePolishEnabled(bool enabled);

    // Backing track
    void setBackingVolume(float vol) { backingVolume.store(vol); }
    bool loadBackingTrack(const juce::File& file);
    void setBackingPosition(double seconds);
    void startBacking();
    void stopBacking();
    // Non-blocking reads — do not acquire backingLock and never block the audio callback
    bool isBackingPlaying() const { return backingPlaying.load(); }
    double getBackingPosition() const { return cachedBackingPosition.load(); }
    double getBackingDuration() const { return cachedBackingDuration.load(); }

    // Metering (read from any thread — atomic)
    float getInputLevel() const { return currentInputLevel.load(); }
    float getOutputLevel() const { return currentOutputLevel.load(); }
    float getInputPeak() const { return inputPeak.load(); }
    float getOutputPeak() const { return outputPeak.load(); }
    void resetPeaks();

    // Latency
    double getLatencyMs() const;

    // Raw input frame snapshot for renderer-side polyphonic chord scoring
    // in notedetect. The audio callback appends the post-input-gain mono
    // signal (same one fed to the pitch detector) into a lock-free ring;
    // callers on the main thread can copy out the most-recent N samples.
    // Capacity is a power of two so audio-thread wrap is a single mask.
    static constexpr int kInputFrameRingCapacity = 8192;
    // Capacity must stay a power of two — the audio-thread store relies
    // on `(write_index + i) & (capacity - 1)` for wraparound, which is
    // only equivalent to modulo for powers of two. A static_assert keeps
    // a future "let's bump the buffer to 10000" patch from silently
    // turning the index into a wrong-direction offset.
    static_assert((kInputFrameRingCapacity & (kInputFrameRingCapacity - 1)) == 0,
                  "kInputFrameRingCapacity must be a power of two");
    // Default snapshot size matches notedetect's _ND_MIN_YIN_SAMPLES (4096
    // samples — enough for low-E autocorrelation at 48 kHz). Caller can
    // request fewer; anything larger than the ring capacity gets clamped.
    std::vector<float> getInputFrame(int numSamples = 4096) const;

    // Gapless input-ring consumption for the onset detector. Copies every
    // sample written since monotonic index `fromIndex` into `out`, and
    // returns the current write index. The samples in `out` span monotonic
    // indices [returnedWriteIndex - out.size(), returnedWriteIndex); when no
    // samples were lost that lower bound equals `fromIndex`. If the gap
    // exceeds the ring capacity the oldest samples are gone and `out` starts
    // later than `fromIndex` (out.size() < writeIndex - fromIndex) — the
    // caller detects the loss by that shortfall. Unlike getInputFrame (which
    // returns overlapping most-recent-N snapshots), consecutive calls here
    // consume each sample exactly once.
    uint64_t getInputSince(uint64_t fromIndex, std::vector<float>& out) const;

    // Score a chord against the latest input-ring samples. The chord
    // context (notes, arrangement, thresholds) comes from the renderer
    // over IPC; audio data stays inside the engine so no buffers cross
    // the N-API boundary. Returns the same `{score, hitStrings,
    // totalStrings, isHit, results[]}` shape as the JS implementation.
    ChordScorer::Result scoreChord(const ChordScorer::Request& req);

    // Continuous engine-side chart verification (notedetect). The renderer
    // pushes the song's note chart once via setChart(); a background
    // NoteVerifier thread scores each note's timing window against the live
    // playhead and input ring, and the renderer drains finalized verdicts
    // via getNoteVerdicts(). This replaces the renderer's per-tick
    // scoreChord IPC loop, which starved during dense passages.
    void setChart(const NoteVerifier::ChartUpdate& chart) { noteVerifier.setChart(chart); }
    void clearChart() { noteVerifier.clearChart(); }
    std::vector<NoteVerifier::Verdict> getNoteVerdicts() { return noteVerifier.drainVerdicts(); }

    // Renderer's unified, already-corrected playhead — the verifier scores
    // against this rather than getBackingPosition(), which is frozen for
    // HTML5-routed (sloppak) songs. Pushed each detect tick via getNoteVerdicts.
    void setPlayhead(double songTime, bool playing) { noteVerifier.setPlayhead(songTime, playing); }

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputData,
                                          int numInputChannels,
                                          float* const* outputData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void stopBackingNoLock(); // stop transport without acquiring backingLock (caller holds it)

    // ML-backed chord scoring against the MlNoteDetector's active-pitch set.
    // Used by scoreChord() when a Basic Pitch model is loaded.
    ChordScorer::Result scoreChordWithMl(const ChordScorer::Request& req) const;

    juce::AudioDeviceManager deviceManager;
    SignalChain signalChain;
    PitchDetector pitchDetector;
    MlNoteDetector mlNoteDetector;
    NoiseGate noiseGate;
    TonePolish tonePolish;
    ChordScorer chordScorer;
    // Background chart-verification thread. Constructed with `*this` so it can
    // read the engine's input ring (getInputFrame) and playhead
    // (getBackingPosition); valid because the reference is bound during
    // AudioEngine construction and the thread is only started in prepare().
    NoteVerifier noteVerifier{ *this };
    juce::AudioFormatManager formatManager;

    std::atomic<float> inputGain{1.0f};
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> chainOutputGain{1.0f};
    std::atomic<float> backingVolume{0.7f};
    std::atomic<float> currentInputLevel{0.0f};
    std::atomic<float> currentOutputLevel{0.0f};
    std::atomic<float> inputPeak{0.0f};
    std::atomic<float> outputPeak{0.0f};
    std::atomic<int> selectedInputChannel{-1}; // -1 = mono mix
    std::atomic<bool> monitorMuted{true}; // mute pass-through by default
    std::atomic<bool> monitorMuteSuppressed{false}; // overrides monitorMuted during chain rebuilds

    // Backing track
    std::unique_ptr<juce::AudioFormatReaderSource> backingSource;
    std::unique_ptr<juce::AudioTransportSource> backingTransport;
    juce::AudioBuffer<float> backingBuffer;
    std::atomic<bool> backingPlaying{false};
    std::atomic<double> cachedBackingPosition{0.0};
    std::atomic<double> cachedBackingDuration{0.0};
    juce::CriticalSection backingLock;

    // Toggled from startAudio()/stopAudio() (main / device-management
    // threads) and read from isAudioRunning() on the JS thread via the
    // audio-bridge dispatch loop. Plain bool would be a data race;
    // relaxed-atomic is well-defined and compiles to a plain MOV.
    std::atomic<bool> audioRunning{false};
    // Sample rate is written from the JUCE device callbacks (audio
    // thread / device-management thread) and read from arbitrary
    // callers including the JS thread via getCurrentSampleRate(),
    // so a plain double would be a C++ data race. std::atomic<double>
    // is well-defined and lock-free on the platforms we ship; the
    // hot reads use relaxed since the consumer just wants the latest
    // observable value, not a synchronization point.
    std::atomic<double> currentSampleRate{48000.0};
    // Block size has the same race shape as sample rate — written from
    // device callbacks, read from getLatencyMs() / loadBackingTrack()
    // on the JS/management thread. Atomic for the same reason.
    std::atomic<int> currentBlockSize{256};

    // Lock-free SPSC ring buffer for raw mono input. Single producer is
    // the audio thread (audioDeviceIOCallbackWithContext); single consumer
    // is the main thread via getInputFrame(). Capacity is a power of two
    // so the audio-thread store can mask instead of modulo. The write
    // index is monotonically increasing in samples while audio is
    // running, and is reset to 0 on audioDeviceAboutToStart() and
    // audioDeviceStopped() so a stop→start cycle delivers a clean
    // cold-start frame instead of mixing in stale samples from the
    // previous run. Within a single run uint64 covers >12 million years
    // at 48 kHz before wrap, so the wraparound case is unreachable in
    // practice between lifecycle resets.
    //
    // Each slot is std::atomic<float> with relaxed loads/stores: plain
    // float concurrent access would be a data race (undefined behavior)
    // when the writer laps mid-snapshot, even though we're prepared to
    // tolerate stale data mathematically. Relaxed-ordered access on a
    // 4-byte type compiles to a plain MOV on x86 and a non-fenced load/
    // store on AArch64, so the audio-thread cost is the same as the
    // unsynchronised version while the C++ memory model now permits the
    // race. Reader still tolerates seeing a few of the oldest samples
    // from a newer write (worst case ~6% of a 4096-sample snapshot at
    // 256-sample blocks); the YIN/HPS chord-scoring math is well below
    // that sensitivity.
    std::array<std::atomic<float>, kInputFrameRingCapacity> inputFrameRing{};
    std::atomic<uint64_t> inputFrameRingWriteIndex{0};

    // Audio-thread scratch buffer used only on zero-output device
    // configurations (input-only ASIO, certain JACK setups). In the
    // common case where numOutputChannels > 0, the post-copy buffer's
    // channel 0 already holds the right mono signal and we read it
    // directly. With zero outputs the buffer is empty, so we need
    // somewhere to materialize the post-gain mono source for the
    // pitch detector and ring. Pre-sized in audioDeviceAboutToStart()
    // so the hot loop never allocates.
    std::vector<float> inputCaptureScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
