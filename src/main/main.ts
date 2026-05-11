// Slopsmith Desktop — Electron Main Process
// Manages: window lifecycle, Python subprocess, audio engine bridge, plugin management

import { app, BrowserWindow, ipcMain, dialog, shell, session } from 'electron';
import * as path from 'path';
import { startPython, stopPython, waitForPython, getPythonPort, getStartupStatus, StartupStatus } from './python';
import { IPC_STARTUP_STATUS, IPC_STARTUP_GET_STATUS, IPC_STARTUP_REQUEST_STATUS } from './ipc-channels';
import { initAudioBridge, shutdownAudio } from './audio-bridge';
import { initPluginManager } from './plugin-manager';
import { initSoundfontManager } from './soundfont-manager';

// Linux: enable Chromium's PipeWire capturer feature so getUserMedia can see
// audio devices on PipeWire-only distros (Fedora 36+, recent Ubuntu, Arch).
// Without this, Chromium falls back to PulseAudio enumeration, which on
// PipeWire systems sometimes returns an empty device list even when the JUCE
// engine sees the hardware fine. Must be set BEFORE app.whenReady() resolves —
// command-line switches are read during Chromium initialization.
//
// This is paired with the per-session permission handler installed in
// startup() below; together they unblock any renderer code that still calls
// navigator.mediaDevices.getUserMedia (the bundled note_detect plugin has
// since been routed through the JUCE bridge in
// slopsmith-plugin-notedetect#27, but third-party plugins may still hit the
// Web-Audio path on their own).
if (process.platform === 'linux') {
    // Merge with any existing `--enable-features=` value (set by Electron
    // defaults, parent env, or future code) instead of overwriting — a bare
    // appendSwitch would replace the comma-separated list and silently
    // disable everything else that was enabled. Split-and-dedupe so the
    // value stays stable across re-initializations (or if Chromium itself
    // already has WebRTCPipeWireCapturer in its baseline list).
    const existing = app.commandLine.getSwitchValue('enable-features');
    const features = new Set<string>(
        (existing || '').split(',').map((f) => f.trim()).filter(Boolean),
    );
    features.add('WebRTCPipeWireCapturer');
    app.commandLine.appendSwitch('enable-features', Array.from(features).join(','));
}

// Prevent error dialogs from showing when the Python subprocess has issues.
// Both handlers log and swallow — don't let a stray rejection in one of the
// subsystems tear the whole app down.
process.on('uncaughtException', (err) => {
    console.error('[main] Uncaught exception:', err.message);
});

process.on('unhandledRejection', (reason, promise) => {
    console.error('[main] Unhandled rejection at:', promise, 'reason:', reason);
});

let mainWindow: BrowserWindow | null = null;
let splashWindow: BrowserWindow | null = null;
// Set to true when the user initiates a quit so the startup-status polling
// loop can break out early instead of waiting for the 5-minute deadline.
let appQuitting = false;
// Milliseconds to keep the splash visible after reaching a terminal state so
// the renderer has time to paint the final status message before the window closes.
const SPLASH_CLOSE_DELAY_MS = 300;
/** Total time budget for the plugin-startup polling loop. */
const STARTUP_DEADLINE_MS = 300_000; // 5 minutes (300 000 ms)
/** How often to poll /api/startup-status during the startup loop. */
const STARTUP_POLL_INTERVAL_MS = 700;
let startupStatusSnapshot: StartupStatus = {
    running: true,
    phase: 'booting',
    message: 'Starting Slopsmith...',
    currentPlugin: '',
    loaded: 0,
    total: 0,
    error: null,
};


function getResourcesPath(): string {
    return app.isPackaged
        ? path.join(process.resourcesPath)
        : path.join(__dirname, '..', '..');
}

function publishStartupStatus(status: Partial<StartupStatus>): void {
    startupStatusSnapshot = { ...startupStatusSnapshot, ...status };
    if (splashWindow && !splashWindow.isDestroyed()) {
        splashWindow.webContents.send(IPC_STARTUP_STATUS, startupStatusSnapshot);
    }
    if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send(IPC_STARTUP_STATUS, startupStatusSnapshot);
    }
}

