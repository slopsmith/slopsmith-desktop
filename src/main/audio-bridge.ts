// Audio Bridge — connects the JUCE native addon to Electron IPC.
// The native addon is loaded via require() and its methods are
// exposed to the renderer process via ipcMain.handle().

import { ipcMain } from 'electron';
import * as path from 'path';
import { app } from 'electron';
import { isDebugEnabled, getDebugLogPath } from './debug-log';
import { initVstCrashGuard, armSentinel, disarmSentinel, armEditorSentinel } from './vst-crash-guard';

type AudioModule = Record<string, (...args: any[]) => any>;

let audio: AudioModule | null = null;

// slotId → VST3 path, populated by audio:loadVST. Lets audio:openPluginEditor
// resolve a slot's plugin path for the crash sentinel without a native
// getChainState call. Kept in sync on remove/clear; slot ids are stable
// across moves so a reorder needs no upkeep.
const vstSlotPaths = new Map<number, string>();

// Plugin editors currently believed to be open: slotId → pluginPath. Map
// insertion order is preserved, so the most-recently-(re)inserted entry is
// "most recently opened" — that's the editor whose path the crash sentinel
// is armed for. Multi-editor case: opening B after A puts B on top; if B
// closes, the sentinel re-arms for A; if A then closes too, sentinel is
// disarmed. Native windows that vanish outside IPC (OS title-bar X, async
// createEditor that returned null) are caught by the periodic watcher.
const openEditors = new Map<number, string>();

// Slots whose isPluginEditorOpen has returned true at least once. The
// watcher only prunes confirmed entries that have flipped back to
// not-open — without this, a slow createEditor (audio:openPluginEditor
// returns before createEditor finishes on the JUCE message thread) could
// be pruned by the very next watcher tick.
const confirmedEditors = new Set<number>();

// Per-slot timestamp of when the open IPC was acknowledged. Used to bound
// the "pending" state (in openEditors but not yet confirmed): if createEditor
// never produces a window (e.g. it silently returned null in the async
// lambda), the pending entry is dropped after PENDING_TIMEOUT_MS so the
// sentinel doesn't stay armed forever.
const openedAt = new Map<number, number>();
const PENDING_TIMEOUT_MS = 30000;

// Maximum time an entry stays in openEditors when the native addon doesn't
// expose isPluginEditorOpen (older builds — version skew at dev time). After
// this we give up and drop the entry, accepting a missed late-crash rather
// than leaving a sentinel armed forever.
const EDITOR_GRACE_MS_NO_NATIVE_QUERY = 6000;

