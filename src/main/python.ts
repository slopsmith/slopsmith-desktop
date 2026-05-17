// Python subprocess management for the embedded Slopsmith server.
// Starts the FastAPI server as a child process, monitors health,
// and forwards logs.

import { ChildProcess, spawn } from 'child_process';
import * as path from 'path';
import * as fs from 'fs';
import { app } from 'electron';
import * as http from 'http';
import * as net from 'net';
import { getActiveSoundfontPath } from './soundfont-manager';

let pythonProcess: ChildProcess | null = null;
let serverPort = 18000; // Use 18000+ to avoid conflicting with Docker Slopsmith on 8000
let serverReady = false;
// Set by startPython when the backend cannot even be spawned (e.g. server.py
// missing). waitForPython checks this so a config error fails fast with a
// specific message instead of running out the full readiness timeout.
let startupError: string | null = null;
// True once waitForPython has confirmed the server with a successful HTTP
// probe. Until then, any child exit is a startup failure — serverReady
// alone is not enough, since the child can log "startup complete" and then
// exit before the probe succeeds.
let startupComplete = false;

export function getPythonPort(): number {
    return serverPort;
}

export interface StartupStatus {
    running: boolean;
    phase: string;
    message: string;
    /** Name of the plugin currently being loaded (raw string value from the backend `current_plugin` field). */
    currentPlugin: string;
    loaded: number;
    total: number;
    error?: string | null;
}

/** Shape of the raw JSON returned by the backend `/api/startup-status` endpoint. */
interface RawStartupStatus {
    running: boolean;
    phase: string;
    message: string;
    current_plugin: string;
    loaded: number;
    total: number;
    error?: string | null;
}

function isRawStartupStatus(value: unknown): value is RawStartupStatus {
    if (typeof value !== 'object' || value === null) return false;
    const v = value as Record<string, unknown>;
    return (
        typeof v['running'] === 'boolean' &&
        typeof v['phase'] === 'string' &&
        typeof v['message'] === 'string' &&
        typeof v['current_plugin'] === 'string' &&
        typeof v['loaded'] === 'number' && Number.isFinite(v['loaded']) &&
        typeof v['total'] === 'number' && Number.isFinite(v['total']) &&
        (v['error'] === undefined || v['error'] === null || typeof v['error'] === 'string')
    );
}

function getJson(pathname: string, timeoutMs = 2000): Promise<unknown> {
    return new Promise((resolve) => {
        let settled = false;
        const done = (value: unknown) => { if (!settled) { settled = true; resolve(value); } };

        const req = http.get(`http://127.0.0.1:${serverPort}${pathname}`, (res) => {
            let raw = '';
            res.on('data', (chunk) => { raw += chunk.toString(); });
            res.on('end', () => {
                try {
                    done(JSON.parse(raw));
                } catch {
                    done(null);
                }
            });
            // Guard against mid-transfer stream errors (e.g. connection reset).
            // Without this, Node emits an unhandled 'error' event that crashes
            // the main process, or the promise never resolves.
            res.on('error', () => done(null));
        });
        req.on('error', () => done(null));
        req.setTimeout(timeoutMs, () => { req.destroy(); done(null); });
    });
}

// Find an available port starting from 8000
async function findPort(startPort: number): Promise<number> {
    return new Promise((resolve) => {
        const server = net.createServer();
        server.listen(startPort, '127.0.0.1', () => {
            server.close(() => resolve(startPort));
        });
        server.on('error', () => {
            resolve(findPort(startPort + 1));
        });
    });
}

