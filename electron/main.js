/* ================================================================
   RC SIM RACING ARCADE — Electron desktop wrapper
   ----------------------------------------------------------------
   Wraps the existing static web app (../index.html) in a Chromium
   window so it runs as a standalone Windows/macOS/Linux program with
   NO browser required. Every browser API the app relies on works here:
   Web Serial (USB transmitter), Gamepad, WebRTC, and getUserMedia.

   The one thing Electron does differently from a browser: when the page
   calls navigator.serial.requestPort(), Chromium normally shows its own
   port chooser. Electron hands that decision to us via 'select-serial-
   port' instead — so we auto-pick the ESP32 transmitter by its USB-
   serial chip vendor id (CP210x / CH340 / FTDI / Espressif native USB),
   falling back to the first port. That means "Connect" just works with
   one transmitter plugged in, no dialog.

   Kiosk mode: set RCARCADE_KIOSK=1 to launch fullscreen kiosk (good for
   an arcade cabinet). Otherwise it opens a normal resizable window.
   ================================================================ */
const { app, BrowserWindow, session, Menu, shell } = require('electron');
const path = require('path');

// USB-serial bridge chips commonly found on ESP32 dev boards, plus the
// ESP32-S3/C3 native-USB vendor id (Espressif 0x303A).
const ESP_VENDOR_IDS = [0x10c4, 0x1a86, 0x0403, 0x303a, 0x1a86];

const KIOSK = process.env.RCARCADE_KIOSK === '1';

function createWindow() {
  const win = new BrowserWindow({
    width: 1360,
    height: 860,
    minWidth: 900,
    minHeight: 600,
    backgroundColor: '#04060a',
    autoHideMenuBar: true,
    kiosk: KIOSK,
    title: 'RC Sim Arcade',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      // The app is fully client-side; no preload/IPC needed.
    },
  });

  const ses = win.webContents.session;

  // ---- Permissions: allow serial + camera/mic (FPV) without prompts ----
  ses.setPermissionCheckHandler(() => true);
  ses.setPermissionRequestHandler((_wc, _perm, cb) => cb(true));
  // Persist device permission so getPorts()/requestPort() are remembered.
  ses.setDevicePermissionHandler(() => true);

  // ---- Web Serial port selection (replaces Chromium's native chooser) ----
  ses.on('select-serial-port', (event, portList, _webContents, callback) => {
    event.preventDefault();
    if (!portList || portList.length === 0) { callback(''); return; }
    // Prefer a port whose USB vendor id matches a known ESP32 bridge.
    const esp = portList.find((p) => ESP_VENDOR_IDS.includes(p.vendorId));
    const chosen = esp || portList[0];
    callback(chosen.portId);
  });

  // Open external links (e.g. docs) in the user's real browser, not the app.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:\/\//i.test(url)) { shell.openExternal(url); return { action: 'deny' }; }
    return { action: 'allow' };
  });

  // Minimal menu (keep F11 fullscreen + devtools for diagnostics).
  const menu = Menu.buildFromTemplate([
    {
      label: 'View',
      submenu: [
        { role: 'reload' },
        { role: 'forceReload' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'togglefullscreen' },
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'quit' },
      ],
    },
  ]);
  Menu.setApplicationMenu(menu);

  win.loadFile(path.join(__dirname, '..', 'index.html'));
}

// Single-instance: focus the existing window instead of opening a second.
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    const w = BrowserWindow.getAllWindows()[0];
    if (w) { if (w.isMinimized()) w.restore(); w.focus(); }
  });

  app.whenReady().then(createWindow);

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
}

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
