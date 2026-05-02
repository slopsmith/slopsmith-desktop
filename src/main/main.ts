// Slopsmith Desktop — Electron Main Process
// Manages: window lifecycle, Python subprocess, audio engine bridge, plugin management

import { app, BrowserWindow, ipcMain, dialog, shell } from 'electron';
import * as path from 'path';
import { startPython, stopPython, waitForPython, getPythonPort, getStartupStatus } from './python';
import { initAudioBridge, shutdownAudio } from './audio-bridge';
import { initPluginManager } from './plugin-manager';
import { initSoundfontManager } from './soundfont-manager';

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
let startupStatusSnapshot: Record<string, unknown> = {
    running: true,
    phase: 'booting',
    message: 'Starting Slopsmith...',
    current_plugin: '',
    loaded: 0,
    total: 0,
    error: null,
};

const SPLASH_SPINNER_DATA = {
    v: '5.7.4',
    fr: 60,
    ip: 0,
    op: 120,
    w: 120,
    h: 120,
    nm: 'slopsmith-spinner',
    ddd: 0,
    assets: [],
    layers: [{
        ddd: 0,
        ind: 1,
        ty: 4,
        nm: 'ring',
        sr: 1,
        ks: {
            o: { a: 0, k: 100 },
            r: { a: 1, k: [{ t: 0, s: [0] }, { t: 120, s: [360] }] },
            p: { a: 0, k: [60, 60, 0] },
            a: { a: 0, k: [0, 0, 0] },
            s: { a: 0, k: [100, 100, 100] },
        },
        ao: 0,
        shapes: [{
            ty: 'gr',
            it: [
                { ind: 0, ty: 'el', s: { a: 0, k: [72, 72] }, p: { a: 0, k: [0, 0] }, nm: 'Ellipse Path 1', mn: 'ADBE Vector Shape - Ellipse', hd: false },
                { ty: 'st', c: { a: 0, k: [0.376, 0.627, 1, 1] }, o: { a: 0, k: 100 }, w: { a: 0, k: 10 }, lc: 2, lj: 2, bm: 0, nm: 'Stroke 1', mn: 'ADBE Vector Graphic - Stroke', hd: false },
                { ty: 'tm', s: { a: 0, k: 0 }, e: { a: 1, k: [{ t: 0, s: [18] }, { t: 60, s: [88] }, { t: 120, s: [18] }] }, o: { a: 1, k: [{ t: 0, s: [0] }, { t: 120, s: [360] }] }, m: 1, ix: 3, nm: 'Trim Paths 1', mn: 'ADBE Vector Filter - Trim', hd: false },
                { ty: 'tr', p: { a: 0, k: [0, 0] }, a: { a: 0, k: [0, 0] }, s: { a: 0, k: [100, 100] }, r: { a: 0, k: 0 }, o: { a: 0, k: 100 }, sk: { a: 0, k: 0 }, sa: { a: 0, k: 0 }, nm: 'Transform' },
            ],
            nm: 'Ellipse 1',
            np: 3,
            cix: 2,
            bm: 0,
            ix: 1,
            mn: 'ADBE Vector Group',
            hd: false,
        }],
        ip: 0,
        op: 120,
        st: 0,
        bm: 0,
    }],
};

function getResourcesPath(): string {
    return app.isPackaged
        ? path.join(process.resourcesPath)
        : path.join(__dirname, '..', '..');
}

function publishStartupStatus(status: Record<string, unknown>): void {
    startupStatusSnapshot = { ...startupStatusSnapshot, ...status };
    if (splashWindow && !splashWindow.isDestroyed()) {
        splashWindow.webContents.send('startup:status', startupStatusSnapshot);
    }
    if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send('startup:status', startupStatusSnapshot);
    }
}

