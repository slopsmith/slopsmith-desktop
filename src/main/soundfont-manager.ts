// Soundfont manager — tracks the active soundfont quality ('default' = bundled
// GeneralUser GS, 'high' = downloaded FluidR3_GM), downloads the high-quality
// soundfont to the user data dir on demand, and exposes IPC for the renderer.
//
// Persistence lives in <userData>/slopsmith-desktop.json. The active
// SLOPSMITH_SOUNDFONT env var is chosen in python.ts at spawn time by reading
// the same config, so flipping quality requires a Python restart.

import { app, BrowserWindow, ipcMain } from 'electron';
import * as fs from 'fs';
import * as http from 'http';
import * as https from 'https';
import * as path from 'path';
import * as crypto from 'crypto';
import { restartPython } from './python';

// ── Source of truth for the high-quality soundfont ──────────────────────────
// Self-hosted mirror of FluidR3_GM.sf2 on this repo's releases. When a new
// soundfonts release ships, bump both constants together; a mismatch fails
// the download fast with a clear error rather than serving silently-wrong
// bytes. Source + licence info live on the release page itself.
const SOUNDFONT_URL =
    'https://github.com/slopsmith/slopsmith-desktop/releases/download/soundfonts-v1/FluidR3_GM.sf2';
const SOUNDFONT_SHA256 = '74594e8f4250680adf590507a306655a299935343583256f3b722c48a1bc1cb0';
const SOUNDFONT_EXPECTED_SIZE_MB = 142;

// ── Config + path helpers ───────────────────────────────────────────────────

export type SoundfontQuality = 'default' | 'high';

interface DesktopConfig {
    soundfontQuality?: SoundfontQuality;
}

function configPath(): string {
    return path.join(app.getPath('userData'), 'slopsmith-desktop.json');
}

export function getDesktopConfig(): DesktopConfig {
    try {
        return JSON.parse(fs.readFileSync(configPath(), 'utf-8')) as DesktopConfig;
    } catch {
        return {};
    }
}

export function setDesktopConfig(patch: Partial<DesktopConfig>): void {
    const current = getDesktopConfig();
    const next = { ...current, ...patch };
    const tmp = configPath() + '.tmp';
    fs.writeFileSync(tmp, JSON.stringify(next, null, 2));
    fs.renameSync(tmp, configPath());
}

function soundfontsDir(): string {
    const dir = path.join(app.getPath('userData'), 'soundfonts');
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
    return dir;
}

function highQualityPath(): string {
    return path.join(soundfontsDir(), 'FluidR3_GM.sf2');
}

/**
 * Returns the absolute path that `python.ts` should export as
 * `SLOPSMITH_SOUNDFONT`, or `null` to let the core's `_find_soundfont()`
 * fall through to the bundled GeneralUser GS in `RESOURCESPATH/soundfonts/`.
 */
export function getActiveSoundfontPath(): string | null {
    const cfg = getDesktopConfig();
    if (cfg.soundfontQuality === 'high' && fs.existsSync(highQualityPath())) {
        return highQualityPath();
    }
    return null;
}

// ── Download manager ────────────────────────────────────────────────────────

interface DownloadState {
    request: http.ClientRequest | null;
    writeStream: fs.WriteStream | null;
    partialPath: string | null;
}

const state: DownloadState = { request: null, writeStream: null, partialPath: null };

function cleanupPartial(): void {
    if (state.writeStream) {
        try { state.writeStream.destroy(); } catch { /* ignore */ }
        state.writeStream = null;
    }
    if (state.partialPath && fs.existsSync(state.partialPath)) {
        try { fs.unlinkSync(state.partialPath); } catch { /* ignore */ }
    }
    state.partialPath = null;
    state.request = null;
}

function fetchFollowingRedirects(url: string): Promise<http.IncomingMessage> {
    return new Promise((resolve, reject) => {
        const go = (u: string, depth: number) => {
            if (depth > 5) return reject(new Error('Too many redirects'));
            const req = https.get(u, (res) => {
                const status = res.statusCode || 0;
                const location = res.headers.location;
                if (status >= 300 && status < 400 && location) {
                    res.resume();
                    const next = new URL(location, u).toString();
                    go(next, depth + 1);
                    return;
                }
                if (status !== 200) {
                    res.resume();
                    reject(new Error(`HTTP ${status} fetching ${u}`));
                    return;
                }
                resolve(res);
            });
            req.on('error', reject);
            state.request = req;
        };
        go(url, 0);
    });
}

