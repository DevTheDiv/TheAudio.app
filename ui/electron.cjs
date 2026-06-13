'use strict';
const {
  app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, dialog,
} = require('electron');
const path  = require('path');
const fs    = require('fs');
const { spawn } = require('child_process');

app.name = 'TheAudio.app';
if (process.platform === 'win32') {
  app.setAppUserModelId('app.theaudio');
}

if (!app.requestSingleInstanceLock()) {
  app.exit(0);
}

app.on('second-instance', () => {
  if (mainWindow) {
    mainWindow.setSkipTaskbar(false);
    mainWindow.show();
    mainWindow.focus();
  }
});

// Redirect all Electron user-data storage away from %APPDATA% to %PROGRAMDATA%
const USER_DATA_DIR = path.join(process.env.PROGRAMDATA || 'C:\\ProgramData', 'TheAudio.app');
app.setPath('userData', USER_DATA_DIR);

/* ── constants ──────────────────────────────────────────────────── */
const isDev      = !app.isPackaged;
const APP_DIR    = isDev
  ? path.join(__dirname, '..', 'core', 'x64', 'Release')
  : path.join(process.resourcesPath, 'app');
const DRIVER_DIR = isDev
  ? path.join(__dirname, '..', 'Driver', 'VirtualAudioDriver')
  : path.join(process.resourcesPath, 'driver');

const MAIN_EXE      = path.join(APP_DIR, 'theaudioapp.exe');
const SETTINGS_PATH = path.join(APP_DIR, 'settings.json');
const PROFILES_PATH = path.join(USER_DATA_DIR, 'profiles.json');


/* ── state ──────────────────────────────────────────────────────── */
let mainWindow  = null;
let tray        = null;
let coreProcess = null;
let isQuitting  = false;
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
async function startCore() {
  if (coreProcess || !fs.existsSync(MAIN_EXE)) return;
  // Kill any orphaned instance holding the single-instance mutex from a previous crash,
  // then wait 300ms for the OS to fully release the mutex before spawning a new one.
  if (process.platform === 'win32') {
    try { require('child_process').execSync('taskkill /IM theaudioapp.exe /F', { timeout: 2000 }); } catch {}
    await new Promise(r => setTimeout(r, 300));
  }

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
  if (!coreProcess) return;
  const pid = coreProcess.pid;
  // Ask C++ to exit cleanly first — it breaks its main loop, releases mutex, and exits.
  try { coreProcess.stdin?.write(JSON.stringify({ cmd: 'quit' }) + '\n'); } catch {}
  try { coreProcess.kill(); } catch {}
  coreProcess = null;
  if (process.platform === 'win32' && pid) {
    try { require('child_process').execSync(`taskkill /PID ${pid} /F /T`, { timeout: 3000 }); } catch {}
  }
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
      lastStatus = { running: msg.running, driverInstalled: msg.driverInstalled ?? false };
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

  mainWindow.on('close', (e) => {
    if (isQuitting) return;
    e.preventDefault();
    mainWindow.setSkipTaskbar(true);
    mainWindow.hide();
  });

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
ipcMain.handle('get-settings',      ()             => readSettings());
ipcMain.handle('save-settings',     (_e, s)        => writeSettings(s));
ipcMain.handle('set-channel-output', (_e, key, deviceId) => {
  if (!settingsCache) readSettings();
  settingsCache.channelOutputs = { ...(settingsCache.channelOutputs || {}), [key]: deviceId };
  fs.writeFileSync(SETTINGS_PATH, JSON.stringify(settingsCache, null, 2), 'utf8');
});


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

ipcMain.handle('install-driver', () => {
  const scriptPath = path.join(DRIVER_DIR, 'install.ps1');
  if (!fs.existsSync(scriptPath)) {
    mainWindow?.webContents.send('driver-log', `Error: install.ps1 not found at ${scriptPath}`);
    mainWindow?.webContents.send('driver-done', { success: false });
    return;
  }
  const child = spawn('powershell', [
    '-ExecutionPolicy', 'Bypass', '-NonInteractive', '-File', scriptPath,
  ], { cwd: DRIVER_DIR });
  const sendLine = (line) => mainWindow?.webContents.send('driver-log', line.trimEnd());
  child.stdout?.on('data', (d) => d.toString().split('\n').filter(l => l.trim()).forEach(sendLine));
  child.stderr?.on('data', (d) => d.toString().split('\n').filter(l => l.trim()).forEach(l => sendLine(`[warn] ${l}`)));
  child.on('exit', (code) => mainWindow?.webContents.send('driver-done', { success: code === 0, exitCode: code }));
});

ipcMain.handle('uninstall-driver', () => {
  const cmd = `
    $drivers = pnputil /enum-drivers | Select-String -Pattern 'virtualaudiodriver' -Context 5,0
    foreach ($m in $drivers) {
      $oem = ($m.Context.PreContext | Select-String 'Published Name').Line -replace '.*:\\s*',''
      if ($oem) { pnputil /delete-driver $oem.Trim() /uninstall /force }
    }
    $null = devcon remove Root\\VirtualAudioDriver 2>&1
    Write-Host 'Uninstall complete.'
  `;
  const child = spawn('powershell', ['-ExecutionPolicy', 'Bypass', '-NonInteractive', '-Command', cmd]);
  const sendLine = (line) => mainWindow?.webContents.send('driver-log', line.trimEnd());
  child.stdout?.on('data', (d) => d.toString().split('\n').filter(l => l.trim()).forEach(sendLine));
  child.stderr?.on('data', (d) => d.toString().split('\n').filter(l => l.trim()).forEach(l => sendLine(`[warn] ${l}`)));
  child.on('exit', (code) => mainWindow?.webContents.send('driver-done', { success: code === 0, exitCode: code }));
});

ipcMain.handle('get-testsigning', () => {
  try {
    const out = require('child_process').execSync('bcdedit /enum "{current}"', { encoding: 'utf8', timeout: 5000 });
    return /testsigning\s+yes/i.test(out);
  } catch { return false; }
});

ipcMain.handle('set-testsigning', () => {
  try {
    require('child_process').execSync('bcdedit /set testsigning on', { encoding: 'utf8', timeout: 5000 });
    return { success: true };
  } catch (e) {
    return { success: false, message: e.message };
  }
});

ipcMain.on('window-minimize', () => mainWindow?.minimize());
ipcMain.on('window-close',    () => mainWindow?.hide());

/* ── app lifecycle ──────────────────────────────────────────────── */
app.whenReady().then(() => { createWindow(); createTray(); startCore(); });
app.on('window-all-closed', (e) => e.preventDefault());
app.on('before-quit', () => { isQuitting = true; stopCore(); tray?.destroy(); });
