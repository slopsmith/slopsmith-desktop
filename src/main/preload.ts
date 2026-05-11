// Preload script — exposes safe APIs to the Slopsmith webview.
// The existing Slopsmith frontend runs unchanged; this adds
// window.slopsmithDesktop for audio engine and desktop features.

const { contextBridge, ipcRenderer } = require('electron');
import type { StartupStatus } from './python';
import { IPC_STARTUP_STATUS, IPC_STARTUP_GET_STATUS, IPC_STARTUP_REQUEST_STATUS } from './ipc-channels';

// Ref count of active `audio:inputFrame` listeners installed via
// `slopsmithDesktop.audio.onInputFrame`. Used to deduplicate the
// per-renderer subscribe/unsubscribe IPC so the main process sees one
// subscribe on the first listener and one unsubscribe when the last
// goes away.
let inputFrameListenerCount = 0;

// Audio sync offset — set as a mutable property via the isolated world bridge.
// The settings panel reads/writes localStorage and updates this at runtime.

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

        // Pitch detection (polled)
        getPitchDetection: () => ipcRenderer.invoke('audio:getPitchDetection'),

        // Current engine sample rate — needed by notedetect's chord
        // scorer to map FFT bins to Hz on the bridge path (no
        // AudioContext to read it from). Queried once at startAudio.
        getSampleRate: (): Promise<number> => ipcRenderer.invoke('audio:getSampleRate'),

        // Raw input frame stream — pushed every ~50ms from the audio
        // engine's lock-free ring buffer. Used by notedetect's
        // polyphonic chord scorer (`_ndScoreChord`), which needs the
        // actual audio samples rather than a derived metric.
        //
        // Returns an unsubscribe closure. Listeners are ref-counted at
        // this preload boundary: the first one triggers an IPC
        // `subscribe`, the last unsubscribe triggers `unsubscribe`.
        // The main process treats this renderer as a single subscriber
        // regardless of how many in-renderer listeners we have, so we
        // can't just forward each install/uninstall as a sub/unsub.
        onInputFrame: (
            callback: (frame: { samples: Float32Array; seq: number }) => void,
        ): (() => void) => {
            const listener = (
                _event: unknown,
                frame: { samples: Float32Array; seq: number },
            ) => callback(frame);
            ipcRenderer.on('audio:inputFrame', listener);
            inputFrameListenerCount += 1;
            if (inputFrameListenerCount === 1) {
                ipcRenderer.send('audio:subscribeInputFrames');
            }
            // Idempotent unsubscribe — calling twice on the same handle
            // must NOT double-decrement the ref-count, or we'd send a
            // stray `unsubscribe` while other listeners are still live.
            let removed = false;
            return () => {
                if (removed) return;
                removed = true;
                ipcRenderer.removeListener('audio:inputFrame', listener);
                inputFrameListenerCount -= 1;
                if (inputFrameListenerCount === 0) {
                    ipcRenderer.send('audio:unsubscribeInputFrames');
                }
            };
        },

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