function findPythonExecutable(): string {
    // In packaged app, look for bundled Python
    if (app.isPackaged) {
        const resourcesPath = process.resourcesPath;
        const candidates = [
            // Linux/macOS: venv with --copies or full runtime
            path.join(resourcesPath, 'python', 'runtime', 'bin', 'python3'),
            path.join(resourcesPath, 'python', 'runtime', 'bin', 'python'),
            // Windows: embedded Python
            path.join(resourcesPath, 'python', 'python.exe'),
            // Fallback paths
            path.join(resourcesPath, 'python', 'bin', 'python3'),
            path.join(resourcesPath, 'python', 'python3'),
            path.join(resourcesPath, 'python', 'python'),
        ];
        for (const candidate of candidates) {
            if (fs.existsSync(candidate)) return candidate;
        }
    }

    // Development: check for local venv first
    const venvPython = path.join(__dirname, '..', '..', '.venv', 'bin', 'python3');
    if (fs.existsSync(venvPython)) return venvPython;

    return 'python3';
}

function findSlopsmithDir(): string {
    // In packaged app, Slopsmith is bundled in resources
    if (app.isPackaged) {
        return path.join(process.resourcesPath, 'slopsmith');
    }

    // Development — same resolution order as scripts/setup-dev.sh:
    //   1. $SLOPSMITH_DIR
    //   2. ../slopsmith (sibling to slopsmith-desktop)
    //   3. ~/Repositories/slopsmith (legacy)
    // An explicit $SLOPSMITH_DIR is honoured verbatim — never fall through
    // to a sibling/legacy checkout, so a typo or partial checkout surfaces
    // as a clear "server.py not found" error in startPython instead of
    // silently starting a different Slopsmith. Matches the build scripts
    // (bundle-python.sh, build-macos.sh). For the unset case a fallback
    // candidate only counts if it actually contains server.py, so a partial
    // or unrelated ../slopsmith directory cannot mask a valid legacy
    // checkout.
    const isSlopsmithRepo = (dir: string): boolean =>
        fs.existsSync(path.join(dir, 'server.py'));

    // $SLOPSMITH_DIR must be a native path. On Windows, pass a native path
    // (C:\\src\\slopsmith), not an MSYS/Git-Bash path (/c/src/slopsmith) —
    // Node resolves the latter against the current drive root. See README.
    const explicit = process.env.SLOPSMITH_DIR;
    if (explicit) return path.resolve(explicit);

    const siblingPath = path.join(__dirname, '..', '..', '..', 'slopsmith');
    if (isSlopsmithRepo(siblingPath)) return siblingPath;

    // app.getPath('home') is the platform-native home (USERPROFILE on
    // Windows, HOME on POSIX). process.env.HOME is an MSYS-style path such
    // as /c/Users/name when Electron is launched from Git Bash, which Node
    // misresolves — see the cacheBase comment in startPython.
    const legacyPath = path.join(app.getPath('home'), 'Repositories', 'slopsmith');
    if (isSlopsmithRepo(legacyPath)) return legacyPath;

    return siblingPath;
}

function getConfigDir(): string {
    // Share config with the Docker Slopsmith instance so DLC dir, favorites etc. are shared
    const sharedConfig = path.join(process.env.HOME || '', '.local', 'share', 'rocksmith-cdlc');
    if (fs.existsSync(sharedConfig)) return sharedConfig;

    // Fallback to app-specific config
    const configDir = path.join(app.getPath('userData'), 'slopsmith-config');
    if (!fs.existsSync(configDir)) {
        fs.mkdirSync(configDir, { recursive: true });
    }
    return configDir;
}

function getPluginsDir(): string {
    const pluginsDir = path.join(app.getPath('userData'), 'plugins');
    if (!fs.existsSync(pluginsDir)) {
        fs.mkdirSync(pluginsDir, { recursive: true });
    }
    return pluginsDir;
}

