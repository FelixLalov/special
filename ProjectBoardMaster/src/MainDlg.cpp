// BoardMaster - dialog-based Win32 front end.
//
// Three tab pages: Settings (configuration file, machine configuration
// directory, boards and ports), I/O (four sections - AI and DI to read, AO and
// DO to write) and Log (the raw frame trace).

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Comm.h"
#include "Config.h"
#include "Profile.h"
#include "resource.h"

// ---------------------------------------------------------------- state

struct BoardRuntime {
  BoardConfig            cfg;
  std::unique_ptr<ILink> link;
  Session                session;
  bool                   connected = false;
  DeviceStatus           status;
  std::string            note = "not connected";
};

struct SectionUi {
  PinType type;
  bool    isInput;
  int     idConn, idPin, idReg, idAct, idValue, idExtra, idStatus;
};

static const SectionUi kSections[4] = {
    {PinType::AI, true,  IDC_AI_CONN, IDC_AI_PIN, IDC_AI_REG, IDC_AI_ACT, IDC_AI_VALUE,
     IDC_AI_EXTRA, IDC_AI_STATUS},
    {PinType::DI, true,  IDC_DI_CONN, IDC_DI_PIN, IDC_DI_REG, IDC_DI_ACT, IDC_DI_VALUE,
     IDC_DI_EXTRA, IDC_DI_STATUS},
    {PinType::AO, false, IDC_AO_CONN, IDC_AO_PIN, IDC_AO_REG, IDC_AO_ACT, IDC_AO_VALUE,
     IDC_AO_EXTRA, IDC_AO_STATUS},
    {PinType::DO, false, IDC_DO_CONN, IDC_DO_PIN, IDC_DO_REG, IDC_DO_ACT, IDC_DO_VALUE,
     IDC_DO_EXTRA, IDC_DO_STATUS},
};

struct App {
  HINSTANCE hInst = nullptr;
  HWND      hMain = nullptr;
  HWND      hTab  = nullptr;
  HWND      hPage[3] = {nullptr, nullptr, nullptr};
  HFONT     hMono = nullptr;

  std::string cfgPath;
  AppConfig   cfg;
  Profile     profile;
  std::string profileError;

  std::vector<BoardRuntime> boards;
  int                       ioBoard = 0;
  bool                      armed   = false;

  // Set while the board editors are being filled from the configuration, so the
  // change notifications that causes are not written back into another board.
  bool loadingEditors = false;

  // Smallest window the controls fit in, filled once the pages are measured.
  int minWinW = 0;
  int minWinH = 0;
};

static App g_app;

// ---------------------------------------------------------------- small helpers

static std::string GetText(HWND dlg, int id) {
  HWND h = GetDlgItem(dlg, id);
  if (!h) return {};
  int len = GetWindowTextLengthA(h);
  if (len <= 0) return {};
  std::string s(size_t(len) + 1, '\0');
  GetWindowTextA(h, &s[0], len + 1);
  s.resize(size_t(len));
  return s;
}

static void SetText(HWND dlg, int id, const std::string& s) {
  HWND h = GetDlgItem(dlg, id);
  if (h) SetWindowTextA(h, s.c_str());
}

