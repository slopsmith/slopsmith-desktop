// Minimal preload for the splash window.
// Exposes only the startup-status IPC bridge — no node APIs in the renderer.

const { contextBridge, ipcRenderer } = require('electron');
import type { StartupStatus } from './python';

contextBridge.exposeInMainWorld('splashBridge', {
    onStatus: (callback: (status: StartupStatus) => void): (() => void) => {
        const listener = (_event: unknown, status: StartupStatus) => callback(status);
        ipcRenderer.on('startup:status', listener);
        ipcRenderer.send('startup:requestStatus');
        return () => ipcRenderer.removeListener('startup:status', listener);
    },
});

export {};
