// Minimal preload for the splash window.
// Exposes only the startup-status IPC bridge — no node APIs in the renderer.

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('splashBridge', {
    onStatus: (callback: (status: Record<string, unknown>) => void): (() => void) => {
        const listener = (_event: unknown, status: Record<string, unknown>) => callback(status);
        ipcRenderer.on('startup:status', listener);
        ipcRenderer.send('startup:requestStatus');
        return () => ipcRenderer.removeListener('startup:status', listener);
    },
});
