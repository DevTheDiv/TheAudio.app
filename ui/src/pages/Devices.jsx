import React, { useContext, useState, useEffect } from 'react';
import { Box, Typography, Paper, Stack, Chip, Button, Tooltip } from '@mui/material';
import SpeakerIcon from '@mui/icons-material/Speaker';
import CheckCircleIcon from '@mui/icons-material/CheckCircle';
import { AudioCtx } from '../App.jsx';

export default function Devices() {
  const { endpoints } = useContext(AudioCtx);
  const [defaultId, setDefaultId] = useState('');

  useEffect(() => {
    window.api?.getSettings().then(s => {
      if (s.defaultDevice) setDefaultId(s.defaultDevice);
    });
  }, []);

  function handleSetDefault(id) {
    setDefaultId(id);
    window.api?.saveSettings({ defaultDevice: id });
  }

  return (
    <Box sx={{ p: 3 }}>
      <Typography variant="h6" fontWeight={700} mb={3}>Devices</Typography>

      {endpoints.length === 0 ? (
        <Box sx={{ textAlign: 'center', py: 8 }}>
          <Typography color="text.disabled">No audio endpoints detected.</Typography>
        </Box>
      ) : (
        <Stack spacing={1}>
          {endpoints.map(ep => {
            const isDefault = ep.id === defaultId;
            return (
              <Paper key={ep.id} variant="outlined"
                sx={{ 
                  px: 2, py: 1.5, display: 'flex', alignItems: 'center', gap: 2,
                  borderColor: isDefault ? 'primary.main' : 'divider',
                  bgcolor: isDefault ? 'rgba(124,92,252,0.04)' : 'background.paper'
                }}>
                <SpeakerIcon sx={{ color: isDefault ? 'primary.main' : 'text.secondary' }} />
                <Box sx={{ flex: 1 }}>
                  <Typography variant="body2" fontWeight={600}>{ep.name}</Typography>
                  <Typography variant="caption" color="text.disabled"
                    sx={{ fontFamily: 'monospace', fontSize: 10 }}>
                    {ep.id}
                  </Typography>
                </Box>
                
                {isDefault ? (
                  <Chip 
                    icon={<CheckCircleIcon sx={{ fontSize: '16px !important' }} />} 
                    label="System Default" 
                    color="primary" 
                    size="small" 
                  />
                ) : (
                  <Button 
                    size="small" 
                    variant="outlined" 
                    onClick={() => handleSetDefault(ep.id)}
                    sx={{ fontSize: 10, py: 0 }}
                  >
                    Set as Default
                  </Button>
                )}
                
                <Chip label="physical" size="small" variant="outlined" sx={{ opacity: 0.5 }} />
              </Paper>
            );
          })}
        </Stack>
      )}
    </Box>
  );
}
