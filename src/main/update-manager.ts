// Velopack-backed auto-updater for Slopsmith Desktop (Windows + macOS).
//
// Architecture:
//   - The renderer persists the user's release channel in localStorage and
//     calls setChannel() on boot so this module's UpdateManager is bound to
//     the right feed (stable | rc | beta | alpha).
//   - On init() and then every 4 hours we run checkForUpdatesAsync(); when a
//     hit comes back we download in the background, broadcast
//     update:available immediately and update:downloaded once the .nupkg is
//     on disk. The renderer shows a banner whose "Restart to apply" button
//     funnels back into applyAndRestart().
//   - Linux has no Velopack pipeline (electron-builder AppImage/.deb only),
//     so every method short-circuits to { status: "unsupported", ... } and
//     never touches the SDK. The renderer can still render the channel
//     dropdown — it just gets a clear "not supported here" status back.
//
// Velopack JS SDK API notes (verified against
//   node_modules/velopack/lib/index.d.ts, package 0.0.1589-ga2c5a97,
//   and the matching native source at src/lib-rust/src/sources/{mod,github}.rs
//   in github.com/velopack/velopack):
//   - There is **no** `GithubSource` class exported from the JS package
//     (unlike the .NET SDK). `UpdateManager`'s constructor takes a plain
//     `urlOrPath: string` — pointing it at the GitHub repo's HTML URL is
//     enough because the Velopack server-side metadata (`releases.<ch>.json`
//     uploaded by `vpk pack`) lives in the release assets and Velopack's
//     native loader knows how to pull them via the GitHub Releases API.
//   - `UpdateOptions` exposes `AllowVersionDowngrade` (plan called it
//     `AllowDowngrade`; the actual key in the typings is the longer name) +
//     `ExplicitChannel` (matches the plan).
//   - **`UpdateOptions` does NOT expose a `Prerelease` field**, and the
//     native `AutoSource::new` for any github.com URL hardcodes
//     `GithubSource::new(input, None, false)`. Result: this SDK cannot
//     see GitHub releases marked `prerelease: true`. The CI release
//     job therefore publishes every tag (incl. alpha/beta/rc) as
//     non-prerelease — see the long comment in .github/workflows/build.yml
//     above the `Create Release` step. Channel scoping is unaffected
//     because Velopack picks the release by channel-manifest filename
//     (releases.<channel>.json), not by GitHub prerelease flag.

import { app, BrowserWindow } from 'electron';
import type { UpdateInfo } from 'velopack';
import { IPC_UPDATE_EVENT_AVAILABLE, IPC_UPDATE_EVENT_DOWNLOADED } from './ipc-channels';

export type UpdateChannel = 'stable' | 'rc' | 'beta' | 'alpha';

export type UpdateStatus =
    | { status: 'unsupported'; platform: 'linux' }
    | { status: 'idle'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null }
    | { status: 'checking'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null }
    | { status: 'downloading'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null; pending: { version: string } }
    | { status: 'downloaded'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null; pending: { version: string } }
    | { status: 'error'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null; message: string };

// Repo the Velopack feed lives in. Matches the existing electron-builder
// release pipeline (slopsmith/slopsmith-desktop) — Velopack's GitHub
// loader looks for `releases.<channel>.json` + `*-full.nupkg` / `*-delta.nupkg`
// assets attached to releases here.
const FEED_URL = 'https://github.com/slopsmith/slopsmith-desktop';

// Background poll cadence. Velopack downloads are cheap when there's nothing
// new (HEAD on the channel manifest), so 4h is a reasonable trade-off
// between freshness and noise on the user's network.
const POLL_INTERVAL_MS = 4 * 60 * 60 * 1000;

// Held in module scope (singleton) — `main.ts` calls `init()` once after
// `app.whenReady()` resolves and never reconstructs us.
let velopackUm: import('velopack').UpdateManager | null = null;
let currentChannel: UpdateChannel = 'stable';
let pollTimer: NodeJS.Timeout | null = null;
let initialCheckTimer: NodeJS.Timeout | null = null;
let inFlightCheck: Promise<UpdateInfo | null> | null = null;
// Generation counter: incremented every time setChannel() replaces velopackUm
// so that in-flight checks from the old channel can detect they are stale and
// skip all state mutations + broadcasts. Without this, a check running on the
// old manager's promise would still write activeState/pendingDownloaded and
// broadcast update:available/downloaded after the channel switch.
let checkGeneration = 0;
let lastChecked: number | null = null;
let pendingVersion: string | null = null;   // set as soon as a target version is known (download starting)
let pendingDownloaded: { version: string } | null = null;  // set after download completes
let lastError: string | null = null;
let activeState: 'idle' | 'checking' | 'downloading' | 'downloaded' | 'error' = 'idle';

