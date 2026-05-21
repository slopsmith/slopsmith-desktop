// Slopsmith Desktop — Electron Main Process
// Manages: window lifecycle, Python subprocess, audio engine bridge, plugin management

// ── Velopack startup hook ─────────────────────────────────────────────────
// MUST run before ANY other side-effecting code (crashReporter, app event
// listeners, the rest of the imports below). When Windows invokes
// `Update.exe` with `--veloapp-install`/`--veloapp-updated`/`--veloapp-firstrun`
// it relaunches our exe with those flags; `VelopackApp.build().run()` is what
// detects them, runs the appropriate hook, and exits. If the hook doesn't
// run first the bootstrapper silently breaks install/upgrade flows.
// On macOS the hook just returns (no-op).
// Linux has no Velopack pipeline (electron-builder AppImage/deb only), so the
// native module is never needed there — skip the require entirely so loading
// it on an unsupported platform can never crash startup. On win/mac a load
// failure is also caught: a broken updater is recoverable, a dead app is not.
if (process.platform !== 'linux') {
    try {
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        const { VelopackApp } = require('velopack') as typeof import('velopack');
        VelopackApp.build().run();
    } catch (err) {
        // Never crash over this — a launchable app beats a dead one. But in a
        // packaged build a hook failure means install/update lifecycle flags
        // won't be handled, so surface it with a dialog instead of a console
        // line nobody reads. In dev/unpackaged builds the hook is a harmless
        // no-op, so a logged warning is enough there.
        console.error('[main] Velopack startup hook failed:', err);
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        const { app, dialog } = require('electron');
        if (app.isPackaged) {
            dialog.showErrorBox(
                'Slopsmith update system error',
                'The Velopack updater failed to initialize. Slopsmith will still '
                + 'run, but automatic updates may not work until it is reinstalled.'
                + `\n\n${String(err)}`,
            );
        }
    }
}
// ──────────────────────────────────────────────────────────────────────────

import { app, BrowserWindow, ipcMain, dialog, shell, session, crashReporter } from 'electron';
import * as path from 'path';

// Enable Electron's Crashpad to capture native crashes (incl. VST/JUCE C++
// access violations) into <userData>/Crashpad/reports/ as .dmp files. Must
// run before app.whenReady(). uploadToServer:false keeps dumps local — they
// can be inspected with WinDbg / minidump-stackwalk.
crashReporter.start({
    productName: 'slopsmith-desktop',
    companyName: 'slopsmith',
    submitURL: '',
    uploadToServer: false,
    compress: false,
});
import { startPython, stopPython, waitForPython, getPythonPort, getStartupStatus, StartupStatus } from './python';
import {
    IPC_STARTUP_STATUS,
    IPC_STARTUP_GET_STATUS,
    IPC_STARTUP_REQUEST_STATUS,
    IPC_UPDATE_GET_STATUS,
    IPC_UPDATE_SET_CHANNEL,
    IPC_UPDATE_CHECK_NOW,
    IPC_UPDATE_APPLY,
    IPC_UPDATE_IS_NSIS_INSTALL,
    IPC_UPDATE_UPGRADE_FROM_NSIS,
} from './ipc-channels';
import { initAudioBridge, shutdownAudio } from './audio-bridge';
import { initDebugLogging, isDebugEnabled } from './debug-log';
import { initPluginManager } from './plugin-manager';
import { initSoundfontManager } from './soundfont-manager';
import * as updateManager from './update-manager';
import type { UpdateChannel } from './update-manager';
import * as nsisMigration from './nsis-migration';

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

// Shared by the main BrowserWindow and any same-origin popup spawned via
// window.open (see setWindowOpenHandler / popupOverrideOptions below).
// Single source of truth so a future security-sensitive change (preload
// path, sandbox, webSecurity, isolation) can't update one path and leave
// the other diverged.
//   - sandbox: false is required for the preload to use require('electron').
//   - webSecurity: false lets the renderer load mixed-origin assets from
//     the localhost Python server.
const rendererWebPreferences: Electron.WebPreferences = {
    preload: path.join(__dirname, 'preload.js'),
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: false,
    webSecurity: false,
};

