import React, { useEffect, useState, createContext, useContext } from 'react';
import { HashRouter, Routes, Route, NavLink } from 'react-router-dom';
import { ThemeProvider, CssBaseline, Box, Typography, IconButton, Tooltip } from '@mui/material';
import DashboardIcon     from '@mui/icons-material/Dashboard';
import AccountTreeIcon   from '@mui/icons-material/AccountTree';
import TuneIcon          from '@mui/icons-material/Tune';
import SettingsIcon      from '@mui/icons-material/Settings';
import RemoveIcon        from '@mui/icons-material/Remove';
import CloseIcon         from '@mui/icons-material/Close';
import theme from './theme.js';
import Dashboard from './pages/Dashboard.jsx';
import Profiles  from './pages/Profiles.jsx';
import Devices   from './pages/Devices.jsx';
import Settings  from './pages/Settings.jsx';

/* ── Global audio state context ──────────────────────────────────── */
export const AudioCtx = createContext({});

const NAV = [
  { to: '/',         icon: <DashboardIcon />,   label: 'Mixer'    },
  { to: '/devices',  icon: <TuneIcon />,        label: 'Devices'  },
  { to: '/settings', icon: <SettingsIcon />,    label: 'Settings' },
];

const SIDEBAR_W = 64;

export default function App() {
  const [sessions,      setSessions]      = useState([]);
  const [endpoints,     setEndpoints]     = useState([]);
  const [status,        setStatus]        = useState({ running: false });
  const [levels,        setLevels]        = useState([0, 0, 0, 0]);
  const [system,        setSystem]        = useState({ volume: 1, muted: false, peak: 0 });
  const [endpointLevels, setEndpointLevels] = useState({});

  useEffect(() => {
    if (!window.api) return;
    window.api.onSessions(setSessions);
    window.api.onEndpoints(setEndpoints);
    window.api.onStatus(setStatus);
    window.api.onLevels(setLevels);
    window.api.onSystem(setSystem);
    window.api.onEndpointLevels(setEndpointLevels);
    window.api.getSessions().then(s  => { if (s?.length) setSessions(s);  });
    window.api.getEndpoints().then(e => { if (e?.length) setEndpoints(e); });
    window.api.getLevels().then(l    => { if (l?.length) setLevels(l);    });
    window.api.getSystem().then(s    => { if (s) setSystem(s); });
  }, []);

  return (
    <ThemeProvider theme={theme}>
      <CssBaseline />
      <AudioCtx.Provider value={{ sessions, endpoints, status, levels, system, endpointLevels }}>
        <HashRouter>
          <Box sx={{ display: 'flex', height: '100vh', overflow: 'hidden', userSelect: 'none' }}>

            {/* Drag region / title bar */}
            <Box sx={{
              position: 'fixed', top: 0, left: 0, right: 0, height: 32,
              WebkitAppRegion: 'drag', zIndex: 9999,
              display: 'flex', alignItems: 'center', justifyContent: 'space-between',
              px: 1,
            }}>
              <Typography variant="caption" sx={{ color: 'text.disabled', pl: `${SIDEBAR_W}px` }}>
                TheAudio.app
              </Typography>
              <Box sx={{ WebkitAppRegion: 'no-drag', display: 'flex', gap: 0.5 }}>
                <IconButton size="small" onClick={() => window.api?.minimize()}>
                  <RemoveIcon fontSize="small" />
                </IconButton>
                <IconButton size="small" onClick={() => window.api?.minimize()}>
                  <CloseIcon fontSize="small" />
                </IconButton>
              </Box>
            </Box>

            {/* Sidebar */}
            <Box sx={{
              width: SIDEBAR_W, flexShrink: 0,
              bgcolor: 'background.paper',
              borderRight: '1px solid', borderColor: 'divider',
              display: 'flex', flexDirection: 'column', alignItems: 'center',
              pt: '40px', pb: 2, gap: 1,
            }}>
              {NAV.map(({ to, icon, label }) => (
                <Tooltip key={to} title={label} placement="right">
                  <IconButton
                    component={NavLink}
                    to={to}
                    end={to === '/'}
                    sx={{
                      width: 44, height: 44, borderRadius: 2,
                      color: 'text.disabled',
                      '&.active': { color: 'primary.main', bgcolor: 'rgba(124,92,252,0.12)' },
                      '&:hover': { bgcolor: 'rgba(255,255,255,0.06)' },
                    }}
                  >
                    {icon}
                  </IconButton>
                </Tooltip>
              ))}
            </Box>

            {/* Main content */}
            <Box sx={{ flex: 1, overflow: 'auto', pt: '32px', bgcolor: 'background.default' }}>
              <Routes>
                <Route path="/"         element={<Dashboard />} />
                <Route path="/profiles" element={<Profiles />}  />
                <Route path="/devices"  element={<Devices />}   />
                <Route path="/settings" element={<Settings />}  />
              </Routes>
            </Box>

          </Box>
        </HashRouter>
      </AudioCtx.Provider>
    </ThemeProvider>
  );
}