function getDLCDir(): string {
    if (process.env.DLC_DIR && fs.existsSync(process.env.DLC_DIR)) return process.env.DLC_DIR;

    // Read from shared config
    const configFile = path.join(getConfigDir(), 'config.json');
    if (fs.existsSync(configFile)) {
        try {
            const cfg = JSON.parse(fs.readFileSync(configFile, 'utf-8'));
            if (cfg.dlc_dir && fs.existsSync(cfg.dlc_dir)) return cfg.dlc_dir;
        } catch { /* ignore */ }
    }

    // Common default locations
    const home = process.env.HOME || process.env.USERPROFILE || '';
    const candidates = [
        path.join(home, '.local', 'share', 'Steam', 'steamapps', 'common', 'Rocksmith2014', 'dlc'),
        path.join(home, 'Music', 'Rocksmith CDLC'),
    ];
    for (const dir of candidates) {
        if (fs.existsSync(dir)) return dir;
    }

    return candidates[0]; // fallback even if not found
}

export async function startPython(): Promise<void> {
    startupError = null;
    startupComplete = false;
    const slopsmithDir = findSlopsmithDir();
    const serverScript = path.join(slopsmithDir, 'server.py');

    if (!fs.existsSync(serverScript)) {
        // findSlopsmithDir returns a bundled path when packaged and ignores
        // SLOPSMITH_DIR there — so the remediation differs by mode.
        startupError = `Slopsmith server.py not found at ${serverScript}. `
            + (app.isPackaged
                ? 'The application bundle is incomplete or corrupt — reinstall Slopsmith Desktop.'
                : 'Set SLOPSMITH_DIR to your Slopsmith checkout, clone it to ../slopsmith, '
                  + 'or use ~/Repositories/slopsmith/.');
        console.error(`[python] ${startupError}`);
        return;
    }

    // Reset the readiness flag — this matters on restarts (`restartPython`),
    // otherwise waitForPython would short-circuit on a flag set by the
    // previous spawn and probe a server that hasn't actually started yet.
    serverReady = false;

    serverPort = await findPort(18000);
    const pythonPath = findPythonExecutable();
    const configDir = getConfigDir();
    const dlcDir = getDLCDir();
    const pluginsDir = getPluginsDir();
    const slopsmithPlugins = path.join(slopsmithDir, 'plugins');

    console.log(`[python] Starting ${pythonPath} ${serverScript} on port ${serverPort}`);
    console.log(`[python] Config dir: ${configDir}`);
    console.log(`[python] DLC dir: ${dlcDir}`);
    console.log(`[python] Slopsmith plugins: ${slopsmithPlugins}`);
    console.log(`[python] User plugins: ${pluginsDir}`);

    // Build PYTHONPATH to include slopsmith's lib directory
    const pythonPathParts = [
        slopsmithDir,
        path.join(slopsmithDir, 'lib'),
    ];
    // On Windows, include embedded Python's Lib/site-packages
    if (app.isPackaged && process.platform === 'win32') {
        const pythonDir = path.join(process.resourcesPath, 'python');
        pythonPathParts.push(path.join(pythonDir, 'Lib', 'site-packages'));
    }
    const pythonPathEnv = pythonPathParts.join(path.delimiter);

    // Pin ML model caches to a persistent root (XDG_CACHE_HOME if set,
    // otherwise the user's home/.cache) so demucs / torch / huggingface
    // weights survive across launches and stay shareable with any other
    // torch app on the machine. The libraries already pick those paths
    // by default, but spelling them out here keeps the cache anchored
    // even if a future Electron sandbox / AppImage relocates HOME.
    // sloppak_convert.py uses env.setdefault on TORCH_HOME and
    // XDG_CACHE_HOME, so values set here win over its CONFIG_DIR
    // fallback — the right behaviour for Desktop; Docker still falls
    // back to /config/torch_cache via the same setdefault.
    //
    // Home is resolved via app.getPath('home') rather than
    // process.env.HOME: HOME is unset on Windows (and some sandboxed
    // contexts), which would otherwise produce a relative `.cache` path
    // and pass HOME='' into the subprocess. app.getPath consults the
    // platform-correct source (HOME on POSIX, USERPROFILE on Windows).
    //
    // cacheBase derives from XDG_CACHE_HOME first so a user who pins
    // their cache to a non-default disk (e.g. XDG_CACHE_HOME=/mnt/big)
    // gets all three caches (XDG/TORCH/HF) under the same root rather
    // than splitting torch/HF off to ~/.cache.
    const homeDir = app.getPath('home');
    const cacheBase = process.env.XDG_CACHE_HOME || path.join(homeDir, '.cache');

    // Build environment for Python process
    const pythonEnv: Record<string, string> = {
        ...process.env as Record<string, string>,
        PYTHONPATH: pythonPathEnv,
        CONFIG_DIR: configDir,
        DLC_DIR: dlcDir,
        SLOPSMITH_PLUGINS_DIR: pluginsDir,
        HOME: homeDir,
        XDG_CACHE_HOME: cacheBase,
        TORCH_HOME: process.env.TORCH_HOME || path.join(cacheBase, 'torch'),
        HF_HOME: process.env.HF_HOME || path.join(cacheBase, 'huggingface'),
        RSCLI_PATH: app.isPackaged
            ? path.join(process.resourcesPath, 'bin', 'rscli', process.platform === 'win32' ? 'RsCli.exe' : 'RsCli')
            : path.join(__dirname, '..', '..', 'resources', 'bin', 'rscli', process.platform === 'win32' ? 'RsCli.exe' : 'RsCli'),
        RESOURCESPATH: app.isPackaged
            ? process.resourcesPath
            : path.join(__dirname, '..', '..', 'resources'),
        PATH: (app.isPackaged
            ? path.join(process.resourcesPath, 'bin') + path.delimiter
            : path.join(__dirname, '..', '..', 'resources', 'bin') + path.delimiter
        ) + (process.env.PATH || ''),
    };

    // Honour the "Audio Quality" preference: if the user has opted into the
    // high-quality FluidR3 soundfont and the file exists, point Python at it.
    // Otherwise fall through to the bundled GeneralUser GS via RESOURCESPATH.
    const activeSoundfont = getActiveSoundfontPath();
    if (activeSoundfont) {
        pythonEnv.SLOPSMITH_SOUNDFONT = activeSoundfont;
    }

    // Set PYTHONHOME for bundled Python on all platforms
    if (app.isPackaged) {
        if (process.platform === 'win32') {
            pythonEnv.PYTHONHOME = path.join(process.resourcesPath, 'python');
        } else {
            const runtimeDir = path.join(process.resourcesPath, 'python', 'runtime');
            pythonEnv.PYTHONHOME = runtimeDir;
            if (process.platform === 'linux') {
                const pythonLibDir = path.join(runtimeDir, 'lib');
                pythonEnv.LD_LIBRARY_PATH = pythonLibDir + path.delimiter + (process.env.LD_LIBRARY_PATH || '');
            }
        }

        // Point stdlib SSL at the bundled certifi CA bundle. `requests`
        // uses certifi automatically, but Slopsmith's own update checker
        // (and community plugins like slopsmith-update-manager) call
        // `urllib.request.urlopen`, which falls back to the platform's
        // system CA bundle and fails inside the AppImage with
        // `[SSL: CERTIFICATE_VERIFY_FAILED] unable to get local issuer
        // certificate` when /etc/ssl/certs isn't reachable from the
        // mount. SSL_CERT_FILE makes stdlib SSL pick certifi too.
        const certifiPath = process.platform === 'win32'
            ? path.join(process.resourcesPath, 'python', 'Lib', 'site-packages', 'certifi', 'cacert.pem')
            : (() => {
                const libDir = path.join(process.resourcesPath, 'python', 'runtime', 'lib');
                if (fs.existsSync(libDir)) {
                    for (const entry of fs.readdirSync(libDir)) {
                        if (entry.startsWith('python')) {
                            const p = path.join(libDir, entry, 'site-packages', 'certifi', 'cacert.pem');
                            if (fs.existsSync(p)) return p;
                        }
                    }
                }
                return '';
            })();
        if (certifiPath && fs.existsSync(certifiPath)) {
            pythonEnv.SSL_CERT_FILE = certifiPath;
            pythonEnv.REQUESTS_CA_BUNDLE = certifiPath;
        } else {
            console.warn('[python] certifi cacert.pem not found in bundle — HTTPS verification may fail');
        }
    }

    const child = spawn(pythonPath, [
        '-m', 'uvicorn', 'server:app',
        '--host', '127.0.0.1',
        '--port', String(serverPort),
        '--no-access-log',
    ], {
        cwd: slopsmithDir,
        env: pythonEnv,
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    pythonProcess = child;

    // Flip `serverReady` when uvicorn emits a startup signal on either
    // stdout or stderr. structlog routes these messages to stdout in dev
    // mode; plain uvicorn uses stderr — so we watch both.
    function checkReadiness(msg: string): void {
        if (!serverReady && (msg.includes('Uvicorn running on') || msg.includes('Application startup complete'))) {
            serverReady = true;
        }
    }

    child.stdout?.on('data', (data: Buffer) => {
        try {
            const msg = data.toString().trim();
            if (msg) {
                console.log(`[python:stdout] ${msg}`);
                checkReadiness(msg);
            }
        } catch { /* EPIPE or similar when the process dies mid-write */ }
    });

    // Swallow stream errors — EPIPE fires when the Python child dies while
    // we're still draining its pipes; that's expected, not a bug.
    child.stdout?.on('error', () => { /* ignore */ });

    child.stderr?.on('data', (data: Buffer) => {
        try {
            const msg = data.toString().trim();
            if (msg) {
                console.log(`[python] ${msg}`);
                checkReadiness(msg);
            }
        } catch { /* EPIPE or similar when the process dies mid-write */ }
    });

    child.stderr?.on('error', () => { /* ignore */ });

    child.on('close', (code: number | null, signal: NodeJS.Signals | null) => {
        // Ignore events from a child that restartPython has already
        // replaced — a stale 'close' must not null out the new process
        // or fail its startup.
        if (pythonProcess !== child) return;
        // On a signal exit Node reports code === null; surface the signal
        // so the cause (SIGKILL/OOM, SIGTERM, ...) is not lost.
        const exitDesc = code !== null ? `code ${code}` : `signal ${signal ?? 'unknown'}`;
        console.log(`[python] Process exited with ${exitDesc}`);
        // If the child exits before waitForPython confirmed the server
        // (HTTP probe), record a startupError so waitForPython fails fast
        // instead of polling out the full timeout. serverReady alone is not
        // enough — the child can log "startup complete" then exit before
        // the probe succeeds. startupComplete === true means startup already
        // succeeded (a later crash is not a startup failure).
        if (!startupComplete && !startupError) {
            startupError = `Python process exited before startup completed (${exitDesc}).`;
        }
        pythonProcess = null;
        serverReady = false;
    });

    child.on('error', (err: Error) => {
        // Ignore errors from a child restartPython has already replaced.
        if (pythonProcess !== child) return;
        console.error(`[python] Failed to start: ${err.message}`);
        // spawn itself failed (e.g. interpreter not found) — surface it to
        // waitForPython rather than waiting out the readiness timeout.
        if (!startupError) {
            startupError = `Failed to start Python process: ${err.message}`;
        }
        pythonProcess = null;
    });
}

export async function waitForPython(): Promise<number> {
    // Wait for the python child process we just spawned to be actually
    // ready to serve. Two signals are required, in order:
    //
    //   1. The stdout-or-stderr handler (via checkReadiness) flips
    //      `serverReady` to true when it sees uvicorn print
    //      "Application startup complete" or "Uvicorn running on".
    //      structlog routes these to stdout in dev mode; plain uvicorn
    //      uses stderr — so we watch both. This is the authoritative
    //      readiness signal — it comes from the child process we just
    //      spawned, so it can't be fooled by a zombie python from a
    //      prior crashed launch still listening on a different port.
    //
    //   2. Once that flag flips, a single HTTP probe to /api/plugins
    //      confirms the listener is reachable on the port we picked.
    //      Belt-and-suspenders against any race between the stderr
    //      message and the socket actually accepting connections.
    //
    // Generous total budget — with ~36 bundled plugins (whisper / NAM /
    // torch get imported), first-run lifespan startup easily exceeds
    // one minute on a cold cache.
    const maxAttempts = 600; // 5 minutes
    const intervalMs = 500;
    for (let i = 0; i < maxAttempts; i++) {
        // Fail fast on a config error startPython already diagnosed (e.g.
        // server.py missing) rather than waiting out the whole timeout.
        if (startupError) throw new Error(startupError);

        if (serverReady) {
            const ok = await new Promise<boolean>((resolve) => {
                const req = http.get(`http://127.0.0.1:${serverPort}/api/plugins`, (res) => {
                    resolve((res.statusCode || 0) >= 200 && (res.statusCode || 0) < 500);
                });
                req.on('error', () => resolve(false));
                req.setTimeout(2000, () => { req.destroy(); resolve(false); });
            });
            if (ok) {
                startupComplete = true;
                return serverPort;
            }
            // serverReady was set but HTTP probe failed — fall through
            // and retry (lifespan-complete may briefly precede socket
            // accept on slow machines).
        }

        // Periodic progress so a long startup doesn't look like a freeze.
        // Logged every 10 s so the dev console / launcher log shows life.
        if (i > 0 && (i * intervalMs) % 10000 === 0) {
            console.log(`[python] waiting for server on port ${serverPort} (${(i * intervalMs) / 1000}s elapsed)`);
        }
        await new Promise((r) => setTimeout(r, intervalMs));
    }

    throw new Error(`Python server failed to start on port ${serverPort} within ${(maxAttempts * intervalMs) / 1000}s`);
}

export async function getStartupStatus(): Promise<StartupStatus | null> {
    const data = await getJson('/api/startup-status');
    if (!isRawStartupStatus(data)) return null;
    return {
        running: data.running,
        phase: data.phase,
        message: data.message,
        currentPlugin: data.current_plugin,
        loaded: data.loaded,
        total: data.total,
        error: data.error,
    };
}

export function stopPython(): void {
    // Capture the child being stopped. During restartPython the global
    // pythonProcess may already point at a new child by the time the
    // force-kill timer fires — SIGTERM/SIGKILL must hit the one being
    // stopped, never the replacement.
    const proc = pythonProcess;
    if (!proc) return;

    console.log('[python] Stopping server...');

    // The stop is intentional, so detach this child from module state now:
    //   - pythonProcess = null makes proc's own 'close'/'error' handlers
    //     no-op (their `pythonProcess !== child` guard), so the expected
    //     shutdown exit can't be recorded as a startup failure;
    //   - clearing the startup flags means a restart (and any later
    //     waitForPython) starts from a clean slate rather than throwing a
    //     stale startupError left by the stopped child.
    pythonProcess = null;
    startupError = null;
    startupComplete = false;
    serverReady = false;

    // Try graceful shutdown first
    let exited = false;
    proc.kill('SIGTERM');

    // Force kill after timeout
    const killTimeout = setTimeout(() => {
        if (!exited) {
            console.log('[python] Force killing...');
            proc.kill('SIGKILL');
        }
    }, 5000);

    proc.once('close', () => {
        exited = true;
        clearTimeout(killTimeout);
    });
}

export function restartPython(): void {
    stopPython();
    // Wait a bit for port to be released, then restart
    setTimeout(() => startPython(), 1000);
}

export { getPluginsDir, getConfigDir };