function createSplashWindow(): void {
    splashWindow = new BrowserWindow({
        width: 560,
        height: 360,
        alwaysOnTop: true,
        resizable: false,
        minimizable: false,
        maximizable: false,
        fullscreenable: false,
        frame: false,
        show: true,
        title: 'Slopsmith',
        backgroundColor: '#050508',
        webPreferences: {
            preload: path.join(__dirname, 'splash-preload.js'),
            nodeIntegration: false,
            contextIsolation: true,
            // sandbox must be false so the preload script can require('electron')
            // (ipcRenderer). contextIsolation: true keeps the renderer isolated.
            sandbox: false,
        },
    });

    splashWindow.loadFile(path.join(__dirname, 'splash.html'));

    // Treat a user-initiated close (Alt+F4 / Cmd+W) as an explicit quit so
    // they are never stuck waiting for the full 5-minute startup deadline.
    // preventDefault() keeps the splash visible while app.quit() propagates
    // through before-quit → will-quit and clears the polling loop.
    splashWindow.on('close', (event) => {
        const currentPhase = startupStatusSnapshot.phase;
        if (!appQuitting && currentPhase !== 'complete' && currentPhase !== 'error') {
            event.preventDefault();
            app.quit();
        }
    });

    splashWindow.on('closed', () => {
        splashWindow = null;
    });
}