function createWindow(port: number): void {
    mainWindow = new BrowserWindow({
        width: 1400,
        height: 900,
        minWidth: 800,
        minHeight: 600,
        title: 'Slopsmith',
        backgroundColor: '#0f172a', // slate-900 to match Slopsmith UI
        webPreferences: rendererWebPreferences,
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

    // window.open() routing:
    //
    // - Same renderer-origin URLs → allow as an Electron BrowserWindow
    //   that mirrors the main window's webPreferences (same preload,
    //   same isolation, same webSecurity: false). Plugin pop-outs like
    //   splitscreen rely on this so the popup shares the renderer's
    //   BroadcastChannel scope and preload-exposed IPC. Without it,
    //   `action: 'deny'` returns null to window.open() (which the
    //   plugin reads as "popup blocked") AND the URL leaks to the
    //   system browser via openWebUrlExternally, where BroadcastChannel
    //   can't reach across Chromium instances.
    //
    // - Off-origin URLs → route to the system browser. Same scheme
    //   gate as will-navigate above (openWebUrlExternally restricts to
    //   http/https) since a target=_blank or stray window.open from a
    //   plugin can supply any string.
    const popupOverrideOptions: Electron.BrowserWindowConstructorOptions = {
        backgroundColor: '#0f172a',
        webPreferences: rendererWebPreferences,
    };
    const rendererWindowOpenHandler = ({ url }: { url: string }) => {
        if (isRendererOrigin(url)) {
            return { action: 'allow' as const, overrideBrowserWindowOptions: popupOverrideOptions };
        }
        openWebUrlExternally(url);
        return { action: 'deny' as const };
    };
    mainWindow.webContents.setWindowOpenHandler(rendererWindowOpenHandler);

    // Apply the same off-origin navigation guards + window-open policy
    // to any popup the renderer opens. Otherwise a popup could be told
    // to navigate off-origin (or spawn another window.open), and the
    // preload-IPC surface installed via popupOverrideOptions would
    // follow along to the new page.
    //
    // Wired recursively: the last line re-registers `did-create-window`
    // on each popup's own webContents so nested popups (popup A →
    // popup B) inherit the same guards. Without that, only popups
    // spawned directly from the main window would be protected, and
    // any popup that spawned another would leave its child guard-less
    // while still carrying the preload-IPC surface.
    function wirePopupGuards(wc: Electron.WebContents): void {
        wc.on('will-navigate', blockOffOriginTopLevel('popup in-window navigation'));
        wc.on('will-redirect', blockOffOriginTopLevel('popup cross-origin redirect'));
        wc.on('will-frame-navigate', (details) => {
            if (details.isMainFrame) return;
            const navUrl = details.url;
            if (isRendererOrigin(navUrl)) return;
            details.preventDefault();
            console.warn(`[main] Blocked popup subframe navigation to non-renderer origin: ${navUrl}`);
        });
        wc.setWindowOpenHandler(rendererWindowOpenHandler);
        wc.on('did-create-window', (nestedWin) => wirePopupGuards(nestedWin.webContents));
    }
    mainWindow.webContents.on('did-create-window', (popupWin) => {
        wirePopupGuards(popupWin.webContents);
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
    // Debug logging first so everything below is captured. SLOPSMITH_SANDBOX_DEBUG
    // gates the addon's VST_TRACE — flip it whenever debug is *requested*, even
    // if the log file couldn't be opened, so addon tracing isn't silently lost.
    // The addon caches the var on first read, so it must be set before
    // initAudioBridge() loads the .node.
    const debugLogPath = initDebugLogging();
    if (isDebugEnabled()) {
        process.env.SLOPSMITH_SANDBOX_DEBUG = '1';
    }
    if (debugLogPath) {
        console.log(`[main] Debug logging enabled → ${debugLogPath}`);
    }

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

    // Initialize audio engine (JUCE native addon).
    initAudioBridge();

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

    // Auto-update (Velopack). The renderer Settings panel reads the persisted
    // channel from localStorage and calls setChannel() on boot — we default
    // to 'stable' here so the first check runs against the safest feed even
    // if the renderer hasn't paged in yet. On Linux every call short-circuits
    // to { status: "unsupported", platform: "linux" } inside update-manager.
    ipcMain.handle(IPC_UPDATE_GET_STATUS, () => updateManager.getStatus());
    ipcMain.handle(IPC_UPDATE_SET_CHANNEL, (_event, channel: unknown) => {
        // IPC is untyped at runtime — validate the channel string before forwarding
        // so a renderer bug or compromised page can't pass arbitrary values into
        // the Velopack SDK.
        const VALID_CHANNELS: readonly string[] = ['stable', 'rc', 'beta', 'alpha'];
        if (typeof channel !== 'string' || !VALID_CHANNELS.includes(channel)) {
            return updateManager.getStatus();
        }
        updateManager.setChannel(channel as UpdateChannel);
        return updateManager.getStatus();
    });
    ipcMain.handle(IPC_UPDATE_CHECK_NOW, () => updateManager.checkNow());
    ipcMain.handle(IPC_UPDATE_APPLY, () => updateManager.applyAndRestart());

    // Legacy NSIS → Velopack migration. isNSISInstall is a pure path check;
    // upgradeFromNSIS kicks off the elevated uninstall + MSI install and we
    // app.quit() ourselves so the NSIS uninstaller can delete the locked exe.
    ipcMain.handle(IPC_UPDATE_IS_NSIS_INSTALL, () => nsisMigration.isNSISInstall());
    ipcMain.handle(IPC_UPDATE_UPGRADE_FROM_NSIS, async () => {
        const result = await nsisMigration.upgradeFromNSIS();
        if (result.ok) {
            // The elevated PowerShell sleeps 3s before touching anything, so
            // setImmediate(quit) gives the IPC reply a tick to return first
            // and still leaves the script plenty of time to start.
            setImmediate(() => app.quit());
        }
        return result;
    });

    // Boot the updater after the main window exists so the first
    // update:available / update:downloaded broadcast has a renderer to land
    // in. Renderer will call setChannel() once it reads localStorage.
    updateManager.init('stable');

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
    updateManager.shutdown();
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
