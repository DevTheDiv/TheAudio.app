'use strict';
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('api', {
  onSessions:      (cb) => ipcRenderer.on('sessions',      (_e, data) => cb(data)),
  onEndpoints:     (cb) => ipcRenderer.on('endpoints',     (_e, data) => cb(data)),
  onStatus:        (cb) => ipcRenderer.on('status',        (_e, data) => cb(data)),
  onSystem:        (cb) => ipcRenderer.on('system',        (_e, data) => cb(data)),
  onEndpointLevels: (cb) => ipcRenderer.on('endpointLevels', (_e, data) => cb(data)),

  setRouting:  (routing)   => ipcRenderer.invoke('set-routing',  routing),
  setVolume:   (name, vol) => ipcRenderer.invoke('set-volume',   name, vol),
  setMute:     (name, m)   => ipcRenderer.invoke('set-mute',     name, m),

  getProfiles:   ()        => ipcRenderer.invoke('get-profiles'),
  saveProfile:   (profile) => ipcRenderer.invoke('save-profile',  profile),
  deleteProfile: (name)    => ipcRenderer.invoke('delete-profile', name),
  applyProfile:  (name)    => ipcRenderer.invoke('apply-profile',  name),

  getEndpoints:  ()        => ipcRenderer.invoke('get-endpoints'),
  getSessions:   ()        => ipcRenderer.invoke('get-sessions'),
  getLevels:     ()        => ipcRenderer.invoke('get-levels'),
  onLevels:      (cb)      => ipcRenderer.on('levels', (_e, data) => cb(data)),
  getSystem:       ()         => ipcRenderer.invoke('get-system'),
  setSystemVolume:  (vol)              => ipcRenderer.invoke('set-system-volume',  vol),
  setSystemMute:    (muted)            => ipcRenderer.invoke('set-system-mute',    muted),
  setDeviceVolume:  (deviceId, vol)    => ipcRenderer.invoke('set-device-volume',  deviceId, vol),
  setDeviceMute:    (deviceId, muted)  => ipcRenderer.invoke('set-device-mute',    deviceId, muted),
  getSettings:   ()        => ipcRenderer.invoke('get-settings'),
  saveSettings:  (s)       => ipcRenderer.invoke('save-settings', s),

  getIcon:  (exePath) => ipcRenderer.invoke('get-icon', exePath),

  minimize: () => ipcRenderer.send('window-minimize'),
  close:    () => ipcRenderer.send('window-close'),
});