// Refresh the on-disk sentinel from the current openEditors state. Arms for
// the most-recent entry, or disarms when no editor is open.
function rearmSentinelForMostRecentEditor(): void {
    if (openEditors.size === 0) {
        disarmSentinel();
        return;
    }
    // Map iteration is in insertion order; the last value yielded is the
    // most-recent (re-)insertion.
    let mostRecentPath: string | null = null;
    for (const path of openEditors.values()) mostRecentPath = path;
    if (mostRecentPath) armEditorSentinel(mostRecentPath);
}

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

    // VST crash guard: promote any leftover crash sentinel into the blocklist,
    // then hand the blocklist to the addon so it sandboxes those plugins.
    if (audio) {
        try {
            const blocked = initVstCrashGuard();
            if (typeof audio.setCrashedPlugins === 'function')
                audio.setCrashedPlugins(blocked);
            if (blocked.length)
                console.log(`[audio] ${blocked.length} VST(s) on the crash blocklist — will load sandboxed`);
        } catch (e: any) {
            console.warn(`[audio] VST crash guard init failed: ${e.message}`);
        }

        // Periodic editor-state watcher: drop entries in openEditors whose
        // window has vanished outside IPC (OS title-bar X close; async
        // createEditor that returned null). To avoid pruning slow async
        // creations, only prune entries that have been *confirmed* open at
        // least once. Pending entries are bounded by PENDING_TIMEOUT_MS so
        // a never-confirming open doesn't leave the sentinel armed forever.
        // Unref'd so it never holds the process open on its own.
        const editorWatcher = setInterval(() => {
            if (openEditors.size === 0) return;
            if (typeof audio?.isPluginEditorOpen !== 'function') return;
            const now = Date.now();
            let changed = false;
            for (const slotId of [...openEditors.keys()]) {
                let isOpen = false;
                try {
                    isOpen = !!audio.isPluginEditorOpen(slotId);
                } catch { continue; }
                if (isOpen) {
                    confirmedEditors.add(slotId);
                    continue;
                }
                if (confirmedEditors.has(slotId)) {
                    // Was confirmed open, now closed — the user clicked the
                    // native X (or some other off-IPC close).
                    openEditors.delete(slotId);
                    confirmedEditors.delete(slotId);
                    openedAt.delete(slotId);
                    changed = true;
                    continue;
                }
                // Pending — give createEditor a generous window.
                const t0 = openedAt.get(slotId) ?? now;
                if (now - t0 > PENDING_TIMEOUT_MS) {
                    openEditors.delete(slotId);
                    openedAt.delete(slotId);
                    changed = true;
                }
            }
            if (changed) rearmSentinelForMostRecentEditor();
        }, 3000);
        editorWatcher.unref();
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
        // Coerce to a real boolean so an unexpected non-boolean caller can't
        // make the N-API binding throw on As<Napi::Boolean>().
        if (audio && typeof audio.setMonitorMuteSuppressed === 'function') {
            audio.setMonitorMuteSuppressed(Boolean(suppressed));
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

    ipcMain.handle(
        'audio:setTonePolish',
        (_event, payload: { enabled: boolean }) => {
            if (audio && typeof audio.setTonePolish === 'function') {
                audio.setTonePolish(payload);
            }
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

    // Push the song's note chart into the engine for continuous, background
    // verification. The notedetect plugin calls this once per arrangement
    // load; the engine's NoteVerifier thread then scores each note against
    // the live playhead, replacing the renderer's per-tick scoreChord loop.
    // Returns null on a downlevel addon (pre-NoteVerifier) so the renderer
    // feature-detects and keeps the old matchNotes path.
    ipcMain.handle('audio:setChart', (_event, chart: unknown) => {
        if (!audio || typeof audio.setChart !== 'function') return null;
        try {
            return audio.setChart(chart);
        } catch (e: unknown) {
            console.warn(`[audio] setChart failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // Drain the verdicts the NoteVerifier thread has finalized since the last
    // call. Returns null on a downlevel addon so the renderer feature-detects.
    // The optional (songTime, playing) args push the renderer's unified
    // playhead — the plugin calls this once per detect tick, so the push rides
    // the same IPC as the drain.
    ipcMain.handle('audio:getNoteVerdicts', (_event, songTime: unknown, playing: unknown) => {
        if (!audio || typeof audio.getNoteVerdicts !== 'function') return null;
        try {
            if (typeof songTime === 'number' && Number.isFinite(songTime)
                && typeof playing === 'boolean') {
                return audio.getNoteVerdicts(songTime, playing);
            }
            return audio.getNoteVerdicts();
        } catch (e: unknown) {
            console.warn(`[audio] getNoteVerdicts failed: ${e instanceof Error ? e.message : String(e)}`);
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
        // Bracket the in-process load with the crash sentinel. The sentinel
        // detects a hard process abort: loadVST is synchronous, so a fault
        // means this call never returns and the `finally` never runs, leaving
        // the sentinel for the next startup to find. Any normal return OR a
        // thrown JS exception (loadVST throws on a required-sandbox spawn
        // failure — a clean error, not a crash) means the process survived.
        //
        // Note: the load sentinel overwrites whatever was on disk, including
        // a still-open editor's sentinel. The finally clears it, then rearms
        // for the most-recent still-open editor — otherwise a user loading
        // another plugin while an editor is open would silently drop crash
        // attribution for that editor.
        armSentinel(pluginPath, 'load');
        let slotId = -1;
        try {
            slotId = audio?.loadVST(pluginPath) ?? -1;
        } finally {
            disarmSentinel();
            rearmSentinelForMostRecentEditor();
        }
        if (slotId >= 0) vstSlotPaths.set(slotId, pluginPath);
        return slotId;
    });

    ipcMain.handle('audio:loadNAMModel', async (_event, modelPath: string) => {
        return await audio?.loadNAMModel(modelPath) ?? -1;
    });

    ipcMain.handle('audio:loadIR', async (_event, irPath: string) => {
        return await audio?.loadIR(irPath) ?? -1;
    });

    ipcMain.handle('audio:removeProcessor', (_event, slotId: number) => {
        audio?.removeProcessor(slotId);
        vstSlotPaths.delete(slotId);
        // The slot is gone — any editor it had is destroyed. Drop it from
        // the open-editors map and rearm for whatever's still open.
        const wasOpen = openEditors.delete(slotId);
        confirmedEditors.delete(slotId);
        openedAt.delete(slotId);
        if (wasOpen) rearmSentinelForMostRecentEditor();
    });

    ipcMain.handle('audio:moveProcessor', (_event, from: number, to: number) => {
        audio?.moveProcessor(from, to);
    });

    ipcMain.handle('audio:setBypass', (_event, slotId: number, bypassed: boolean) => {
        audio?.setBypass(slotId, bypassed);
    });

    ipcMain.handle('audio:clearChain', () => {
        audio?.clearChain();
        vstSlotPaths.clear();
        // Every editor window is destroyed — drop all open-editor entries
        // and disarm the sentinel.
        openEditors.clear();
        confirmedEditors.clear();
        openedAt.clear();
        rearmSentinelForMostRecentEditor();
    });

    ipcMain.handle('audio:getChainState', () => {
        return audio?.getChainState() ?? [];
    });

    // ── Plugin Editor ──────────────────────────────────────────────────────

    ipcMain.handle('audio:openPluginEditor', (_event, slotId: number) => {
        // Editor creation AND any later editor message dispatch is a fault
        // point (plugins whose editor needs the OS main thread, e.g.
        // AmpliTube, can crash long after open returns — observed at
        // ~95 s mid-interaction). Arm the sentinel with the slot's plugin
        // path and keep it armed for the editor's lifetime; the sentinel is
        // cleared by closePluginEditor / removeProcessor (same slot) or by
        // clearChain / will-quit. The path comes from the loadVST map first;
        // getChainState is only a fallback for slots created another way
        // (e.g. preset restore).
        let pluginPath = vstSlotPaths.get(slotId);
        if (!pluginPath) {
            const slot = (audio?.getChainState() ?? []).find((s: any) => s?.id === slotId);
            if (slot && typeof slot.path === 'string') pluginPath = slot.path;
        }
        // Pre-arm before the call to close the tiny window between
        // audio.openPluginEditor returning and JS recording the slot: the
        // queued createEditor on the JUCE message thread could fault almost
        // immediately, and without an on-disk sentinel the crash would go
        // un-attributed. On success/failure below, rearmSentinelForMostRecent-
        // Editor overwrites this with the correct value derived from the
        // openEditors map.
        if (pluginPath) armEditorSentinel(pluginPath);

        let opened = false;
        try {
            opened = audio?.openPluginEditor(slotId) ?? false;
        } catch (e) {
            // Clean failure (a thrown call is not a hard crash). Revert the
            // pre-arm to whatever the existing open editors should have
            // armed for.
            rearmSentinelForMostRecentEditor();
            throw e;
        }
        if (opened && pluginPath) {
            // Re-insert moves the slot to most-recent; rearm writes that
            // (same) path to the sentinel. No-op on disk but keeps the
            // map and file in sync.
            openEditors.delete(slotId);
            openEditors.set(slotId, pluginPath);
            openedAt.set(slotId, Date.now());
            rearmSentinelForMostRecentEditor();
            // Fallback for version skew: if the native addon predates the
            // isPluginEditorOpen export, the watcher can't prune entries on
            // its own, so bound the sentinel lifetime by a grace timer.
            // Loses late-crash attribution for the AmpliTube-style pattern
            // on downlevel addons, but avoids a sentinel armed forever.
            if (typeof audio?.isPluginEditorOpen !== 'function') {
                setTimeout(() => {
                    const wasOpen = openEditors.delete(slotId);
                    confirmedEditors.delete(slotId);
                    openedAt.delete(slotId);
                    if (wasOpen) rearmSentinelForMostRecentEditor();
                }, EDITOR_GRACE_MS_NO_NATIVE_QUERY).unref();
            }
        } else {
            // !opened: revert the pre-arm so a previous still-open editor's
            // attribution isn't overwritten by this slot's path.
            rearmSentinelForMostRecentEditor();
        }
        return opened;
    });

    ipcMain.handle('audio:closePluginEditor', (_event, slotId: number) => {
        const result = audio?.closePluginEditor(slotId) ?? false;
        // Only drop the entry when the native close actually closed an
        // editor. A false return can happen if close is requested before
        // the async createEditor callback inserted the window into
        // editorWindows — in that case the editor may still appear moments
        // later, so keep tracking it; the periodic watcher will prune it
        // if it never does.
        if (result && openEditors.delete(slotId)) {
            confirmedEditors.delete(slotId);
            openedAt.delete(slotId);
            rearmSentinelForMostRecentEditor();
        }
        return result;
    });

    // ── Parameters ─────────────────────────────────────────────────────────

    ipcMain.handle('audio:getParameters', (_event, slotId: number) => {
        return audio?.getParameters(slotId) ?? [];
    });

    ipcMain.handle('audio:setParameter', (_event, slotId: number, paramIndex: number, value: number) => {
        audio?.setParameter(slotId, paramIndex, value);
    });

    ipcMain.handle('audio:setSlotState', (_event, slotId: number, base64State: string): boolean => {
        // typeof-guarded so a downlevel addon is a no-op rather than a thrown
        // IPC error (Constitution VII fail-soft). Returns true when the native
        // addon supports the call (feature-detect signal — the preload always
        // exposes the method, so a renderer-side typeof check cannot tell a
        // downlevel addon apart). try/catch so an addon-side throw resolves
        // to false rather than rejecting the renderer's ipcRenderer.invoke.
        if (audio && typeof audio.setSlotState === 'function') {
            try {
                audio.setSlotState(slotId, base64State);
                return true;
            } catch (err) {
                console.warn('[audio-bridge] setSlotState threw:', err);
                return false;
            }
        }
        return false;
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
        const result = await audio?.loadPreset(presetJson) ?? { success: false, error: 'No audio' };
        // loadPreset rebuilds the native chain from scratch, so the cached
        // slotId→path map no longer reflects it. Clear it — openPluginEditor
        // then falls back to the live getChainState lookup for these slots
        // rather than trusting a stale (possibly id-reused) entry.
        vstSlotPaths.clear();
        return result;
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