function createWindow(port: number): void {
    mainWindow = new BrowserWindow({
        width: 1400,
        height: 900,
        minWidth: 800,
        minHeight: 600,
        title: 'Slopsmith',
        backgroundColor: '#0f172a', // slate-900 to match Slopsmith UI
        webPreferences: {
            preload: path.join(__dirname, 'preload.js'),
            contextIsolation: true,
            nodeIntegration: false,
            sandbox: false, // required for preload to use require('electron')
            webSecurity: false, // allow loading from localhost
        },
    });

    // Forward renderer console to main process stdout
    mainWindow.webContents.on('console-message', (_event, level, message, line, sourceId) => {
        const prefix = ['[renderer:verbose]', '[renderer:info]', '[renderer:warn]', '[renderer:error]'][level] || '[renderer]';
        console.log(`${prefix} ${message}`);
    });

    const serverUrl = `http://127.0.0.1:${port}`;

    // Small delay to ensure server is fully accepting connections, then load
    setTimeout(() => mainWindow?.loadURL(serverUrl), 500);

    // Retry loading if the server wasn't reachable yet. Previously this
    // retried just once, which left the window stuck on Chromium's
    // built-in error page when the python lifespan startup happened to
    // run long (or a zombie was holding the candidate port). Retry up
    // to maxRetries × intervalMs (~30 s total) with a fixed cadence —
    // long enough to ride out cold-cache plugin imports without giving
    // up. Only retry on the network-side error codes that indicate
    // "server not up yet"; don't retry on 404/etc. that mean the server
    // is up but served something we can't load.
    //   -102 = ERR_CONNECTION_REFUSED
    //   -6   = ERR_FILE_NOT_FOUND   (rare; shows up on transient races)
    //   -118 = ERR_CONNECTION_TIMED_OUT
    //   -2   = ERR_FAILED          (catch-all for transient socket errors)
    const retryableErrors = new Set([-102, -6, -118, -2]);
    const maxRetries = 20;
    const retryIntervalMs = 1500;
    let retryCount = 0;
    mainWindow.webContents.on('did-fail-load', (_event, errorCode) => {
        if (!retryableErrors.has(errorCode)) return;
        if (retryCount >= maxRetries) {
            console.log(`[main] gave up loading ${serverUrl} after ${maxRetries} retries (last errorCode=${errorCode})`);
            return;
        }
        retryCount += 1;
        setTimeout(() => {
            if (!mainWindow) return;
            mainWindow.loadURL(serverUrl);
        }, retryIntervalMs);
    });

    // Inject mutable sync offset after page loads (default 200ms, overridden by settings).
    // Gate on the actual server URL — Chromium fires did-finish-load on its
    // built-in error pages too, and those have a null origin, so reading
    // localStorage from them throws SecurityError. Only inject when we
    // actually loaded the http://127.0.0.1 origin.
    mainWindow.webContents.on('did-finish-load', () => {
        const url = mainWindow?.webContents.getURL() || '';
        if (!url.startsWith('http://127.0.0.1:')) return;
        mainWindow?.webContents.executeJavaScript(`
            window._slopsmithSyncOffset = parseFloat(localStorage.getItem('slopsmith-sync-offset') || '0.2');
        `).catch(() => {});
    });

    // Block in-window navigation away from the renderer origin. The window
    // ships with `webSecurity: false` so the renderer can load
    // mixed-origin assets from the localhost server, but that same loose
    // setting means a stray click on a same-window link (or a 30x
    // redirect served through the local proxy) would still load a remote
    // page in our chrome — same preload, same exposed IPC. The permission
    // handler installed in startup() denies media/clipboard/etc for that
    // case, but it doesn't stop the navigation itself.
    //
    // Allow only navigations whose target matches the resolved renderer
    // origin. Anything else gets cancelled here and (via the
    // setWindowOpenHandler below) re-routed to the user's default browser
    // via shell.openExternal. Re-derive the predicate locally — the
    // permission handler captured it in a closure that isn't exposed.
    const isRendererOrigin = makeRendererOriginPredicate(port);
    // Block off-origin navigations at every layer Electron exposes:
    //
    // - `will-navigate`: user/script-initiated navigations on the main
    //   frame (link clicks, `location =`). Doesn't fire for
    //   programmatic loadURL — that's how we still let the initial
    //   load succeed.
    // - `will-redirect`: server-side 30x redirects during an
    //   in-progress navigation. The local proxy returning a 302 to a
    //   remote URL would otherwise bypass `will-navigate`.
    // - `will-frame-navigate`: navigations inside *any* frame
    //   (including the main frame). With `webSecurity: false` and a
    //   privileged preload running in every frame, an iframe loading
    //   a remote URL would inherit the IPC surface. We skip the main
    //   frame here so we don't double-process what `will-navigate`
    //   already handled.
    //
    // Electron 35 fires all three with a single `details` Event whose
    // `url` / `isMainFrame` are properties on the event, *not*
    // positional callback args. Reading them positionally returns
    // undefined and silently inverts the policy — `preventDefault()`
    // fires on every navigation including legitimate ones.
    function blockOffOriginTopLevel(reason: string) {
        return (details: Electron.Event<{ url: string }>) => {
            const navUrl = details.url;
            if (isRendererOrigin(navUrl)) return;
            details.preventDefault();
            console.warn(`[main] Blocked ${reason} to non-renderer origin: ${navUrl}`);
            // Only forward web URLs to the system browser. `file:`,
            // `javascript:`, `mailto:`, or custom schemes would
            // otherwise trigger the user's registered protocol handler
            // from a page-controlled string — a foot-gun even for a
            // navigation we're already blocking.
            openWebUrlExternally(navUrl);
        };
    }
    mainWindow.webContents.on('will-navigate', blockOffOriginTopLevel('in-window navigation'));
    mainWindow.webContents.on('will-redirect', blockOffOriginTopLevel('cross-origin redirect'));
    mainWindow.webContents.on('will-frame-navigate', (details) => {
        // Top-level frame is handled by will-navigate above; skip so
        // we don't double-log or route to openExternal twice.
        if (details.isMainFrame) return;
        const navUrl = details.url;
        if (isRendererOrigin(navUrl)) return;
        details.preventDefault();
        // Don't openExternal subframe blocks — popping the system
        // browser every time an embedded video / ad-frame tries to
        // load is worse UX than silently refusing.
        console.warn(`[main] Blocked subframe navigation to non-renderer origin: ${navUrl}`);
    });

    // Open external links in system browser. Same scheme gate as
    // will-navigate above — a `target=_blank` or `window.open` from the
    // renderer page can supply any string, so don't pass it to
    // shell.openExternal blindly.
    mainWindow.webContents.setWindowOpenHandler(({ url }) => {
        openWebUrlExternally(url);
        return { action: 'deny' };
    });

    mainWindow.on('closed', () => {
        mainWindow = null;
    });

    // Dev tools in development
    if (!app.isPackaged) {
        mainWindow.webContents.openDevTools({ mode: 'detach' });
    }
}

