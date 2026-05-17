// Preload script — exposes safe APIs to the Slopsmith webview.
// The existing Slopsmith frontend runs unchanged; this adds
// window.slopsmithDesktop for audio engine and desktop features.

const { contextBridge, ipcRenderer } = require('electron');
import type { StartupStatus } from './python';
import { IPC_STARTUP_STATUS, IPC_STARTUP_GET_STATUS, IPC_STARTUP_REQUEST_STATUS } from './ipc-channels';

// Audio sync offset — set as a mutable property via the isolated world bridge.
// The settings panel reads/writes localStorage and updates this at runtime.

// Polyphonic chord-scoring request/response contract. Mirrors the C++
// ChordScorer::Request / Result structs and the JS _ndScoreChord
// payload — kept in sync by hand since N-API doesn't generate
// bindings. Optional `mt` field on Note isn't read by the scorer
// (matches JS) but is allowed in the request shape so callers can
// pass through the same chart-note objects they consume in JS.
export interface ChordScoreNote {
    s: number;   // 0-based string index
    f: number;   // fret
    mt?: number; // mute flag (ignored by scorer)
    ho?: boolean; // hammer-on
    po?: boolean; // pull-off
    b?: boolean;  // bend
    sl?: boolean; // slide
    hm?: boolean; // harmonic (energy-only check)
}
export interface ChordScoreRequest {
    // arrangement and stringCount are optional on the wire — the
    // native parser defaults them to 'guitar' / 6 when omitted, so a
    // request that supplies six standard-tuning offsets and notes
    // can leave them out entirely. They become effectively required
    // only when you want to score a non-default tuning (any bass
    // arrangement, or a 7/8-string guitar), since `offsets.length`
    // must equal `stringCount` for the scorer's validation to pass.
    arrangement?: 'guitar' | 'bass';
    stringCount?: number;
    offsets: number[];        // tuning offsets, length must equal stringCount (default 6)
    capo?: number;
    pitchCheckCents?: number; // 0 = energy-only chord check
    minHitRatio?: number;
    numSamples?: number;      // override the 4096 default window
    notes: ChordScoreNote[];
}
export interface ChordScoreNoteResult {
    s: number;
    f: number;
    hit: boolean;
    bandEnergy: number;
    centsDiff: number | null;
    centsError: number | null;
}
export interface ChordScoreResult {
    score: number;
    hitStrings: number;
    totalStrings: number;
    isHit: boolean;
    results: ChordScoreNoteResult[];
}

// Raw polyphonic transcription from the ML note detector (Basic Pitch).
export interface DetectedNote {
    midi: number;       // MIDI pitch, 21..108
    confidence: number; // note posteriorgram, 0..1
    onsetMs: number;    // ms since this pitch's onset (large = sustained tail)
}
export interface NoteDetection {
    notes: DetectedNote[];
    sampleRate: number;
}

