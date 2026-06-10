import { createTheme } from '@mui/material/styles';

const theme = createTheme({
  palette: {
    mode: 'dark',
    primary:   { main: '#7c5cfc' },
    secondary: { main: '#00d4aa' },
    background: {
      default: '#0f1117',
      paper:   '#1a1d27',
    },
    divider: 'rgba(255,255,255,0.08)',
  },
  typography: {
    fontFamily: '"Inter", "Segoe UI", sans-serif',
  },
  shape: { borderRadius: 10 },
  components: {
    MuiPaper: {
      styleOverrides: {
        root: { backgroundImage: 'none' },
      },
    },
    MuiSlider: {
      styleOverrides: {
        root: { color: '#7c5cfc' },
      },
    },
  },
});

export default theme;
