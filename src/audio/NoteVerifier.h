#pragma once

// NoteVerifier — continuous, engine-side scoring of a chart's guitar notes.
//
// The notedetect plugin used to run a renderer `setInterval` loop that called
// `audio.scoreChord` over IPC on every tick. During dense passages the renderer
// event loop is starved and the loop blacks out for 1-3 s — whole runs of notes
// are never scored. NoteVerifier moves that work into the engine: a background
// `juce::Thread` walks the pushed chart against the live playhead and scores
// each note's timing window exactly once, publishing a verdict the renderer
// just drains.
//
// Threading contract mirrors MlNoteDetector: a background thread does the work
// and publishes a snapshot guarded by a juce::CriticalSection; any non-audio
// thread (the N-API thread) polls it via drainVerdicts(). The thread reads the
// engine's input ring through AudioEngine::getInputFrame() (already designed
// for off-audio-thread reads). It never touches the audio thread directly.
//
// Playhead: the worker does NOT use AudioEngine::getBackingPosition() — that
// only advances for JUCE-routed songs, and stem-based (sloppak) CDLC always
// plays on the renderer's HTML5 <audio>, leaving the backing transport frozen.
// Instead the renderer pushes its own unified, already-corrected playhead via
// setPlayhead() each detect tick; the worker interpolates between pushes.
//
// Scoring: a note is a HIT when the harmonic-comb confirms its pitch present
// anywhere in its timing window (presence). Timing is taken from a separate
// spectral-flux OnsetDetector — for a picked note the nearest detected pick
// attack gives a precise strike time; legato (hammer-on / pull-off) notes have
// no attack, so they are reported on-time. The comb's 85 ms window is far too
// coarse to time an onset, hence the dedicated detector.

#include <juce_core/juce_core.h>
#include "ChordScorer.h"
#include "OnsetDetector.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class AudioEngine;  // resolved in NoteVerifier.cpp — avoids a circular include

class NoteVerifier
{
public:
    explicit NoteVerifier(AudioEngine& ownerEngine);
    ~NoteVerifier();

    // One chart note plus the technique flags ChordScorer consumes.
    struct ChartNote
    {
        std::string id;
        double t = 0.0;     // chart time (seconds) of the note onset
        int string = 0;
        int fret = 0;
        double sus = 0.0;   // sustain length (seconds)
        bool ho = false, po = false, b = false, sl = false, hm = false;
    };

    // Chart context — the per-song scoring parameters. Mirrors the fields the
    // plugin previously passed into `audio.scoreChord` per tick.
    struct ChartUpdate
    {
        std::string arrangement = "guitar";
        int stringCount = 6;
        std::vector<int> tuningOffsets;
        int capo = 0;
        float pitchCheckCents = 0.0f;
        float harmonicSnr = 3.0f;
        double timingTolerance = 0.1; // seconds — half-width of the scoring window
        std::vector<ChartNote> notes;
    };

    // A finalized per-note verdict, drained by the renderer.
    struct Verdict
    {
        std::string id;
        bool detected = false;
        double detectedSongTime = 0.0; // playhead at which the note was scored
        float centsError = 0.0f;
        float snr = 0.0f;
    };

    // Replace the chart + context, resetting all finalized state. Thread-safe.
    void setChart(const ChartUpdate& update);

    // Empty the chart — no notes are scored until the next setChart().
    void clearChart();

    // Verdicts finalized since the last drain; clears the pending buffer.
    std::vector<Verdict> drainVerdicts();

    // Receive the renderer's unified playhead. `songTime` is already
    // avOffset/latency-corrected — the same clock the plugin correlates chart
    // note times against. Safe from the N-API thread; the timing trio is
    // published under `lock` so the worker reads a coherent snapshot.
    void setPlayhead(double songTime, bool playing);

    // Start / stop the background thread — matches MlNoteDetector's lifecycle.
    void prepare(double sampleRate, int blockSize);
    void stop();

private:
    void run();

    // The playhead the worker should score against right now: the last pushed
    // song time, advanced by wall-clock elapsed since the push while playing.
    // Freezes on a stale push (renderer tick stopped) or when paused.
    double currentPlayhead() const;

    // Per-note finalized state, parallel to `chart`. The harmonic-comb scores
    // every open-window note each tick; a note that is ever confirmed present
    // is a hit (timing then comes from the OnsetDetector, or the chart time
    // for legato). A note never present is a miss.
    struct NoteState
    {
        bool finalized = false;
        bool detected = false;
        double detectedSongTime = 0.0;
        float centsError = 0.0f;
        float snr = 0.0f;
        bool everPresent = false;  // comb confirmed pitch present sometime in-window
        float bestSnr = 0.0f;      // strongest SNR among present ticks
        float bestCents = 0.0f;    // cents error at the strongest present tick
    };

    AudioEngine& engine;
    ChordScorer chordScorer;

    // Background worker. Defined in the .cpp so this header stays free of the
    // juce::Thread subclass.
    struct Worker;
    std::unique_ptr<Worker> worker;

    // Spectral-flux onset detection — worker-thread only. `readCursor` is the
    // monotonic input-ring index consumed so far; `onsetLog` holds recent pick
    // attacks tagged in song time, claimed one-per-note at finalization.
    OnsetDetector onsetDetector;
    uint64_t readCursor = 0;
    struct LoggedOnset { double songTime = 0.0; bool claimed = false; };
    std::deque<LoggedOnset> onsetLog;
    // setChart() (N-API thread) sets this; run() clears the stale onset log on
    // the next tick. The log itself stays worker-thread only.
    std::atomic<bool> onsetResetPending { false };

    // Chart + context + per-note state, all guarded by `lock`. The worker
    // thread mutates `state` and `pending`; setChart()/clearChart()/drain()
    // run on the N-API thread. Neither side is the audio thread.
    juce::CriticalSection lock;
    ChartUpdate chart;
    // Bumped on every setChart()/clearChart(). run() snapshots it with the
    // chart and re-checks it before applying verdicts — a note-count match is
    // not enough, a same-size chart swap must still invalidate the snapshot.
    uint64_t chartEpoch = 0;
    std::vector<NoteState> state;
    std::vector<Verdict> pending;  // verdicts finalized since the last drain

    // Renderer-pushed playhead. The timing trio is written by setPlayhead()
    // (N-API thread) and read by currentPlayhead() (worker thread) — both
    // under `lock`, as one coherent snapshot, so a fresh songTime can never
    // be paired with a stale receiptMs/playing from an earlier push.
    double pushedSongTime = 0.0;
    double pushedReceiptMs = 0.0;  // getMillisecondCounterHiRes() at push
    bool   pushedPlaying = false;
    // One-way latch (false→true on first push, reset by setChart). Read
    // lock-free at the top of run(); a slightly stale read is harmless.
    std::atomic<bool> havePushedPlayhead { false };

    // Worker-thread only — last playhead seen, for backward-jump detection.
    double lastPlayhead = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteVerifier)
};
