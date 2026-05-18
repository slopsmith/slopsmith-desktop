// Audio Bridge — connects the JUCE native addon to Electron IPC.
// The native addon is loaded via require() and its methods are
// exposed to the renderer process via ipcMain.handle().

import { ipcMain } from 'electron';
import * as path from 'path';
import { app } from 'electron';
import { isDebugEnabled, getDebugLogPath } from './debug-log';

type AudioModule = Record<string, (...args: any[]) => any>;

let audio: AudioModule | null = null;

function loadNativeAddon(): AudioModule | null {
    const addonPaths = [
        // Development build
        path.join(__dirname, '..', '..', 'build', 'Release', 'slopsmith_audio.node'),
        // Packaged build (unpacked from asar, the default for electron-builder asarUnpack entries)
        path.join(process.resourcesPath || '', 'app.asar.unpacked', 'build', 'Release', 'slopsmith_audio.node'),
        // Alternative location (direct copy to resources)
        path.join(process.resourcesPath || '', 'build', 'Release', 'slopsmith_audio.node'),
    ];

    for (const addonPath of addonPaths) {
        try {
            const mod = require(addonPath);
            console.log(`[audio] Loaded native addon from ${addonPath}`);
            return mod;
        } catch (e: any) {
            console.log(`[audio] Could not load from ${addonPath}: ${e.message}`);
        }
    }

    console.warn('[audio] Native audio addon not found — audio features disabled');
    console.warn('[audio] Build it with: npm run build:audio');
    return null;
}

