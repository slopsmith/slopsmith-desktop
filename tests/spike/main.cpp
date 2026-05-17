// Phase 0 de-risk spike — TST-style polyphonic ML note detection.
//
// THROWAWAY. Not wired into the addon build. Proves that Spotify Basic Pitch
// (nmp.onnx) loads and runs under ONNX Runtime's C++ API, that the I/O contract
// matches expectations, and that a minimal onset/frame post-processing yields
// interpretable MIDI notes. See README.md for provenance.
//
// Build: see CMakeLists.txt in this directory.
// Run:   ./spike <model.onnx> <audio.wav>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// --- Basic Pitch constants (basic_pitch/constants.py) ---------------------
static constexpr int   kModelSampleRate = 22050;
static constexpr int   kFftHop          = 256;
static constexpr int   kAudioNSamples   = 22050 * 2 - 256; // 43844, ~2 s window
static constexpr int   kFramesPerSecond = 22050 / 256;     // 86
static constexpr int   kNumPitches      = 88;              // MIDI 21..108
static constexpr int   kLowestMidi      = 21;              // A0, base freq 27.5 Hz
static constexpr float kOnsetThreshold  = 0.5f;
static constexpr float kFrameThreshold  = 0.3f;

// nmp.onnx tensor names (verified via the Python ONNX Runtime in Phase 0).
static const char* kInputName  = "serving_default_input_2:0";
static const char* kNoteOutput = "StatefulPartitionedCall:1"; // frame/note posteriorgram
static const char* kOnsetOutput= "StatefulPartitionedCall:2"; // onset posteriorgram

// --------------------------------------------------------------------------
// Minimal WAV reader: 16-bit PCM or 32-bit float, any channel count -> mono.
// --------------------------------------------------------------------------
struct Wav { std::vector<float> samples; int sampleRate = 0; };

static uint32_t rdU32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (uint32_t(p[3])<<24); }
static uint16_t rdU16(const uint8_t* p) { return uint16_t(p[0] | (p[1]<<8)); }

static bool readWav(const std::string& path, Wav& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << "\n"; return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data()+8, "WAVE", 4))
    { std::cerr << "not a RIFF/WAVE file\n"; return false; }

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    uint32_t dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= buf.size())
    {
        const char* id = reinterpret_cast<const char*>(buf.data() + pos);
        uint32_t sz = rdU32(buf.data() + pos + 4);
        const uint8_t* body = buf.data() + pos + 8;
        if (!std::memcmp(id, "fmt ", 4) && sz >= 16)
        {
            fmt = rdU16(body); channels = rdU16(body+2); rate = rdU32(body+4); bits = rdU16(body+14);
        }
        else if (!std::memcmp(id, "data", 4))
        {
            data = body;
            dataLen = std::min<uint32_t>(sz, uint32_t(buf.size() - (pos + 8)));
        }
        pos += 8 + sz + (sz & 1); // chunks are word-aligned
    }
    if (!data || channels == 0) { std::cerr << "no data/fmt chunk\n"; return false; }

    out.sampleRate = int(rate);
    const int bytesPerSample = bits / 8;
    const size_t frames = dataLen / (bytesPerSample * channels);
    out.samples.resize(frames);
    for (size_t i = 0; i < frames; ++i)
    {
        double acc = 0.0;
        for (int c = 0; c < channels; ++c)
        {
            const uint8_t* s = data + (i * channels + c) * bytesPerSample;
            if (fmt == 3 && bits == 32)        { float v; std::memcpy(&v, s, 4); acc += v; }
            else if (fmt == 1 && bits == 16)   { acc += int16_t(rdU16(s)) / 32768.0; }
            else if (fmt == 1 && bits == 32)   { acc += int32_t(rdU32(s)) / 2147483648.0; }
            else { std::cerr << "unsupported fmt=" << fmt << " bits=" << bits << "\n"; return false; }
        }
        out.samples[i] = float(acc / channels);
    }
    return true;
}

// --------------------------------------------------------------------------
// Resample to 22050 Hz. Catmull-Rom cubic interpolation — adequate for the
// spike; the real MlNoteDetector will use juce::LagrangeInterpolator.
// --------------------------------------------------------------------------
static std::vector<float> resampleTo22050(const std::vector<float>& in, int srcRate)
{
    if (srcRate == kModelSampleRate) return in;
    const double ratio = double(srcRate) / kModelSampleRate;
    const size_t outLen = size_t(in.size() / ratio);
    std::vector<float> out(outLen);
    auto at = [&](long i) -> float {
        if (i < 0) i = 0;
        if (i >= long(in.size())) i = long(in.size()) - 1;
        return in[size_t(i)];
    };
    for (size_t n = 0; n < outLen; ++n)
    {
        const double srcPos = n * ratio;
        const long   i = long(srcPos);
        const float  t = float(srcPos - i);
        const float p0 = at(i-1), p1 = at(i), p2 = at(i+1), p3 = at(i+2);
        out[n] = p1 + 0.5f * t * ((p2 - p0) + t * ((2*p0 - 5*p1 + 4*p2 - p3)
                                  + t * (3*(p1 - p2) + p3 - p0)));
    }
    return out;
}

