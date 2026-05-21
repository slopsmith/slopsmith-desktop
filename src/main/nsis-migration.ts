// NSIS → Velopack migration for Windows.
//
// Legacy users installed Slopsmith via an NSIS installer that wrote to
// `C:\Program Files\Slopsmith\Slopsmith.exe` and registered a per-machine
// uninstall key at:
//   HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\Slopsmith
//
// The new Velopack pipeline ships TWO Windows artifacts:
//   - Setup.exe (per-user, %LOCALAPPDATA%\Slopsmith\current\)
//   - Slopsmith-MachineSetup.msi (per-machine, %ProgramFiles%\Slopsmith\current\)
//
// Velopack layouts always contain a `\current\` segment in process.execPath;
// the NSIS layout never does — that's the detection signal.
//
// The upgrade flow:
//   1. Renderer banner offers "Upgrade Now" when isNSISInstall() returns true.
//   2. Main fetches the GitHub Releases "latest" record, finds the .msi asset,
//      and downloads it to %TEMP%\Slopsmith-Setup.msi.
//   3. Main spawns an elevated PowerShell that (under one UAC prompt) runs
//      the NSIS QuietUninstallString, waits for the HKLM uninstall key to
//      disappear, then runs `msiexec /i ... /qb /norestart`.
//   4. Main quits itself so the NSIS uninstaller can delete our locked
//      executable; the MSI then installs to Program Files and relaunches.

import { app } from 'electron';
import { spawn, execSync } from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

const RELEASES_LATEST = 'https://api.github.com/repos/byrongamatos/slopsmith-desktop/releases/latest';
const NSIS_UNINSTALL_REG_KEY = 'HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Slopsmith';

export interface UpgradeResult {
    ok: boolean;
    message?: string;
}

/**
 * True when this process is running from the legacy NSIS-installed location
 * (i.e. there is no `\current\` segment in process.execPath). Returns false
 * off-Windows, in dev/unpackaged builds, and on any Velopack-installed
 * layout (per-user or per-machine — both contain `\current\`).
 */
export function isNSISInstall(): boolean {
    if (process.platform !== 'win32') return false;
    if (!app.isPackaged) return false;
    // Windows paths are case-insensitive; process.execPath may surface
    // either casing depending on how the shortcut was created.
    return !process.execPath.toLowerCase().includes('\\current\\');
}

interface GithubAsset {
    name: string;
    browser_download_url: string;
}
interface GithubRelease {
    assets: GithubAsset[];
    tag_name: string;
}

async function fetchLatestMsiAsset(): Promise<GithubAsset | null> {
    const resp = await fetch(RELEASES_LATEST, {
        headers: { Accept: 'application/vnd.github+json' },
    });
    if (!resp.ok) {
        throw new Error(`GitHub Releases API returned ${resp.status} ${resp.statusText}`);
    }
    const release = await resp.json() as GithubRelease;
    // vpk --msi x64 emits a name like `Slopsmith-MachineSetup-win-x64-<channel>.msi`
    // (exact form varies by Velopack version). Match any `.msi` asset — the
    // Velopack feed only ever attaches one MSI per release.
    const msi = release.assets.find((a) => a.name.toLowerCase().endsWith('.msi'));
    return msi ?? null;
}

async function downloadToTemp(url: string, destName: string): Promise<string> {
    const dest = path.join(os.tmpdir(), destName);
    const resp = await fetch(url, { redirect: 'follow' });
    if (!resp.ok) {
        throw new Error(`Failed to download MSI: ${resp.status} ${resp.statusText}`);
    }
    const buf = Buffer.from(await resp.arrayBuffer());
    fs.writeFileSync(dest, buf);
    return dest;
}

interface ParsedQuietUninstall {
    exe: string;
    args: string[];
}

/**
 * Read the NSIS QuietUninstallString from HKLM and parse it into an exe path
 * and argv. Returns null when the key is absent (no NSIS install), unreadable,
 * or in an unexpected shape.
 *
 * We parse rather than passing the whole string through cmd /c because the
 * PowerShell → cmd quoting handoff is famously broken for paths with spaces
 * (the embedded quotes around the exe end up double-escaped and cmd fails to
 * find the file). Invoking the exe via PowerShell's `&` operator with a
 * separate ArgumentList sidesteps the whole mess.
 */
function readNsisQuietUninstall(): ParsedQuietUninstall | null {
    let raw: string;
    try {
        raw = execSync(
            `reg query "${NSIS_UNINSTALL_REG_KEY}" /v QuietUninstallString`,
            { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] },
        );
    } catch {
        return null;
    }
    // Output format:
    //     QuietUninstallString    REG_SZ    "C:\Program Files\Slopsmith\Uninstall Slopsmith.exe" /allusers /S
    const match = raw.match(/QuietUninstallString\s+REG_SZ\s+(.+)/);
    if (!match) return null;
    const value = match[1].trim();
    // Expect a quoted exe path followed by optional whitespace-delimited args.
    // NSIS always emits the path in quotes; if we see anything else, bail out.
    const exeMatch = value.match(/^"([^"]+)"\s*(.*)$/);
    if (!exeMatch) return null;
    const exe = exeMatch[1];
    const rest = exeMatch[2].trim();
    const args = rest.length > 0 ? rest.split(/\s+/) : [];
    return { exe, args };
}

