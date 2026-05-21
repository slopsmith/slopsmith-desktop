// Legacy NSIS → Velopack MSI cleanup, run from VelopackApp's after-install
// fast callback during MSI install.
//
// Why this exists: the legacy NSIS installer registered an uninstall key at
//   HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\Slopsmith
// and laid files into C:\Program Files\Slopsmith\. The new per-machine MSI
// installs to the same directory. Both can coexist — the MSI overwrites the
// root Slopsmith.exe with a Velopack stub and adds Update.exe + current\,
// leaving the rest of the NSIS files untouched — but if a user later
// uninstalls the legacy entry from Add/Remove Programs, NSIS would delete
// every file it tracked, including the path the MSI's stub now occupies,
// silently breaking the new install.
//
// This module removes that footgun by running the NSIS uninstaller silently
// at MSI install time and restoring the stub afterward.
//
// Constraints from running inside VelopackApp.onAfterInstallFastCallback:
//   - Synchronous void return. We can't await.
//   - 30-second budget; the process exits when the callback returns.
//   - Electron's `app` is NOT initialized — this is plain Node-in-Electron-exe.
//     Only fs / child_process / path are available; no BrowserWindow, no IPC.
//
// Timing of file overlap:
//   1. MSI's InstallFiles places everything (incl. Velopack stub at root).
//   2. MSI fires this hook.
//   3. We run NSIS uninstaller (silent /S /allusers, already elevated since
//      the MSI install owns the UAC token).
//   4. NSIS deletes every file it tracked, including the root Slopsmith.exe
//      (the MSI stub now lives at that path).
//   5. We copy current\Slopsmith.exe → root Slopsmith.exe to give the
//      Start Menu shortcut a working target again. The copy isn't the
//      Velopack execution stub specifically, but it is the real app exe
//      and launches the same app code Velopack would have routed to anyway.

import { spawnSync } from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

const NSIS_UNINSTALL_REG_KEY = 'HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Slopsmith';

// NSIS /S returns immediately (it copies itself to %TEMP% and runs detached),
// so we poll the registry as the real "uninstall finished" signal. The key
// is the last thing NSIS removes before exiting, so its absence is reliable.
const POLL_TIMEOUT_MS = 25_000;
const POLL_INTERVAL_MS = 500;

interface ParsedQuietUninstall {
    exe: string;
    args: string[];
}

function readNsisUninstaller(): ParsedQuietUninstall | null {
    // spawnSync with an args array (no shell) + windowsHide keeps the MSI
    // install UI clean — no console flash — and sidesteps shell quoting of
    // the registry key path. Mirrors isUninstallKeyGone() below.
    const proc = spawnSync('reg', ['query', NSIS_UNINSTALL_REG_KEY, '/v', 'QuietUninstallString'], {
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'ignore'],
        windowsHide: true,
    });
    // reg query exits non-zero when the key is absent (the common no-legacy-
    // install case); error is set if reg itself couldn't launch.
    if (proc.status !== 0 || proc.error || typeof proc.stdout !== 'string') {
        return null;
    }
    const raw = proc.stdout;
    // Output:
    //     QuietUninstallString    REG_SZ    "C:\Program Files\Slopsmith\Uninstall Slopsmith.exe" /allusers /S
    const match = raw.match(/QuietUninstallString\s+REG_SZ\s+(.+)/);
    if (!match) return null;
    const value = match[1].trim();
    // The exe path is always quoted in NSIS output; if it isn't, treat as
    // malformed and bail rather than guess.
    const exeMatch = value.match(/^"([^"]+)"\s*(.*)$/);
    if (!exeMatch) return null;
    const args = exeMatch[2].trim();
    return {
        exe: exeMatch[1],
        args: args.length > 0 ? args.split(/\s+/) : [],
    };
}

function isUninstallKeyGone(): boolean {
    const result = spawnSync('reg', ['query', NSIS_UNINSTALL_REG_KEY], {
        stdio: ['ignore', 'ignore', 'ignore'],
        windowsHide: true,
    });
    // reg query exits 0 when the key is present, non-zero when it isn't.
    return result.status !== 0;
}

function sleepSync(ms: number): void {
    // Synchronous sleep inside the fast callback (we have no event loop to
    // await on — the SDK exits as soon as we return). Atomics.wait is a
    // real blocking sleep on Node's main thread (unlike browsers, Node
    // allows it there). The first attempt here used `ping -n 1 -w <ms>
    // 127.0.0.1` but `-w` is a per-reply timeout, not a delay — the
    // loopback replies immediately so the call returns in <1ms and the
    // polling loop spins, hammering `reg query` until the deadline.
    if (ms <= 0) return;
    const view = new Int32Array(new SharedArrayBuffer(4));
    Atomics.wait(view, 0, 0, ms);
}