static std::string Fmt(const char* fmt, ...) {
  char    buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

static void Info(const std::string& text, const char* title = "BoardMaster") {
  MessageBoxA(g_app.hMain, text.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

static void Warn(const std::string& text, const char* title = "BoardMaster") {
  MessageBoxA(g_app.hMain, text.c_str(), title, MB_OK | MB_ICONWARNING);
}

// ---------------------------------------------------------------- logging

// The log text is kept here rather than only in the control: EM_REPLACESEL is
// ignored on a read-only edit, so the buffer is the source of truth and the
// control is refreshed from it.
static std::string g_logText;

static void LogAppend(const std::string& line) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  g_logText += Fmt("%02d:%02d:%02d.%03d  %s\r\n", st.wHour, st.wMinute, st.wSecond,
                   st.wMilliseconds, line.c_str());

  // Keep the buffer bounded; drop whole lines off the front.
  const size_t kMax = 512 * 1024;
  if (g_logText.size() > kMax) {
    size_t cut = g_logText.find("\r\n", g_logText.size() - kMax);
    g_logText.erase(0, cut == std::string::npos ? g_logText.size() - kMax : cut + 2);
  }

  HWND page = g_app.hPage[2];
  if (!page) return;
  HWND edit = GetDlgItem(page, IDC_LOG_TEXT);
  if (!edit) return;

  SetWindowTextA(edit, g_logText.c_str());
  const int len = GetWindowTextLengthA(edit);
  SendMessageA(edit, EM_SETSEL, WPARAM(len), LPARAM(len));
  SendMessageA(edit, EM_SCROLLCARET, 0, 0);
}

// ---------------------------------------------------------------- status bar

static void RefreshStatusBar() {
  if (!g_app.hMain) return;

  int connected = 0;
  for (const BoardRuntime& b : g_app.boards) {
    if (b.connected) ++connected;
  }
  std::string s = Fmt("%d board%s configured, %d connected", int(g_app.boards.size()),
                      g_app.boards.size() == 1 ? "" : "s", connected);
  for (size_t i = 0; i < g_app.boards.size(); ++i) {
    const BoardRuntime& b = g_app.boards[i];
    s += "  |  " + b.cfg.name + " " +
         (b.cfg.transport == Transport::Serial ? b.cfg.port
                                               : b.cfg.host + ":" + std::to_string(b.cfg.tcpPort));
    s += b.connected ? " ok" : " -";
  }
  s += g_app.armed ? "  |  OUTPUTS ARMED" : "  |  OUTPUTS LOCKED";
  SetText(g_app.hMain, IDC_STATUSTEXT, s);
}

// ---------------------------------------------------------------- boards

static void SyncRuntimeFromConfig() {
  const size_t n = g_app.cfg.boards.size();
  g_app.boards.resize(n);
  for (size_t i = 0; i < n; ++i) {
    BoardRuntime& rt = g_app.boards[i];
    const bool    portChanged =
        rt.cfg.transport != g_app.cfg.boards[i].transport ||
        rt.cfg.port != g_app.cfg.boards[i].port || rt.cfg.host != g_app.cfg.boards[i].host ||
        rt.cfg.tcpPort != g_app.cfg.boards[i].tcpPort;
    if (portChanged && rt.link) {
      rt.link->close();
      rt.link.reset();
      rt.connected = false;
      rt.note      = "not connected";
    }
    rt.cfg = g_app.cfg.boards[i];
    rt.session.setStation(u8(rt.cfg.stationId & 0xFF));
    rt.session.setTimeout(rt.cfg.timeoutMs);
    rt.session.setTag(Fmt("B%d", int(i) + 1));
    rt.session.setLogger(&LogAppend);
  }
  if (g_app.ioBoard >= int(n)) g_app.ioBoard = 0;
}

static bool ConnectBoard(size_t i, std::string& err) {
  if (i >= g_app.boards.size()) { err = "No such board."; return false; }
  BoardRuntime& rt = g_app.boards[i];

  if (rt.link) {
    rt.link->close();
    rt.link.reset();
  }
  if (rt.cfg.transport == Transport::Serial) {
    rt.link = std::make_unique<ComLink>(rt.cfg.port, rt.cfg.baud);
  } else {
    rt.link = std::make_unique<TcpLink>(rt.cfg.host, rt.cfg.tcpPort);
  }
  rt.session.setLink(rt.link.get());

  if (!rt.link->open(err)) {
    rt.connected = false;
    rt.note      = err;
    rt.link.reset();
    rt.session.setLink(nullptr);
    LogAppend(Fmt("B%d !!  %s", int(i) + 1, err.c_str()));
    return false;
  }

  rt.connected = true;
  rt.note      = "open on " + rt.link->describe();
  LogAppend(Fmt("B%d --  opened %s", int(i) + 1, rt.link->describe().c_str()));

  // Identify straight away: it is the only way to know a MiniGPIO is answering.
  DeviceStatus st;
  std::string  ierr;
  if (rt.session.identify(st, ierr)) {
    rt.status = st;
    if (st.isBootloader) {
      rt.note = Fmt("bootloader running, application is not - card id %d", st.cardId);
    } else if (st.cardId != MINI_GPIO_CARD_ID) {
      rt.note = Fmt("card id %d, expected %d (not a MiniGPIO)", st.cardId, MINI_GPIO_CARD_ID);
    } else {
      rt.note = Fmt("connected, card id %d, fw %d.%d", st.cardId, st.major, st.minor);
    }
  } else {
    rt.note = "open, but no reply: " + ierr;
  }
  err.clear();
  return true;
}

static void DisconnectBoard(size_t i) {
  if (i >= g_app.boards.size()) return;
  BoardRuntime& rt = g_app.boards[i];
  if (rt.link) {
    LogAppend(Fmt("B%d --  closed %s", int(i) + 1, rt.link->describe().c_str()));
    rt.link->close();
    rt.link.reset();
  }
  rt.session.setLink(nullptr);
  rt.connected = false;
  rt.note      = "not connected";
}

// ---------------------------------------------------------------- browsing

static bool BrowseForFolder(HWND owner, const std::string& title, std::string& out) {
  char         buf[MAX_PATH] = {0};
  BROWSEINFOA  bi            = {};
  bi.hwndOwner = owner;
  bi.pszDisplayName = buf;
  bi.lpszTitle = title.c_str();
  bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;

  LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
  if (!pidl) return false;
  char path[MAX_PATH] = {0};
  bool ok             = SHGetPathFromIDListA(pidl, path) != FALSE;
  CoTaskMemFree(pidl);
  if (ok) out = path;
  return ok;
}

static bool BrowseForFile(HWND owner, bool saving, std::string& path) {
  char buf[MAX_PATH] = {0};
  std::snprintf(buf, sizeof(buf), "%s", path.c_str());

  OPENFILENAMEA ofn = {};
  ofn.lStructSize   = sizeof(ofn);
  ofn.hwndOwner     = owner;
  ofn.lpstrFilter   = "JSON configuration (*.json)\0*.json\0All files (*.*)\0*.*\0";
  ofn.lpstrFile     = buf;
  ofn.nMaxFile      = MAX_PATH;
  ofn.lpstrDefExt   = "json";
  ofn.lpstrTitle    = saving ? "Save BoardMaster configuration" : "Load BoardMaster configuration";
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | (saving ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

  const BOOL ok = saving ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
  if (!ok) return false;
  path = buf;
  return true;
}

// ---------------------------------------------------------------- I/O page

static void UpdateArmUi();

static void FillPinCombo(HWND page, const SectionUi& s) {
  HWND        conn = GetDlgItem(page, s.idConn);
  HWND        pin  = GetDlgItem(page, s.idPin);
  std::string c;
  int         sel = int(SendMessageA(conn, CB_GETCURSEL, 0, 0));
  if (sel >= 0) {
    char buf[64] = {0};
    SendMessageA(conn, CB_GETLBTEXT, WPARAM(sel), LPARAM(buf));
    c = buf;
  }
  SendMessageA(pin, CB_RESETCONTENT, 0, 0);
  for (int p : g_app.profile.pins(s.type, c)) {
    SendMessageA(pin, CB_ADDSTRING, 0, LPARAM(std::to_string(p).c_str()));
  }
  if (SendMessageA(pin, CB_GETCOUNT, 0, 0) > 0) SendMessageA(pin, CB_SETCURSEL, 0, 0);
}

static bool CurrentPin(HWND page, const SectionUi& s, std::string& conn, int& pin) {
  HWND hc = GetDlgItem(page, s.idConn);
  HWND hp = GetDlgItem(page, s.idPin);
  int  ci = int(SendMessageA(hc, CB_GETCURSEL, 0, 0));
  int  pi = int(SendMessageA(hp, CB_GETCURSEL, 0, 0));
  if (ci < 0 || pi < 0) return false;
  char cbuf[64] = {0}, pbuf[32] = {0};
  SendMessageA(hc, CB_GETLBTEXT, WPARAM(ci), LPARAM(cbuf));
  SendMessageA(hp, CB_GETLBTEXT, WPARAM(pi), LPARAM(pbuf));
  conn = cbuf;
  pin  = std::atoi(pbuf);
  return true;
}

static void UpdateRegText(HWND page, const SectionUi& s) {
  std::string conn;
  int         pin = 0;
  if (!g_app.profile.loaded() || !CurrentPin(page, s, conn, pin)) {
    SetText(page, s.idReg, g_app.profile.loaded() ? "no pin selected"
                                                  : "no machine configuration loaded");
    return;
  }
  const int idx  = g_app.profile.index(s.type, conn, pin);
  const int addr = g_app.profile.regAddr(s.type, conn, pin);
  if (addr < 0) {
    SetText(page, s.idReg, "not in the scheme for this type");
    return;
  }
  SetText(page, s.idReg,
          Fmt("index %d  ->  reg %d (0x%X)   IR%d   base %d", idx, addr, addr,
              PinTypeRegWidth(s.type), g_app.profile.base(s.type)));
}

static void FillSectionCombos(HWND page) {
  for (const SectionUi& s : kSections) {
    HWND conn = GetDlgItem(page, s.idConn);
    SendMessageA(conn, CB_RESETCONTENT, 0, 0);

    // Default to a real wiring connector ("J..."), not a pseudo-entry such as
    // AO's ALL_CHANNELS_CMD or DO's ACC_TEST, which would otherwise be selected
    // first just by sorting order.
    int  preferred = -1;
    int  i         = 0;
    for (const std::string& c : g_app.profile.connectors(s.type)) {
      SendMessageA(conn, CB_ADDSTRING, 0, LPARAM(c.c_str()));
      if (preferred < 0 && !c.empty() && (c[0] == 'J' || c[0] == 'j')) preferred = i;
      ++i;
    }
    if (SendMessageA(conn, CB_GETCOUNT, 0, 0) > 0) {
      SendMessageA(conn, CB_SETCURSEL, WPARAM(preferred < 0 ? 0 : preferred), 0);
    }
    FillPinCombo(page, s);
    UpdateRegText(page, s);
    SetText(page, s.idValue, "");
    SetText(page, s.idExtra, s.type == PinType::AO   ? "0 - 4095  =  0 - 10 V"
                             : s.type == PinType::DO ? "0 = off, 1 = on"
                                                     : "");
    SetText(page, s.idStatus, "");
  }
}

static void FillBoardCombo(HWND page) {
  HWND h = GetDlgItem(page, IDC_IO_BOARD);
  SendMessageA(h, CB_RESETCONTENT, 0, 0);
  for (const BoardRuntime& b : g_app.boards) {
    SendMessageA(h, CB_ADDSTRING, 0, LPARAM(b.cfg.name.c_str()));
  }
  if (g_app.ioBoard >= int(g_app.boards.size())) g_app.ioBoard = 0;
  if (!g_app.boards.empty()) SendMessageA(h, CB_SETCURSEL, WPARAM(g_app.ioBoard), 0);
}

static void UpdateArmUi() {
  HWND page = g_app.hPage[1];
  if (!page) return;
  for (const SectionUi& s : kSections) {
    if (s.isInput) continue;
    EnableWindow(GetDlgItem(page, s.idAct), g_app.armed ? TRUE : FALSE);
  }
  SetText(page, IDC_IO_NOTE,
          g_app.armed ? "Writes are enabled - they take effect immediately."
                      : "Writes are disabled. Tick the box to enable them.");
  RefreshStatusBar();
}

static void DoRead(HWND page, const SectionUi& s) {
  if (!g_app.profile.loaded()) {
    SetText(page, s.idStatus, "Set a machine configuration directory first.");
    return;
  }
  if (g_app.ioBoard < 0 || g_app.ioBoard >= int(g_app.boards.size())) return;
  BoardRuntime& rt = g_app.boards[size_t(g_app.ioBoard)];
  if (!rt.connected) {
    SetText(page, s.idStatus, "Board is not connected.");
    return;
  }

  std::string conn;
  int         pin = 0;
  if (!CurrentPin(page, s, conn, pin)) {
    SetText(page, s.idStatus, "Pick a connector and pin.");
    return;
  }
  const int addr = g_app.profile.regAddr(s.type, conn, pin);
  if (addr < 0) {
    SetText(page, s.idStatus, "That connector and pin are not in the scheme.");
    return;
  }

  std::string err;
  if (PinTypeRegWidth(s.type) == 8) {
    u8 v = 0;
    if (!rt.session.readReg8(addr, v, err)) {
      SetText(page, s.idStatus, err);
      return;
    }
    SetText(page, s.idValue, std::to_string(unsigned(v)));
    SetText(page, s.idExtra, Fmt("0x%02X   %s", v, v ? "high" : "low"));
  } else {
    u16 v = 0;
    if (!rt.session.readReg16(addr, v, err)) {
      SetText(page, s.idStatus, err);
      return;
    }
    SetText(page, s.idValue, std::to_string(unsigned(v)));
    SetText(page, s.idExtra, Fmt("0x%04X", v));
  }
  SetText(page, s.idStatus, Fmt("read %s %s pin %d ok", PinTypeName(s.type), conn.c_str(), pin));
}

static void DoWrite(HWND page, const SectionUi& s) {
  if (!g_app.armed) {
    SetText(page, s.idStatus, "Writes are disabled.");
    return;
  }
  if (!g_app.profile.loaded()) {
    SetText(page, s.idStatus, "Set a machine configuration directory first.");
    return;
  }
  if (g_app.ioBoard < 0 || g_app.ioBoard >= int(g_app.boards.size())) return;
  BoardRuntime& rt = g_app.boards[size_t(g_app.ioBoard)];
  if (!rt.connected) {
    SetText(page, s.idStatus, "Board is not connected.");
    return;
  }

  std::string conn;
  int         pin = 0;
  if (!CurrentPin(page, s, conn, pin)) {
    SetText(page, s.idStatus, "Pick a connector and pin.");
    return;
  }
  const int addr = g_app.profile.regAddr(s.type, conn, pin);
  if (addr < 0) {
    SetText(page, s.idStatus, "That connector and pin are not in the scheme.");
    return;
  }

  const std::string raw = GetText(page, s.idValue);
  if (raw.empty()) {
    SetText(page, s.idStatus, "Enter a value to write.");
    return;
  }
  const long value = std::atol(raw.c_str());

  std::string err;
  if (s.type == PinType::DO) {
    if (value < 0 || value > 255) {
      SetText(page, s.idStatus, "A DO value must be 0 - 255 (normally 0 or 1).");
      return;
    }
    if (!rt.session.writeReg8(addr, u8(value), err)) {
      SetText(page, s.idStatus, err);
      return;
    }
  } else {
    if (value < 0 || value > 4095) {
      SetText(page, s.idStatus, "An AO value must be 0 - 4095.");
      return;
    }
    if (!rt.session.writeReg16(addr, u16(value), err)) {
      SetText(page, s.idStatus, err);
      return;
    }
  }
  SetText(page, s.idStatus,
          Fmt("wrote %ld to %s %s pin %d, board acknowledged", value, PinTypeName(s.type),
              conn.c_str(), pin));
}

// ---------------------------------------------------------------- settings page

static void FillBoardsList(HWND page) {
  HWND lv = GetDlgItem(page, IDC_BOARDS_LIST);

  // Keep the row the operator had selected; refilling must not silently move
  // the selection, or the board editors would reload from a different board.
  int keep = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
  if (keep < 0 || keep >= int(g_app.boards.size())) keep = 0;

  const bool wasLoading = g_app.loadingEditors;
  g_app.loadingEditors  = true;
  ListView_DeleteAllItems(lv);
  for (size_t i = 0; i < g_app.boards.size(); ++i) {
    const BoardRuntime& b = g_app.boards[i];

    LVITEMA it = {};
    it.mask     = LVIF_TEXT;
    it.iItem    = int(i);
    it.iSubItem = 0;
    std::string name = b.cfg.name;
    it.pszText  = const_cast<char*>(name.c_str());
    ListView_InsertItem(lv, &it);

    std::string transport = (b.cfg.transport == Transport::Serial) ? "Serial" : "Simulator (TCP)";
    ListView_SetItemText(lv, int(i), 1, const_cast<char*>(transport.c_str()));

    std::string port = (b.cfg.transport == Transport::Serial)
                           ? b.cfg.port
                           : b.cfg.host + ":" + std::to_string(b.cfg.tcpPort);
    ListView_SetItemText(lv, int(i), 2, const_cast<char*>(port.c_str()));
    ListView_SetItemText(lv, int(i), 3, const_cast<char*>(b.note.c_str()));
  }

  g_app.loadingEditors = wasLoading;
  if (!g_app.boards.empty()) {
    ListView_SetItemState(lv, keep, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }
}

static void LoadBoardEditors(HWND page, int idx) {
  if (idx < 0 || idx >= int(g_app.cfg.boards.size())) return;
  const BoardConfig& b = g_app.cfg.boards[size_t(idx)];

  g_app.loadingEditors = true;
  SendMessageA(GetDlgItem(page, IDC_TRANSPORT), CB_SETCURSEL,
               WPARAM(b.transport == Transport::Serial ? 0 : 1), 0);
  SetText(page, IDC_PORT, b.port);
  SetText(page, IDC_HOST, b.host);
  SetText(page, IDC_TCPPORT, std::to_string(b.tcpPort));
  g_app.loadingEditors = false;
}

static int SelectedBoard(HWND page) {
  HWND lv = GetDlgItem(page, IDC_BOARDS_LIST);
  return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

// Writes the transport/port/host/tcp-port editors into the selected board.
// Called whenever one of those fields is left and again before any save, so the
// configuration always matches what is on screen - pressing "Apply to board"
// first is never required.
static bool CommitBoardEditors(HWND page) {
  if (!page || g_app.loadingEditors) return false;

  const int idx = SelectedBoard(page);
  if (idx < 0 || idx >= int(g_app.cfg.boards.size())) return false;

  BoardConfig& b = g_app.cfg.boards[size_t(idx)];
  BoardConfig  before = b;

  b.transport = (SendMessageA(GetDlgItem(page, IDC_TRANSPORT), CB_GETCURSEL, 0, 0) == 1)
                    ? Transport::Tcp
                    : Transport::Serial;

  const std::string port = GetText(page, IDC_PORT);
  if (!port.empty()) b.port = port;
  const std::string host = GetText(page, IDC_HOST);
  if (!host.empty()) b.host = host;
  const std::string tp = GetText(page, IDC_TCPPORT);
  if (!tp.empty()) b.tcpPort = std::atoi(tp.c_str());

  const bool changed = before.transport != b.transport || before.port != b.port ||
                       before.host != b.host || before.tcpPort != b.tcpPort;
  if (!changed) return false;

  SyncRuntimeFromConfig();
  FillBoardsList(page);
  if (g_app.hPage[1]) FillBoardCombo(g_app.hPage[1]);
  RefreshStatusBar();
  LogAppend(Fmt("--  %s set to %s", b.name.c_str(),
                b.transport == Transport::Serial
                    ? b.port.c_str()
                    : (b.host + ":" + std::to_string(b.tcpPort)).c_str()));
  return true;
}

static void RefreshProfileInfo(HWND page) {
  if (g_app.profile.loaded()) {
    SetText(page, IDC_PROFILE_INFO, g_app.profile.summary());
  } else {
    SetText(page, IDC_PROFILE_INFO,
            g_app.profileError.empty() ? "No machine configuration loaded."
                                       : g_app.profileError);
  }
}

static void ApplyProfileDir(HWND page, const std::string& dir) {
  g_app.cfg.profileDir = dir;
  SetText(page, IDC_PROFILE_DIR, dir);

  g_app.profileError.clear();
  if (dir.empty()) {
    g_app.profile.clear();
  } else if (!g_app.profile.load(dir, g_app.profileError)) {
    LogAppend("--  machine configuration: " + g_app.profileError);
  } else {
    LogAppend("--  machine configuration loaded from " + dir);
  }
  RefreshProfileInfo(page);
  if (g_app.hPage[1]) FillSectionCombos(g_app.hPage[1]);
}

static void ApplyConfigToUi(HWND page) {
  SetText(page, IDC_CFG_PATH, g_app.cfgPath);
  SetText(page, IDC_BOARDS_NUM, std::to_string(g_app.cfg.boards.size()));
  SendMessageA(GetDlgItem(page, IDC_BOARDS_SPIN), UDM_SETPOS32, 0,
               LPARAM(g_app.cfg.boards.size()));
  SyncRuntimeFromConfig();
  FillBoardsList(page);
  LoadBoardEditors(page, 0);
  ApplyProfileDir(page, g_app.cfg.profileDir);
  if (g_app.hPage[1]) FillBoardCombo(g_app.hPage[1]);
  RefreshStatusBar();
}

// ---------------------------------------------------------------- page procs

static INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      HWND lv = GetDlgItem(dlg, IDC_BOARDS_LIST);
      ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
      const char* cols[] = {"Board", "Transport", "Port", "Status"};
      const int   widths[] = {54, 82, 78, 200};
      for (int i = 0; i < 4; ++i) {
        LVCOLUMNA c = {};
        c.mask      = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.iSubItem  = i;
        c.cx        = widths[i];
        c.pszText   = const_cast<char*>(cols[i]);
        ListView_InsertColumn(lv, i, &c);
      }

      HWND tr = GetDlgItem(dlg, IDC_TRANSPORT);
      SendMessageA(tr, CB_ADDSTRING, 0, LPARAM("Serial"));
      SendMessageA(tr, CB_ADDSTRING, 0, LPARAM("Simulator"));
      SendMessageA(tr, CB_SETCURSEL, 0, 0);

      SendMessageA(GetDlgItem(dlg, IDC_BOARDS_SPIN), UDM_SETRANGE32, 1, 16);

      if (g_app.hMono) {
        SendMessageA(GetDlgItem(dlg, IDC_PROFILE_INFO), WM_SETFONT, WPARAM(g_app.hMono), TRUE);
      }
      SetText(dlg, IDC_CFG_NOTE, "Default: " + DefaultConfigPath());
      return TRUE;
    }

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_CFG_BROWSE: {
          std::string p = GetText(dlg, IDC_CFG_PATH);
          if (p.empty()) p = DefaultConfigPath();
          if (BrowseForFile(dlg, false, p)) SetText(dlg, IDC_CFG_PATH, p);
          return TRUE;
        }
        case IDC_CFG_DEFAULT:
          SetText(dlg, IDC_CFG_PATH, DefaultConfigPath());
          return TRUE;

        case IDC_CFG_LOAD: {
          const std::string path = GetText(dlg, IDC_CFG_PATH);
          std::string       err;
          AppConfig         loaded;
          if (!loaded.load(path, err)) {
            Warn(err, "Load configuration");
            return TRUE;
          }
          for (size_t i = 0; i < g_app.boards.size(); ++i) DisconnectBoard(i);
          g_app.cfg     = loaded;
          g_app.cfgPath = path;
          ApplyConfigToUi(dlg);
          LogAppend("--  configuration loaded from " + path);
          return TRUE;
        }

        case IDC_CFG_SAVE: {
          CommitBoardEditors(dlg);   // never save a stale port
          const std::string path = GetText(dlg, IDC_CFG_PATH);
          g_app.cfg.profileDir   = GetText(dlg, IDC_PROFILE_DIR);
          std::string err;
          if (!g_app.cfg.save(path, err)) {
            Warn(err, "Save configuration");
            return TRUE;
          }
          g_app.cfgPath = path;
          LogAppend("--  configuration saved to " + path);
          Info("Configuration saved to\n" + path, "Save configuration");
          return TRUE;
        }

        case IDC_PROFILE_BROWSE: {
          std::string dir = GetText(dlg, IDC_PROFILE_DIR);
          if (BrowseForFolder(dlg, "Select a machine configuration directory "
                                   "(the one holding Mini_GPIO_Scheme.py)", dir)) {
            ApplyProfileDir(dlg, dir);
          }
          return TRUE;
        }
        case IDC_PROFILE_RELOAD:
          ApplyProfileDir(dlg, GetText(dlg, IDC_PROFILE_DIR));
          return TRUE;

        case IDC_BOARD_APPLY:
          if (SelectedBoard(dlg) < 0) {
            Warn("Select a board in the list first.", "Boards");
            return TRUE;
          }
          CommitBoardEditors(dlg);
          return TRUE;

        // Committing when a field is left means Save always writes what is on
        // screen, whether or not "Apply to board" was pressed.
        case IDC_PORT:
        case IDC_HOST:
        case IDC_TCPPORT:
          if (HIWORD(wp) == EN_KILLFOCUS) CommitBoardEditors(dlg);
          return TRUE;

        case IDC_TRANSPORT:
          if (HIWORD(wp) == CBN_SELCHANGE) CommitBoardEditors(dlg);
          return TRUE;

        case IDC_CONNECT_ALL:
          SendMessageA(g_app.hMain, WM_COMMAND, IDM_BOARD_CONNECT, 0);
          return TRUE;
        case IDC_DISCONNECT_ALL:
          SendMessageA(g_app.hMain, WM_COMMAND, IDM_BOARD_DISCONNECT, 0);
          return TRUE;
        case IDC_IDENTIFY:
          SendMessageA(g_app.hMain, WM_COMMAND, IDM_BOARD_IDENTIFY, 0);
          return TRUE;

        case IDC_BOARDS_NUM:
          if (HIWORD(wp) == EN_CHANGE) {
            const std::string t = GetText(dlg, IDC_BOARDS_NUM);
            if (!t.empty()) {
              const int n = std::atoi(t.c_str());
              if (n >= 1 && n <= 16 && n != int(g_app.cfg.boards.size())) {
                for (size_t i = size_t(n); i < g_app.boards.size(); ++i) DisconnectBoard(i);
                g_app.cfg.setBoardsNum(n);
                SyncRuntimeFromConfig();
                FillBoardsList(dlg);
                LoadBoardEditors(dlg, SelectedBoard(dlg));
                if (g_app.hPage[1]) FillBoardCombo(g_app.hPage[1]);
                RefreshStatusBar();
              }
            }
          }
          return TRUE;

        default:
          break;
      }
      return FALSE;

    case WM_NOTIFY: {
      LPNMHDR nm = reinterpret_cast<LPNMHDR>(lp);
      if (nm->idFrom == IDC_BOARDS_LIST && nm->code == LVN_ITEMCHANGED) {
        LPNMLISTVIEW nlv = reinterpret_cast<LPNMLISTVIEW>(lp);
        if ((nlv->uNewState & LVIS_SELECTED) && !(nlv->uOldState & LVIS_SELECTED)) {
          LoadBoardEditors(dlg, nlv->iItem);
        }
      }
      return FALSE;
    }

    default:
      break;
  }
  return FALSE;
}

static INT_PTR CALLBACK IoProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  (void)lp;
  switch (msg) {
    case WM_INITDIALOG:
      for (const SectionUi& s : kSections) {
        if (g_app.hMono) {
          SendMessageA(GetDlgItem(dlg, s.idReg), WM_SETFONT, WPARAM(g_app.hMono), TRUE);
        }
      }
      FillBoardCombo(dlg);
      FillSectionCombos(dlg);
      return TRUE;

    case WM_COMMAND: {
      const int id = LOWORD(wp);
      const int nc = HIWORD(wp);

      if (id == IDC_IO_ARM) {
        g_app.armed = IsDlgButtonChecked(dlg, IDC_IO_ARM) == BST_CHECKED;
        LogAppend(g_app.armed ? "--  outputs ARMED by the operator"
                              : "--  outputs disarmed");
        UpdateArmUi();
        return TRUE;
      }
      if (id == IDC_IO_BOARD && nc == CBN_SELCHANGE) {
        g_app.ioBoard = int(SendMessageA(GetDlgItem(dlg, IDC_IO_BOARD), CB_GETCURSEL, 0, 0));
        RefreshStatusBar();
        return TRUE;
      }

      for (const SectionUi& s : kSections) {
        if (id == s.idConn && nc == CBN_SELCHANGE) {
          FillPinCombo(dlg, s);
          UpdateRegText(dlg, s);
          return TRUE;
        }
        if (id == s.idPin && nc == CBN_SELCHANGE) {
          UpdateRegText(dlg, s);
          return TRUE;
        }
        if (id == s.idAct && nc == BN_CLICKED) {
          if (s.isInput) {
            DoRead(dlg, s);
          } else {
            DoWrite(dlg, s);
          }
          return TRUE;
        }
      }
      return FALSE;
    }

    default:
      break;
  }
  return FALSE;
}

