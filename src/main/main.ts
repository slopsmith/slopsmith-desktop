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

    // Open external links in system browser
    mainWindow.webContents.setWindowOpenHandler(({ url }) => {
        shell.openExternal(url);
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

// Predicate: did a permission request originate from the localhost-served
// Slopsmith renderer? Parses via WHATWG URL so that query strings,
// fragments, IPv6 brackets and other valid-but-uncommon shapes don't trip
// the check (a bare regex like /^https?:\/\/(127\.0\.0\.1|localhost)(:\d+)?(\/.*)?$/
// would reject `http://127.0.0.1?x=1` because there's no `/` before the
// query string). URL parsing also gives strong host-matching, so
// `http://localhost.evil.com/` cannot impersonate localhost. Used by
// `installLocalhostPermissions` below to gate every permission request.
function isLocalRendererOrigin(url: string): boolean {
    if (!url) return false;
    let parsed: URL;
    try { parsed = new URL(url); } catch { return false; }
    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') return false;
    return parsed.hostname === '127.0.0.1' || parsed.hostname === 'localhost';
}

// Origin-scoped permission policy for the default session.
//
// Why this exists: clicking Detect in the bundled note_detect plugin on
// Linux used to show "Could not access audio input" (#52) because Chromium
// in Electron silently denies `media` for the localhost-served renderer
// when no permission handler is installed. The plugin itself now routes
// pitch detection through the JUCE bridge instead of getUserMedia, but
// we still want a defensive handler so future renderer code / third-party
// plugins don't hit the same wall.
//
// Policy: gate EVERY permission (not just `media`) on origin.
// - Localhost origins (where the Slopsmith app actually runs): grant.
//   This matches Electron's prior default-allow for that origin, so
//   unrelated renderer/plugin features (clipboard, notifications,
//   fullscreen, …) keep working unchanged.
// - Any other origin: deny. The main window runs with webSecurity: false
//   and there's no will-navigate enforcement, so without this, a stray
//   redirect or pasted URL would silently inherit clipboard / geolocation /
//   notifications etc. Denying non-local origins is strictly safer than
//   the pre-handler default for that case.
function installLocalhostPermissions(): void {
    const def = session.defaultSession;
    def.setPermissionRequestHandler((_wc, _permission, callback, details) => {
        callback(isLocalRendererOrigin(details.requestingUrl || ''));
    });
    def.setPermissionCheckHandler((_wc, _permission, requestingOrigin) => {
        return isLocalRendererOrigin(requestingOrigin || '');
    });
}

async function startup(): Promise<void> {
    console.log('[main] Starting Slopsmith Desktop...');

    // Install permission handlers first so any preload script / plugin that
    // probes getUserMedia during early renderer startup sees the localhost
    // grant rather than the default-deny.
    installLocalhostPermissions();

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

    // Initialize audio engine (JUCE native addon)
    initAudioBridge(mainWindow);

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
