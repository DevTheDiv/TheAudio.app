import React, { useContext, useEffect, useRef, useState } from 'react';
import {
  Box, Typography, Paper, Button, Chip, Divider, Stack,
} from '@mui/material';
import CheckCircleIcon   from '@mui/icons-material/CheckCircle';
import WarningAmberIcon  from '@mui/icons-material/WarningAmber';
import DownloadingIcon   from '@mui/icons-material/Downloading';
import DeleteOutlineIcon from '@mui/icons-material/DeleteOutline';
import SecurityIcon      from '@mui/icons-material/Security';
import { AudioCtx } from '../App.jsx';

const STEPS = [
  {
    label: 'Prerequisites',
    body: 'Secure Boot must be off in your BIOS/UEFI firmware. The driver is test-signed and will not load with Secure Boot enabled.',
  },
  {
    label: 'Test Signing',
    body: 'The installer enables Windows Test Signing mode (bcdedit /set testsigning on). A small watermark will appear in the bottom-right corner of the desktop. A restart is required after this is set for the first time.',
  },
  {
    label: 'Install',
    body: 'Click Install Driver. The installer creates a self-signed certificate, signs the driver, and registers it via pnputil. No WDK installation is required on your machine.',
  },
  {
    label: 'Restart',
    body: 'Restart your PC. After reboot the three virtual audio devices — TheAudio.app Games, Media, and Voice — will appear in Sound Settings and in the Mixer.',
  },
];

