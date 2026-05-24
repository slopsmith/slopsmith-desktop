// Preload script — exposes safe APIs to the Slopsmith webview.
// The existing Slopsmith frontend runs unchanged; this adds
// window.slopsmithDesktop for audio engine and desktop features.

const { contextBridge, ipcRenderer } = require('electron');
import type { StartupStatus } from './python';
import {
    IPC_STARTUP_STATUS,
    IPC_STARTUP_GET_STATUS,
    IPC_STARTUP_REQUEST_STATUS,
    IPC_UPDATE_GET_STATUS,
    IPC_UPDATE_SET_CHANNEL,
    IPC_UPDATE_CHECK_NOW,
    IPC_UPDATE_APPLY,
    IPC_UPDATE_EVENT_AVAILABLE,
    IPC_UPDATE_EVENT_DOWNLOADED,
} from './ipc-channels';

// Auto-update channel + event payloads. Kept here (rather than re-exported
// from update-manager.ts) so the preload bundle doesn't drag in the Velopack
// SDK — preload runs in a restricted context and we don't want native
// require()s evaluated here.
export type UpdateChannel = 'stable' | 'rc' | 'beta' | 'alpha';
export interface UpdateAvailablePayload { version: string; channel: UpdateChannel }
export interface UpdateDownloadedPayload { version: string; channel: UpdateChannel }

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

// One note in a chart pushed to the engine for continuous verification.
export interface ChartNote {
    id: string;        // stable note key (matches the plugin's noteKey)
    t: number;         // chart time of the onset, seconds
    s: number;         // string index
    f: number;         // fret
    sus: number;       // sustain length, seconds
    ho?: boolean;      // hammer-on
    po?: boolean;      // pull-off
    b?: boolean;       // bend
    sl?: boolean;      // slide
    hm?: boolean;      // harmonic
}
// The full song chart + scoring context, pushed once per arrangement load.
export interface ChartUpdate {
    arrangement?: 'guitar' | 'bass';
    stringCount?: number;
    tuningOffsets: number[];
    capo?: number;
    pitchCheckCents?: number;
    harmonicSnr?: number;
    timingTolerance?: number;  // seconds — half-width of the scoring window
    notes: ChartNote[];
}
// A finalized per-note verdict drained from the engine's NoteVerifier.
export interface NoteVerdict {
    id: string;
    detected: boolean;
    detectedSongTime: number;
    centsError: number;
    snr: number;
}

// Raw polyphonic transcription from the ML note detector (Basic Pitch).
export interface DetectedNote {
    midi: number;       // MIDI pitch, 21..108
    confidence: number; // note posteriorgram, 0..1
    onsetMs: number;    // ms since this pitch's onset (large = sustained tail)
    onsetSeq: number;   // per-pitch onset counter; a change == a new note struck
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
        loadDeviceSettings: () => ipcRenderer.invoke('audio:loadDeviceSettings'),
        saveDeviceSettings: (settings: unknown) => ipcRenderer.invoke('audio:saveDeviceSettings', settings),

        // Audio control
        startAudio: () => ipcRenderer.invoke('audio:startAudio'),
        stopAudio: () => ipcRenderer.invoke('audio:stopAudio'),
        isAudioRunning: () => ipcRenderer.invoke('audio:isAudioRunning'),

        // Gain
        setGain: (which: string, value: number) => ipcRenderer.invoke('audio:setGain', which, value),
        setInputChannel: (channel: number) => ipcRenderer.invoke('audio:setInputChannel', channel),
        setMonitorMute: (mute: boolean) => ipcRenderer.invoke('audio:setMonitorMute', mute),
        setMonitorMuteSuppressed: (suppressed: boolean) =>
            ipcRenderer.invoke('audio:setMonitorMuteSuppressed', suppressed),
        isMonitorMuted: () => ipcRenderer.invoke('audio:isMonitorMuted'),
        setNoiseGate: (payload: {
            enabled: boolean;
            thresholdDb: number;
            releaseMs: number;
            depthDb: number;
        }) => ipcRenderer.invoke('audio:setNoiseGate', payload),
        setTonePolish: (payload: { enabled: boolean }) =>
            ipcRenderer.invoke('audio:setTonePolish', payload),

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

        // Push the song's note chart into the engine for continuous,
        // background verification. The notedetect plugin calls this once per
        // arrangement load; the engine's NoteVerifier thread scores each note
        // against the live playhead, replacing the renderer's per-tick
        // scoreChord loop. Resolves null on a downlevel addon (pre-NoteVerifier)
        // so the caller feature-detects and keeps the old matchNotes path.
        setChart: (chart: ChartUpdate): Promise<boolean | null> =>
            ipcRenderer.invoke('audio:setChart', chart),

        // Drain the verdicts the engine's NoteVerifier has finalized since the
        // last call. Resolves null on a downlevel addon so the caller
        // feature-detects. The optional (songTime, playing) args push the
        // renderer's unified, already-corrected playhead — the verifier scores
        // against this rather than the JUCE backing transport (frozen for
        // HTML5-routed sloppak songs). Pass them every detect tick.
        getNoteVerdicts: (songTime?: number, playing?: boolean): Promise<NoteVerdict[] | null> =>
            ipcRenderer.invoke('audio:getNoteVerdicts', songTime, playing),

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
        setSlotState: (slotId: number, base64State: string): Promise<boolean> =>
            ipcRenderer.invoke('audio:setSlotState', slotId, base64State),

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
        setBackingSpeed: (speed: number): Promise<boolean> => ipcRenderer.invoke('audio:setBackingSpeed', speed),
        setBackingPreservePitch: (preserve: boolean): Promise<boolean> =>
            ipcRenderer.invoke('audio:setBackingPreservePitch', preserve),

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

    // Auto-update (Velopack). The Settings panel reads/writes
    // localStorage['slopsmith-update-channel'] and mirrors it via setChannel.
    // Linux short-circuits to { status: "unsupported", platform: "linux" } on
    // every call — renderer should branch on that and surface a "download
    // from Releases" note rather than disabling the panel entirely.
    update: {
        getStatus: () => ipcRenderer.invoke(IPC_UPDATE_GET_STATUS),
        setChannel: (channel: UpdateChannel) => ipcRenderer.invoke(IPC_UPDATE_SET_CHANNEL, channel),
        checkNow: () => ipcRenderer.invoke(IPC_UPDATE_CHECK_NOW),
        apply: () => ipcRenderer.invoke(IPC_UPDATE_APPLY),
        onAvailable: (callback: (payload: UpdateAvailablePayload) => void) => {
            const listener = (_event: unknown, payload: UpdateAvailablePayload) => callback(payload);
            ipcRenderer.on(IPC_UPDATE_EVENT_AVAILABLE, listener);
            return () => ipcRenderer.removeListener(IPC_UPDATE_EVENT_AVAILABLE, listener);
        },
        onDownloaded: (callback: (payload: UpdateDownloadedPayload) => void) => {
            const listener = (_event: unknown, payload: UpdateDownloadedPayload) => callback(payload);
            ipcRenderer.on(IPC_UPDATE_EVENT_DOWNLOADED, listener);
            return () => ipcRenderer.removeListener(IPC_UPDATE_EVENT_DOWNLOADED, listener);
        },
    },
});

export {};
