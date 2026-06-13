import React, { useContext, useState, useEffect, useLayoutEffect, useRef, useMemo, useCallback } from 'react';
import { keyframes } from '@emotion/react';
import {
  Box, Typography, Slider, Select, MenuItem, IconButton,
  Paper, Stack, Tooltip,
} from '@mui/material';
import VolumeUpIcon      from '@mui/icons-material/VolumeUp';
import VolumeOffIcon     from '@mui/icons-material/VolumeOff';
import SpeakerIcon       from '@mui/icons-material/Speaker';
import DeviceHubIcon     from '@mui/icons-material/DeviceHub';
import ExpandMoreIcon    from '@mui/icons-material/ExpandMore';
import ExpandLessIcon    from '@mui/icons-material/ExpandLess';
import { AudioCtx } from '../App.jsx';

// Throttle fn to at most once per 41ms (24Hz). Always fires final value.
function useThrottle24(fn) {
  const lastAt  = useRef(0);
  const pending = useRef(null);
  const timer   = useRef(null);
  return useCallback((val) => {
    pending.current = val;
    const now = Date.now();
    if (now - lastAt.current >= 41) {
      lastAt.current = now;
      fn(val);
    }
    clearTimeout(timer.current);
    timer.current = setTimeout(() => { fn(pending.current); }, 50);
  }, [fn]);
}

// Map a linear peak (0–1) to a display percentage using a logarithmic dB scale.
// Range: -60dB (silence) → 0dB (full). Gives a much more responsive meter since
// human hearing is logarithmic — a linear 0.1 peak is quite audible but looks dead
// at 10%. With this scale, -20dB (peak=0.1) maps to ~67%.
function peakToPct(peak) {
  if (!peak || peak <= 0) return 0;
  const db = 20 * Math.log10(peak);
  return Math.max(0, Math.min(100, (db + 60) / 60 * 100));
}

const STRIP_SX = {
  position: 'relative',
  width: 110, height: 450, flexShrink: 0,
  px: 1.5, py: 2.5,
  overflow: 'hidden',
  display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 1.5,
  bgcolor: 'background.paper',
};

function DeviceSelect({ value, onChange, physical, disabled = false, hideDefault = false, showNoOutput = false }) {
  return (
    <Select
      value={value}
      onChange={e => onChange(e.target.value)}
      size="small"
      disabled={disabled}
      sx={{
        width: '100%', fontSize: 11, height: 26,
        bgcolor: 'rgba(0,0,0,0.2)',
        '& .MuiSelect-select': { py: 0.5 },
      }}
      renderValue={(val) => {
        const ep = physical.find(e => e.id === val);
        const name = ep ? ep.name
          : val === 'Default' ? 'Default'
          : val === 'none'    ? 'No Output'
          : val;
        return (
          <Stack direction="row" alignItems="center" gap={0.5}>
            <SpeakerIcon sx={{ fontSize: 11, color: 'text.secondary', flexShrink: 0 }} />
            <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontSize: 11 }}>
              {name}
            </span>
          </Stack>
        );
      }}
    >
      {showNoOutput && (
        <MenuItem value="none" sx={{ fontSize: 12 }}>
          <Stack direction="row" alignItems="center" gap={1}>
            <VolumeOffIcon sx={{ fontSize: 14, color: 'text.disabled' }} />
            No Output
          </Stack>
        </MenuItem>
      )}
      {!hideDefault && (
        <MenuItem value="Default" sx={{ fontSize: 12 }}>
          <Stack direction="row" alignItems="center" gap={1}>
            <SpeakerIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
            Default
          </Stack>
        </MenuItem>
      )}
      {physical.map(dev => (
        <MenuItem key={dev.id} value={dev.id} sx={{ fontSize: 12 }}>
          <Stack direction="row" alignItems="center" gap={1}>
            <SpeakerIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
            <span>{dev.name}</span>
          </Stack>
        </MenuItem>
      ))}
    </Select>
  );
}

