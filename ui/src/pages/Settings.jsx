import React from 'react';
import { Box, Typography, Paper, Button } from '@mui/material';
import OpenInNewIcon from '@mui/icons-material/OpenInNew';

export default function Settings() {
  return (
    <Box sx={{ p: 3, maxWidth: 560 }}>
      <Typography variant="h6" fontWeight={700} mb={3}>Settings</Typography>

      <Paper variant="outlined" sx={{ p: 2 }}>
        <Typography variant="subtitle2" fontWeight={700} mb={1}>About</Typography>
        <Typography variant="body2" color="text.secondary" mb={2}>
          TheAudio.app routes audio applications to specific output devices using the
          Windows per-process audio preference API. No drivers required for basic use.
        </Typography>
        <Button
          size="small" startIcon={<OpenInNewIcon />} sx={{ justifyContent: 'flex-start' }}
          onClick={() => window.api?.openExternal?.('https://github.com')}
        >
          View on GitHub
        </Button>
      </Paper>
    </Box>
  );
}