// Forward a URL to the OS default browser only if it's a web URL.
// `shell.openExternal` will gladly hand any string to the user's
// registered protocol handlers (file:, javascript:, mailto:, custom
// schemes), and the strings we pass through come from page-controlled
// places (will-navigate, window.open). Restrict to http(s) so a
// malformed link, plugin bug, or attacker-shaped string can't reach
// for arbitrary scheme handlers.
function openWebUrlExternally(url: string): void {
    let parsed: URL;
    try { parsed = new URL(url); } catch {
        console.warn(`[main] Refusing to openExternal malformed URL: ${url}`);
        return;
    }
    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
        console.warn(`[main] Refusing to openExternal non-web scheme: ${parsed.protocol}`);
        return;
    }
    // Pass the canonicalised href, not the raw input — page-controlled
    // strings can carry whitespace / control characters that the URL
    // parser strips, and openExternal should see exactly the bytes we
    // validated.
    shell.openExternal(parsed.href).catch(() => { /* user dismissed / system error */ });
}

// Permissions we always deny, regardless of origin. These are
// high-impact device APIs Slopsmith has no use for — refusing them up
// front shrinks the attack surface even on the trusted renderer
// origin, so a malicious or compromised plugin can't reach for them.
// Add to this list if a real Slopsmith feature ever needs one.
const DENY_PERMISSIONS = new Set([
    'serial',
    'hid',
    'usb',
    'bluetooth',
    'geolocation',
    'idle-detection',
]);

// Predicate factory: did a permission request originate from the exact
// origin we load the renderer from? The window's `loadURL` target is
// `http://127.0.0.1:${port}` (see createWindow), so the trusted origin
// is precisely that protocol + hostname + port triple — no wider
// hostname allow-list. `http://localhost:${port}` could resolve to a
// different listener on a dual-stack host and must NOT inherit the
// grant.
//
// Parses via WHATWG URL so query strings, fragments, and other
// valid-but-uncommon URL shapes don't trip the check. URL.origin
// canonicalises to `scheme://host:port`, which is exactly the equality
// we want — anything else (different port, different scheme, different
// hostname, malformed URL) compares unequal.
function makeRendererOriginPredicate(rendererPort: number): (url: string) => boolean {
    const expectedOrigin = `http://127.0.0.1:${rendererPort}`;
    return (url: string): boolean => {
        if (!url) return false;
        try {
            return new URL(url).origin === expectedOrigin;
        } catch {
            return false;
        }
    };
}