const isLinux = process.platform === 'linux';

function broadcast(channel: string, payload: unknown): void {
    for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) {
            win.webContents.send(channel, payload);
        }
    }
}

function currentVersion(): string | null {
    if (!velopackUm) return null;
    try {
        return velopackUm.getCurrentVersion();
    } catch {
        // Velopack throws when run from an unpackaged build (no manifest on
        // disk). That's a normal dev-loop state — surface null rather than
        // letting the throw bubble up into the IPC layer.
        return null;
    }
}

// Velopack requires every unique os/rid to have its own channel when one
// feed (a single GitHub release) serves multiple platforms — otherwise the
// per-channel `releases.<channel>.json` manifests collide as release
// assets. We ship x64-only Windows and arm64-only macOS, so the rid prefix
// is fixed per platform. `vpk pack` in CI publishes manifests under these
// exact names (win-x64-<track> / osx-arm64-<track>).
function veloChannel(track: UpdateChannel): string {
    // The rid is derived from BOTH platform and arch: an x64 (Intel) vs
    // arm64 macOS build must query its own channel manifest. Hardcoding
    // osx-arm64 would point an Intel build at the wrong (incompatible) feed.
    const os = process.platform === 'win32' ? 'win' : 'osx';
    const arch = process.arch === 'arm64' ? 'arm64' : 'x64';
    return `${os}-${arch}-${track}`;
}

function createManager(channel: UpdateChannel): void {
    // main.ts runs the Velopack startup hook (require('velopack') +
    // VelopackApp.build().run()) on win/mac before anything else. This lazy
    // require is a second layer of safety: a constructor-level failure here
    // (bad options, corrupted state dir) — or a velopack load failure that
    // main.ts caught and logged rather than crashed on — is surfaced as the
    // 'error' state by init()/setChannel() instead of crashing the process.
    // createManager() is only ever reached on win/mac (init()/setChannel()
    // short-circuit on Linux), so the require is safe to run here.
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const { UpdateManager } = require('velopack') as typeof import('velopack');
    velopackUm = new UpdateManager(FEED_URL, {
        ExplicitChannel: veloChannel(channel),
        AllowVersionDowngrade: false,
        MaximumDeltasBeforeFallback: 10,
    });
}

/**
 * Initialize the updater. Must be called once after `app.whenReady()` and
 * after at least one BrowserWindow exists (so the first broadcast lands).
 * On Linux this is a no-op.
 */
export function init(channel: UpdateChannel = 'stable'): void {
    if (isLinux) {
        console.log('[update-manager] Linux: auto-update disabled (electron-builder AppImage/deb only).');
        return;
    }
    currentChannel = channel;
    try {
        createManager(channel);
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] Failed to construct Velopack UpdateManager:', message);
        lastError = message;
        activeState = 'error';
        return;
    }
    // Hydrate pending-restart state from the previous session. If the user
    // already downloaded an update and the app was restarted without applying
    // (or the apply failed), getUpdatePendingRestart() returns the pending
    // release from disk — no network call needed. Surfacing this immediately
    // ensures the restart banner renders without waiting for a fresh download.
    const alreadyPending = velopackUm!.getUpdatePendingRestart();
    if (alreadyPending) {
        // getUpdatePendingRestart() returns a VelopackAsset with a Version field
        // (not UpdateInfo.TargetFullRelease.Version — VelopackAsset is flat).
        const v = alreadyPending.Version;
        pendingVersion = v;
        pendingDownloaded = { version: v };
        activeState = 'downloaded';
        // Broadcast so any already-open windows show the banner immediately.
        broadcast(IPC_UPDATE_EVENT_DOWNLOADED, { version: v, channel: currentChannel });
    }
    // Kick the first check shortly after launch so we don't compete with the
    // splash/audio-engine bring-up for CPU + network. Store the handle so
    // shutdown() can cancel it if the user quits within the 30s window.
    initialCheckTimer = setTimeout(() => {
        initialCheckTimer = null;
        void checkNow();
    }, 30_000);
    pollTimer = setInterval(() => { void checkNow(); }, POLL_INTERVAL_MS);
}