export default function Settings() {
  const { status } = useContext(AudioCtx);
  const installed = status?.driverInstalled ?? false;

  const [tsEnabled,  setTsEnabled]  = useState(null); // null = loading
  const [tsWorking,  setTsWorking]  = useState(false);

  const [running, setRunning]   = useState(false);
  const [log,     setLog]       = useState([]);
  const [result,  setResult]    = useState(null); // { success, exitCode }
  const logRef = useRef(null);

  useEffect(() => {
    window.api?.getTestSigning().then(v => setTsEnabled(v));
  }, []);

  async function handleEnableTestSigning() {
    setTsWorking(true);
    const res = await window.api?.setTestSigning();
    if (res?.success) setTsEnabled(true);
    setTsWorking(false);
  }

  useEffect(() => {
    if (!window.api) return;
    window.api.onDriverLog(line => setLog(prev => [...prev, line]));
    window.api.onDriverDone(res => { setRunning(false); setResult(res); });
  }, []);

  useEffect(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [log]);

  function handleInstall() {
    setLog([]);
    setResult(null);
    setRunning(true);
    window.api?.installDriver();
  }

  function handleUninstall() {
    setLog([]);
    setResult(null);
    setRunning(true);
    window.api?.uninstallDriver();
  }

  return (
    <Box sx={{ p: 3, maxWidth: 580 }}>
      <Typography variant="h6" fontWeight={700} mb={3}>Settings</Typography>

      {/* ── Driver status ── */}
      <Paper variant="outlined" sx={{ p: 2, mb: 2 }}>
        <Stack direction="row" alignItems="center" justifyContent="space-between" mb={1}>
          <Typography variant="subtitle2" fontWeight={700}>Virtual Audio Driver</Typography>
          <Chip
            size="small"
            icon={installed
              ? <CheckCircleIcon sx={{ fontSize: 14 }} />
              : <WarningAmberIcon sx={{ fontSize: 14 }} />}
            label={installed ? 'Installed' : 'Not Installed'}
            color={installed ? 'success' : 'warning'}
            variant="outlined"
          />
        </Stack>
        <Typography variant="body2" color="text.secondary">
          Adds three virtual output devices — Games, Media, and Voice — that
          appear as regular audio endpoints. Route apps into a channel in the
          Mixer, then choose a physical output for each channel.
        </Typography>
      </Paper>

      {/* ── Installation steps ── */}
      <Paper variant="outlined" sx={{ p: 2, mb: 2 }}>
        <Typography variant="subtitle2" fontWeight={700} mb={1.5}>
          Installation Steps
        </Typography>
        <Stack spacing={1.5}>
          {STEPS.map((step, i) => (
            <Box key={i} sx={{ display: 'flex', gap: 1.5 }}>
              <Box sx={{
                width: 22, height: 22, borderRadius: '50%', flexShrink: 0,
                bgcolor: 'primary.main', display: 'flex', alignItems: 'center',
                justifyContent: 'center', mt: 0.1,
              }}>
                <Typography sx={{ fontSize: 11, fontWeight: 700, color: '#fff' }}>{i + 1}</Typography>
              </Box>
              <Box>
                <Typography variant="body2" fontWeight={600}>{step.label}</Typography>
                <Typography variant="body2" color="text.secondary" sx={{ mt: 0.25 }}>
                  {step.body}
                </Typography>
              </Box>
            </Box>
          ))}
        </Stack>
      </Paper>

      {/* ── Test Signing ── */}
      <Paper variant="outlined" sx={{ p: 2, mb: 2 }}>
        <Stack direction="row" alignItems="center" justifyContent="space-between">
          <Box>
            <Typography variant="subtitle2" fontWeight={700}>Test Signing Mode</Typography>
            <Typography variant="body2" color="text.secondary" sx={{ mt: 0.25 }}>
              Required for the test-signed virtual audio driver to load.
              Adds a small watermark to the desktop. A restart is needed after enabling.
            </Typography>
          </Box>
          <Stack direction="row" alignItems="center" gap={1} ml={2} flexShrink={0}>
            <Chip
              size="small"
              icon={tsEnabled
                ? <CheckCircleIcon sx={{ fontSize: 14 }} />
                : <WarningAmberIcon sx={{ fontSize: 14 }} />}
              label={tsEnabled === null ? 'Checking…' : tsEnabled ? 'Enabled' : 'Disabled'}
              color={tsEnabled ? 'success' : 'warning'}
              variant="outlined"
            />
            {!tsEnabled && tsEnabled !== null && (
              <Button
                variant="contained"
                size="small"
                startIcon={<SecurityIcon />}
                disabled={tsWorking}
                onClick={handleEnableTestSigning}
              >
                {tsWorking ? 'Enabling…' : 'Enable'}
              </Button>
            )}
          </Stack>
        </Stack>
      </Paper>

      {/* ── Actions ── */}
      <Stack direction="row" gap={1} mb={log.length > 0 ? 2 : 0}>
        {!installed && (
          <Button
            variant="contained"
            size="small"
            startIcon={<DownloadingIcon />}
            disabled={running}
            onClick={handleInstall}
          >
            {running ? 'Installing…' : 'Install Driver'}
          </Button>
        )}
        {installed && (
          <Button
            variant="outlined"
            size="small"
            color="error"
            startIcon={<DeleteOutlineIcon />}
            disabled={running}
            onClick={handleUninstall}
          >
            {running ? 'Uninstalling…' : 'Uninstall Driver'}
          </Button>
        )}
      </Stack>

      {/* ── Log output ── */}
      {log.length > 0 && (
        <Paper variant="outlined" sx={{ p: 0, overflow: 'hidden' }}>
          {result && (
            <Box sx={{
              px: 1.5, py: 0.75,
              bgcolor: result.success ? 'success.dark' : 'error.dark',
              borderBottom: '1px solid',
              borderColor: result.success ? 'success.main' : 'error.main',
            }}>
              <Typography variant="caption" fontWeight={700}>
                {result.success ? 'Completed successfully. Restart your PC.' : `Failed (exit code ${result.exitCode})`}
              </Typography>
            </Box>
          )}
          <Box
            ref={logRef}
            sx={{
              maxHeight: 200, overflowY: 'auto', p: 1.5,
              fontFamily: 'monospace', fontSize: 11,
              color: 'text.secondary',
              '&::-webkit-scrollbar':       { width: 4 },
              '&::-webkit-scrollbar-thumb': { bgcolor: 'rgba(255,255,255,0.12)', borderRadius: 2 },
            }}
          >
            {log.map((line, i) => (
              <Box key={i} component="div" sx={{
                color: line.startsWith('[warn]') ? 'warning.main'
                     : line.startsWith('Error')  ? 'error.main'
                     : 'text.secondary',
              }}>
                {line}
              </Box>
            ))}
          </Box>
        </Paper>
      )}

      <Divider sx={{ my: 3 }} />

      {/* ── About ── */}
      <Paper variant="outlined" sx={{ p: 2 }}>
        <Typography variant="subtitle2" fontWeight={700} mb={0.5}>About</Typography>
        <Typography variant="body2" color="text.secondary">
          TheAudio.app — per-application audio routing for Windows.
          Basic routing works without the driver via the Windows per-process audio API.
        </Typography>
      </Paper>
    </Box>
  );
}