function StripBackgroundVisualizer({ pct }) {
  return (
    <>
      <Box sx={{
        position: 'absolute', bottom: 0, left: 0, right: 0, top: 0,
        background: 'linear-gradient(0deg, #00ff88 0%, #00ccff 25%, #7c5cfc 50%, #ffea00 80%, #ff3d00 100%)',
        opacity: 0.25,
        clipPath: `inset(${100 - pct}% 0 0 0)`,
        transition: 'clip-path 0.08s linear',
        pointerEvents: 'none', zIndex: 0,
      }} />
      <Box sx={{
        position: 'absolute', bottom: 0, left: 0, right: 0, top: 0,
        background: 'linear-gradient(0deg, #00ff88 0%, #00ccff 25%, #7c5cfc 50%, #ffea00 80%, #ff3d00 100%)',
        opacity: 0.2,
        filter: 'blur(25px)',
        clipPath: `inset(${100 - pct}% 0 0 0)`,
        transition: 'clip-path 0.1s linear',
        pointerEvents: 'none', zIndex: 0,
      }} />
    </>
  );
}

const MixerDivider = () => (
  <Box sx={{
    alignSelf: 'stretch', minHeight: 340,
    borderLeft: '2px dashed',
    borderColor: 'rgba(255,255,255,0.18)',
    mx: 0.5, flexShrink: 0,
  }} />
);