/**
 * Switch release channel at runtime. Recreates the underlying Velopack
 * UpdateManager (the SDK has no in-place channel swap) and triggers an
 * immediate check so the renderer can update its banner without waiting for
 * the next 4h tick. On Linux this is a no-op.
 */
export function setChannel(channel: UpdateChannel): void {
    if (isLinux) return;
    if (channel === currentChannel && velopackUm) return;
    currentChannel = channel;
    pendingVersion = null;
    pendingDownloaded = null;
    // Reset lastChecked too: until the immediate checkNow() below completes,
    // getStatus() should not report a stale "last checked" time that belongs
    // to the previous channel's feed.
    lastChecked = null;
    lastError = null;
    activeState = 'idle';
    // Bump the generation counter so any still-running check from the old
    // channel sees its epoch is stale and skips all state mutations +
    // broadcasts when its promise resolves. Also null inFlightCheck so the
    // new checkNow() below starts a fresh lock rather than coalescing onto
    // the old (stale) promise.
    checkGeneration++;
    inFlightCheck = null;
    // The immediate checkNow() below supersedes init()'s pending boot check.
    // The renderer calls setChannel() on boot to sync the persisted channel,
    // so without this a non-stable channel would fire a second redundant
    // network check ~30s later when initialCheckTimer elapses.
    if (initialCheckTimer) {
        clearTimeout(initialCheckTimer);
        initialCheckTimer = null;
    }
    try {
        createManager(channel);
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] Failed to switch channel:', message);
        lastError = message;
        activeState = 'error';
        return;
    }
    void checkNow();
}

/**
 * Trigger an immediate update check + download. Coalesces concurrent calls
 * (renderer button-mashing, overlapping poll timer) onto the same promise
 * so we don't fire parallel HTTP requests at the GitHub feed.
 */
export async function checkNow(): Promise<UpdateStatus> {
    if (isLinux) {
        return { status: 'unsupported', platform: 'linux' };
    }
    if (!velopackUm) {
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: null,
            lastChecked,
            message: lastError ?? 'Update manager not initialized',
        };
    }
    if (inFlightCheck) {
        await inFlightCheck.catch(() => undefined);
        return getStatus();
    }
    // Capture the current generation before any await so we can detect a
    // concurrent setChannel() call that happened while this check was in flight.
    const myGeneration = checkGeneration;
    activeState = 'checking';
    const um = velopackUm;
    const thisCheck = um.checkForUpdatesAsync();
    inFlightCheck = thisCheck;
    try {
        const info = await thisCheck;
        // If the channel was switched while we were awaiting, discard all results
        // from this check — they belong to the old channel's feed.
        if (checkGeneration !== myGeneration) {
            return getStatus();
        }
        lastChecked = Date.now();
        lastError = null;
        if (!info) {
            // No release newer than what's installed. But an update may
            // already be downloaded and staged — by a prior session
            // (hydrated in init()) or an earlier check this session.
            // getUpdatePendingRestart() is the source of truth: a null
            // checkForUpdatesAsync() result does NOT mean "nothing pending"
            // (Velopack won't re-report an update already on disk), so
            // clearing pending state here would wrongly drop the restart
            // banner. Preserve it whenever a staged update still exists.
            const stillPending = um.getUpdatePendingRestart();
            if (stillPending) {
                pendingVersion = stillPending.Version;
                pendingDownloaded = { version: stillPending.Version };
                activeState = 'downloaded';
            } else {
                activeState = 'idle';
                pendingVersion = null;
                pendingDownloaded = null;
            }
            return getStatus();
        }
        const targetVersion = info.TargetFullRelease.Version;
        // Set pendingVersion immediately so getStatus() can surface the target
        // version in the 'downloading' state (before the download completes and
        // pendingDownloaded is set). Without this, the renderer shows version: ''
        // while the download is in progress.
        pendingVersion = targetVersion;
        activeState = 'downloading';
        broadcast(IPC_UPDATE_EVENT_AVAILABLE, { version: targetVersion, channel: currentChannel });
        await um.downloadUpdateAsync(info);
        // Re-check generation after the (potentially long) download.
        if (checkGeneration !== myGeneration) {
            return getStatus();
        }
        pendingDownloaded = { version: targetVersion };
        activeState = 'downloaded';
        broadcast(IPC_UPDATE_EVENT_DOWNLOADED, { version: targetVersion, channel: currentChannel });
    } catch (err) {
        if (checkGeneration !== myGeneration) {
            return getStatus();
        }
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] checkNow failed:', message);
        lastError = message;
        activeState = 'error';
    } finally {
        // Only clear the lock if this invocation still owns it. If setChannel()
        // already nulled inFlightCheck and a new check started, we must not
        // clear that new check's lock.
        if (inFlightCheck === thisCheck) {
            inFlightCheck = null;
        }
    }
    return getStatus();
}

