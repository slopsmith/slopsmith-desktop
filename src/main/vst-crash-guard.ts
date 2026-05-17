// VST crash guard — a dead-man's-pedal that learns which VST3s crash the app.
//
// Some plugins fault when loaded or when their editor is opened in-process
// (a common cause: an editor that must be created on the OS main thread,
// which the audio addon can't provide because Electron owns it). We can't
// predict which plugins are affected. So: before each risky in-process VST
// op, drop a sentinel file naming the plugin; clear it once the op has
// demonstrably survived. If a sentinel is still on disk at the next startup,
// that plugin took the app down — record it in a persistent blocklist. The
// blocklist is handed to the addon, which then routes those plugins through
// the out-of-process sandbox (slopsmith-vst-host.exe).

import { app } from 'electron';
import * as fs from 'fs';
import * as path from 'path';

// Editor creation is asynchronous on the addon's message thread, so there is
// no synchronous success signal. Clear the sentinel after this grace window —
// if the app is still alive by then, the editor didn't fault.
const EDITOR_GRACE_MS = 6000;

let sentinelPath = '';
let blocklistPath = '';
const blocklist = new Set<string>();

// The pending editor-grace timer, if any. Tracked so a second editor-open
// cancels the first's timer — otherwise the older timer would clear the
// newer sentinel early and a crash from the second plugin would go
// unrecorded.
let editorTimer: ReturnType<typeof setTimeout> | null = null;

// Windows VST paths are case-insensitive; normalise so the addon's
// case-insensitive match and ours agree.
const norm = (p: string): string => p.trim().toLowerCase();

// Run once at startup, before any VST can be loaded. Promotes a leftover
// sentinel (= the app crashed mid-op last run) into the persistent blocklist,
// then returns the full blocklist for handing to the addon.
export function initVstCrashGuard(): string[] {
    const dir = app.getPath('userData');
    sentinelPath = path.join(dir, 'vst-load-sentinel.json');
    blocklistPath = path.join(dir, 'vst-crash-blocklist.json');

    try {
        const raw = JSON.parse(fs.readFileSync(blocklistPath, 'utf8'));
        if (Array.isArray(raw))
            for (const p of raw) if (typeof p === 'string' && p) blocklist.add(norm(p));
    } catch { /* no blocklist yet — fine */ }

    try {
        const s = JSON.parse(fs.readFileSync(sentinelPath, 'utf8'));
        if (s && typeof s.plugin === 'string' && s.plugin) {
            blocklist.add(norm(s.plugin));
            console.warn(`[vst-crash-guard] ${s.plugin} crashed the app during `
                + `'${s.op}' last run — it will load sandboxed from now on`);
            persist();
        }
    } catch { /* no sentinel — the app exited cleanly last run */ }
    clearSentinel();

    // A clean shutdown is, by definition, not a crash. If the app quits while
    // an editor grace-window sentinel is still armed (the unref'd timer
    // hasn't fired yet), disarm it here so the plugin isn't falsely
    // blocklisted on next launch — only an abrupt termination should leave a
    // sentinel. disarmSentinel also cancels the pending timer so no stale
    // callback can fire mid-shutdown.
    app.once('will-quit', () => disarmSentinel());

    return [...blocklist];
}

function persist(): void {
    try {
        fs.writeFileSync(blocklistPath, JSON.stringify([...blocklist], null, 2));
    } catch (e: any) {
        console.warn(`[vst-crash-guard] could not persist blocklist: ${e?.message}`);
    }
}

function clearSentinel(): void {
    try {
        if (sentinelPath) fs.rmSync(sentinelPath, { force: true });
    } catch { /* best-effort */ }
}

// Drop the sentinel just before a risky in-process VST op. A plain
// writeFileSync is enough: an access violation kills the process but not the
// OS page cache, so the file survives for the next startup to find.
//
// Cancels any pending editor-grace timer first: that timer would otherwise
// clear this fresh sentinel mid-op (e.g. an editor-open followed by a load
// inside the grace window), losing the crash attribution.
export function armSentinel(pluginPath: string, op: 'load' | 'editor'): void {
    if (!pluginPath || !sentinelPath) return;
    if (editorTimer) { clearTimeout(editorTimer); editorTimer = null; }
    try {
        fs.writeFileSync(sentinelPath,
            JSON.stringify({ plugin: pluginPath, op, at: Date.now() }));
    } catch { /* best-effort — a missed sentinel just means no auto-blocklist */ }
}

// Clear the sentinel once the op has survived (use after a synchronous load,
// or when an editor-open returns failure without ever creating a window).
// Also cancels a pending editor-grace timer so it can't fire stale later.
export function disarmSentinel(): void {
    if (editorTimer) { clearTimeout(editorTimer); editorTimer = null; }
    clearSentinel();
}

// Arm for an editor-open and self-clear after the grace window. armSentinel
// cancels any pending grace timer from an earlier editor-open, so an older
// timer can't clear this newer sentinel early. The timer is unref'd so it
// never holds the app open on its own.
export function armEditorSentinel(pluginPath: string): void {
    if (!pluginPath || !sentinelPath) return;
    armSentinel(pluginPath, 'editor');
    editorTimer = setTimeout(() => {
        editorTimer = null;
        disarmSentinel();
    }, EDITOR_GRACE_MS);
    editorTimer.unref();
}