async function downloadHighQuality(
    window: BrowserWindow,
): Promise<{ success: boolean; message: string; path?: string }> {
    if (state.request || state.writeStream) {
        return { success: false, message: 'Download already in progress' };
    }

    const finalPath = highQualityPath();
    const partialPath = finalPath + '.partial';
    state.partialPath = partialPath;

    try {
        // Remove any stale partial before starting.
        if (fs.existsSync(partialPath)) fs.unlinkSync(partialPath);

        const res = await fetchFollowingRedirects(SOUNDFONT_URL);
        const totalBytes = parseInt(res.headers['content-length'] || '0', 10);

        const writeStream = fs.createWriteStream(partialPath);
        state.writeStream = writeStream;
        const hash = crypto.createHash('sha256');
        let bytesDownloaded = 0;
        let lastProgressAt = 0;

        res.on('data', (chunk: Buffer) => {
            hash.update(chunk);
            bytesDownloaded += chunk.length;
            // Throttle progress events to ~5/sec so we don't flood IPC.
            const now = Date.now();
            if (now - lastProgressAt > 200) {
                lastProgressAt = now;
                const percent = totalBytes > 0 ? (bytesDownloaded / totalBytes) * 100 : 0;
                if (!window.isDestroyed()) {
                    window.webContents.send('soundfont:downloadProgress', {
                        bytesDownloaded, totalBytes, percent,
                    });
                }
            }
        });

        await new Promise<void>((resolve, reject) => {
            res.pipe(writeStream);
            writeStream.on('finish', () => resolve());
            writeStream.on('error', reject);
            res.on('error', reject);
        });

        state.writeStream = null;
        state.request = null;

        const digest = hash.digest('hex');
        if (digest !== SOUNDFONT_SHA256) {
            fs.unlinkSync(partialPath);
            return {
                success: false,
                message: `Checksum mismatch — expected ${SOUNDFONT_SHA256}, got ${digest}. File has been discarded.`,
            };
        }

        fs.renameSync(partialPath, finalPath);
        state.partialPath = null;

        if (!window.isDestroyed()) {
            window.webContents.send('soundfont:downloadProgress', {
                bytesDownloaded, totalBytes, percent: 100,
            });
        }

        return { success: true, message: 'Download complete', path: finalPath };
    } catch (err: any) {
        cleanupPartial();
        return { success: false, message: `Download failed: ${err?.message || err}` };
    }
}

function cancelDownload(): { success: boolean; message: string } {
    if (!state.request && !state.writeStream) {
        return { success: false, message: 'No download in progress' };
    }
    try { state.request?.destroy(new Error('Download cancelled by user')); } catch { /* ignore */ }
    cleanupPartial();
    return { success: true, message: 'Download cancelled' };
}

// ── IPC wiring ──────────────────────────────────────────────────────────────

export function initSoundfontManager(getMainWindow: () => BrowserWindow | null): void {
    ipcMain.handle('soundfont:getStatus', () => {
        const cfg = getDesktopConfig();
        const highPath = highQualityPath();
        const highDownloaded = fs.existsSync(highPath);
        return {
            activeQuality: cfg.soundfontQuality === 'high' ? 'high' : 'default',
            highDownloaded,
            highPath: highDownloaded ? highPath : null,
            downloadInProgress: !!state.request || !!state.writeStream,
            expectedSizeMB: SOUNDFONT_EXPECTED_SIZE_MB,
        };
    });

    ipcMain.handle('soundfont:downloadHighQuality', async () => {
        const win = getMainWindow();
        if (!win) return { success: false, message: 'No main window' };
        return await downloadHighQuality(win);
    });

    ipcMain.handle('soundfont:cancelDownload', () => {
        return cancelDownload();
    });

    ipcMain.handle('soundfont:setQuality', (_event, quality: SoundfontQuality) => {
        if (quality !== 'default' && quality !== 'high') {
            return { success: false, message: `Unknown quality: ${quality}` };
        }
        if (quality === 'high' && !fs.existsSync(highQualityPath())) {
            return { success: false, message: 'High-quality soundfont not downloaded yet' };
        }
        setDesktopConfig({ soundfontQuality: quality });
        restartPython();
        return { success: true, message: 'Restarting audio engine…' };
    });

    // Clean up any partial on quit so disk state stays tidy.
    app.on('before-quit', () => {
        cleanupPartial();
    });
}