contextBridge.exposeInMainWorld('slopsmithDesktop', {
    // Platform detection
    isDesktop: true,
    platform: process.platform,

    // Audio engine
    audio: {
        isAvailable: () => ipcRenderer.invoke('audio:isAvailable'),

        // Device management
        getDeviceTypes: () => ipcRenderer.invoke('audio:getDeviceTypes'),
        getSampleRates: () => ipcRenderer.invoke('audio:getSampleRates'),
        getBufferSizes: () => ipcRenderer.invoke('audio:getBufferSizes'),
        probeDeviceOptions: (typeName: string, input: string, output: string) =>
            ipcRenderer.invoke('audio:probeDeviceOptions', typeName, input, output),
        getCurrentDevice: () => ipcRenderer.invoke('audio:getCurrentDevice'),
        setDeviceType: (typeName: string) => ipcRenderer.invoke('audio:setDeviceType', typeName),
        setDevice: (input: string, output: string, sampleRate: number, bufferSize: number) =>
            ipcRenderer.invoke('audio:setDevice', input, output, sampleRate, bufferSize),

        // Audio control
        startAudio: () => ipcRenderer.invoke('audio:startAudio'),
        stopAudio: () => ipcRenderer.invoke('audio:stopAudio'),
        isAudioRunning: () => ipcRenderer.invoke('audio:isAudioRunning'),

        // Gain
        setGain: (which: string, value: number) => ipcRenderer.invoke('audio:setGain', which, value),
        setInputChannel: (channel: number) => ipcRenderer.invoke('audio:setInputChannel', channel),
        setMonitorMute: (mute: boolean) => ipcRenderer.invoke('audio:setMonitorMute', mute),
        isMonitorMuted: () => ipcRenderer.invoke('audio:isMonitorMuted'),
        setNoiseGate: (payload: {
            enabled: boolean;
            thresholdDb: number;
            releaseMs: number;
            depthDb: number;
        }) => ipcRenderer.invoke('audio:setNoiseGate', payload),

        // Metering (polled at 60fps from renderer)
        getLevels: () => ipcRenderer.invoke('audio:getLevels'),
        resetPeaks: () => ipcRenderer.invoke('audio:resetPeaks'),

        // Pitch detection (polled). Backed by the polyphonic ML detector
        // (Basic Pitch) when a model is loaded, else the YIN detector —
        // same result shape either way.
        getPitchDetection: () => ipcRenderer.invoke('audio:getPitchDetection'),

        // Whether the ML note detector (Basic Pitch) is active vs. the YIN
        // fallback. Resolves false on a downlevel addon.
        isMlNoteDetection: (): Promise<boolean> => ipcRenderer.invoke('audio:isMlNoteDetection'),

        // Current engine sample rate — needed by notedetect's chord
        // scorer to map FFT bins to Hz on the bridge path (no
        // AudioContext to read it from). Queried once at startAudio.
        getSampleRate: (): Promise<number> => ipcRenderer.invoke('audio:getSampleRate'),

        // Polyphonic chord scoring — native ChordScorer in the audio
        // engine evaluates the chord context against the most recent
        // input-ring samples and returns the same
        // `{ score, hitStrings, totalStrings, isHit, results[] }` shape
        // the in-renderer `_ndScoreChord` produces. No audio buffers
        // cross IPC; only the small result object does. Returns `null`
        // on a downlevel addon that predates ChordScorer so the caller
        // can fall back gracefully.
        scoreChord: (ctx: ChordScoreRequest): Promise<ChordScoreResult | null> =>
            ipcRenderer.invoke('audio:scoreChord', ctx),

        // Raw polyphonic transcription — the ML note detector's full
        // active-pitch set. Resolves null when the ML detector isn't active
        // (downlevel addon, ONNX support absent, or no model loaded) so the
        // caller feature-detects and falls back to getPitchDetection /
        // scoreChord.
        detectNotes: (): Promise<NoteDetection | null> =>
            ipcRenderer.invoke('audio:detectNotes'),

        // VST plugins
        scanPlugins: (dirs?: string[]) => ipcRenderer.invoke('audio:scanPlugins', dirs),
        getKnownPlugins: () => ipcRenderer.invoke('audio:getKnownPlugins'),
        savePluginList: (path?: string) => ipcRenderer.invoke('audio:savePluginList', path),
        loadPluginList: (path?: string) => ipcRenderer.invoke('audio:loadPluginList', path),

        // Signal chain
        loadVST: (pluginPath: string) => ipcRenderer.invoke('audio:loadVST', pluginPath),
        loadNAMModel: (modelPath: string) => ipcRenderer.invoke('audio:loadNAMModel', modelPath),
        loadIR: (irPath: string) => ipcRenderer.invoke('audio:loadIR', irPath),
        removeProcessor: (slotId: number) => ipcRenderer.invoke('audio:removeProcessor', slotId),
        moveProcessor: (from: number, to: number) => ipcRenderer.invoke('audio:moveProcessor', from, to),
        setBypass: (slotId: number, bypassed: boolean) => ipcRenderer.invoke('audio:setBypass', slotId, bypassed),
        clearChain: () => ipcRenderer.invoke('audio:clearChain'),
        getChainState: () => ipcRenderer.invoke('audio:getChainState'),

        // Plugin editor
        openPluginEditor: (slotId: number) => ipcRenderer.invoke('audio:openPluginEditor', slotId),
        closePluginEditor: (slotId: number) => ipcRenderer.invoke('audio:closePluginEditor', slotId),

        // Parameters
        getParameters: (slotId: number) => ipcRenderer.invoke('audio:getParameters', slotId),
        setParameter: (slotId: number, paramIndex: number, value: number) =>
            ipcRenderer.invoke('audio:setParameter', slotId, paramIndex, value),

        // MIDI
        sendMidiToSlot: (slotId: number, msgType: number, channel: number, param1: number, param2?: number) =>
            ipcRenderer.invoke('audio:sendMidiToSlot', slotId, msgType, channel, param1, param2),

        // Backing track
        loadBackingTrack: (filePath: string) => ipcRenderer.invoke('audio:loadBackingTrack', filePath),
        startBacking: () => ipcRenderer.invoke('audio:startBacking'),
        stopBacking: () => ipcRenderer.invoke('audio:stopBacking'),
        seekBacking: (seconds: number) => ipcRenderer.invoke('audio:seekBacking', seconds),
        getBackingPosition: (): Promise<number> => ipcRenderer.invoke('audio:getBackingPosition'),
        getBackingDuration: (): Promise<number> => ipcRenderer.invoke('audio:getBackingDuration'),
        isBackingPlaying: (): Promise<boolean> => ipcRenderer.invoke('audio:isBackingPlaying'),

        // Presets
        savePreset: () => ipcRenderer.invoke('audio:savePreset'),
        loadPreset: (presetJson: string) => ipcRenderer.invoke('audio:loadPreset', presetJson),

        // Tone switching
        setMultiBypass: (changes: Array<{slotId: number, bypassed: boolean}>) =>
            ipcRenderer.invoke('audio:setMultiBypass', changes),
    },

    // Plugin management
    plugins: {
        install: (gitUrl: string, name?: string) => ipcRenderer.invoke('plugins:install', gitUrl, name),
        remove: (name: string) => ipcRenderer.invoke('plugins:remove', name),
        update: (name: string) => ipcRenderer.invoke('plugins:update', name),
        listInstalled: () => ipcRenderer.invoke('plugins:listInstalled'),
    },

    // Soundfont (Audio Quality preference for GP5 → audio rendering)
    soundfont: {
        getStatus: () => ipcRenderer.invoke('soundfont:getStatus'),
        downloadHighQuality: () => ipcRenderer.invoke('soundfont:downloadHighQuality'),
        cancelDownload: () => ipcRenderer.invoke('soundfont:cancelDownload'),
        setQuality: (quality: 'default' | 'high') => ipcRenderer.invoke('soundfont:setQuality', quality),
        onDownloadProgress: (
            callback: (progress: { bytesDownloaded: number; totalBytes: number; percent: number }) => void,
        ) => {
            const listener = (_event: unknown, progress: { bytesDownloaded: number; totalBytes: number; percent: number }) => callback(progress);
            ipcRenderer.on('soundfont:downloadProgress', listener);
            return () => ipcRenderer.removeListener('soundfont:downloadProgress', listener);
        },
    },

    // File dialogs
    pickFile: (filters?: { name: string; extensions: string[] }[]) =>
        ipcRenderer.invoke('dialog:pickFile', filters),
    pickDirectory: () => ipcRenderer.invoke('dialog:pickDirectory'),
    pickFiles: (filters?: { name: string; extensions: string[] }[]) =>
        ipcRenderer.invoke('dialog:pickFiles', filters),

    // App info
    getInfo: () => ipcRenderer.invoke('app:getInfo'),
    getConfigDir: () => ipcRenderer.invoke('app:getConfigDir'),

    // Startup status
    startup: {
        getStatus: () => ipcRenderer.invoke(IPC_STARTUP_GET_STATUS),
        onStatus: (callback: (status: StartupStatus) => void) => {
            const listener = (_event: unknown, status: StartupStatus) => callback(status);
            ipcRenderer.on(IPC_STARTUP_STATUS, listener);
            ipcRenderer.send(IPC_STARTUP_REQUEST_STATUS);
            return () => ipcRenderer.removeListener(IPC_STARTUP_STATUS, listener);
        },
    },
});

export {};