static INT_PTR CALLBACK LogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  (void)lp;
  switch (msg) {
    case WM_INITDIALOG:
      if (g_app.hMono) {
        SendMessageA(GetDlgItem(dlg, IDC_LOG_TEXT), WM_SETFONT, WPARAM(g_app.hMono), TRUE);
      }
      SendMessageA(GetDlgItem(dlg, IDC_LOG_TEXT), EM_SETLIMITTEXT, 0, 0);
      SetText(dlg, IDC_LOG_INFO,
              "Every frame sent and received, bytes exactly as they went on the wire.");
      return TRUE;

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_LOG_CLEAR:
          g_logText.clear();
          SetText(dlg, IDC_LOG_TEXT, "");
          return TRUE;
        case IDC_LOG_SAVE: {
          char          buf[MAX_PATH] = "BoardMasterLog.txt";
          OPENFILENAMEA ofn           = {};
          ofn.lStructSize = sizeof(ofn);
          ofn.hwndOwner   = dlg;
          ofn.lpstrFilter = "Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
          ofn.lpstrFile   = buf;
          ofn.nMaxFile    = MAX_PATH;
          ofn.lpstrDefExt = "txt";
          ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_EXPLORER;
          if (!GetSaveFileNameA(&ofn)) return TRUE;

          const std::string text = g_logText;
          FILE*             f    = std::fopen(buf, "wb");
          if (!f) {
            Warn(std::string("Cannot write \"") + buf + "\".", "Save log");
            return TRUE;
          }
          std::fwrite(text.data(), 1, text.size(), f);
          std::fclose(f);
          return TRUE;
        }
        default:
          break;
      }
      return FALSE;

    default:
      break;
  }
  return FALSE;
}