function createSplashWindow(): void {
    splashWindow = new BrowserWindow({
        width: 560,
        height: 360,
        resizable: false,
        minimizable: false,
        maximizable: false,
        fullscreenable: false,
        frame: false,
        show: true,
        title: 'Slopsmith',
        backgroundColor: '#050508',
        webPreferences: {
            nodeIntegration: true,
            contextIsolation: false,
            sandbox: false,
            webSecurity: false,
        },
    });

    const html = `
<!doctype html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<style>
    body { margin: 0; background: radial-gradient(1200px 500px at 20% 10%, #131a2d, #050508); color: #cfd7e5; font-family: Inter, system-ui, sans-serif; }
    .wrap { height: 100vh; display: flex; align-items: center; justify-content: center; }
    .card { width: 440px; border: 1px solid rgba(255,255,255,0.08); border-radius: 16px; background: rgba(8,12,22,0.75); backdrop-filter: blur(6px); padding: 22px 24px; box-shadow: 0 18px 50px rgba(0,0,0,0.4); }
    .brand { font-size: 20px; font-weight: 700; letter-spacing: 0.2px; color: #f3f7ff; }
    .sub { margin-top: 3px; font-size: 12px; color: #86a0c4; }
    .anim { margin: 22px auto 10px; width: 86px; height: 86px; }
    .status { margin-top: 8px; min-height: 20px; font-size: 13px; color: #a9bbd5; text-align: center; }
    .count { font-size: 11px; color: #7186a9; margin-top: 6px; text-align: center; }
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div class="brand">Slopsmith</div>
    <div class="sub">Initializing desktop runtime</div>
    <div id="anim" class="anim"></div>
    <div id="status" class="status">Starting Slopsmith...</div>
    <div id="count" class="count"></div>
  </div>
</div>
<script>
    const { ipcRenderer } = require('electron');
    const lottie = require('lottie-web');
    const statusEl = document.getElementById('status');
    const countEl = document.getElementById('count');
    const animEl = document.getElementById('anim');
    const data = ${JSON.stringify(SPLASH_SPINNER_DATA)};
    try {
        lottie.loadAnimation({ container: animEl, renderer: 'svg', loop: true, autoplay: true, animationData: data });
    } catch (e) {}
    function render(status) {
        if (!status) return;
        statusEl.textContent = status.message || 'Starting Slopsmith...';
        if (status.total && status.total > 0) {
            countEl.textContent = (status.loaded || 0) + ' / ' + status.total;
        } else {
            countEl.textContent = '';
        }
    }
    ipcRenderer.on('startup:status', (_event, status) => render(status));
    render(${JSON.stringify(startupStatusSnapshot)});
</script>
</body>
</html>`;

    splashWindow.loadURL(`data:text/html;charset=UTF-8,${encodeURIComponent(html)}`);
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

async function startup(): Promise<void> {
    console.log('[main] Starting Slopsmith Desktop...');
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

    ipcMain.handle('startup:getStatus', () => startupStatusSnapshot);
    ipcMain.on('startup:requestStatus', (event) => {
        event.sender.send('startup:status', startupStatusSnapshot);
    });

    // Wait for Python server to be ready
    const port = await waitForPython();
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

    const startupDeadline = Date.now() + 300000; // 5 minutes
    while (Date.now() < startupDeadline) {
        const status = await getStartupStatus();
        if (status && typeof status === 'object') {
            publishStartupStatus(status as Record<string, unknown>);
            const phase = String((status as Record<string, unknown>).phase || '');
            const running = Boolean((status as Record<string, unknown>).running);
            if (!running && (phase === 'complete' || phase === 'error')) break;
        }
        await new Promise((resolve) => setTimeout(resolve, 700));
    }
    publishStartupStatus({ message: 'Ready', phase: 'complete', running: false });
    if (splashWindow && !splashWindow.isDestroyed()) splashWindow.close();

}

app.whenReady().then(startup);

app.on('window-all-closed', () => {
    shutdown();
    app.quit();
});

app.on('before-quit', () => {
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