// Origin- AND port-scoped permission policy for the default session.
//
// Why this exists: clicking Detect in the bundled note_detect plugin on
// Linux used to show "Could not access audio input" (#52) because Chromium
// in Electron silently denies `media` for the localhost-served renderer
// when no permission handler is installed. The plugin itself now routes
// pitch detection through the JUCE bridge, but we still want a defensive
// handler so future renderer code / third-party plugins don't hit the
// same wall. createWindow() also installs a `will-navigate` listener that
// cancels any same-window navigation off the renderer origin, so the
// scenarios this handler defends against are mostly belt-and-braces;
// `webSecurity: false` is still on, so a defense-in-depth permission
// policy is worth keeping anyway.
//
// Policy:
// - Block DENY_PERMISSIONS (serial / hid / usb / bluetooth / geolocation /
//   idle-detection) for *every* origin, including the renderer. Slopsmith
//   has no use for these; pre-denying them keeps a compromised plugin
//   from reaching for them.
// - For `media` from the renderer origin, allow audio-only requests
//   (Slopsmith uses the microphone for pitch detection) but deny when
//   `details.mediaTypes` includes `video` — we have no camera feature,
//   so a getUserMedia({video:true}) call must be a plugin bug or worse.
// - For every other permission, grant when the request comes from the
//   exact rendererPort we resolved at startup. This matches Electron's
//   prior default-allow for the only origin we actually load, so
//   unrelated renderer/plugin features (clipboard, notifications,
//   fullscreen, midi, …) keep working unchanged.
// - For any other origin (including other ports on 127.0.0.1, redirects
//   to external URLs, etc.), deny. Stops a stray redirect from
//   inheriting clipboard / notifications / etc. that would have been
//   default-allowed without a handler.
function installRendererPermissions(rendererPort: number): void {
    const isRendererOrigin = makeRendererOriginPredicate(rendererPort);
    const def = session.defaultSession;
    def.setPermissionRequestHandler((_wc, permission, callback, details) => {
        if (DENY_PERMISSIONS.has(permission)) {
            callback(false);
            return;
        }
        if (!isRendererOrigin(details.requestingUrl || '')) {
            callback(false);
            return;
        }
        if (permission === 'media') {
            // Electron passes mediaTypes (`'audio'` / `'video'`) for the
            // request-handler path. Allow only when the request is
            // explicitly audio-only — Slopsmith doesn't use the camera,
            // so we want both "video requested" AND "mediaTypes
            // missing/empty" to deny (the latter would otherwise let an
            // older / synthetic request slip through).
            const mediaTypes = (details as { mediaTypes?: string[] }).mediaTypes;
            const types = new Set(mediaTypes ?? []);
            const audioOnly = types.has('audio') && !types.has('video');
            callback(audioOnly);
            return;
        }
        callback(true);
    });
    def.setPermissionCheckHandler((_wc, permission, requestingOrigin, details) => {
        if (DENY_PERMISSIONS.has(permission)) return false;
        if (!isRendererOrigin(requestingOrigin || '')) return false;
        if (permission === 'media') {
            // Mirror the request-handler's audio-only policy in the
            // synchronous check path so navigator.permissions.query()
            // reports the same state we'll actually grant. Electron's
            // check-handler `details` exposes `mediaType` (singular),
            // unlike the request-handler's `mediaTypes` (plural array).
            // Treat `'video'` and `'unknown'` as denied, only explicit
            // `'audio'` as granted.
            const mediaType = (details as { mediaType?: string }).mediaType;
            return mediaType === 'audio';
        }
        return true;
    });
}