// ---------------------------------------------------------------- main dialog

static void ShowPage(int index) {
  for (int i = 0; i < 3; ++i) {
    if (g_app.hPage[i]) ShowWindow(g_app.hPage[i], i == index ? SW_SHOW : SW_HIDE);
  }
}

// ---------------------------------------------------------------- resizing
//
// Each control gets four anchor flags. An edge with its flag set stays the same
// distance from the page's corresponding edge; so AL|AR stretches horizontally,
// AR alone moves right, and AL alone stays put.
   
enum : unsigned { AL = 1, AT = 2, AR = 4, AB = 8 };

struct AnchorEntry {
  int      id;
  unsigned flags;
};

static const AnchorEntry kSettingsAnchors[] = {
    {IDC_GRP_CFG, AL | AT | AR},
    {IDC_CFG_PATH, AL | AT | AR},
    {IDC_CFG_BROWSE, AR | AT},
    {IDC_CFG_LOAD, AR | AT},
    {IDC_CFG_SAVE, AR | AT},
    {IDC_CFG_DEFAULT, AR | AT},
    {IDC_CFG_NOTE, AL | AT | AR},
    {IDC_GRP_PROFILE, AL | AT | AR},
    {IDC_PROFILE_DIR, AL | AT | AR},
    {IDC_PROFILE_BROWSE, AR | AT},
    {IDC_PROFILE_RELOAD, AR | AT},
    {IDC_PROFILE_INFO, AL | AT | AR},
    {IDC_GRP_BOARDS, AL | AT | AR | AB},
    {IDC_CONNECT_ALL, AR | AT},
    {IDC_DISCONNECT_ALL, AR | AT},
    {IDC_IDENTIFY, AR | AT},
    {IDC_BOARDS_LIST, AL | AT | AR | AB},
    {IDC_LBL_TRANSPORT, AR | AT},
    {IDC_TRANSPORT, AR | AT},
    {IDC_LBL_PORT, AR | AT},
    {IDC_PORT, AR | AT},
    {IDC_LBL_HOST, AR | AT},
    {IDC_HOST, AR | AT},
    {IDC_LBL_TCPPORT, AR | AT},
    {IDC_TCPPORT, AR | AT},
    {IDC_BOARD_APPLY, AR | AT},
};

