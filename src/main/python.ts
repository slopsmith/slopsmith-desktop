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

export function getPythonPort(): number {
    return serverPort;
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

    // Development: use the local slopsmith repo
    const devPath = path.join(process.env.HOME || '', 'Repositories', 'slopsmith');
    if (fs.existsSync(devPath)) return devPath;

    // Fallback: assume it's next to this repo
    return path.join(__dirname, '..', '..', '..', 'slopsmith');
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
    const slopsmithDir = findSlopsmithDir();
    const serverScript = path.join(slopsmithDir, 'server.py');

    if (!fs.existsSync(serverScript)) {
        console.error(`[python] server.py not found at ${serverScript}`);
        console.error('[python] Make sure slopsmith is available at ~/Repositories/slopsmith/');
        return;
    }

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

    // Build environment for Python process
    const pythonEnv: Record<string, string> = {
        ...process.env as Record<string, string>,
        PYTHONPATH: pythonPathEnv,
        CONFIG_DIR: configDir,
        DLC_DIR: dlcDir,
        SLOPSMITH_PLUGINS_DIR: pluginsDir,
        RSCLI_PATH: app.isPackaged
            ? path.join(process.resourcesPath, 'bin', 'rscli', process.platform === 'win32' ? 'RsCli.exe' : 'RsCli')
            : '',
        RESOURCESPATH: app.isPackaged ? process.resourcesPath : '',
        PATH: (app.isPackaged
            ? path.join(process.resourcesPath, 'bin') + path.delimiter
            : '') + (process.env.PATH || ''),
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
    }

    pythonProcess = spawn(pythonPath, [
        '-m', 'uvicorn', 'server:app',
        '--host', '127.0.0.1',
        '--port', String(serverPort),
        '--no-access-log',
    ], {
        cwd: slopsmithDir,
        env: pythonEnv,
        stdio: ['pipe', 'pipe', 'pipe'],
    });

    pythonProcess.stdout?.on('data', (data: Buffer) => {
        try {
            const msg = data.toString().trim();
            if (msg) console.log(`[python:stdout] ${msg}`);
        } catch { /* EPIPE or similar when the process dies mid-write */ }
    });

    // Swallow stream errors — EPIPE fires when the Python child dies while
    // we're still draining its pipes; that's expected, not a bug.
    pythonProcess.stdout?.on('error', () => { /* ignore */ });

    pythonProcess.stderr?.on('data', (data: Buffer) => {
        try {
            const msg = data.toString().trim();
            if (msg) {
                console.log(`[python] ${msg}`);
                // Detect uvicorn startup message
                if (msg.includes('Uvicorn running on') || msg.includes('Application startup complete')) {
                    serverReady = true;
                }
            }
        } catch { /* EPIPE or similar when the process dies mid-write */ }
    });

    pythonProcess.stderr?.on('error', () => { /* ignore */ });

    pythonProcess.on('close', (code: number | null) => {
        console.log(`[python] Process exited with code ${code}`);
        pythonProcess = null;
        serverReady = false;
    });

    pythonProcess.on('error', (err: Error) => {
        console.error(`[python] Failed to start: ${err.message}`);
        pythonProcess = null;
    });
}

export async function waitForPython(): Promise<number> {
    // Poll the server until it responds
    const maxAttempts = 120; // 60 seconds
    for (let i = 0; i < maxAttempts; i++) {
        try {
            const ok = await new Promise<boolean>((resolve) => {
                const req = http.get(`http://127.0.0.1:${serverPort}/api/plugins`, (res) => {
                    resolve(res.statusCode === 200);
                });
                req.on('error', () => resolve(false));
                req.setTimeout(500, () => { req.destroy(); resolve(false); });
            });
            if (ok) {
                serverReady = true;
                return serverPort;
            }
        } catch { /* retry */ }

        await new Promise((r) => setTimeout(r, 500));
    }

    throw new Error(`Python server failed to start on port ${serverPort}`);
}

export function stopPython(): void {
    if (!pythonProcess) return;

    console.log('[python] Stopping server...');

    // Try graceful shutdown first
    pythonProcess.kill('SIGTERM');

    // Force kill after timeout
    const killTimeout = setTimeout(() => {
        if (pythonProcess) {
            console.log('[python] Force killing...');
            pythonProcess.kill('SIGKILL');
        }
    }, 5000);

    pythonProcess.on('close', () => {
        clearTimeout(killTimeout);
    });
}

export function restartPython(): void {
    stopPython();
    // Wait a bit for port to be released, then restart
    setTimeout(() => startPython(), 1000);
}

export { getPluginsDir, getConfigDir };