export function initAudioBridge(): void {
    audio = loadNativeAddon();

    if (audio) {
        // Redirect native stderr to the debug log before init() runs — that's
        // when the [AudioEngine] device diagnostics start. enableFileLogging
        // returns "" on success or an error description (with errno) so a
        // failure is diagnosable from the log itself. Best-effort.
        if (isDebugEnabled() && typeof audio.enableFileLogging === 'function') {
            try {
                const err = audio.enableFileLogging(getDebugLogPath());
                console.log(err
                    ? `[audio] Native file logging failed: ${err}`
                    : '[audio] Native file logging enabled');
            } catch (e: any) {
                console.warn(`[audio] enableFileLogging threw: ${e.message}`);
            }
        }

        try {
            audio.init();
            console.log('[audio] Engine initialized');
        } catch (e: any) {
            console.error(`[audio] Init failed: ${e.message}`);
            audio = null;
        }
    }

    // Load the Basic Pitch ONNX model for the polyphonic ML note detector.
    // Bundled offline (Constitution IV) under resources/models/. Fail-soft:
    // a missing model / disabled ONNX support just leaves the engine on the
    // YIN PitchDetector / ChordScorer fallback (Constitution VII).
    if (audio && typeof audio.loadNoteModel === 'function') {
        const modelPath = app.isPackaged
            ? path.join(process.resourcesPath, 'models', 'basic_pitch.onnx')
            : path.join(__dirname, '..', '..', 'resources', 'models', 'basic_pitch.onnx');
        try {
            const ok = audio.loadNoteModel(modelPath);
            console.log(ok
                ? `[audio] ML note detection model loaded from ${modelPath}`
                : `[audio] ML note model unavailable (${modelPath}) — using YIN fallback`);
        } catch (e: any) {
            console.warn(`[audio] loadNoteModel failed: ${e.message} — using YIN fallback`);
        }
    }

    // Preset/plugin list paths
    const configDir = app.getPath('userData');

    // ── Lifecycle ──────────────────────────────────────────────────────────

    ipcMain.handle('audio:isAvailable', () => audio !== null);

    // ── Device Management ──────────────────────────────────────────────────

    ipcMain.handle('audio:getDeviceTypes', () => {
        const result = audio?.getDeviceTypes() ?? [];
        return result.map((t: any) => ({
            name: String(t.name || ''),
            inputs: Array.isArray(t.inputs) ? t.inputs.map(String) : [],
            outputs: Array.isArray(t.outputs) ? t.outputs.map(String) : [],
        }));
    });

    ipcMain.handle('audio:getSampleRates', () => {
        return audio?.getSampleRates() ?? [];
    });

    ipcMain.handle('audio:getBufferSizes', () => {
        return audio?.getBufferSizes() ?? [];
    });

    ipcMain.handle('audio:probeDeviceOptions', (_event, typeName: string, input: string, output: string) => {
        return audio?.probeDeviceOptions(typeName, input, output) ?? {
            type: String(typeName || ''),
            input: String(input || ''),
            output: String(output || ''),
            sampleRates: [],
            bufferSizes: [],
            error: 'Native audio addon not available',
        };
    });

    ipcMain.handle('audio:getCurrentDevice', () => {
        return audio?.getCurrentDevice() ?? null;
    });

    ipcMain.handle('audio:setDeviceType', (_event, typeName: string) => {
        return audio?.setDeviceType(typeName) ?? false;
    });

    ipcMain.handle('audio:setDevice', (_event, input: string, output: string, sampleRate: number, bufferSize: number) => {
        return audio?.setDevice(input, output, sampleRate, bufferSize) ?? false;
    });

    // ── Audio Control ──────────────────────────────────────────────────────

    ipcMain.handle('audio:startAudio', () => {
        audio?.startAudio();
    });

    ipcMain.handle('audio:stopAudio', () => {
        audio?.stopAudio();
    });

    ipcMain.handle('audio:isAudioRunning', () => {
        return audio?.isAudioRunning() ?? false;
    });

    // ── Gain ───────────────────────────────────────────────────────────────

    ipcMain.handle('audio:setGain', (_event, which: string, value: number) => {
        audio?.setGain(which, value);
    });

    ipcMain.handle('audio:setInputChannel', (_event, channel: number) => {
        audio?.setInputChannel(channel);
    });

    ipcMain.handle('audio:setMonitorMute', (_event, mute: boolean) => {
        audio?.setMonitorMute(mute);
    });

    ipcMain.handle('audio:setMonitorMuteSuppressed', (_event, suppressed: boolean) => {
        // typeof-guarded: a downlevel addon without this method is a no-op
        // rather than a thrown IPC error (Constitution VII fail-soft).
        if (audio && typeof audio.setMonitorMuteSuppressed === 'function') {
            audio.setMonitorMuteSuppressed(suppressed);
        }
    });

    ipcMain.handle('audio:isMonitorMuted', () => {
        return audio?.isMonitorMuted() ?? true;
    });

    ipcMain.handle(
        'audio:setNoiseGate',
        (
            _event,
            payload: {
                enabled: boolean;
                thresholdDb: number;
                releaseMs: number;
                depthDb: number;
            },
        ) => {
            audio?.setNoiseGate(payload);
        },
    );

    // ── Metering ───────────────────────────────────────────────────────────

    ipcMain.handle('audio:getLevels', () => {
        return audio?.getLevels() ?? { inputLevel: 0, outputLevel: 0, inputPeak: 0, outputPeak: 0 };
    });

    ipcMain.handle('audio:resetPeaks', () => {
        audio?.resetPeaks();
    });

    // ── Pitch Detection ────────────────────────────────────────────────────

    ipcMain.handle('audio:getPitchDetection', () => {
        return audio?.getPitchDetection() ?? { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
    });

    // Whether the polyphonic ML note detector (Basic Pitch) is active. When
    // false the engine is on the YIN PitchDetector / ChordScorer fallback.
    // typeof-guarded so a downlevel addon simply reports false.
    ipcMain.handle('audio:isMlNoteDetection', () => {
        if (!audio || typeof audio.isMlNoteDetection !== 'function') return false;
        try {
            return audio.isMlNoteDetection() === true;
        } catch {
            return false;
        }
    });

    // ── Chord Scoring (polyphonic) ─────────────────────────────────────────
    // The notedetect plugin's chord-scoring branch hands us a chord
    // context — notes, arrangement, tuning offsets, thresholds — and
    // we score it against the engine's most recent input-ring samples
    // entirely inside the native scorer. No audio buffers cross the
    // N-API boundary; only the small result object travels over IPC.

    ipcMain.handle('audio:getSampleRate', () => {
        // typeof guard covers the version-skew case where the JS/TS was
        // updated but the native addon predates getSampleRate — without
        // it, `audio.getSampleRate()` would throw rather than fall back.
        // The native side already substitutes 48000 for non-finite / <=0
        // values, but we double-check here so a malformed return value
        // from a stub or test harness can't leak through to the renderer
        // and feed a zero into downstream tolerance math.
        if (audio && typeof audio.getSampleRate === 'function') {
            try {
                const sr = audio.getSampleRate();
                if (typeof sr === 'number' && Number.isFinite(sr) && sr > 0) return sr;
            } catch { /* fall through */ }
        }
        return 48000;
    });

    // Why this is a synchronous handler (and not an N-API AsyncWorker /
    // worker thread): the only caller is the notedetect plugin's
    // `processFrame()` tick, which fires at ~20 Hz and awaits each
    // result before issuing the next — natural back-pressure, no
    // queueing or coalescing needed. A 16384-point juce::dsp::FFT at
    // 48 kHz takes ~0.5 ms on modern x86, plus negligible per-string
    // band-energy work, for a per-call cost well under the 50 ms
    // budget. The JS path it replaces runs the same FFT synchronously
    // in the renderer event loop today; moving to async would also
    // require a mutex around ChordScorer's reusable FFT/scratch state.
    // The trade-off doesn't pay back for this workload — revisit only
    // if profiling shows actual main-loop stalls.
    ipcMain.handle('audio:scoreChord', (_event, ctx: unknown) => {
        // Feature-detect the native method the same way getSampleRate
        // above does — a downlevel addon (pre-ChordScorer build) should
        // return null so the renderer can fall back to skipping chord
        // scoring instead of throwing on every call.
        if (!audio || typeof audio.scoreChord !== 'function') return null;
        try {
            return audio.scoreChord(ctx);
        } catch (e: unknown) {
            console.warn(`[audio] scoreChord failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // Raw polyphonic transcription — the ML detector's full active-pitch set.
    // Returns null when the ML detector isn't active (downlevel addon, no ONNX
    // support, or no model loaded) so the renderer feature-detects and falls
    // back to the getPitchDetection / scoreChord path.
    ipcMain.handle('audio:detectNotes', () => {
        if (!audio || typeof audio.detectNotes !== 'function') return null;
        try {
            return audio.detectNotes();
        } catch (e: unknown) {
            console.warn(`[audio] detectNotes failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // ── VST Scanning ───────────────────────────────────────────────────────

    ipcMain.handle('audio:scanPlugins', async (_event, dirs?: string[]) => {
        if (!audio) return [];
        return await audio.scanPlugins(dirs);
    });

    ipcMain.handle('audio:getKnownPlugins', () => {
        return audio?.getKnownPlugins() ?? [];
    });

    ipcMain.handle('audio:savePluginList', (_event, filePath: string) => {
        audio?.savePluginList(filePath || path.join(configDir, 'known-plugins.xml'));
    });

    ipcMain.handle('audio:loadPluginList', (_event, filePath: string) => {
        audio?.loadPluginList(filePath || path.join(configDir, 'known-plugins.xml'));
    });

    // ── Signal Chain ───────────────────────────────────────────────────────

    ipcMain.handle('audio:loadVST', (_event, pluginPath: string) => {
        return audio?.loadVST(pluginPath) ?? -1;
    });

    ipcMain.handle('audio:loadNAMModel', async (_event, modelPath: string) => {
        return await audio?.loadNAMModel(modelPath) ?? -1;
    });

    ipcMain.handle('audio:loadIR', async (_event, irPath: string) => {
        return await audio?.loadIR(irPath) ?? -1;
    });

    ipcMain.handle('audio:removeProcessor', (_event, slotId: number) => {
        audio?.removeProcessor(slotId);
    });

    ipcMain.handle('audio:moveProcessor', (_event, from: number, to: number) => {
        audio?.moveProcessor(from, to);
    });

    ipcMain.handle('audio:setBypass', (_event, slotId: number, bypassed: boolean) => {
        audio?.setBypass(slotId, bypassed);
    });

    ipcMain.handle('audio:clearChain', () => {
        audio?.clearChain();
    });

    ipcMain.handle('audio:getChainState', () => {
        return audio?.getChainState() ?? [];
    });

    // ── Plugin Editor ──────────────────────────────────────────────────────

    ipcMain.handle('audio:openPluginEditor', (_event, slotId: number) => {
        return audio?.openPluginEditor(slotId) ?? false;
    });

    ipcMain.handle('audio:closePluginEditor', (_event, slotId: number) => {
        return audio?.closePluginEditor(slotId) ?? false;
    });

    // ── Parameters ─────────────────────────────────────────────────────────

    ipcMain.handle('audio:getParameters', (_event, slotId: number) => {
        return audio?.getParameters(slotId) ?? [];
    });

    ipcMain.handle('audio:setParameter', (_event, slotId: number, paramIndex: number, value: number) => {
        audio?.setParameter(slotId, paramIndex, value);
    });

    // ── MIDI ───────────────────────────────────────────────────────────────

    ipcMain.handle('audio:sendMidiToSlot', (_event, slotId: number, msgType: number, channel: number, param1: number, param2?: number) => {
        return audio?.sendMidiToSlot(slotId, msgType, channel, param1, param2 ?? 0) ?? false;
    });

    // ── Backing Track ──────────────────────────────────────────────────────

    ipcMain.handle('audio:loadBackingTrack', (_event, filePath: string) => {
        return audio?.loadBackingTrack(filePath) ?? false;
    });

    ipcMain.handle('audio:startBacking', () => audio?.startBacking());
    ipcMain.handle('audio:stopBacking', () => audio?.stopBacking());
    ipcMain.handle('audio:seekBacking', (_event, seconds: number) => audio?.seekBacking(seconds));
    ipcMain.handle('audio:getBackingPosition', () => audio?.getBackingPosition() ?? 0);
    ipcMain.handle('audio:getBackingDuration', () => audio?.getBackingDuration() ?? 0);
    ipcMain.handle('audio:isBackingPlaying', () => audio?.isBackingPlaying() ?? false);

    // ── Presets ────────────────────────────────────────────────────────────

    ipcMain.handle('audio:savePreset', () => {
        return audio?.savePreset() ?? null;
    });

    ipcMain.handle('audio:loadPreset', async (_event, presetJson: string) => {
        return await audio?.loadPreset(presetJson) ?? { success: false, error: 'No audio' };
    });

    ipcMain.handle('audio:setMultiBypass', (_event, changes: Array<{slotId: number, bypassed: boolean}>) => {
        return audio?.setMultiBypass(changes) ?? false;
    });
}

export function shutdownAudio(): void {
    if (audio) {
        try {
            audio.shutdown();
            try { console.log('[audio] Engine shut down'); } catch { /* console may be gone */ }
        } catch { /* silent fail during shutdown */ }
        audio = null;
    }
}