/**
 * Apply the downloaded update and restart the app. waitExitThenApplyUpdate()
 * launches the Velopack updater and tells it to wait for THIS process to
 * exit — it does NOT exit us. We must quit the app ourselves; the updater
 * then swaps binaries and relaunches. It only waits ~60s for our exit, so we
 * quit promptly (on the next tick, so this IPC call can return first).
 * activeState is left at 'downloaded' so that if the quit is vetoed or
 * delayed the restart banner stays visible for a retry.
 */
export function applyAndRestart(): UpdateStatus {
    if (isLinux) {
        return { status: 'unsupported', platform: 'linux' };
    }
    if (!velopackUm) {
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: null,
            lastChecked,
            message: 'Update manager not initialized',
        };
    }
    const pending = velopackUm.getUpdatePendingRestart();
    if (!pending) {
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: currentVersion(),
            lastChecked,
            message: 'No update is ready to apply',
        };
    }
    try {
        // silent=false (show Velopack's restart UI on Windows), restart=true.
        velopackUm.waitExitThenApplyUpdate(pending, false, true);
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] applyAndRestart failed:', message);
        lastError = message;
        // A failed launch of the updater does not undo the download — the
        // staged package is still on disk. Keep activeState 'downloaded' so
        // the restart banner stays and the user can retry; only fall to
        // 'error' if the package somehow vanished.
        const stillPending = velopackUm.getUpdatePendingRestart();
        if (stillPending) {
            pendingVersion = stillPending.Version;
            pendingDownloaded = { version: stillPending.Version };
            activeState = 'downloaded';
        } else {
            activeState = 'error';
        }
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: currentVersion(),
            lastChecked,
            message,
        };
    }
    // The Velopack updater is now waiting for this process to exit (~60s
    // budget). Quit on the next tick so this IPC call returns to the renderer
    // first; activeState stays 'downloaded' until the process actually exits.
    setImmediate(() => app.quit());
    return getStatus();
}

export function getStatus(): UpdateStatus {
    if (isLinux) {
        return { status: 'unsupported', platform: 'linux' };
    }
    const base = {
        channel: currentChannel,
        currentVersion: currentVersion(),
        lastChecked,
    };
    switch (activeState) {
        case 'checking':
            return { status: 'checking', ...base };
        case 'downloading':
            return {
                status: 'downloading',
                ...base,
                // Use pendingVersion (set when download starts) so the renderer
                // can show the target version even before the download completes
                // and pendingDownloaded is populated.
                pending: { version: pendingVersion ?? '' },
            };
        case 'downloaded':
            return {
                status: 'downloaded',
                ...base,
                pending: pendingDownloaded ?? { version: '' },
            };
        case 'error':
            return { status: 'error', ...base, message: lastError ?? 'Unknown error' };
        case 'idle':
        default:
            return { status: 'idle', ...base };
    }
}

/** Tear down the background poll and initial-check timers. Safe to call multiple times. */
export function shutdown(): void {
    if (initialCheckTimer) {
        clearTimeout(initialCheckTimer);
        initialCheckTimer = null;
    }
    if (pollTimer) {
        clearInterval(pollTimer);
        pollTimer = null;
    }
}