async function startup(): Promise<void> {
    console.log('[main] Starting Slopsmith Desktop...');

    // Register startup status IPC handlers before creating the splash window
    // so the splash preload's immediate startup:requestStatus is handled.
    ipcMain.handle(IPC_STARTUP_GET_STATUS, () => startupStatusSnapshot);
    ipcMain.on(IPC_STARTUP_REQUEST_STATUS, (event) => {
        event.sender.send(IPC_STARTUP_STATUS, startupStatusSnapshot);
    });

    createSplashWindow();
    publishStartupStatus({ message: 'Starting backend service...', phase: 'booting', running: true });

    // Start Python server (Slopsmith backend)
    startPython();

    // Initialize audio engine (JUCE native addon). Pass a getter rather
    // than the (currently-null) mainWindow reference so the bridge can
    // check the live value when IPCs arrive — the renderer doesn't
    // exist yet at this point in startup.
    initAudioBridge(() => mainWindow);

    // Initialize plugin manager IPC handlers
    initPluginManager();

    // Initialize soundfont manager IPC handlers (Audio Quality preference)
    initSoundfontManager(() => mainWindow);

    // Wait for Python server to be ready; null means the backend failed to start.
    const port = await waitForPython().catch((err: unknown) => {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[main] Backend failed to start:', message);
        publishStartupStatus({ message: `Backend failed to start: ${message}`, phase: 'error', running: false });
        return null;
    });

    if (port === null) {
        await new Promise((resolve) => setTimeout(resolve, SPLASH_CLOSE_DELAY_MS));
        if (splashWindow && !splashWindow.isDestroyed()) splashWindow.close();
        app.quit();
        return;
    }

    console.log(`[main] Python server ready on port ${port}`);
    publishStartupStatus({ message: 'Backend ready. Opening app window...', phase: 'core-ready', running: true });

    // Permission handlers must be installed before the renderer loads so
    // its first permission request hits our policy, not Chromium's
    // default. We deferred until now because the policy is scoped to the
    // exact renderer port — a stray navigation to another local service
    // on a different port must not inherit the trusted-renderer grant.
    installRendererPermissions(port);

    // Create the main window
    createWindow(port);

    // Register file picker IPC
    ipcMain.handle('dialog:pickFile', async (_event, filters?: { name: string; extensions: string[] }[]) => {
        if (!mainWindow) return null;
        const result = await dialog.showOpenDialog(mainWindow, {
            properties: ['openFile'],
            filters: filters || [{ name: 'All Files', extensions: ['*'] }],
        });
        return result.canceled ? null : result.filePaths[0];
    });

    ipcMain.handle('dialog:pickDirectory', async () => {
        if (!mainWindow) return null;
        const result = await dialog.showOpenDialog(mainWindow, {
            properties: ['openDirectory'],
        });
        return result.canceled ? null : result.filePaths[0];
    });

    ipcMain.handle('dialog:pickFiles', async (_event, filters?: { name: string; extensions: string[] }[]) => {
        if (!mainWindow) return [];
        const result = await dialog.showOpenDialog(mainWindow, {
            properties: ['openFile', 'multiSelections'],
            filters: filters || [{ name: 'All Files', extensions: ['*'] }],
        });
        return result.canceled ? [] : result.filePaths;
    });

    // App info
    ipcMain.handle('app:getInfo', () => ({
        version: app.getVersion(),
        isPackaged: app.isPackaged,
        platform: process.platform,
        resourcesPath: getResourcesPath(),
    }));

    // Config directory
    ipcMain.handle('app:getConfigDir', () => {
        return app.getPath('userData');
    });

    const startupDeadline = Date.now() + STARTUP_DEADLINE_MS;
    let reachedTerminalState = false;
    while (Date.now() < startupDeadline && !appQuitting) {
        const status = await getStartupStatus();
        if (appQuitting) break;
        if (status) {
            publishStartupStatus(status);
            if (!status.running && (status.phase === 'complete' || status.phase === 'error')) {
                reachedTerminalState = true;
                break;
            }
        }
        await new Promise((resolve) => setTimeout(resolve, STARTUP_POLL_INTERVAL_MS));
    }
    if (!reachedTerminalState && !appQuitting) {
        publishStartupStatus({ message: 'Startup timed out', phase: 'error', running: false });
    }
    // Give the renderer a tick to paint the final status before closing
    await new Promise((resolve) => setTimeout(resolve, SPLASH_CLOSE_DELAY_MS));
    if (splashWindow && !splashWindow.isDestroyed()) splashWindow.close();

}

app.whenReady().then(startup);

app.on('window-all-closed', () => {
    shutdown();
    app.quit();
});

app.on('before-quit', () => {
    appQuitting = true;
    shutdown();
});

function shutdown(): void {
    try {
        console.log('[main] Shutting down...');
    } catch { /* console may already be gone mid-teardown */ }
    shutdownAudio();
    stopPython();
}

// macOS: re-create window when dock icon is clicked
app.on('activate', async () => {
    if (BrowserWindow.getAllWindows().length === 0) {
        const port = getPythonPort();
        if (port > 0) createWindow(port);
    }
});
