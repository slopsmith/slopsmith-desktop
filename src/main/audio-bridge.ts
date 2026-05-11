// Audio Bridge — connects the JUCE native addon to Electron IPC.
// The native addon is loaded via require() and its methods are
// exposed to the renderer process via ipcMain.handle().

import { ipcMain, BrowserWindow, WebContents } from 'electron';
import * as path from 'path';
import { app } from 'electron';

type AudioModule = Record<string, (...args: any[]) => any>;

let audio: AudioModule | null = null;

// Renderers that have called `audio:subscribeInputFrames`. We push raw
// input frames to each one on a shared 50 ms timer that's only running
// while this set is non-empty, so unsubscribed/closed windows don't pay
// the IPC cost.
const inputFrameSubscribers = new Set<WebContents>();
// WebContents we've already attached a `destroyed` cleanup listener to.
// A renderer may subscribe and unsubscribe many times across its
// lifetime; registering a fresh `once('destroyed', ...)` per subscribe
// would accumulate one-shot listeners that never fire until window
// close, slowly growing the listener registry. A WeakSet drops the
// reference automatically once the WebContents is GC'd.
const inputFrameDestroyHooked = new WeakSet<WebContents>();
let inputFrameTimer: NodeJS.Timeout | null = null;
let inputFrameSequence = 0;
const INPUT_FRAME_INTERVAL_MS = 50;
const INPUT_FRAME_SAMPLES = 4096;

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

export function initAudioBridge(mainWindow: BrowserWindow | null): void {
    audio = loadNativeAddon();

    if (audio) {
        try {
            audio.init();
            console.log('[audio] Engine initialized');
        } catch (e: any) {
            console.error(`[audio] Init failed: ${e.message}`);
            audio = null;
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

    // ── Input Frame Streaming ──────────────────────────────────────────────
    // Used by the notedetect plugin's polyphonic chord scorer, which needs
    // raw audio frames (not just monophonic pitch). The renderer subscribes
    // via `audio:subscribeInputFrames`; we drive a single 50ms timer that
    // polls the JUCE engine's ring buffer and dispatches to every active
    // subscriber. Timer stops the moment the subscriber set goes empty so
    // we don't burn IPC bandwidth when no one is listening.

    ipcMain.handle('audio:getSampleRate', () => {
        // typeof guard covers the version-skew case where the JS/TS was
        // updated but the native addon predates getSampleRate — without
        // it, `audio.getSampleRate()` would throw rather than fall back.
        if (audio && typeof audio.getSampleRate === 'function') {
            try { return audio.getSampleRate(); } catch { /* fall through */ }
        }
        return 48000;
    });

    function dispatchInputFrames(): void {
        // If the audio engine has gone away (init failure, shutdown, or
        // a downlevel addon missing getInputFrame), there's nothing to
        // stream — drop every subscriber so we don't accumulate stale
        // WebContents references waiting for an engine that won't come
        // back, and stop the timer immediately.
        if (!audio || typeof audio.getInputFrame !== 'function') {
            inputFrameSubscribers.clear();
            stopInputFrameTimer();
            return;
        }
        if (inputFrameSubscribers.size === 0) return;
        let samples: Float32Array | undefined;
        try {
            samples = audio.getInputFrame(INPUT_FRAME_SAMPLES);
        } catch (e: unknown) {
            // A throw from the native side means the addon is in a bad
            // state (engine torn down underneath us, OOM, etc.). Bail
            // out of dispatch the same way as the missing-method case
            // rather than retrying every 50ms and re-throwing in the
            // interval callback (which would unhandled-reject and crash
            // the main process on some Electron versions).
            console.warn(`[audio] input-frame dispatch failed: ${e instanceof Error ? e.message : String(e)}`);
            inputFrameSubscribers.clear();
            stopInputFrameTimer();
            return;
        }
        if (!samples || samples.length === 0) return;
        const seq = ++inputFrameSequence;
        for (const wc of inputFrameSubscribers) {
            if (wc.isDestroyed()) {
                inputFrameSubscribers.delete(wc);
                continue;
            }
            try {
                // Electron's structured-clone IPC copies the typed array
                // per recipient, so each subscriber gets its own buffer
                // — no shared-state worries. The try/catch covers the
                // race window between isDestroyed() and send(): a
                // WebContents can transition to destroyed concurrently,
                // and send() throws on a torn-down target.
                wc.send('audio:inputFrame', { samples, seq });
            } catch {
                inputFrameSubscribers.delete(wc);
            }
        }
        // Cleared destroyed subscribers above may have emptied the set;
        // collapse the timer if so.
        if (inputFrameSubscribers.size === 0) stopInputFrameTimer();
    }

    function ensureInputFrameTimer(): void {
        if (inputFrameTimer || inputFrameSubscribers.size === 0) return;
        inputFrameTimer = setInterval(dispatchInputFrames, INPUT_FRAME_INTERVAL_MS);
    }

    function stopInputFrameTimer(): void {
        if (inputFrameTimer && inputFrameSubscribers.size === 0) {
            clearInterval(inputFrameTimer);
            inputFrameTimer = null;
        }
    }

    function removeInputFrameSubscriber(wc: WebContents): void {
        if (inputFrameSubscribers.delete(wc)) {
            stopInputFrameTimer();
        }
    }

    ipcMain.on('audio:subscribeInputFrames', (event) => {
        const wc = event.sender;
        // Refuse the subscription if the engine isn't available — better
        // to leave the renderer to retry on next session start than to
        // keep an indefinitely-stalled subscriber. The plugin's
        // feature-detect can also probe `audio:isAvailable` first to
        // avoid this path entirely.
        if (!audio) return;
        if (inputFrameSubscribers.has(wc)) return;
        inputFrameSubscribers.add(wc);
        // Attach the destroyed-cleanup listener at most once per
        // WebContents lifetime — without the WeakSet guard, repeated
        // subscribe/unsubscribe cycles (plugin mount/unmount) would
        // accumulate one-shot listeners that never fire until window
        // close.
        if (!inputFrameDestroyHooked.has(wc)) {
            inputFrameDestroyHooked.add(wc);
            wc.once('destroyed', () => removeInputFrameSubscriber(wc));
        }
        ensureInputFrameTimer();
    });

    ipcMain.on('audio:unsubscribeInputFrames', (event) => {
        removeInputFrameSubscriber(event.sender);
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
    // Drop input-frame subscribers and stop the dispatch timer before
    // tearing down the engine — once `audio` is null the dispatch
    // callback would clear them itself, but stopping the timer here is
    // an unconditional one-shot (we're never re-init'ing in the same
    // process, so the stop-timer helper's empty-set guard doesn't add
    // value here).
    inputFrameSubscribers.clear();
    if (inputFrameTimer) {
        clearInterval(inputFrameTimer);
        inputFrameTimer = null;
    }
    if (audio) {
        try {
            audio.shutdown();
            try { console.log('[audio] Engine shut down'); } catch { /* console may be gone */ }
        } catch { /* silent fail during shutdown */ }
    }
}
