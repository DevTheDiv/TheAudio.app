import React, { useContext, useEffect, useState } from 'react';
import {
  Box, Typography, Button, Paper, Stack, IconButton,
  TextField, Dialog, DialogTitle, DialogContent, DialogActions, Chip,
} from '@mui/material';
import AddIcon    from '@mui/icons-material/Add';
import DeleteIcon from '@mui/icons-material/Delete';
import PlayArrowIcon from '@mui/icons-material/PlayArrow';
import { AudioCtx } from '../App.jsx';

export default function Profiles() {
  const { sessions } = useContext(AudioCtx);
  const [profiles,  setProfiles]  = useState([]);
  const [saveOpen,  setSaveOpen]  = useState(false);
  const [profileName, setProfileName] = useState('');

  useEffect(() => {
    window.api?.getProfiles().then(setProfiles);
  }, []);

  async function handleSave() {
    if (!profileName.trim()) return;
    const routing = {};
    sessions.forEach(s => { if (s.endpoint) routing[s.name] = s.endpoint; });
    const profile = { name: profileName.trim(), routing, createdAt: Date.now() };
    await window.api?.saveProfile(profile);
    setProfiles(await window.api?.getProfiles());
    setProfileName('');
    setSaveOpen(false);
  }

  async function handleDelete(name) {
    await window.api?.deleteProfile(name);
    setProfiles(await window.api?.getProfiles());
  }

  async function handleApply(name) {
    await window.api?.applyProfile(name);
  }

  return (
    <Box sx={{ p: 3 }}>
      <Box sx={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', mb: 3 }}>
        <Typography variant="h6" fontWeight={700}>Profiles</Typography>
        <Button
          variant="contained" size="small" startIcon={<AddIcon />}
          onClick={() => setSaveOpen(true)}
        >
          Save Current
        </Button>
      </Box>

      {profiles.length === 0 ? (
        <Box sx={{ textAlign: 'center', py: 8 }}>
          <Typography color="text.disabled">No profiles saved yet.</Typography>
          <Typography variant="caption" color="text.disabled">
            Set up your routing in Mixer, then save it as a profile.
          </Typography>
        </Box>
      ) : (
        <Stack spacing={1}>
          {profiles.map(p => (
            <Paper key={p.name} variant="outlined" sx={{ px: 2, py: 1.5, display: 'flex', alignItems: 'center', gap: 2 }}>
              <Box sx={{ flex: 1 }}>
                <Typography variant="body2" fontWeight={600}>{p.name}</Typography>
                <Stack direction="row" flexWrap="wrap" gap={0.5} mt={0.5}>
                  {Object.entries(p.routing || {}).map(([app, ep]) => (
                    <Chip key={app} label={`${app} → ${ep}`} size="small" variant="outlined" sx={{ fontSize: 11 }} />
                  ))}
                </Stack>
              </Box>
              <IconButton size="small" color="primary" onClick={() => handleApply(p.name)}>
                <PlayArrowIcon />
              </IconButton>
              <IconButton size="small" color="error" onClick={() => handleDelete(p.name)}>
                <DeleteIcon />
              </IconButton>
            </Paper>
          ))}
        </Stack>
      )}

      <Dialog open={saveOpen} onClose={() => setSaveOpen(false)}>
        <DialogTitle>Save Profile</DialogTitle>
        <DialogContent>
          <TextField
            autoFocus fullWidth label="Profile name" size="small" sx={{ mt: 1 }}
            value={profileName}
            onChange={e => setProfileName(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter') handleSave(); }}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setSaveOpen(false)}>Cancel</Button>
          <Button variant="contained" onClick={handleSave} disabled={!profileName.trim()}>Save</Button>
        </DialogActions>
      </Dialog>
    </Box>
  );
}
