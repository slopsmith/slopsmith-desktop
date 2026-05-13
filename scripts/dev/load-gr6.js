// Smoke-test harness for the Windows VST sandbox path. Loads the addon,
// spawns the sandbox subprocess for Guitar Rig 6, opens its editor, then
// closes and shuts down cleanly. Used by clean-rerun.cmd on the test VM.
//
// Run from the repo root: `node scripts/dev/load-gr6.js > load-gr6-sandbox.log`.
// The Guitar Rig 6 path is hardcoded to its standard Win11 install location;
// adjust the GR6 path below if your install differs.
'use strict';
const path = require('path');
const addonPath = path.join(process.cwd(), 'build', 'Release', 'slopsmith_audio.node');
console.log('[test] loading addon from', addonPath);
const addon = require(addonPath);
console.log('[test] addon loaded; methods:', Object.keys(addon).slice(0, 10).join(', '), '...');

// Global watchdog — best-effort coverage for *asynchronous* hang paths
// (event-loop livelocks, setTimeout-stacked cleanup). Cannot pre-empt a
// *synchronous* native block: loadVST and addon.shutdown both park the
// event loop via dispatchOnMessageThread → done->wait(15000) inside the
// addon, so if those block the timer callback never fires. The
// synchronous-native-hang case is bounded by JUCE's own 15 s timeout
// inside the addon; a proper supervisor-process + SIGKILL watchdog
// belongs in the CI harness (tracked in the test-suite follow-up).
//
// The timer callback hard-exits — do NOT call addon.shutdown() here,
// it would block on the same dispatchOnMessageThread the addon is
// already stuck in and deadlock the process.
const WATCHDOG_MS = 60000;
const watchdog = setTimeout(() => {
    console.error(`[test] FATAL: watchdog tripped after ${WATCHDOG_MS} ms (async hang)`);
    process.exit(1);
}, WATCHDOG_MS);

function failExit(msg) {
    if (msg) console.log('[test] FAIL:', msg);
    try { addon.shutdown(); } catch (_) {}
    try { clearTimeout(watchdog); } catch (_) {}
    process.exit(1);
}

console.log('[test] addon.init()');
try {
    addon.init();
} catch (e) {
    failExit('EXCEPTION on init: ' + e.message);
}

setTimeout(() => {
    // Allow override for CI / dev machines whose VST3 layout differs from
    // the standard "C:\Program Files\Common Files\VST3" install location.
    const gr6 = process.env.GR6_PATH
             || 'C:\\Program Files\\Common Files\\VST3\\Guitar Rig 6.vst3';
    console.log('[test] calling addon.loadVST(' + gr6 + ')');
    let slot;
    try {
        slot = addon.loadVST(gr6);
        console.log('[test] loadVST returned slot:', slot);
    } catch (e) {
        failExit('EXCEPTION on loadVST: ' + e.message);
        return;
    }

    if (!Number.isInteger(slot) || slot < 0) {
        failExit('loadVST returned invalid slot: ' + String(slot));
        return;
    }

    setTimeout(() => {
        console.log('[test] calling addon.openPluginEditor(' + slot + ')');
        let ok = false;
        try {
            ok = addon.openPluginEditor(slot);
            console.log('[test] openPluginEditor returned:', ok);
        } catch (e) {
            failExit('EXCEPTION on openPluginEditor: ' + e.message);
            return;
        }
        if (!ok) {
            try { addon.closePluginEditor(slot); } catch (_) {}
            failExit('openPluginEditor returned false');
            return;
        }
        console.log('[test] sleeping 5s for editor creation + potential crash...');
        setTimeout(() => {
            console.log('[test] still alive after editor wait; closing');
            try { addon.closePluginEditor(slot); } catch (e) {}
            try { addon.shutdown(); } catch (e) {}
            clearTimeout(watchdog);
            setTimeout(() => process.exit(0), 1000);
        }, 5000);
    }, 1500);
}, 2000);
