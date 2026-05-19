#include "NoteVerifier.h"
#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
// The plugin's MAX_SUS_LATE_GRACE — a sustained note's scoring window may run
// past its onset by up to its sustain length, capped at 1 s, so a held note is
// still verified while it rings.
double susGraceFor(double sus)
{
    return std::min(std::max(sus, 0.0), 1.0);
}

// A push older than this means the renderer's detect tick has stopped (plugin
// disabled, song unloaded, renderer wedged) — freeze the playhead rather than
// let interpolation run a clock away from reality. ~20 missed 50 ms ticks.
constexpr double kPlayheadStaleMs = 1000.0;

// A playhead that drops by more than this is a drill A-B loop wrap or a manual
// seek-back, not push-to-push interpolation jitter (bounded by the ~50 ms
// tick). Notes at/after the new position are re-opened for re-scoring.
constexpr double kBackwardJumpSeconds = 0.25;
} // namespace

// ── Background scoring thread ────────────────────────────────────────────────
struct NoteVerifier::Worker : public juce::Thread
{
    explicit Worker(NoteVerifier& ownerVerifier)
        : Thread("NoteVerifier"), owner(ownerVerifier) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            owner.run();
            sleep(10);
        }
    }

    NoteVerifier& owner;
};

// ── NoteVerifier ─────────────────────────────────────────────────────────────
NoteVerifier::NoteVerifier(AudioEngine& ownerEngine)
    : engine(ownerEngine) {}

NoteVerifier::~NoteVerifier() { stop(); }

void NoteVerifier::setChart(const ChartUpdate& update)
{
    const juce::ScopedLock sl(lock);
    ++chartEpoch;
    chart = update;
    // Fresh per-note state — nothing is finalized yet for the new chart.
    state.assign(chart.notes.size(), NoteState{});
    pending.clear();
    // A new chart means a new song/arrangement: drop the old playhead so the
    // worker waits for the renderer's first push rather than scoring against a
    // stale position. lastPlayhead is worker-only but guarded by `lock` here.
    havePushedPlayhead.store(false);
    pushedPlaying = false;  // plain bool, guarded by `lock` (held here)
    lastPlayhead = 0.0;
    // The song-time-tagged onset log is stale for the new chart; run() drops
    // it on its next tick (the log itself is worker-thread only).
    onsetResetPending.store(true);
}

void NoteVerifier::clearChart()
{
    const juce::ScopedLock sl(lock);
    ++chartEpoch;
    chart.notes.clear();
    state.clear();
    pending.clear();
    // Reset the playhead state too — same as setChart(). Otherwise a
    // clearChart() between songs leaves a stale lastPlayhead/havePushedPlayhead
    // for the worker to score the next setPlayhead() against.
    havePushedPlayhead.store(false);
    pushedPlaying = false;
    lastPlayhead = 0.0;
    onsetResetPending.store(true);
}

std::vector<NoteVerifier::Verdict> NoteVerifier::drainVerdicts()
{
    const juce::ScopedLock sl(lock);
    std::vector<Verdict> out;
    out.swap(pending);
    return out;
}

void NoteVerifier::setPlayhead(double songTime, bool playing)
{
    {
        // Publish the timing trio as one snapshot under `lock` so
        // currentPlayhead() can never read a half-updated set.
        const juce::ScopedLock sl(lock);
        pushedReceiptMs = juce::Time::getMillisecondCounterHiRes();
        pushedSongTime = songTime;
        pushedPlaying = playing;
    }
    havePushedPlayhead.store(true);
}

double NoteVerifier::currentPlayhead() const
{
    double base, receiptMs;
    bool playing;
    {
        const juce::ScopedLock sl(lock);
        base      = pushedSongTime;
        receiptMs = pushedReceiptMs;
        playing   = pushedPlaying;
    }
    const double ageMs = juce::Time::getMillisecondCounterHiRes() - receiptMs;
    if (ageMs > kPlayheadStaleMs) return base;   // tick stopped — freeze
    if (! playing)                return base;   // paused — hold
    return base + ageMs / 1000.0;                // playing — interpolate forward
    // Note: after a stale freeze the playhead jumps forward on the first
    // fresh push; any note whose whole window was traversed during the freeze
    // finalizes as a miss next tick — correct, detection genuinely was down.
}

void NoteVerifier::prepare(double sampleRate, int /*blockSize*/)
{
    // Match MlNoteDetector: a stop→start cycle tears the thread down before
    // restarting so a device restart can't race the worker against state.
    stop();
    // The worker is not running here — configure the onset state directly.
    onsetDetector.prepare(sampleRate);
    readCursor = 0;
    onsetLog.clear();
    worker = std::make_unique<Worker>(*this);
    worker->startThread(juce::Thread::Priority::normal);
}

