// Central registry of IPC channel names shared between the main process and
// preload scripts. Import this module in both sides so a rename never drifts.

export const IPC_STARTUP_STATUS = 'startup:status' as const;
export const IPC_STARTUP_GET_STATUS = 'startup:getStatus' as const;
export const IPC_STARTUP_REQUEST_STATUS = 'startup:requestStatus' as const;

// Auto-update (Velopack). The renderer (Settings panel + restart banner) reads
// status, switches release channel, kicks a manual check, and applies a
// downloaded update.
export const IPC_UPDATE_GET_STATUS = 'update:getStatus' as const;
export const IPC_UPDATE_SET_CHANNEL = 'update:setChannel' as const;
export const IPC_UPDATE_CHECK_NOW = 'update:checkNow' as const;
export const IPC_UPDATE_APPLY = 'update:apply' as const;

// One-way push events the main side broadcasts to every BrowserWindow via
// webContents.send (not ipcMain.handle channels). Registered here so the
// update-manager broadcaster and the preload listeners can't drift.
export const IPC_UPDATE_EVENT_AVAILABLE = 'update:available' as const;
export const IPC_UPDATE_EVENT_DOWNLOADED = 'update:downloaded' as const;
