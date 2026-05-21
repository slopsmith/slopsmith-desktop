// Controlled detection benchmark for MlNoteDetector.
//
// Replays a fixed DI recording through the REAL MlNoteDetector and scores the
// detected onsets against a known note chart — so a parameter change can be
// measured (recall / timing) instead of guessed from noisy live takes.
//
// Build: see CMakeLists.txt.  Run:
//   ./mlnd_bench <model.onnx> <di-take.wav> <chart.txt> [channel]
//     chart.txt : one "<chartTimeSec> <midi>" per line (jq-extracted from a
//                 note_detect diagnostic: .events[] | "\(.t) \(.ex)").
//     channel   : mix (default) | left | right
//
// The recording and the chart start at unknown relative offsets, so the
// harness searches for the time offset that best aligns them, then reports
// recall and the timing-error distribution at that offset.

#include "MlNoteDetector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
uint32_t rdU32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (uint32_t(p[3])<<24); }
uint16_t rdU16(const uint8_t* p) { return uint16_t(p[0] | (p[1]<<8)); }

// Read a 16-bit PCM WAV to mono float. channel: 0 = mix, 1 = left, 2 = right.
bool readWav(const std::string& path, int channelMode, std::vector<float>& out, int& sampleRate)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data()+8, "WAVE", 4))
        return false;

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0, dataLen = 0;
    const uint8_t* data = nullptr;
    size_t pos = 12;
    while (pos + 8 <= buf.size())
    {
        const char* id = reinterpret_cast<const char*>(buf.data() + pos);
        const uint32_t sz = rdU32(buf.data() + pos + 4);
        const uint8_t* body = buf.data() + pos + 8;
        // Guard the fmt-body reads (up to body+14, i.e. 16 bytes) against a
        // truncated file: a declared sz >= 16 doesn't mean 16 bytes are
        // actually present.
        if (!std::memcmp(id, "fmt ", 4) && sz >= 16 && pos + 8 + 16 <= buf.size())
        { fmt = rdU16(body); channels = rdU16(body+2); rate = rdU32(body+4); bits = rdU16(body+14); }
        else if (!std::memcmp(id, "data", 4))
        { data = body; dataLen = std::min<uint32_t>(sz, uint32_t(buf.size() - (pos + 8))); }
        pos += 8 + sz + (sz & 1);
    }
    if (!data || channels == 0 || rate == 0 || fmt != 1 || bits != 16) return false;

    sampleRate = int(rate);
    const size_t frames = dataLen / (size_t(2) * channels);
    out.resize(frames);
    for (size_t i = 0; i < frames; ++i)
    {
        auto sample = [&](int c) -> double {
            return int16_t(rdU16(data + (i * channels + c) * 2)) / 32768.0;
        };
        double v;
        if (channelMode == 1)                       v = sample(0);
        else if (channelMode == 2 && channels > 1)  v = sample(1);
        else { double a = 0; for (int c = 0; c < channels; ++c) a += sample(c); v = a / channels; }
        out[i] = float(v);
    }
    return true;
}