static const AnchorEntry kLogAnchors[] = {
    {IDC_LOG_TEXT, AL | AT | AR | AB},
    {IDC_LOG_CLEAR, AL | AB},
    {IDC_LOG_SAVE, AL | AB},
    {IDC_LOG_INFO, AL | AR | AB},
};

struct AnchoredCtl {
  HWND     h;
  RECT     orig;
  unsigned flags;
};

struct PageAnchors {
  SIZE                     orig = {0, 0};
  std::vector<AnchoredCtl> ctls;
};

static PageAnchors g_anchors[3];

static unsigned AnchorFor(int pageIndex, int id, const RECT& r, const SIZE& page) {
  if (pageIndex == 0) {
    for (const AnchorEntry& e : kSettingsAnchors) {
      if (e.id == id) return e.flags;
    }
    return AL | AT;   // labels and anything unlisted stay put
  }
  if (pageIndex == 2) {
    for (const AnchorEntry& e : kLogAnchors) {
      if (e.id == id) return e.flags;
    }
    return AL | AB;
  }
  // I/O page: the four sections are a 2x2 grid of fixed-size controls, so each
  // one follows the corner of the quadrant it sits in and the four sections
  // spread to the corners of the window.
  unsigned f = 0;
  f |= (r.left * 100 >= page.cx * 50) ? AR : AL;
  f |= (r.top * 100 >= page.cy * 45) ? AB : AT;
  return f;
}