// ── Physical output device strip ──────────────────────────────────────────────
function DeviceStrip({ endpoint, isDefault }) {
  const { endpointLevels } = useContext(AudioCtx);
  const info = endpointLevels[endpoint.id] ?? { peak: 0, volume: 1, muted: false };

  const [volume, setVolume] = useState(Math.round((info.volume ?? 1) * 100));
  const [muted,  setMuted]  = useState(info.muted ?? false);
  const dragging = useRef(false);

  const displayVolume = dragging.current ? volume : Math.round((info.volume ?? 1) * 100);
  useEffect(() => { if (!dragging.current) setVolume(Math.round((info.volume ?? 1) * 100)); }, [info.volume]);
  useEffect(() => { setMuted(info.muted ?? false); }, [info.muted]);

  const sendVolume = useThrottle24(useCallback((val) => window.api?.setDeviceVolume(endpoint.id, val / 100), [endpoint.id]));
  function handleVolumeChange(_e, val) { dragging.current = true;  setVolume(val); sendVolume(val); }
  function handleVolumeCommit(_e, val) { dragging.current = false; window.api?.setDeviceVolume(endpoint.id, val / 100); }
  function handleMute() { const next = !muted; setMuted(next); window.api?.setDeviceMute(endpoint.id, next); }

  const peakHeight = muted ? 0 : peakToPct((info.peak ?? 0) * displayVolume / 100);

  return (
    <Paper
      variant="outlined"
      sx={{
        ...STRIP_SX,
        borderColor: isDefault ? 'primary.main' : undefined,
        transition: 'border-color 0.2s ease',
        '&:hover': { borderColor: isDefault ? 'primary.light' : 'text.secondary' },
      }}
    >
      <StripBackgroundVisualizer pct={peakHeight} />

      <Box sx={{ position: 'relative', zIndex: 1, width: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 1 }}>
        <Box sx={{ width: '100%', textAlign: 'center' }}>
          <Box sx={{ height: 32, display: 'flex', alignItems: 'center', justifyContent: 'center', mb: 0.5 }}>
            <SpeakerIcon sx={{ fontSize: 28, color: isDefault ? 'primary.main' : 'text.secondary', transition: 'color 0.2s ease' }} />
          </Box>
          <ScrollingName name={endpoint.name} />
          <Typography variant="caption" sx={{ fontSize: 9, color: isDefault ? 'primary.main' : 'transparent', transition: 'color 0.2s ease' }}>
            Default
          </Typography>
        </Box>

        <Box sx={{ height: 240, py: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <Slider
            orientation="vertical"
            value={muted ? 0 : displayVolume}
            onChange={handleVolumeChange}
            onChangeCommitted={handleVolumeCommit}
            min={0} max={100} step={1}
            size="small"
            disabled={muted}
            sx={{
              '& .MuiSlider-track': { border: 'none' },
              '& .MuiSlider-thumb': {
                width: 12, height: 12,
                transition: '0.1s cubic-bezier(.47,1.64,.41,.8)',
                '&:before': { boxShadow: '0 2px 4px 0 rgba(0,0,0,0.5)' },
              },
            }}
          />
        </Box>

        <Typography variant="caption" color="text.secondary" sx={{ fontFamily: 'monospace', fontSize: 11 }}>
          {muted ? '—' : `${displayVolume}%`}
        </Typography>

        <Tooltip title={muted ? 'Unmute' : 'Mute'}>
          <IconButton size="small" onClick={handleMute} color={muted ? 'error' : 'default'}>
            {muted ? <VolumeOffIcon sx={{ fontSize: 18 }} /> : <VolumeUpIcon sx={{ fontSize: 18 }} />}
          </IconButton>
        </Tooltip>
      </Box>
    </Paper>
  );
}

function MuteButtonSpacer() {
  return (
    <IconButton size="small" disabled sx={{ visibility: 'hidden' }}>
      <VolumeUpIcon sx={{ fontSize: 18 }} />
    </IconButton>
  );
}

// ── System audio strip ────────────────────────────────────────────────────────
function SystemStrip({ endpoints, defaultDevice, onDefaultDeviceChange, subs = [] }) {
  const { system } = useContext(AudioCtx);
  // Include virtual channels so the system default can be set to a virtual channel.
  const physical = endpoints;
  const selectValue = defaultDevice || (physical.length > 0 ? physical[0].id : '');
  const [expanded, setExpanded] = useState(false);

  const [volume, setVolume] = useState(Math.round((system.volume ?? 1) * 100));
  const [muted,  setMuted]  = useState(system.muted ?? false);
  const dragging = useRef(false);

  const displayVolume = dragging.current ? volume : Math.round((system.volume ?? 1) * 100);
  useEffect(() => {
    if (!dragging.current) setVolume(Math.round((system.volume ?? 1) * 100));
  }, [system.volume]);
  useEffect(() => { setMuted(system.muted ?? false); }, [system.muted]);

  const sendVolume = useThrottle24(useCallback((val) => window.api?.setSystemVolume(val / 100), []));
  function handleVolumeChange(_e, val) { dragging.current = true; setVolume(val); sendVolume(val); }
  function handleVolumeCommit(_e, val) { dragging.current = false; window.api?.setSystemVolume(val / 100); }
  function handleMute() {
    const next = !muted;
    setMuted(next);
    window.api?.setSystemMute(next);
  }

  const peakHeight = muted ? 0 : peakToPct((system.peak ?? 0) * displayVolume / 100);

  return (
    <Box sx={{ display: 'flex', flexDirection: 'row', gap: 0.5, alignItems: 'flex-start' }}>
    <Paper variant="outlined" sx={{ ...STRIP_SX, '&:hover': { borderColor: 'text.secondary' } }}>
      {subs.length > 0 && (
        <Tooltip title={expanded ? 'Hide system sounds' : 'Show system sounds'}>
          <IconButton
            size="small"
            onClick={() => setExpanded(e => !e)}
            sx={{ position: 'absolute', top: 4, right: 4, p: 0.25, zIndex: 2 }}
          >
            {expanded
              ? <ExpandLessIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
              : <ExpandMoreIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
            }
          </IconButton>
        </Tooltip>
      )}

      <StripBackgroundVisualizer pct={peakHeight} />

      <Box sx={{ position: 'relative', zIndex: 1, width: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 1 }}>
        <Box sx={{ width: '100%', textAlign: 'center' }}>
          <Box sx={{ height: 32, display: 'flex', alignItems: 'center', justifyContent: 'center', mb: 0.5 }}>
            <VolumeUpIcon sx={{ fontSize: 28, color: 'text.secondary' }} />
          </Box>
          <Typography sx={{ fontWeight: 600, fontSize: 12 }}>System</Typography>
          <Typography variant="caption" color="text.disabled" sx={{ fontSize: 9, display: 'block' }}>
            Default Output
          </Typography>
        </Box>

        <DeviceSelect
          value={selectValue}
          onChange={onDefaultDeviceChange}
          physical={physical}
          hideDefault={true}
        />

        <Box sx={{ height: 240, py: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <Slider
            orientation="vertical"
            value={muted ? 0 : displayVolume}
            onChange={handleVolumeChange}
            onChangeCommitted={handleVolumeCommit}
            min={0} max={100} step={1}
            size="small"
            disabled={muted}
            sx={{
              '& .MuiSlider-track': { border: 'none' },
              '& .MuiSlider-thumb': {
                width: 12, height: 12,
                transition: '0.1s cubic-bezier(.47,1.64,.41,.8)',
                '&:before': { boxShadow: '0 2px 4px 0 rgba(0,0,0,0.5)' },
              },
            }}
          />
        </Box>

        <Typography variant="caption" color="text.secondary" sx={{ fontFamily: 'monospace', fontSize: 11 }}>
          {muted ? '—' : `${displayVolume}%`}
        </Typography>

        <Tooltip title={muted ? 'Unmute' : 'Mute'}>
          <IconButton size="small" onClick={handleMute} color={muted ? 'error' : 'default'}>
            {muted ? <VolumeOffIcon sx={{ fontSize: 18 }} /> : <VolumeUpIcon sx={{ fontSize: 18 }} />}
          </IconButton>
        </Tooltip>
      </Box>
    </Paper>
    {expanded && subs.map(sub => (
      <SubSessionRow key={sub.pid} session={sub} />
    ))}
    </Box>
  );
}

// ── Scrolling app name ────────────────────────────────────────────────────────
function ScrollingName({ name }) {
  const containerRef = useRef(null);
  const textRef      = useRef(null);
  const [scrollPx, setScrollPx] = useState(0);

  useLayoutEffect(() => {
    const c = containerRef.current;
    const t = textRef.current;
    if (c && t) {
      const dist = t.scrollWidth - c.clientWidth;
      setScrollPx(dist > 1 ? dist : 0);
    }
  }, [name]);

  const anim = useMemo(() => scrollPx > 0 ? keyframes({
    '0%, 15%':  { transform: 'translateX(0)' },
    '65%, 85%': { transform: `translateX(-${scrollPx}px)` },
    '100%':     { transform: 'translateX(0)' },
  }) : null, [scrollPx]);

  return (
    <Box ref={containerRef} sx={{ width: '100%', overflow: 'hidden' }}>
      <Box
        ref={textRef}
        title={name}
        sx={{
          display: 'inline-block', whiteSpace: 'nowrap',
          fontWeight: 600, fontSize: 12,
          animation: anim ? `${anim} 5s ease-in-out infinite` : 'none',
        }}
      >
        {name}
      </Box>
    </Box>
  );
}

// ── Main app mixer strip ──────────────────────────────────────────────────────
function SessionRow({ session, endpoints, expandable = false, expanded = false, onToggleExpand }) {
  const [icon,   setIcon]   = useState(null);
  const [volume, setVolume] = useState(Math.round((session.volume ?? 1) * 100));
  const [muted,  setMuted]  = useState(session.muted ?? false);
  const [route,  setRoute]  = useState(session.endpointId || 'Default');
  const selecting = useRef(false);

  useEffect(() => {
    if (session.exePath) window.api?.getIcon(session.exePath).then(url => url && setIcon(url));
  }, [session.exePath]);

  useEffect(() => {
    if (!selecting.current) setRoute(session.endpointId || 'Default');
  }, [session.endpointId]);

  const dragging = useRef(false);
  const displayVolume = dragging.current ? volume : Math.round((session.volume ?? 1) * 100);
  useEffect(() => {
    if (!dragging.current) setVolume(Math.round((session.volume ?? 1) * 100));
  }, [session.volume]);

  useEffect(() => { setMuted(session.muted ?? false); }, [session.muted]);

  const sendVolume = useThrottle24(useCallback((val) => window.api?.setVolume(session.name, val / 100), [session.name]));
  function handleVolumeChange(_e, val) { dragging.current = true; setVolume(val); sendVolume(val); }
  function handleVolumeCommit(_e, val) { dragging.current = false; window.api?.setVolume(session.name, val / 100); }
  function handleMute() {
    const next = !muted;
    setMuted(next);
    window.api?.setMute(session.name, next);
  }
  function handleRoute(e) {
    const epId = e.target.value;
    setRoute(epId);
    selecting.current = true;
    window.api?.setRouting({ [session.name]: epId });
    setTimeout(() => { selecting.current = false; }, 300);
  }

  const peakHeight = muted ? 0 : peakToPct((session.peak ?? 0) * displayVolume / 100);

  return (
    <Paper
      variant="outlined"
      sx={{
        ...STRIP_SX,
        transition: 'border-color 0.2s ease',
        '&:hover': { borderColor: 'primary.main' },
      }}
    >
      {expandable && (
        <Tooltip title={expanded ? 'Hide sub-processes' : 'Show sub-processes'}>
          <IconButton
            size="small"
            onClick={onToggleExpand}
            sx={{ position: 'absolute', top: 4, right: 4, p: 0.25, zIndex: 2 }}
          >
            {expanded
              ? <ExpandLessIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
              : <ExpandMoreIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
            }
          </IconButton>
        </Tooltip>
      )}

      <StripBackgroundVisualizer pct={peakHeight} />

      <Box sx={{ position: 'relative', zIndex: 1, width: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 1 }}>

        <Box sx={{ width: '100%', textAlign: 'center' }}>
          <Box sx={{ height: 32, display: 'flex', alignItems: 'center', justifyContent: 'center', mb: 0.5 }}>
            {icon
              ? <Box component="img" src={icon} sx={{ width: 28, height: 28, objectFit: 'contain' }} />
              : <Box sx={{ width: 28, height: 28, borderRadius: 1, bgcolor: 'rgba(255,255,255,0.06)' }} />
            }
          </Box>
          <ScrollingName name={session.name} />
          <Typography variant="caption" color="text.disabled" sx={{ fontSize: 9, display: 'block' }}>
            PID {session.pid}
          </Typography>
        </Box>

        <Select
          value={route}
          onChange={handleRoute}
          size="small"
          sx={{
            width: '100%', fontSize: 11, height: 26,
            bgcolor: 'rgba(0,0,0,0.2)',
            '& .MuiSelect-select': { py: 0.5 },
          }}
          renderValue={(val) => {
            const ep   = endpoints.find(e => e.id === val);
            const name = ep ? ep.name : (val === 'Default' ? 'Default' : val);
            return (
              <Stack direction="row" alignItems="center" gap={0.5}>
                <SpeakerIcon sx={{ fontSize: 11, color: 'text.secondary', flexShrink: 0 }} />
                <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontSize: 11 }}>
                  {name}
                </span>
              </Stack>
            );
          }}
        >
          <MenuItem value="Default" sx={{ fontSize: 12 }}>
            <Stack direction="row" alignItems="center" gap={1}>
              <SpeakerIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
              Default
            </Stack>
          </MenuItem>
          {endpoints.map(ep => (
            <MenuItem key={ep.id} value={ep.id} sx={{ fontSize: 12 }}>
              <Stack direction="row" alignItems="center" gap={1}>
                <SpeakerIcon sx={{ fontSize: 14, color: 'text.secondary' }} />
                <span>{ep.name}</span>
              </Stack>
            </MenuItem>
          ))}
        </Select>

        <Box sx={{ height: 240, py: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <Slider
            orientation="vertical"
            value={muted ? 0 : displayVolume}
            onChange={handleVolumeChange}
            onChangeCommitted={handleVolumeCommit}
            min={0} max={100} step={1}
            size="small"
            disabled={muted}
            sx={{
              '& .MuiSlider-track': { border: 'none' },
              '& .MuiSlider-thumb': {
                width: 12, height: 12,
                transition: '0.1s cubic-bezier(.47,1.64,.41,.8)',
                '&:before': { boxShadow: '0 2px 4px 0 rgba(0,0,0,0.5)' },
              },
            }}
          />
        </Box>

        <Typography variant="caption" color="text.secondary" sx={{ fontFamily: 'monospace', fontSize: 11 }}>
          {muted ? '—' : `${displayVolume}%`}
        </Typography>

        <Tooltip title={muted ? 'Unmute' : 'Mute'}>
          <IconButton size="small" onClick={handleMute} color={muted ? 'error' : 'default'}>
            {muted ? <VolumeOffIcon sx={{ fontSize: 18 }} /> : <VolumeUpIcon sx={{ fontSize: 18 }} />}
          </IconButton>
        </Tooltip>

      </Box>
    </Paper>
  );
}

// ── Sub-process strip (shown when main strip is expanded) ─────────────────────
function SubSessionRow({ session }) {
  const [volume, setVolume] = useState(Math.round((session.volume ?? 1) * 100));
  const [muted,  setMuted]  = useState(session.muted ?? false);
  const dragging = useRef(false);

  const displayVolume = dragging.current ? volume : Math.round((session.volume ?? 1) * 100);
  useEffect(() => {
    if (!dragging.current) setVolume(Math.round((session.volume ?? 1) * 100));
  }, [session.volume]);
  useEffect(() => { setMuted(session.muted ?? false); }, [session.muted]);

  const sendVolume = useThrottle24(useCallback((val) => window.api?.setVolume(session.name, val / 100), [session.name]));
  function handleVolumeChange(_e, val) { dragging.current = true; setVolume(val); sendVolume(val); }
  function handleVolumeCommit(_e, val) { dragging.current = false; window.api?.setVolume(session.name, val / 100); }
  function handleMute() {
    const next = !muted;
    setMuted(next);
    window.api?.setMute(session.name, next);
  }

  const peakHeight = muted ? 0 : peakToPct((session.peak ?? 0) * displayVolume / 100);

  return (
    <Paper
      variant="outlined"
      sx={{
        ...STRIP_SX,
        bgcolor: 'rgba(0,0,0,0.25)',
        borderStyle: 'dashed',
        borderColor: 'rgba(255,255,255,0.12)',
        transition: 'border-color 0.2s ease, opacity 0.2s ease',
        opacity: 0.75,
        '&:hover': { borderColor: 'primary.light', opacity: 1 },
      }}
    >
      <StripBackgroundVisualizer pct={peakHeight} />

      <Box sx={{ position: 'relative', zIndex: 1, width: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 1 }}>

        <Box sx={{ width: '100%', textAlign: 'center' }}>
          <Box sx={{ height: 32, display: 'flex', alignItems: 'center', justifyContent: 'center', mb: 0.5 }}>
            <Box sx={{
              width: 10, height: 10, borderRadius: '50%',
              bgcolor: 'rgba(255,255,255,0.15)',
              border: '1px solid rgba(255,255,255,0.2)',
            }} />
          </Box>
          <Typography sx={{ fontWeight: 400, fontSize: 11, color: 'text.secondary' }}>subprocess</Typography>
          <Typography variant="caption" color="text.disabled" sx={{ fontSize: 9, display: 'block' }}>
            PID {session.pid}
          </Typography>
        </Box>

        {/* endpoint read-only */}
        <Stack direction="row" alignItems="center" gap={0.5} sx={{
          width: '100%', px: 0.5, py: 0.5, borderRadius: 1,
          bgcolor: 'rgba(0,0,0,0.2)', overflow: 'hidden',
        }}>
          <SpeakerIcon sx={{ fontSize: 11, color: 'text.disabled', flexShrink: 0 }} />
          <Typography sx={{
            fontSize: 10, color: 'text.disabled',
            overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          }}>
            {session.endpoint || 'Default'}
          </Typography>
        </Stack>

        <Box sx={{ height: 240, py: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <Slider
            orientation="vertical"
            value={muted ? 0 : displayVolume}
            onChange={handleVolumeChange}
            onChangeCommitted={handleVolumeCommit}
            min={0} max={100} step={1}
            size="small"
            disabled={muted}
            sx={{
              color: 'text.secondary',
              '& .MuiSlider-track': { border: 'none' },
              '& .MuiSlider-thumb': {
                width: 12, height: 12,
                transition: '0.1s cubic-bezier(.47,1.64,.41,.8)',
                '&:before': { boxShadow: '0 2px 4px 0 rgba(0,0,0,0.5)' },
              },
            }}
          />
        </Box>

        <Typography variant="caption" color="text.disabled" sx={{ fontFamily: 'monospace', fontSize: 11 }}>
          {muted ? '—' : `${displayVolume}%`}
        </Typography>

        <Tooltip title={muted ? 'Unmute' : 'Mute'}>
          <IconButton size="small" onClick={handleMute} color={muted ? 'error' : 'default'}>
            {muted ? <VolumeOffIcon sx={{ fontSize: 18 }} /> : <VolumeUpIcon sx={{ fontSize: 18 }} />}
          </IconButton>
        </Tooltip>

      </Box>
    </Paper>
  );
}

// ── App group: main strip + optional sub-strips ───────────────────────────────
function AppGroup({ mainProcess, allSessions, endpoints }) {
  const [expanded, setExpanded] = useState(false);

  // Show the session that's actually producing audio as the primary strip.
  // This ensures the slider reflects what you hear. Falls back to the
  // process-tree parent when all sessions are silent.
  const mostActive = allSessions.reduce(
    (best, s) => (s.peak ?? 0) > (best.peak ?? 0) ? s : best,
    mainProcess,
  );
  const primary = (mostActive.peak ?? 0) > 0.005 ? mostActive : mainProcess;
  const subs = allSessions.filter(s => s !== primary);

  return (
    <Box sx={{ display: 'flex', flexDirection: 'row', gap: 0.5, alignItems: 'flex-start' }}>
      <SessionRow
        session={primary}
        endpoints={endpoints}
        expandable={subs.length > 0}
        expanded={expanded}
        onToggleExpand={() => setExpanded(e => !e)}
      />
      {expanded && subs.map(sub => (
        <SubSessionRow key={sub.pid} session={sub} />
      ))}
    </Box>
  );
}

// ── Virtual channel strip ─────────────────────────────────────────────────────
// Shows one of the TheAudio.app virtual endpoints (Games / Media / Voice) with
// a selector for which physical device its audio should be forwarded to.
function ChannelStrip({ endpoint, outputId, onOutputChange, physical, defaultDevice }) {
  const { endpointLevels } = useContext(AudioCtx);
  const info = endpointLevels[endpoint.id] ?? { peak: 0 };
  const shortName = endpoint.name.replace('TheAudio.app ', '');
  // If this virtual channel is the current system default device, "Default" would
  // route audio back into itself. Hide it to prevent the loop.
  const wouldLoop = defaultDevice === endpoint.id;

  return (
    <Paper
      variant="outlined"
      sx={{
        ...STRIP_SX,
        borderColor: 'secondary.dark',
        '&:hover': { borderColor: 'secondary.main' },
      }}
    >
      <StripBackgroundVisualizer pct={peakToPct(info.peak ?? 0)} />

      <Box sx={{ position: 'relative', zIndex: 1, width: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 1 }}>
        <Box sx={{ width: '100%', textAlign: 'center' }}>
          <Box sx={{ height: 32, display: 'flex', alignItems: 'center', justifyContent: 'center', mb: 0.5 }}>
            <DeviceHubIcon sx={{ fontSize: 26, color: 'secondary.main' }} />
          </Box>
          <ScrollingName name={shortName} />
          <Typography variant="caption" sx={{ fontSize: 9, color: 'secondary.main', display: 'block' }}>
            Virtual Channel
          </Typography>
        </Box>

        <DeviceSelect
          value={outputId || 'none'}
          onChange={onOutputChange}
          physical={physical}
          hideDefault={wouldLoop}
          showNoOutput={true}
        />

        <Box sx={{ height: 240, py: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <Box sx={{
            width: 8, height: '100%',
            bgcolor: 'rgba(255,255,255,0.05)',
            borderRadius: 4,
            position: 'relative',
            overflow: 'hidden',
          }}>
            <Box sx={{
              position: 'absolute', bottom: 0, left: 0, right: 0,
              height: `${peakToPct(info.peak ?? 0)}%`,
              background: 'linear-gradient(0deg, #00ff88 0%, #00ccff 60%, #7c5cfc 100%)',
              transition: 'height 0.08s linear',
              borderRadius: 4,
            }} />
          </Box>
        </Box>

        <Typography variant="caption" color="text.disabled" sx={{ fontSize: 10 }}>
          {outputId ? 'Routed' : 'No output'}
        </Typography>
      </Box>
    </Paper>
  );
}

// ── Dashboard ─────────────────────────────────────────────────────────────────
export default function Dashboard() {
  const { sessions, endpoints } = useContext(AudioCtx);
  const [defaultDevice,   setDefaultDevice]   = useState('');
  const [channelOutputs,  setChannelOutputs]  = useState({});
  const scrollRef = useRef(null);

  useEffect(() => {
    window.api?.getSettings().then(s => {
      setDefaultDevice(s?.defaultDevice ?? '');
      setChannelOutputs(s?.channelOutputs ?? {});
    });
  }, []);

  function handleChannelOutputChange(channelKey, deviceId) {
    const next = { ...channelOutputs, [channelKey]: deviceId };
    setChannelOutputs(next);
    window.api?.setChannelOutput(channelKey, deviceId);
  }

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    const onWheel = (e) => { e.preventDefault(); el.scrollLeft += e.deltaY; };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, []);

  function handleDefaultDeviceChange(epId) {
    setDefaultDevice(epId);
    window.api?.saveSettings({ defaultDevice: epId });
  }

  const systemSounds = sessions.filter(s => s.pid === 0);

  // Group sessions by process name (exclude System Sounds — shown under SystemStrip).
  // Within each group, find the process-tree root (the session whose parentPid
  // is not another session in the group). This is used as the identity anchor
  // for icon/name. AppGroup itself picks the audio-active session for display.
  const groups = {};
  sessions.forEach(s => {
    if (s.pid === 0) return;
    if (!groups[s.name]) groups[s.name] = [];
    groups[s.name].push(s);
  });

  const appGroups = Object.values(groups).map(group => {
    const pids = new Set(group.map(s => s.pid));
    const hasParentData = group.some(s => (s.parentPid ?? 0) > 0);
    let mainProcess = hasParentData
      ? group.find(s => !pids.has(s.parentPid ?? 0))
      : null;
    if (!mainProcess) mainProcess = group.reduce((a, b) => a.pid < b.pid ? a : b);
    return { mainProcess, allSessions: group };
  });

  return (
    <Box ref={scrollRef} sx={{
      p: 3, overflowX: 'auto', overflowY: 'hidden',
      '&::-webkit-scrollbar':       { height: 5 },
      '&::-webkit-scrollbar-track': { bgcolor: 'rgba(255,255,255,0.04)', borderRadius: 3, marginLeft: '24px', marginRight: '24px' },
      '&::-webkit-scrollbar-thumb': {
        bgcolor: 'rgba(255,255,255,0.12)', borderRadius: 3,
        '&:hover': { bgcolor: 'rgba(124,92,252,0.55)' },
      },
    }}>
      <Typography variant="h6" fontWeight={700} mb={3}>Mixer</Typography>

      <Box sx={{ display: 'flex', flexWrap: 'nowrap', gap: 1, alignItems: 'flex-start' }}>

        {endpoints.filter(e => e.type === 'physical').map(ep => (
          <DeviceStrip
            key={ep.id}
            endpoint={ep}
            isDefault={ep.id === defaultDevice}
          />
        ))}

        {endpoints.filter(e => e.type === 'virtual').length > 0 && <MixerDivider />}

        {endpoints.filter(e => e.type === 'virtual').map(ep => {
          const key = ep.key || ep.name.replace('TheAudio.app ', '');
          return (
            <ChannelStrip
              key={ep.id}
              endpoint={ep}
              outputId={channelOutputs[key] || ''}
              onOutputChange={(deviceId) => handleChannelOutputChange(key, deviceId)}
              physical={endpoints.filter(e => e.type === 'physical')}
              defaultDevice={defaultDevice}
            />
          );
        })}

        <MixerDivider />

        <SystemStrip
          endpoints={endpoints}
          defaultDevice={defaultDevice}
          onDefaultDeviceChange={handleDefaultDeviceChange}
          subs={systemSounds}
        />

        <MixerDivider />

        {appGroups.length === 0 ? (
          <Typography color="text.disabled" sx={{ alignSelf: 'center', pl: 2 }}>
            No audio apps detected.
          </Typography>
        ) : (
          appGroups.map(group => (
            <AppGroup
              key={group.mainProcess.name}
              mainProcess={group.mainProcess}
              allSessions={group.allSessions}
              endpoints={endpoints}
            />
          ))
        )}

      </Box>
    </Box>
  );
}
