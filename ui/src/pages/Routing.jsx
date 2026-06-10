import React, { useContext, useState, useEffect } from 'react';
import {
  Box, Typography, Paper, Stack, Select, MenuItem,
  Chip, Divider, Alert, IconButton, Tooltip, CircularProgress,
} from '@mui/material';
import ArrowForwardIcon   from '@mui/icons-material/ArrowForward';
import SurroundSoundIcon  from '@mui/icons-material/SurroundSound';
import DeviceHubIcon      from '@mui/icons-material/DeviceHub';
import SpeakerIcon        from '@mui/icons-material/Speaker';
import PlayCircleIcon     from '@mui/icons-material/PlayCircle';
import { AudioCtx } from '../App.jsx';

const SAR_BASE_NAMES = [
  'TheAudio.app Spacial Audio',
  'TheAudio.app Game',
  'TheAudio.app Voice',
  'TheAudio.app Music',
];

function getLevelIndex(endpointName) {
  return SAR_BASE_NAMES.findIndex(n => endpointName.startsWith(n));
}

function LevelBar({ level }) {
  const pct = Math.min(100, Math.round(level * 100));
  const color = pct > 90 ? '#f44336' : pct > 60 ? '#ff9800' : '#4caf50';
  return (
    <Box sx={{ display: 'flex', alignItems: 'center', gap: 0.5, minWidth: 80 }}>
      <Box sx={{ flex: 1, height: 6, borderRadius: 3, bgcolor: 'rgba(255,255,255,0.08)', overflow: 'hidden' }}>
        <Box sx={{
          width: `${pct}%`, height: '100%', borderRadius: 3, bgcolor: color,
          transition: 'width 80ms linear, background-color 150ms',
        }} />
      </Box>
      <Typography variant="caption" sx={{ color: 'text.disabled', minWidth: 28, textAlign: 'right', fontSize: 10 }}>
        {pct}%
      </Typography>
    </Box>
  );
}

function TestButton({ endpointName }) {
  const [testing, setTesting] = useState(false);

  async function handleTest() {
    setTesting(true);
    // Trigger a 440→880 Hz chirp via the C++ audio thread.
    // The timestamp suffix ensures re-clicking always triggers a fresh tone.
    await window.api?.saveSettings({ testToneEndpoint: `${endpointName}|${Date.now()}` });
    setTimeout(() => setTesting(false), 2200);
  }

  if (testing) {
    return (
      <Tooltip title="Playing test tone through C++ audio thread…">
        <CircularProgress size={20} thickness={5} sx={{ color: 'primary.main' }} />
      </Tooltip>
    );
  }
  return (
    <Tooltip title="Play test tone through this channel">
      <IconButton size="small" onClick={handleTest} sx={{ p: 0.5 }}>
        <PlayCircleIcon sx={{ fontSize: 22, color: 'text.disabled', '&:hover': { color: 'primary.main' } }} />
      </IconButton>
    </Tooltip>
  );
}

function sarIcon(type) {
  if (type === 'spatial') return <SurroundSoundIcon sx={{ color: 'primary.main', fontSize: 20 }} />;
  return <DeviceHubIcon sx={{ color: 'secondary.main', fontSize: 20 }} />;
}