// Records every control's natural rect. Must run while the pages are still at
// their template size, before the first resize.
static void CaptureAnchors() {
  for (int i = 0; i < 3; ++i) {
    PageAnchors& pa = g_anchors[i];
    pa.ctls.clear();
    pa.orig = SIZE{0, 0};
    if (!g_app.hPage[i]) continue;

    RECT pr;
    GetClientRect(g_app.hPage[i], &pr);
    pa.orig.cx = pr.right;
    pa.orig.cy = pr.bottom;

    for (HWND c = GetWindow(g_app.hPage[i], GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
      RECT cr;
      GetWindowRect(c, &cr);
      MapWindowPoints(nullptr, g_app.hPage[i], reinterpret_cast<POINT*>(&cr), 2);
      AnchoredCtl a;
      a.h     = c;
      a.orig  = cr;
      a.flags = AnchorFor(i, GetDlgCtrlID(c), cr, pa.orig);
      pa.ctls.push_back(a);
    }
  }
}

static void ApplyAnchors(int i, int newW, int newH) {
  PageAnchors& pa = g_anchors[i];
  if (pa.ctls.empty() || pa.orig.cx == 0 || pa.orig.cy == 0) return;

  const int dx = newW - pa.orig.cx;
  const int dy = newH - pa.orig.cy;

  HDWP dwp = BeginDeferWindowPos(int(pa.ctls.size()));
  for (const AnchoredCtl& a : pa.ctls) {
    const int l = (a.flags & AL) ? a.orig.left : a.orig.left + dx;
    const int r = (a.flags & AR) ? a.orig.right + dx : a.orig.right;
    const int t = (a.flags & AT) ? a.orig.top : a.orig.top + dy;
    const int b = (a.flags & AB) ? a.orig.bottom + dy : a.orig.bottom;

    int w = r - l;
    int h = b - t;
    if (w < 4) w = 4;
    if (h < 4) h = 4;

    if (dwp) {
      dwp = DeferWindowPos(dwp, a.h, nullptr, l, t, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
      SetWindowPos(a.h, nullptr, l, t, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  if (dwp) EndDeferWindowPos(dwp);
}

// Lays the tab, the status line and the pages out for a given client size.
static void ResizeMain(int cw, int ch) {
  if (!g_app.hMain || !g_app.hTab) return;

  const int kMargin = 6;
  const int kStatus = 18;

  const int tabW = cw - 2 * kMargin;
  const int tabH = ch - 2 * kMargin - kStatus;
  if (tabW < 80 || tabH < 60) return;

  SetWindowPos(g_app.hTab, nullptr, kMargin, kMargin, tabW, tabH, SWP_NOZORDER);

  HWND status = GetDlgItem(g_app.hMain, IDC_STATUSTEXT);
  if (status) {
    SetWindowPos(status, nullptr, kMargin + 2, kMargin + tabH + 5, tabW - 4, 14, SWP_NOZORDER);
  }

  RECT disp;
  GetWindowRect(g_app.hTab, &disp);
  MapWindowPoints(nullptr, g_app.hMain, reinterpret_cast<POINT*>(&disp), 2);
  SendMessageA(g_app.hTab, TCM_ADJUSTRECT, FALSE, LPARAM(&disp));

  const int pw = disp.right - disp.left;
  const int ph = disp.bottom - disp.top;
  for (int i = 0; i < 3; ++i) {
    if (!g_app.hPage[i]) continue;
    SetWindowPos(g_app.hPage[i], HWND_TOP, disp.left, disp.top, pw, ph, SWP_NOZORDER);
    ApplyAnchors(i, pw, ph);
    // Group boxes and static frames leave trails when they move or stretch.
    RedrawWindow(g_app.hPage[i], nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
  }
}

// Sizes the tab control and the main window around the largest page, then puts
// every page in the tab's display area. Doing it from the measured pages keeps
// the window correct whatever dialog-unit scale the pages ended up with, and
// fixes the smallest size the window may be dragged to.
static void LayoutPages() {
  const int kMargin = 6;
  const int kStatus = 18;

  // A page's own client rect is the template size, but a control can sit past it
  // if the page was created with a different font metric than assumed, so take
  // the union of the client rect and every child control's extent.
  int pageW = 0, pageH = 0;
  for (int i = 0; i < 3; ++i) {
    if (!g_app.hPage[i]) continue;
    RECT pr;
    GetClientRect(g_app.hPage[i], &pr);
    int w = pr.right, h = pr.bottom;

    for (HWND c = GetWindow(g_app.hPage[i], GW_CHILD); c;
         c = GetWindow(c, GW_HWNDNEXT)) {
      RECT cr;
      GetWindowRect(c, &cr);
      MapWindowPoints(nullptr, g_app.hPage[i], reinterpret_cast<POINT*>(&cr), 2);
      if (cr.right > w) w = cr.right;
      if (cr.bottom > h) h = cr.bottom;
    }
    w += 6;   // right/bottom breathing room inside the page
    h += 6;
    if (w > pageW) pageW = w;
    if (h > pageH) pageH = h;
  }
  if (pageW == 0 || pageH == 0) return;
  LogAppend(Fmt("--  layout: page content %d x %d px", pageW, pageH));

  // Record natural rects before anything moves.
  CaptureAnchors();

  // Grow the display area into the tab's window size.
  RECT need = {0, 0, pageW, pageH};
  SendMessageA(g_app.hTab, TCM_ADJUSTRECT, TRUE, LPARAM(&need));
  const int tabW = need.right - need.left;
  const int tabH = need.bottom - need.top;

  // The natural client size, and the frame that fits around it.
  RECT want = {0, 0, kMargin * 2 + tabW, kMargin * 2 + tabH + kStatus};
  const LONG style   = GetWindowLongA(g_app.hMain, GWL_STYLE);
  const LONG exStyle = GetWindowLongA(g_app.hMain, GWL_EXSTYLE);
  AdjustWindowRectEx(&want, DWORD(style), GetMenu(g_app.hMain) != nullptr, DWORD(exStyle));
  const int winW = want.right - want.left;
  const int winH = want.bottom - want.top;

  // Never let the operator drag it smaller than the controls need.
  g_app.minWinW = winW;
  g_app.minWinH = winH;

  const int screenW = GetSystemMetrics(SM_CXSCREEN);
  const int screenH = GetSystemMetrics(SM_CYSCREEN);
  SetWindowPos(g_app.hMain, nullptr, (screenW - winW) / 2, (screenH - winH) / 3, winW, winH,
               SWP_NOZORDER);

  RECT cr;
  GetClientRect(g_app.hMain, &cr);
  ResizeMain(cr.right, cr.bottom);
}

static void ConnectAll() {
  for (size_t i = 0; i < g_app.boards.size(); ++i) {
    std::string err;
    ConnectBoard(i, err);
  }
  if (g_app.hPage[0]) FillBoardsList(g_app.hPage[0]);
  RefreshStatusBar();
}

static void DisconnectAll() {
  for (size_t i = 0; i < g_app.boards.size(); ++i) DisconnectBoard(i);
  if (g_app.hPage[0]) FillBoardsList(g_app.hPage[0]);
  RefreshStatusBar();
}

static void IdentifyAll() {
  std::string report;
  for (size_t i = 0; i < g_app.boards.size(); ++i) {
    BoardRuntime& rt = g_app.boards[i];
    if (!rt.connected) {
      report += rt.cfg.name + ": not connected\n";
      continue;
    }
    DeviceStatus st;
    std::string  err;
    if (rt.session.identify(st, err)) {
      rt.status = st;
      rt.note   = Fmt("card id %d, fw %d.%d%s", st.cardId, st.major, st.minor,
                      st.isBootloader ? ", BOOTLOADER" : "");
      report += rt.cfg.name + ": " + rt.note + "\n";
    } else {
      rt.note = err;
      report += rt.cfg.name + ": " + err + "\n";
    }
  }
  if (g_app.hPage[0]) FillBoardsList(g_app.hPage[0]);
  RefreshStatusBar();
  Info(report.empty() ? "No boards configured." : report, "Identify");
}

static INT_PTR CALLBACK MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      g_app.hMain = dlg;
      g_app.hTab  = GetDlgItem(dlg, IDC_TAB);

      g_app.hMono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                FIXED_PITCH | FF_MODERN, "Consolas");

      TCITEMA t = {};
      t.mask    = TCIF_TEXT;
      const char* names[] = {"Settings", "I/O", "Log"};
      for (int i = 0; i < 3; ++i) {
        t.pszText = const_cast<char*>(names[i]);
        SendMessageA(g_app.hTab, TCM_INSERTITEMA, WPARAM(i), LPARAM(&t));
      }

      // The Log page is created first so that everything else can log into it.
      g_app.hPage[2] = CreateDialogParamA(g_app.hInst, MAKEINTRESOURCEA(IDD_LOG), dlg, LogProc, 0);
      g_app.hPage[0] =
          CreateDialogParamA(g_app.hInst, MAKEINTRESOURCEA(IDD_SETTINGS), dlg, SettingsProc, 0);
      g_app.hPage[1] = CreateDialogParamA(g_app.hInst, MAKEINTRESOURCEA(IDD_IO), dlg, IoProc, 0);
      LayoutPages();
      ShowPage(0);

      LogAppend("--  BoardMaster started");

      // Load the default configuration if it is there; otherwise keep the
      // built-in defaults and say so.
      g_app.cfgPath = DefaultConfigPath();
      std::string err;
      AppConfig   loaded;
      if (loaded.load(g_app.cfgPath, err)) {
        g_app.cfg = loaded;
        LogAppend("--  configuration loaded from " + g_app.cfgPath);
      } else {
        LogAppend("--  no configuration loaded (" + err + "), using defaults");
      }
      ApplyConfigToUi(g_app.hPage[0]);
      UpdateArmUi();
      RefreshStatusBar();
      return TRUE;
    }

    case WM_NOTIFY: {
      LPNMHDR nm = reinterpret_cast<LPNMHDR>(lp);
      if (nm->hwndFrom == g_app.hTab && nm->code == TCN_SELCHANGE) {
        ShowPage(int(SendMessageA(g_app.hTab, TCM_GETCURSEL, 0, 0)));
        return TRUE;
      }
      return FALSE;
    }

    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) ResizeMain(LOWORD(lp), HIWORD(lp));
      return TRUE;

    case WM_GETMINMAXINFO: {
      // Arrives before WM_INITDIALOG, so only clamp once the pages are measured.
      if (g_app.minWinW > 0) {
        MINMAXINFO* mmi        = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x  = g_app.minWinW;
        mmi->ptMinTrackSize.y  = g_app.minWinH;
        return TRUE;
      }
      return FALSE;
    }

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDM_FILE_LOAD: {
          std::string p = g_app.cfgPath.empty() ? DefaultConfigPath() : g_app.cfgPath;
          if (!BrowseForFile(dlg, false, p)) return TRUE;
          AppConfig   loaded;
          std::string err;
          if (!loaded.load(p, err)) {
            Warn(err, "Load configuration");
            return TRUE;
          }
          DisconnectAll();
          g_app.cfg     = loaded;
          g_app.cfgPath = p;
          ApplyConfigToUi(g_app.hPage[0]);
          LogAppend("--  configuration loaded from " + p);
          return TRUE;
        }
        case IDM_FILE_SAVE: {
          CommitBoardEditors(g_app.hPage[0]);
          if (g_app.cfgPath.empty()) g_app.cfgPath = DefaultConfigPath();
          g_app.cfg.profileDir = GetText(g_app.hPage[0], IDC_PROFILE_DIR);
          std::string err;
          if (!g_app.cfg.save(g_app.cfgPath, err)) {
            Warn(err, "Save configuration");
            return TRUE;
          }
          LogAppend("--  configuration saved to " + g_app.cfgPath);
          return TRUE;
        }
        case IDM_FILE_SAVEAS: {
          CommitBoardEditors(g_app.hPage[0]);
          std::string p = g_app.cfgPath.empty() ? DefaultConfigPath() : g_app.cfgPath;
          if (!BrowseForFile(dlg, true, p)) return TRUE;
          g_app.cfg.profileDir = GetText(g_app.hPage[0], IDC_PROFILE_DIR);
          std::string err;
          if (!g_app.cfg.save(p, err)) {
            Warn(err, "Save configuration");
            return TRUE;
          }
          g_app.cfgPath = p;
          SetText(g_app.hPage[0], IDC_CFG_PATH, p);
          LogAppend("--  configuration saved to " + p);
          return TRUE;
        }
        case IDM_BOARD_CONNECT:
          ConnectAll();
          return TRUE;
        case IDM_BOARD_DISCONNECT:
          DisconnectAll();
          return TRUE;
        case IDM_BOARD_IDENTIFY:
          IdentifyAll();
          return TRUE;
        case IDM_HELP_ABOUT:
          Info("BoardMaster 1.0\n"
               "MiniGPIO bench tool (x64)\n\n"
               "Configuration file:\n  " + (g_app.cfgPath.empty() ? DefaultConfigPath()
                                                                  : g_app.cfgPath) +
                   "\n\nMachine configuration:\n  " +
                   (g_app.cfg.profileDir.empty() ? std::string("(none)") : g_app.cfg.profileDir),
               "About BoardMaster");
          return TRUE;
        case IDM_FILE_EXIT:
        case IDCANCEL:
          DisconnectAll();
          EndDialog(dlg, 0);
          return TRUE;
        default:
          break;
      }
      return FALSE;

    case WM_CLOSE:
      DisconnectAll();
      EndDialog(dlg, 0);
      return TRUE;

    case WM_DESTROY:
      if (g_app.hMono) {
        DeleteObject(g_app.hMono);
        g_app.hMono = nullptr;
      }
      return FALSE;

    default:
      break;
  }
  return FALSE;
}

// ---------------------------------------------------------------- entry

int RunApp(HINSTANCE hInst) {
  g_app.hInst = hInst;

  INITCOMMONCONTROLSEX icc = {};
  icc.dwSize               = sizeof(icc);
  icc.dwICC                = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS |
              ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&icc);

  CommGlobalInit();
  const INT_PTR rc = DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_MAIN), nullptr, MainProc, 0);
  CommGlobalShutdown();
  return int(rc);
}
