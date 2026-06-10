'use strict';
const {
  app, BrowserWindow, ipcMain, Tray, Menu, nativeImage,
} = require('electron');
const path  = require('path');
const fs    = require('fs');
const { spawn } = require('child_process');

app.name = 'TheAudio.app';
if (process.platform === 'win32') {
  app.setAppUserModelId('app.theaudio');
}

// Redirect all Electron user-data storage away from %APPDATA% to %PROGRAMDATA%
const USER_DATA_DIR = path.join(process.env.PROGRAMDATA || 'C:\\ProgramData', 'TheAudio.app');
app.setPath('userData', USER_DATA_DIR);

/* ── constants ──────────────────────────────────────────────────── */
const isDev   = !app.isPackaged;
const APP_DIR = isDev
  ? path.join(__dirname, '..', 'Source', 'theaudioapp', 'x64', 'Release')
  : path.join(process.resourcesPath, 'app');

const MAIN_EXE      = path.join(APP_DIR, 'theaudioapp.exe');
const SETTINGS_PATH = path.join(APP_DIR, 'settings.json');
const PROFILES_PATH = path.join(USER_DATA_DIR, 'profiles.json');


/* ── state ──────────────────────────────────────────────────────── */
let mainWindow  = null;
let tray        = null;
let coreProcess = null;
let lastSessions  = [];
let lastEndpoints = [];
let lastStatus    = { running: false };
let lastLevels    = [0, 0, 0, 0];
let lastSystem    = { volume: 1, muted: false, peak: 0 };

/* ── settings helpers ───────────────────────────────────────────── */
let settingsCache = null;

function readSettings() {
  try {
    settingsCache = JSON.parse(fs.readFileSync(SETTINGS_PATH, 'utf8'));
  } catch {
    settingsCache = { routing: {} };
  }
  return settingsCache;
}

function writeSettings(data) {
  if (!settingsCache) readSettings();
  Object.assign(settingsCache, data);
  fs.writeFileSync(SETTINGS_PATH, JSON.stringify(settingsCache, null, 2), 'utf8');
}

/* ── profiles helpers ───────────────────────────────────────────── */
function readProfiles() {
  try {
    return JSON.parse(fs.readFileSync(PROFILES_PATH, 'utf8'));
  } catch {
    return [];
  }
}

function writeProfiles(profiles) {
  fs.writeFileSync(PROFILES_PATH, JSON.stringify(profiles, null, 2), 'utf8');
}

/* ── C++ core process ───────────────────────────────────────────── */
function startCore() {
  if (coreProcess || !fs.existsSync(MAIN_EXE)) return;

  coreProcess = spawn(MAIN_EXE, [], { stdio: ['pipe', 'pipe', 'pipe'] });

  let buffer = '';
  coreProcess.stdout.on('data', (chunk) => {
    buffer += chunk.toString('utf8');
    const lines = buffer.split('\n');
    buffer = lines.pop();
    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed) continue;
      try { handleCoreMessage(JSON.parse(trimmed)); } catch { }
    }
  });

  coreProcess.stderr?.on('data', (d) => {
    console.error('[C++]', d.toString().trimEnd());
  });

  coreProcess.on('exit', () => {
    coreProcess = null;
    lastStatus  = { ...lastStatus, running: false };
    mainWindow?.webContents.send('status', lastStatus);
  });
}

function stopCore() {
  coreProcess?.kill();
  coreProcess = null;
}

function sendCmd(obj) {
  if (!coreProcess?.stdin?.writable) return;
  coreProcess.stdin.write(JSON.stringify(obj) + '\n');
}

function handleCoreMessage(msg) {
  if (!mainWindow) return;
  switch (msg.type) {
    case 'sessions':
      lastSessions = msg.sessions;
      mainWindow.webContents.send('sessions', msg.sessions);
      break;
    case 'endpoints':
      lastEndpoints = msg.endpoints;
      mainWindow.webContents.send('endpoints', msg.endpoints);
      break;
    case 'status':
      lastStatus = { running: msg.running };
      mainWindow.webContents.send('status', lastStatus);
      break;
    case 'levels':
      lastLevels = msg.levels;
      mainWindow.webContents.send('levels', msg.levels);
      break;
    case 'system':
      lastSystem = { volume: msg.volume, muted: msg.muted, peak: msg.peak };
      mainWindow.webContents.send('system', lastSystem);
      break;
    case 'endpointLevels':
      mainWindow.webContents.send('endpointLevels', msg.levels);
      break;
  }
}