function waitForUninstall(): boolean {
    const deadline = Date.now() + POLL_TIMEOUT_MS;
    while (Date.now() < deadline) {
        if (isUninstallKeyGone()) return true;
        sleepSync(POLL_INTERVAL_MS);
    }
    return isUninstallKeyGone();
}

// The root Slopsmith.exe is the Velopack stub/shim — a small standalone
// launcher that forwards to current\Slopsmith.exe. The NSIS uninstaller
// deletes that path, so we snapshot the genuine stub BEFORE running the
// uninstaller (backupStub) and put it back afterward (restoreStub).
//
// Copying current\Slopsmith.exe into the stub's place would NOT work: that
// is the real Electron executable and needs its sibling resources\ (and the
// other unpacked assets), which do not exist at the install root — the
// result would be a non-launchable Slopsmith.exe.

function stubPath(): string {
    // process.execPath inside the post-install hook is
    //   C:\Program Files\Slopsmith\current\Slopsmith.exe
    // so the stub sits one directory up, at the install root.
    const rootDir = path.dirname(path.dirname(process.execPath));
    return path.join(rootDir, 'Slopsmith.exe');
}

/**
 * Copy the Velopack stub to a temp file before the NSIS uninstaller runs.
 * Returns the backup path, or null if there was nothing to back up.
 */
function backupStub(): string | null {
    try {
        const stub = stubPath();
        if (!fs.existsSync(stub)) return null;
        const backupPath = path.join(os.tmpdir(), `slopsmith-velopack-stub-${process.pid}.exe`);
        fs.copyFileSync(stub, backupPath);
        return backupPath;
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[nsis-cleanup] Could not back up Velopack stub:', message);
        return null;
    }
}

/**
 * Restore the genuine Velopack stub if the NSIS uninstaller deleted it, then
 * drop the temp backup. Safe on every path — a no-op when the stub is still
 * present.
 */
function restoreStub(backupPath: string | null): void {
    try {
        const stub = stubPath();
        if (fs.existsSync(stub)) {
            // NSIS didn't delete it — its file list may never have tracked the
            // root Slopsmith.exe. Nothing to restore.
            return;
        }
        if (!backupPath || !fs.existsSync(backupPath)) {
            console.error('[nsis-cleanup] Velopack stub was removed and no backup is available to restore it.');
            return;
        }
        fs.copyFileSync(backupPath, stub);
        console.log(`[nsis-cleanup] Restored Velopack stub at ${stub}.`);
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[nsis-cleanup] Stub restoration failed:', message);
    } finally {
        if (backupPath) {
            try {
                fs.rmSync(backupPath, { force: true });
            } catch {
                /* temp-file cleanup is best-effort */
            }
        }
    }
}

/**
 * Fire from VelopackApp.onAfterInstallFastCallback during MSI install. No-op
 * when no legacy NSIS install exists. Safe to call on every install — the
 * registry check makes it idempotent.
 */
export function maybeUninstallLegacyNsis(): void {
    if (process.platform !== 'win32') return;

    const uninstaller = readNsisUninstaller();
    if (!uninstaller) {
        // No legacy install — most users post-shipping will hit this branch.
        return;
    }

    console.log(`[nsis-cleanup] Legacy NSIS install detected; running ${uninstaller.exe}`);
    // Snapshot the Velopack stub before the uninstaller can delete the root
    // Slopsmith.exe path it occupies.
    const stubBackup = backupStub();
    try {
        let launched = false;
        try {
            // NSIS's /S handler spawns a detached temp copy and the original
            // process returns immediately, so we don't gate on spawnSync's
            // exit status — waitForUninstall() polling the registry is the
            // real signal. Set a small timeout so a wedged launch can't burn
            // through the whole 30s callback budget here.
            const result = spawnSync(uninstaller.exe, uninstaller.args, {
                stdio: 'ignore',
                windowsHide: true,
                timeout: 5_000,
            });
            // spawnSync reports a launch failure (missing/blocked exe, or the
            // timeout above) via result.error rather than throwing.
            if (result.error) {
                console.error('[nsis-cleanup] NSIS uninstaller did not launch:', result.error.message);
            } else {
                launched = true;
            }
        } catch (err) {
            const message = err instanceof Error ? err.message : String(err);
            console.error('[nsis-cleanup] Failed to launch NSIS uninstaller:', message);
        }

        // Only poll when the uninstaller actually started — otherwise there
        // is nothing to wait for.
        if (launched) {
            if (waitForUninstall()) {
                console.log('[nsis-cleanup] NSIS uninstall complete.');
            } else {
                // Couldn't confirm completion within the budget — restoreStub()
                // in the finally still runs, better than a missing stub if the
                // uninstaller did partially succeed.
                console.warn('[nsis-cleanup] NSIS uninstall did not signal completion within 25s.');
            }
        }
    } finally {
        // Put the genuine stub back if NSIS removed it, and drop the temp
        // backup. No-op restore when the stub is still present.
        restoreStub(stubBackup);
    }
}