struct ChartNote { double t; int midi; };
struct Onset     { double t; int midi; float conf; };
// One bridge poll: the full active-note set, mirroring audio.detectNotes().
struct PollRec { double t; std::vector<MlNoteDetector::ActiveNote> notes; };
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: mlnd_bench <model.onnx> <di-take.wav> <chart.txt> [mix|left|right]\n";
        return 2;
    }
    int channelMode = 0;
    if (argc > 4)
    {
        const std::string c = argv[4];
        channelMode = (c == "left") ? 1 : (c == "right") ? 2 : 0;
    }

    std::vector<float> wav;
    int sampleRate = 0;
    if (!readWav(argv[2], channelMode, wav, sampleRate))
    { std::cerr << "FAIL: cannot read 16-bit WAV " << argv[2] << "\n"; return 1; }
    const double wavSec = double(wav.size()) / sampleRate;
    std::cout << "WAV: " << wav.size() << " samples @ " << sampleRate << " Hz ("
              << wavSec << " s), channel=" << (channelMode==1?"left":channelMode==2?"right":"mix") << "\n";

    std::vector<ChartNote> chart;
    {
        std::ifstream cf(argv[3]);
        if (!cf) { std::cerr << "FAIL: cannot read chart " << argv[3] << "\n"; return 1; }
        std::string line;
        while (std::getline(cf, line))
        {
            std::istringstream is(line);
            ChartNote n{};
            if (is >> n.t >> n.midi) chart.push_back(n);
        }
    }
    std::cout << "chart: " << chart.size() << " notes\n";

    MlNoteDetector det;
    if (!det.loadModel(juce::File(juce::String(argv[1]))))
    { std::cerr << "FAIL: loadModel returned false\n"; return 1; }
    det.prepare((double) sampleRate, 256);

    // Feed the WAV at real time (1.0x) and poll the active set every 50 ms,
    // the same cadence as the plugin. A rising per-pitch onsetSeq is a
    // detected onset; back-date it by onsetAgeMs to its true time.
    //
    // feedRate MUST stay 1.0: onsetAgeMs is measured in wall-clock time inside
    // MlNoteDetector, while fedSec is the fed-audio timeline. Feeding faster
    // than real time desynchronises the two — a wall-clock age would map to a
    // larger span of fed audio — so back-dated onset times would drift later
    // than the chart over a long recording. At 1.0x the two timelines agree.
    const int block = 256;
    const double feedRate = 1.0;
    std::map<int, int> lastSeq;
    std::vector<Onset> onsets;
    std::vector<PollRec> polls;   // full detectNotes() stream, one entry per poll
    double nextPollSec = 0.0;

    for (size_t i = 0; i < wav.size(); i += block)
    {
        const int n = (int) std::min<size_t>(block, wav.size() - i);
        det.pushSamples(wav.data() + i, n);
        std::this_thread::sleep_for(std::chrono::microseconds(
            (long long) (1e6 * n / sampleRate / feedRate)));

        const double fedSec = double(i + n) / sampleRate;
        if (fedSec >= nextPollSec)
        {
            nextPollSec += 0.050;
            auto active = det.getActiveNotes();
            polls.push_back({ fedSec, active });
            for (const auto& a : active)
            {
                auto it = lastSeq.find(a.midi);
                const int prevSeq = (it == lastSeq.end()) ? 0 : it->second;
                // onsetSeq == 0 means "no detected onset" (sustained activity),
                // so only a strictly-advancing, non-zero counter is a real new
                // onset — otherwise back-dating by the sentinel age would forge
                // an onset at poll time. Still track the pitch either way.
                if (a.onsetSeq > prevSeq && a.onsetSeq > 0)
                {
                    const double age = (a.onsetAgeMs < 1.0e6f) ? a.onsetAgeMs / 1000.0 : 0.0;
                    onsets.push_back({ fedSec - age, a.midi, a.confidence });
                }
                lastSeq[a.midi] = a.onsetSeq;
            }
        }
    }
    constexpr double kDrainSec = 0.400;
    std::this_thread::sleep_for(std::chrono::milliseconds(
        (int) (kDrainSec * 1000)));
    // One final poll after the drain delay: late onsets / active notes the
    // detector only resolved during the trailing inference would otherwise be
    // omitted from the metrics and detectstream.json. Wall time has advanced
    // by the drain sleep, so the poll timestamp is wavSec + kDrainSec — using
    // a bare wavSec would back-date trailing onsets by the drain duration,
    // since onsetAgeMs is measured at this (later) wall-clock instant.
    {
        const double finalPollSec = wavSec + kDrainSec;
        auto active = det.getActiveNotes();
        polls.push_back({ finalPollSec, active });
        for (const auto& a : active)
        {
            auto it = lastSeq.find(a.midi);
            const int prevSeq = (it == lastSeq.end()) ? 0 : it->second;
            // See the in-loop poll above: onsetSeq 0 is "no onset", not a hit.
            if (a.onsetSeq > prevSeq && a.onsetSeq > 0)
            {
                const double age = (a.onsetAgeMs < 1.0e6f) ? a.onsetAgeMs / 1000.0 : 0.0;
                onsets.push_back({ finalPollSec - age, a.midi, a.confidence });
            }
            lastSeq[a.midi] = a.onsetSeq;
        }
    }
    det.stop();
    std::cout << "detected onsets: " << onsets.size() << "\n";

    // --- Align: find the chart->WAV time offset maximising matches ----------
    const double tol = 0.10;  // ±100 ms match window
    auto countMatches = [&](double delta) {
        int m = 0;
        for (const auto& c : chart)
        {
            const double target = c.t + delta;
            for (const auto& o : onsets)
                if (o.midi == c.midi && std::fabs(o.t - target) <= tol) { ++m; break; }
        }
        return m;
    };
    double bestDelta = 0.0;
    int bestMatches = -1;
    for (double d = -2.0; d <= 30.0; d += 0.010)
    {
        const int m = countMatches(d);
        if (m > bestMatches) { bestMatches = m; bestDelta = d; }
    }

    // --- Dump the detect-stream (chart-aligned) for the JS matching harness --
    {
        std::ofstream js("detectstream.json");
        js << "{\"offset\":" << bestDelta << ",\"polls\":[";
        for (size_t pi = 0; pi < polls.size(); ++pi)
        {
            if (pi) js << ",";
            js << "{\"t\":" << (polls[pi].t - bestDelta) << ",\"notes\":[";
            for (size_t ni = 0; ni < polls[pi].notes.size(); ++ni)
            {
                const auto& a = polls[pi].notes[ni];
                if (ni) js << ",";
                js << "{\"midi\":" << a.midi
                   << ",\"confidence\":" << a.confidence
                   << ",\"onsetMs\":" << a.onsetAgeMs
                   << ",\"onsetSeq\":" << a.onsetSeq << "}";
            }
            js << "]}";
        }
        js << "]}";
    }
    std::cout << "detect-stream: " << polls.size() << " polls -> detectstream.json\n";

    // --- Report at the best offset -----------------------------------------
    std::vector<double> te;       // timing errors of matched notes
    int matched = 0;
    for (const auto& c : chart)
    {
        const double target = c.t + bestDelta;
        double best = 1e9;
        for (const auto& o : onsets)
            if (o.midi == c.midi && std::fabs(o.t - target) <= tol)
                if (std::fabs(o.t - target) < std::fabs(best)) best = o.t - target;
        if (best < 1e8) { ++matched; te.push_back(best); }
    }
    std::sort(te.begin(), te.end());
    auto pct = [&](double p) {
        return te.empty() ? 0.0 : te[std::min(te.size()-1, (size_t)(p * te.size()))];
    };

    std::cout << "\n=== alignment ===\n";
    std::cout << "chart->WAV offset: " << bestDelta << " s\n";
    std::cout << "\n=== detection quality (±" << (tol*1000) << " ms) ===\n";
    std::cout << "recall:        " << matched << " / " << chart.size()
              << "  (" << (100.0 * matched / std::max<size_t>(1, chart.size())) << "%)\n";
    if (!te.empty())
    {
        double sum = 0; for (double x : te) sum += x;
        std::cout << "timing error:  median " << (te[te.size()/2]*1000) << " ms"
                  << "   p10 " << (pct(0.10)*1000) << "   p90 " << (pct(0.90)*1000)
                  << "   mean " << (sum/te.size()*1000) << " ms\n";
    }
    std::cout << "onsets/note:   " << (double(onsets.size()) / std::max<size_t>(1, chart.size()))
              << "  (>1 = extra detections: harmonics, noise)\n";

    // --- Predicted score: one-onset-one-note greedy matching ----------------
    // Mirrors the plugin's fixed matcher — each onset (earliest first) claims
    // the nearest still-unclaimed chart note of its pitch within ±tol. This
    // predicts the live hit rate, where the "recall" above is the loose
    // upper bound (one onset allowed to satisfy many notes).
    {
        std::vector<Onset> sorted = onsets;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Onset& a, const Onset& b){ return a.t < b.t; });
        std::vector<char> claimed(chart.size(), 0);
        std::vector<double> hte;
        for (const auto& o : sorted)
        {
            int bestIdx = -1; double bestDist = 1e9;
            for (size_t ci = 0; ci < chart.size(); ++ci)
            {
                if (claimed[ci] || chart[ci].midi != o.midi) continue;
                const double d = std::fabs(o.t - (chart[ci].t + bestDelta));
                if (d <= tol && d < bestDist) { bestDist = d; bestIdx = (int) ci; }
            }
            if (bestIdx >= 0)
            { claimed[(size_t) bestIdx] = 1; hte.push_back(o.t - (chart[(size_t) bestIdx].t + bestDelta)); }
        }
        std::sort(hte.begin(), hte.end());
        const size_t hits = hte.size();
        std::cout << "\n=== predicted score (one-onset-one-note) ===\n";
        std::cout << "hits:          " << hits << " / " << chart.size()
                  << "  (" << (100.0 * hits / std::max<size_t>(1, chart.size())) << "%)\n";
        if (!hte.empty())
            std::cout << "timing error:  median " << (hte[hte.size()/2]*1000) << " ms"
                      << "   p10 " << (hte[hte.size()/10]*1000)
                      << "   p90 " << (hte[hte.size()*9/10]*1000) << " ms\n";
    }
    return 0;
}