/* ── window ─────────────────────────────────────────────────────── */
function createWindow() {
  mainWindow = new BrowserWindow({
    width: 900, height: 620, minWidth: 800, minHeight: 560,
    frame: false,
    skipTaskbar: true,
    backgroundColor: '#0f1117',
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  if (isDev) {
    mainWindow.loadURL('http://localhost:5173');
    mainWindow.webContents.openDevTools({ mode: 'detach' });
  } else {
    mainWindow.loadFile(path.join(__dirname, 'dist', 'index.html'));
  }

  mainWindow.on('close', (e) => { e.preventDefault(); mainWindow.setSkipTaskbar(true); mainWindow.hide(); });

  mainWindow.webContents.on('did-finish-load', () => {
    if (lastSessions.length)  mainWindow.webContents.send('sessions',  lastSessions);
    if (lastEndpoints.length) mainWindow.webContents.send('endpoints', lastEndpoints);
    mainWindow.webContents.send('status', lastStatus);
    mainWindow.webContents.send('levels', lastLevels);
    mainWindow.webContents.send('system', lastSystem);
  });
}

/* ── tray ───────────────────────────────────────────────────────── */
function createTray() {
  const iconPath = path.join(__dirname, 'assets', 'tray.png');
  const icon = fs.existsSync(iconPath)
    ? nativeImage.createFromPath(iconPath)
    : nativeImage.createEmpty();

  tray = new Tray(icon);
  tray.setToolTip('TheAudio.app');
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: 'Open', click: () => { mainWindow.setSkipTaskbar(false); mainWindow.show(); mainWindow.focus(); } },
    { type: 'separator' },
    { label: 'Quit', click: () => app.quit() },
  ]));
  tray.on('double-click', () => { mainWindow.setSkipTaskbar(false); mainWindow.show(); mainWindow.focus(); });
}

/* ── IPC handlers ───────────────────────────────────────────────── */
ipcMain.handle('set-routing',    (_e, routing)     => { if (!settingsCache) readSettings(); settingsCache.routing = { ...settingsCache.routing, ...routing }; writeSettings({}); });
ipcMain.handle('set-volume',     (_e, name, vol)   => sendCmd({ cmd: 'volume', name, value: vol }));
ipcMain.handle('set-mute',       (_e, name, muted) => sendCmd({ cmd: 'mute', name, muted }));
ipcMain.handle('get-profiles',   ()                => readProfiles());
ipcMain.handle('save-profile',   (_e, profile)     => { const p = readProfiles().filter(x => x.name !== profile.name); p.push(profile); writeProfiles(p); });
ipcMain.handle('delete-profile', (_e, name)        => writeProfiles(readProfiles().filter(p => p.name !== name)));
ipcMain.handle('apply-profile',  (_e, name)        => {
  const p = readProfiles().find(x => x.name === name);
  if (!p) return;
  writeSettings({ routing: p.routing });
  if (p.volumes) for (const [n, v] of Object.entries(p.volumes)) sendCmd({ cmd: 'volume', name: n, value: v });
});
ipcMain.handle('get-settings',   ()                => readSettings());
ipcMain.handle('save-settings',  (_e, s)           => writeSettings(s));


const iconCache = new Map();
ipcMain.handle('get-icon', async (_e, exePath) => {
  if (!exePath) return null;
  if (iconCache.has(exePath)) return iconCache.get(exePath);
  try {
    const img = await app.getFileIcon(exePath, { size: 'normal' });
    const url = img.toDataURL();
    iconCache.set(exePath, url);
    return url;
  } catch { return null; }
});

ipcMain.handle('get-endpoints',    () => lastEndpoints);
ipcMain.handle('get-sessions',     () => lastSessions);
ipcMain.handle('get-levels',       () => lastLevels);
ipcMain.handle('get-system',       () => lastSystem);
ipcMain.handle('set-system-volume',  (_e, vol)            => sendCmd({ cmd: 'systemVolume', value: vol }));
ipcMain.handle('set-system-mute',    (_e, muted)          => sendCmd({ cmd: 'systemMute', muted }));
ipcMain.handle('set-device-volume',  (_e, deviceId, vol)  => sendCmd({ cmd: 'deviceVolume', deviceId, value: vol }));
ipcMain.handle('set-device-mute',    (_e, deviceId, muted)=> sendCmd({ cmd: 'deviceMute',   deviceId, muted }));

ipcMain.on('window-minimize', () => mainWindow?.minimize());
ipcMain.on('window-close',    () => mainWindow?.hide());

/* ── app lifecycle ──────────────────────────────────────────────── */
app.whenReady().then(() => { createWindow(); createTray(); startCore(); });
app.on('window-all-closed', (e) => e.preventDefault());
app.on('before-quit', () => { stopCore(); tray?.destroy(); });