/**
 * Build the PowerShell script that runs (elevated) the NSIS uninstall +
 * MSI install in sequence under a single UAC prompt.
 *
 * Two reliability tricks:
 *   - Sleep 3s up front so we can app.quit() before the uninstaller tries
 *     to delete our running Slopsmith.exe (which would fail with file-in-use).
 *   - Poll for the HKLM uninstall key to disappear. NSIS's silent uninstall
 *     spawns a detached temp copy and the original exe returns immediately —
 *     waiting on the original process tells us nothing. The registry key is
 *     the last thing NSIS removes before exiting, so its absence is a
 *     reliable "uninstall done" signal.
 */
function buildElevatedScript(uninstall: ParsedQuietUninstall | null, msiPath: string): string {
    // Escape single quotes for PowerShell single-quoted string literals.
    // The only special character inside '...' is the single quote itself,
    // doubled to escape it. Backslashes, $, etc. are all literal.
    const esc = (s: string): string => s.replace(/'/g, "''");

    // Build the PowerShell ArgumentList for the uninstaller — each arg is a
    // separate single-quoted literal so spaces and special chars inside an
    // individual arg can't collapse into the next one. NSIS args here are
    // typically `/allusers /S` so this is overkill but defensive.
    const uninstallBlock = uninstall
        ? `
# Run the NSIS uninstaller. PowerShell's & operator invokes a native exe with
# a clean argv — no cmd.exe re-parsing of quotes.
& '${esc(uninstall.exe)}' ${uninstall.args.map((a) => `'${esc(a)}'`).join(' ')} | Out-Null

# NSIS /S spawns a detached temp copy and returns immediately, so we poll
# for the registry key removal as the real "uninstall finished" signal.
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadline) {
    if (-not (Test-Path 'HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Slopsmith')) { break }
    Start-Sleep -Milliseconds 500
}
# Small extra grace so any in-flight file deletes settle before msiexec
# tries to write to the same directory.
Start-Sleep -Seconds 2
`
        : '';

    return `
$ErrorActionPreference = 'Continue'
# Give the unelevated launcher time to quit so the NSIS uninstaller can
# delete the locked Slopsmith.exe.
Start-Sleep -Seconds 3
${uninstallBlock}
# Install the new MSI. /qb shows a basic progress UI (better than /quiet
# which gives zero feedback during what can be a 30-60s install).
Start-Process -FilePath 'msiexec.exe' \`
    -ArgumentList '/i',('"' + '${esc(msiPath)}' + '"'),'/qb','/norestart' \`
    -Wait
`;
}

/**
 * Execute the NSIS → Velopack upgrade. Resolves before the elevated
 * uninstall+install actually completes — the caller should app.quit()
 * immediately so the NSIS uninstaller can succeed.
 */
export async function upgradeFromNSIS(): Promise<UpgradeResult> {
    if (process.platform !== 'win32') {
        return { ok: false, message: 'NSIS migration is Windows-only.' };
    }

    let msiAsset: GithubAsset | null;
    try {
        msiAsset = await fetchLatestMsiAsset();
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        return { ok: false, message: `Could not query latest release: ${message}` };
    }
    if (!msiAsset) {
        return {
            ok: false,
            message: 'Latest release does not include an MSI. Please download from GitHub Releases manually.',
        };
    }

    let msiPath: string;
    try {
        msiPath = await downloadToTemp(msiAsset.browser_download_url, 'Slopsmith-Setup.msi');
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        return { ok: false, message: `MSI download failed: ${message}` };
    }

    const quietUninstall = readNsisQuietUninstall();
    if (!quietUninstall) {
        // No NSIS uninstall entry found. The caller already gated on
        // isNSISInstall() so this is a real edge case (registry hand-edit
        // or partial install). Fall back to just running the MSI; the user
        // will end up with a stale Add/Remove Programs entry but a working
        // Velopack install.
        console.warn('[nsis-migration] No NSIS QuietUninstallString in registry; running MSI without uninstall step.');
    }

    const script = buildElevatedScript(quietUninstall, msiPath);

    // PowerShell -EncodedCommand expects UTF-16LE base64. This avoids every
    // shell-escape landmine in the script (multiline, embedded quotes, &).
    const encoded = Buffer.from(script, 'utf16le').toString('base64');

    // Outer (unelevated) PowerShell that fires the UAC prompt via
    // Start-Process -Verb RunAs. Once the user clicks Yes, the inner
    // (elevated) PowerShell runs the actual script.
    const outerCommand =
        `Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden `
        + `-ArgumentList '-NoProfile','-WindowStyle','Hidden','-EncodedCommand','${encoded}'`;

    try {
        const child = spawn('powershell.exe', [
            '-NoProfile',
            '-WindowStyle', 'Hidden',
            '-Command', outerCommand,
        ], { detached: true, stdio: 'ignore', windowsHide: true });
        child.unref();
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        return { ok: false, message: `Failed to launch elevated upgrade: ${message}` };
    }

    return { ok: true };
}
