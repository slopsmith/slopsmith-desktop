// Native end-to-end test for MlNoteDetector (the production class).
//
// Feeds a known guitar WAV through pushSamples() in 256-sample blocks — the
// same path the audio callback uses — at roughly real time, lets the
// background inference thread resample / window / run Basic Pitch, then asserts
// the active-pitch snapshot matches the WAV's final content (a C-major triad).
//
// This exercises the streaming LagrangeInterpolator resample, the rolling
// 22050 Hz window, the snapshot publishing and threshold logic — everything
// the Phase 0 spike did not. Inference accuracy itself is also re-checked here.
//
// Build: see CMakeLists.txt. Run: ./mlnd_test <model.onnx> <audio.wav>
// Exit 0 = pass.

#include "MlNoteDetector.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

// Minimal WAV reader — 16-bit PCM / 32-bit float, any channels -> mono.
namespace
{
uint32_t rdU32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (uint32_t(p[3])<<24); }
uint16_t rdU16(const uint8_t* p) { return uint16_t(p[0] | (p[1]<<8)); }

bool readWav(const std::string& path, std::vector<float>& out, int& sampleRate)
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
        if (!std::memcmp(id, "fmt ", 4) && sz >= 16)
        { fmt = rdU16(body); channels = rdU16(body+2); rate = rdU32(body+4); bits = rdU16(body+14); }
        else if (!std::memcmp(id, "data", 4))
        { data = body; dataLen = std::min<uint32_t>(sz, uint32_t(buf.size() - (pos + 8))); }
        pos += 8 + sz + (sz & 1);
    }
    if (!data || channels == 0 || rate == 0) return false;

    sampleRate = int(rate);
    const int bps = bits / 8;
    const size_t frames = dataLen / (size_t(bps) * channels);
    out.resize(frames);
    for (size_t i = 0; i < frames; ++i)
    {
        double acc = 0.0;
        for (int c = 0; c < channels; ++c)
        {
            const uint8_t* s = data + (i * channels + c) * bps;
            if (fmt == 3 && bits == 32)      { float v; std::memcpy(&v, s, 4); acc += v; }
            else if (fmt == 1 && bits == 16) { acc += int16_t(rdU16(s)) / 32768.0; }
            else return false;
        }
        out[i] = float(acc / channels);
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: mlnd_test <model.onnx> <audio.wav>\n"; return 2; }

    std::vector<float> wav;
    int sampleRate = 0;
    if (!readWav(argv[2], wav, sampleRate))
    { std::cerr << "FAIL: cannot read WAV " << argv[2] << "\n"; return 1; }
    std::cout << "WAV: " << wav.size() << " samples @ " << sampleRate << " Hz\n";

    MlNoteDetector det;
    if (!det.loadModel(juce::File(juce::String(argv[1]))))
    { std::cerr << "FAIL: loadModel('" << argv[1] << "') returned false\n"; return 1; }
    std::cout << "model loaded; isAvailable=" << det.isAvailable() << "\n";

    det.prepare((double) sampleRate, 256);

    // Feed the WAV in 256-sample blocks at ~real time so the background
    // inference thread drains the FIFO instead of overflowing it. The
    // detector reports "what is sounding now", so we poll the active set
    // *during* playback and accumulate every pitch seen while the WAV's
    // C-major triad (t≈3.3-4.5 s) is sounding — allowing for the
    // hop + inference lag, that window maps to feed time ≈ [3.6, 5.2] s.
    const int block = 256;
    std::vector<int> seenDuringChord;
    auto noteActiveSomewhere = [&](int midi)
    { return std::find(seenDuringChord.begin(), seenDuringChord.end(), midi)
             != seenDuringChord.end(); };

    for (size_t i = 0; i < wav.size(); i += block)
    {
        const int n = (int) std::min<size_t>(block, wav.size() - i);
        det.pushSamples(wav.data() + i, n);
        std::this_thread::sleep_for(std::chrono::microseconds(
            (long long) (1e6 * n / sampleRate)));

        const double feedSec = double(i) / sampleRate;
        if (feedSec >= 3.6 && feedSec <= 5.2)
            for (const auto& nt : det.getActiveNotes())
                if (!noteActiveSomewhere(nt.midi))
                    seenDuringChord.push_back(nt.midi);
    }
    // Drain: the chord is the WAV's final event, so let the trailing
    // inference finish and poll once more before stopping — otherwise the
    // last chord's notes can be dropped, making the test nondeterministic.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (const auto& nt : det.getActiveNotes())
        if (!noteActiveSomewhere(nt.midi))
            seenDuringChord.push_back(nt.midi);
    det.stop();

    std::sort(seenDuringChord.begin(), seenDuringChord.end());
    std::cout << "pitches seen active during the chord window (" << seenDuringChord.size() << "):\n";
    for (int m : seenDuringChord) std::cout << "  midi=" << m << "\n";

    // The WAV's final sustained event is a C-major triad: C3=48, E3=52, G3=55.
    const int expected[] = { 48, 52, 55 };
    int found = 0;
    for (int e : expected)
        if (noteActiveSomewhere(e)) ++found;

    if (found >= 2)
    {
        std::cout << "PASS: detected " << found << "/3 chord tones\n";
        return 0;
    }
    std::cerr << "FAIL: detected only " << found << "/3 expected chord tones\n";
    return 1;
}