void NoteVerifier::stop()
{
    if (worker)
    {
        // stopThread() signals, waits, and only force-kills on timeout — after
        // it returns the thread is definitively not running, so resetting the
        // unique_ptr can't destroy a live juce::Thread.
        worker->stopThread(2000);
        worker.reset();
    }
}

void NoteVerifier::run()
{
    // Score nothing until the renderer has pushed a playhead at least once —
    // otherwise the loop below would finalize the whole chart against
    // playhead 0 the instant a chart is set.
    if (! havePushedPlayhead.load()) return;

    // A fresh chart (new song/arrangement) invalidates the song-time-tagged
    // onset log. The flux detector itself keeps running — the input stream is
    // continuous regardless of song position.
    if (onsetResetPending.exchange(false))
        onsetLog.clear();

    // The playhead for this whole pass — interpolated from the last push so
    // every note here is judged against the same chart position.
    const double playhead = currentPlayhead();

    double sr = engine.getCurrentSampleRate();
    if (! std::isfinite(sr) || sr <= 0.0) sr = 48000.0;

    // ── Onset detection ─────────────────────────────────────────────────────
    // Feed every input sample captured since the last tick into the flux
    // detector, and log each detected pick attack in song time. Input-ring
    // index `w` ("now") corresponds to `playhead`; an earlier sample is
    // proportionally earlier.
    {
        std::vector<float> fresh;
        const uint64_t w = engine.getInputSince(readCursor, fresh);
        readCursor = w;
        if (! fresh.empty())
        {
            const uint64_t firstIdx = w - (uint64_t) fresh.size();
            std::vector<OnsetDetector::Onset> onsets;
            onsetDetector.process(fresh.data(), fresh.size(), firstIdx, onsets);
            for (const auto& o : onsets)
            {
                const double songT = playhead - (double) (w - o.sampleIndex) / sr;
                onsetLog.push_back({ songT, false });
            }
        }
    }
    // Drop onsets far behind the playhead — no note window still reaches them.
    while (! onsetLog.empty() && onsetLog.front().songTime < playhead - 4.0)
        onsetLog.pop_front();

    // ── Snapshot the chart + collect this tick's open / passed notes ────────
    struct Candidate { size_t index; ChordScorer::Note note; };
    std::vector<Candidate> batch;       // notes whose window is open — score now
    std::vector<size_t>    passedIdx;   // notes whose window has fully passed
    bool backwardJump = false;

    ChartUpdate ctx;
    uint64_t snapshotEpoch = 0;
    {
        const juce::ScopedLock sl(lock);
        snapshotEpoch = chartEpoch;
        // Copy only the scalar scoring context — NOT chart.notes. The notes
        // vector is large and is never read off-lock (the open-window batch is
        // assembled under this lock below), so deep-copying it on every ~10 ms
        // worker tick would be wasted allocation under a contended lock.
        ctx.arrangement     = chart.arrangement;
        ctx.stringCount     = chart.stringCount;
        ctx.tuningOffsets   = chart.tuningOffsets;
        ctx.capo            = chart.capo;
        ctx.pitchCheckCents = chart.pitchCheckCents;
        ctx.harmonicSnr     = chart.harmonicSnr;
        ctx.timingTolerance = chart.timingTolerance;

        // A drill A-B loop wrap (or a manual seek-back) jumps the playhead
        // backward. Re-open every note at/after the new position so the next
        // pass re-scores it; notes before the loop point keep their verdict.
        if (playhead < lastPlayhead - kBackwardJumpSeconds)
        {
            backwardJump = true;
            for (size_t i = 0; i < chart.notes.size() && i < state.size(); ++i)
                if (chart.notes[i].t >= playhead)
                    state[i] = NoteState{};
            pending.erase(std::remove_if(pending.begin(), pending.end(),
                [&](const Verdict& v)
                {
                    for (size_t i = 0; i < chart.notes.size() && i < state.size(); ++i)
                        if (chart.notes[i].id == v.id) return ! state[i].finalized;
                    return false;
                }), pending.end());
        }
        lastPlayhead = playhead;

        const double tol = chart.timingTolerance;
        for (size_t i = 0; i < chart.notes.size(); ++i)
        {
            if (i >= state.size() || state[i].finalized) continue;
            const auto& cn = chart.notes[i];
            const double grace = susGraceFor(cn.sus);

            if (playhead > cn.t + tol + grace)
                passedIdx.push_back(i);
            // Sustain grace extends only the LATE edge — a note rings *after*
            // its onset, never before. The early edge is the plain timing
            // tolerance; widening it by `grace` would open a long sustain up
            // to a second early and let a same-pitch note still ringing from
            // an earlier strike set everPresent → a false hit.
            else if (playhead >= cn.t - tol)
            {
                ChordScorer::Note n{};
                n.string = cn.string;
                n.fret = cn.fret;
                n.hammerOn = cn.ho;
                n.pullOff = cn.po;
                n.bend = cn.b;
                n.slide = cn.sl;
                n.harmonic = cn.hm;
                batch.push_back({ i, n });
            }
        }
    }

    // A backward jump stranded the logged onsets in the old song-time range.
    if (backwardJump)
        onsetLog.clear();

    // ── Harmonic-comb presence pass ─────────────────────────────────────────
    // The comb only answers "is this note's pitch present?" — timing comes
    // from the onset log. Score every open-window note against the latest
    // input frame.
    struct ScoredNote { size_t index; bool present; float centsError; float snr; };
    std::vector<ScoredNote> scored;

    if (! batch.empty())
    {
        ChordScorer::Request req;
        req.numSamples = 4096;
        req.arrangement = ctx.arrangement;
        req.stringCount = ctx.stringCount;
        req.tuningOffsets = ctx.tuningOffsets;
        req.capo = ctx.capo;
        req.pitchCheckCents = ctx.pitchCheckCents;
        req.harmonicVerify = true;          // Rocksmith-style targeted check
        req.harmonicSnr = ctx.harmonicSnr;
        req.notes.reserve(batch.size());
        for (const auto& c : batch)
            req.notes.push_back(c.note);

        const auto frame = engine.getInputFrame(4096);
        const auto result = chordScorer.scoreChord(frame.data(), (int) frame.size(),
                                                   sr, req);

        const size_t n = std::min(result.results.size(), batch.size());
        for (size_t i = 0; i < n; ++i)
        {
            ScoredNote s;
            s.index = batch[i].index;
            s.present = result.results[i].hit;
            s.centsError = result.results[i].centsError;
            s.snr = result.results[i].bandEnergy;  // harmonicVerify puts SNR here
            scored.push_back(s);
        }
    }

    // ── Update presence, then finalize passed notes ─────────────────────────
    {
        const juce::ScopedLock sl(lock);
        // A setChart()/clearChart() since the snapshot — even one that kept the
        // same note count — invalidates `state`/`passedIdx`; drop this pass.
        if (chartEpoch != snapshotEpoch) return;

        for (const auto& s : scored)
        {
            if (s.index >= state.size()) continue;
            NoteState& st = state[s.index];
            if (st.finalized || ! s.present) continue;
            st.everPresent = true;
            if (s.snr > st.bestSnr) { st.bestSnr = s.snr; st.bestCents = s.centsError; }
        }

        const double tol = ctx.timingTolerance;
        for (size_t idx : passedIdx)
        {
            if (idx >= state.size() || state[idx].finalized) continue;
            NoteState& st = state[idx];
            const auto& cn = chart.notes[idx];
            st.finalized = true;

            if (st.everPresent)
            {
                // The comb confirmed this note's pitch in its window — a hit.
                // Timing: a picked note claims the nearest unclaimed pick
                // attack from the onset log; legato (hammer-on / pull-off) has
                // no attack, so it is reported on-time.
                st.detected = true;
                st.centsError = st.bestCents;
                st.snr = st.bestSnr;

                double when = cn.t;
                if (! cn.ho && ! cn.po)
                {
                    int best = -1;
                    double bestDist = tol;
                    for (size_t k = 0; k < onsetLog.size(); ++k)
                    {
                        if (onsetLog[k].claimed) continue;
                        const double d = std::abs(onsetLog[k].songTime - cn.t);
                        if (d <= bestDist) { bestDist = d; best = (int) k; }
                    }
                    if (best >= 0)
                    {
                        onsetLog[(size_t) best].claimed = true;
                        when = onsetLog[(size_t) best].songTime;
                    }
                }
                st.detectedSongTime = when;

                Verdict v;
                v.id = cn.id;
                v.detected = true;
                v.detectedSongTime = when;
                v.centsError = st.bestCents;
                v.snr = st.bestSnr;
                pending.push_back(v);
            }
            else
            {
                st.detected = false;
                Verdict v;
                v.id = cn.id;
                v.detected = false;
                pending.push_back(v);
            }
        }
    }
}