static const char* noteName(int midi)
{
    static const char* n[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    std::snprintf(buf, sizeof(buf), "%s%d", n[midi % 12], midi / 12 - 1);
    return buf;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: spike <model.onnx> <audio.wav>\n"; return 2; }
    const std::string modelPath = argv[1];
    const std::string wavPath   = argv[2];

    Wav wav;
    if (!readWav(wavPath, wav)) return 1;
    std::cout << "WAV: " << wav.samples.size() << " samples @ " << wav.sampleRate << " Hz\n";

    const std::vector<float> mono = resampleTo22050(wav.samples, wav.sampleRate);
    std::cout << "resampled: " << mono.size() << " samples @ " << kModelSampleRate << " Hz\n";

    // --- ONNX Runtime session ---
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "bp-spike");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session session(env, modelPath.c_str(), opts);

    {
        Ort::AllocatorWithDefaultOptions alloc;
        std::cout << "model inputs:\n";
        for (size_t i = 0; i < session.GetInputCount(); ++i)
        {
            auto name = session.GetInputNameAllocated(i, alloc);
            auto shp  = session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            std::cout << "  " << name.get() << " [";
            for (auto d : shp) std::cout << d << " ";
            std::cout << "]\n";
        }
        std::cout << "model outputs:\n";
        for (size_t i = 0; i < session.GetOutputCount(); ++i)
        {
            auto name = session.GetOutputNameAllocated(i, alloc);
            auto shp  = session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            std::cout << "  " << name.get() << " [";
            for (auto d : shp) std::cout << d << " ";
            std::cout << "]\n";
        }
    }

    auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const char* inNames[]  = { kInputName };
    const char* outNames[] = { kNoteOutput, kOnsetOutput };

    // Process non-overlapping ~2 s windows. (The production detector uses a
    // rolling buffer reading only fresh frames; non-overlapping is fine here
    // and is the source of the boundary re-onsets noted in README.md.)
    struct Hit { double timeSec; int midi; float conf; };
    std::vector<Hit> hits;

    int windows = 0;
    double totalInferMs = 0.0;

    for (size_t base = 0; base < mono.size(); base += kAudioNSamples)
    {
        std::vector<float> window(kAudioNSamples, 0.0f);
        const size_t n = std::min<size_t>(kAudioNSamples, mono.size() - base);
        std::memcpy(window.data(), mono.data() + base, n * sizeof(float));

        const int64_t inShape[3] = { 1, kAudioNSamples, 1 };
        Ort::Value inTensor = Ort::Value::CreateTensor<float>(
            memInfo, window.data(), window.size(), inShape, 3);

        const auto t0 = std::chrono::steady_clock::now();
        auto out = session.Run(Ort::RunOptions{nullptr}, inNames, &inTensor, 1, outNames, 2);
        const auto t1 = std::chrono::steady_clock::now();
        totalInferMs += std::chrono::duration<double, std::milli>(t1 - t0).count();

        const float* note  = out[0].GetTensorData<float>();
        const float* onset = out[1].GetTensorData<float>();
        // Validate the output shapes before indexing: both heads must be
        // rank-3 [1, frames, kNumPitches] and agree on the frame count, or
        // the flat indexing below would read out of bounds.
        const auto noteShape  = out[0].GetTensorTypeAndShapeInfo().GetShape();
        const auto onsetShape = out[1].GetTensorTypeAndShapeInfo().GetShape();
        if (noteShape.size() != 3 || onsetShape.size() != 3
            || noteShape[2] != kNumPitches || onsetShape[2] != kNumPitches
            || noteShape[1] != onsetShape[1])
        {
            std::cerr << "FAIL: unexpected model output shape\n";
            return 1;
        }
        const int frames = int(noteShape[1]); // 172

        const double windowStartSec = double(base) / kModelSampleRate;
        for (int p = 0; p < kNumPitches; ++p)
            for (int f = 1; f < frames; ++f)
            {
                const float on  = onset[f * kNumPitches + p];
                const float onPrev = onset[(f-1) * kNumPitches + p];
                const float fr  = note[f * kNumPitches + p];
                if (on >= kOnsetThreshold && onPrev < kOnsetThreshold && fr >= kFrameThreshold)
                    hits.push_back({ windowStartSec + double(f) / kFramesPerSecond,
                                     kLowestMidi + p, on });
            }
        ++windows;
    }

    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b){ return a.timeSec < b.timeSec; });

    std::cout << "\n" << windows << " windows, total inference "
              << totalInferMs << " ms, " << (totalInferMs / std::max(1, windows))
              << " ms/window\n";
    std::cout << "\nDETECTED onsets (time_s, midi, note, onset_conf):\n";
    for (const auto& h : hits)
        std::cout << "  t=" << h.timeSec << "  midi=" << h.midi
                  << "  " << noteName(h.midi) << "  conf=" << h.conf << "\n";

    std::cout << "\nspike OK\n";
    return 0;
}
