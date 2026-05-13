'use strict';
const path = require('path');
const addonPath = path.join(process.cwd(), 'build', 'Release', 'slopsmith_audio.node');
console.log('[test] loading addon from', addonPath);
const addon = require(addonPath);
console.log('[test] addon loaded; methods:', Object.keys(addon).slice(0, 10).join(', '), '...');

console.log('[test] addon.init()');
addon.init();

setTimeout(() => {
    const gr6 = 'C:\\Program Files\\Common Files\\VST3\\Guitar Rig 6.vst3';
    console.log('[test] calling addon.loadVST(' + gr6 + ')');
    let slot;
    try {
        slot = addon.loadVST(gr6);
        console.log('[test] loadVST returned slot:', slot);
    } catch (e) {
        console.log('[test] EXCEPTION on loadVST:', e.message);
        process.exit(1);
    }

    if (slot < 0) {
        console.log('[test] loadVST failed (negative slot); exiting');
        process.exit(1);
    }

    setTimeout(() => {
        console.log('[test] calling addon.openPluginEditor(' + slot + ')');
        try {
            const ok = addon.openPluginEditor(slot);
            console.log('[test] openPluginEditor returned:', ok);
        } catch (e) {
            console.log('[test] EXCEPTION on openPluginEditor:', e.message);
            try { addon.shutdown(); } catch (_) {}
            process.exit(1);
        }
        console.log('[test] sleeping 5s for editor creation + potential crash...');
        setTimeout(() => {
            console.log('[test] still alive after editor wait; closing');
            try { addon.closePluginEditor(slot); } catch (e) {}
            try { addon.shutdown(); } catch (e) {}
            setTimeout(() => process.exit(0), 1000);
        }, 5000);
    }, 1500);
}, 2000);
