// Debug logging — opt-in diagnostic capture for bug reports.
//
// Enabled by the SLOPSMITH_DEBUG env var or a --verbose / --debug CLI flag.
// When on, console.* output is routed to <logs>/slopsmith-debug.log, and the
// native addon redirects its stderr into the same file (see audio-bridge.ts /
// NodeAddon enableFileLogging), so one file captures the Electron main
// process, the native [AudioEngine] diagnostics, and (forwarded as [python]
// lines) the Python subprocess.

import { app } from 'electron';
import * as fs from 'fs';
import * as path from 'path';
import * as util from 'util';

let debugEnabled: boolean | null = null;
let logFilePath: string | null = null;

export function isDebugEnabled(): boolean {
    if (debugEnabled !== null) return debugEnabled;
    const env = (process.env.SLOPSMITH_DEBUG || '').trim().toLowerCase();
    const envOn = env !== '' && env !== '0' && env !== 'false';
    const argOn = process.argv.includes('--verbose') || process.argv.includes('--debug');
    debugEnabled = envOn || argOn;
    return debugEnabled;
}

// <logs>/slopsmith-debug.log — Windows: %APPDATA%\<app>\logs, macOS:
// ~/Library/Logs/<app>, Linux: ~/.config/<app>/logs.
export function getDebugLogPath(): string {
    if (logFilePath) return logFilePath;
    logFilePath = path.join(app.getPath('logs'), 'slopsmith-debug.log');
    return logFilePath;
}

// Truncate the log with a fresh header and route every console.* call into it
// via fs.appendFileSync. Writing directly to the file (rather than relying on
// the native stderr redirect) captures the JS-side logs from the moment this
// runs — the startup banner and early Python output, before the addon is even
// loaded. The addon separately redirects native stderr into the same file
// (enableFileLogging) for the [AudioEngine] diagnostics.
// Returns the log path when debug mode is on, otherwise null.
export function initDebugLogging(): string | null {
    if (!isDebugEnabled()) return null;

    const file = getDebugLogPath();
    try {
        fs.mkdirSync(path.dirname(file), { recursive: true });
        fs.writeFileSync(file, `=== Slopsmith debug log — ${new Date().toISOString()} ===\n`);
    } catch {
        // Can't open the log file — stay console-only rather than crash.
        return null;
    }

    // Debug mode → console.* is routed to the log file only, NOT also tee'd to
    // the original console. Deliberate: enableFileLogging freopen's the native
    // stderr stream onto this same file, so letting console.error/warn also
    // reach their original stderr could write those lines into the file twice.
    // A packaged build has no console anyway; a dev who wants live output can
    // tail the file. A transient write failure is swallowed so logging can't
    // take the app down.
    const writeLine = (...args: unknown[]) => {
        try {
            fs.appendFileSync(file, util.format(...args) + '\n');
        } catch {
            /* ignore log-write failures */
        }
    };
    console.log = writeLine;
    console.info = writeLine;
    console.debug = writeLine;
    console.warn = writeLine;
    console.error = writeLine;

    return file;
}