function SarRouteRow({ endpoint, physical, savedTarget, level, onSave }) {
  const [target, setTarget] = useState(savedTarget ?? 'Default');
  const displayName = endpoint.name.replace(/\s*\(Synchronous Audio Router\)$/, '');

  useEffect(() => {
    setTarget(savedTarget ?? 'Default');
  }, [savedTarget]);

  function handleChange(e) {
    const val = e.target.value;
    setTarget(val);
    onSave(endpoint.name, val);
  }

  return (
    <Paper variant="outlined" sx={{ px: 2, py: 1.5, display: 'flex', alignItems: 'center', gap: 2 }}>
      {/* SAR endpoint label */}
      <Box sx={{ display: 'flex', alignItems: 'center', gap: 1, minWidth: 200 }}>
        {sarIcon(endpoint.type)}
        <Box>
          <Typography variant="body2" fontWeight={600}>{displayName}</Typography>
          <Chip
            label={endpoint.type === 'spatial' ? 'Spatial' : 'SAR'}
            color={endpoint.type === 'spatial' ? 'primary' : 'secondary'}
            size="small" variant="outlined"
            sx={{ fontSize: 10, height: 16, mt: 0.25 }}
          />
        </Box>
      </Box>

      {/* Live level meter */}
      <LevelBar level={level} />

      {/* Test tone button */}
      <TestButton endpointName={endpoint.name} />

      <ArrowForwardIcon sx={{ color: 'text.disabled', flexShrink: 0 }} />

      {/* Physical output selector */}
      <Select
        value={target}
        onChange={handleChange}
        size="small"
        sx={{ flex: 1, minWidth: 180, fontSize: 13 }}
        renderValue={(val) => (
          <Stack direction="row" alignItems="center" gap={1}>
            <SpeakerIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
            <span>{val}</span>
          </Stack>
        )}
      >
        <MenuItem value="Default">
          <Stack direction="row" alignItems="center" gap={1}>
            <SpeakerIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
            <span>Default</span>
          </Stack>
        </MenuItem>
        {physical.map(dev => (
          <MenuItem key={dev.id} value={dev.name}>
            <Stack direction="row" alignItems="center" gap={1}>
              <SpeakerIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
              <span>{dev.name}</span>
            </Stack>
          </MenuItem>
        ))}
      </Select>
    </Paper>
  );
}

export default function Routing() {
  const { endpoints, levels } = useContext(AudioCtx);
  const [sarRouting, setSarRouting] = useState({});

  const sarEndpoints = endpoints.filter(e => e.type === 'sar' || e.type === 'spatial');
  const physical     = endpoints.filter(e => e.type === 'physical');

  useEffect(() => {
    window.api?.getSettings().then(s => {
      setSarRouting(s.sarRouting ?? {});
    });
  }, []);

  function handleSave(endpointName, targetDevice) {
    const next = { ...sarRouting, [endpointName]: targetDevice };
    setSarRouting(next);
    window.api?.saveSettings({ sarRouting: next });
  }

  return (
    <Box sx={{ p: 3 }}>
      <Box sx={{ mb: 3 }}>
        <Typography variant="h6" fontWeight={700}>Output Routing</Typography>
        <Typography variant="caption" color="text.disabled">
          Map each virtual channel to a physical output device.
          Use the <PlayCircleIcon sx={{ fontSize: 12, verticalAlign: 'middle' }} /> button to play a test tone through that channel.
        </Typography>
      </Box>

      {sarEndpoints.length === 0 && (
        <Alert severity="info" sx={{ mb: 2 }}>
          No SAR virtual endpoints detected. SAR must be installed and running.
        </Alert>
      )}

      <Stack spacing={1}>
        {sarEndpoints.map(ep => {
          const idx = getLevelIndex(ep.name);
          return (
            <SarRouteRow
              key={ep.id}
              endpoint={ep}
              physical={physical}
              savedTarget={sarRouting[ep.name]}
              level={idx >= 0 ? (levels[idx] ?? 0) : 0}
              onSave={handleSave}
            />
          );
        })}
      </Stack>

      {physical.length === 0 && sarEndpoints.length > 0 && (
        <Box sx={{ mt: 2 }}>
          <Alert severity="warning">
            No physical output devices found. Check that audio devices are connected.
          </Alert>
        </Box>
      )}

      {sarEndpoints.length > 0 && physical.length > 0 && (
        <>
          <Divider sx={{ my: 3 }} />
          <Typography variant="caption" color="text.disabled">
            Test tone: 440→880 Hz sweep, ~2 seconds. If you hear it from the selected device, routing is confirmed end-to-end.
          </Typography>
        </>
      )}
    </Box>
  );
}
