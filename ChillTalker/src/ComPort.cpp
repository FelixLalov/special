#include "ComPort.h"

#include <windows.h>

#include <algorithm>

static std::string LastErrText(DWORD e)
{
  char* msg = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, e, 0, (LPSTR)&msg, 0, nullptr);
  std::string out = msg ? msg : "unknown error";
  if (msg) LocalFree(msg);
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
  return out;
}

bool ComPort::open(const std::string& name, const LineSettings& line, std::string& err)
{
  close();

  // \\.\ prefix is required for COM10 and above and harmless below it.
  const std::string dev = "\\\\.\\" + name;

  HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) {
      err = name + " is already open in another program.";
    } else if (e == ERROR_FILE_NOT_FOUND) {
      err = name + " does not exist on this computer.";
    } else {
      err = "Cannot open " + name + ": " + LastErrText(e);
    }
    return false;
  }

  DCB dcb = {};
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(h, &dcb)) {
    err = "GetCommState failed on " + name + ": " + LastErrText(GetLastError());
    CloseHandle(h);
    return false;
  }
  dcb.BaudRate        = DWORD(line.baud);
  dcb.ByteSize        = BYTE(line.dataBits);
  dcb.Parity          = line.parity == 'E' ? EVENPARITY : line.parity == 'O' ? ODDPARITY : NOPARITY;
  dcb.StopBits        = line.stopBits == 1 ? ONESTOPBIT : TWOSTOPBITS;
  dcb.fBinary         = TRUE;
  dcb.fParity         = line.parity != 'N';
  dcb.fOutxCtsFlow    = FALSE;
  dcb.fOutxDsrFlow    = FALSE;
  dcb.fDsrSensitivity = FALSE;
  // DTR and RTS are held asserted, as a terminal program does: some RS-232 / RS-485
  // converters take their power from them. The chiller uses no handshake.
  dcb.fDtrControl     = DTR_CONTROL_ENABLE;
  dcb.fRtsControl     = RTS_CONTROL_ENABLE;
  dcb.fOutX           = FALSE;
  dcb.fInX            = FALSE;
  dcb.fErrorChar      = FALSE;
  dcb.fNull           = FALSE;
  dcb.fAbortOnError   = FALSE;
  if (!SetCommState(h, &dcb)) {
    err = "SetCommState failed on " + name + ": " + LastErrText(GetLastError());
    CloseHandle(h);
    return false;
  }

  // A read returns at once with whatever has arrived, even nothing.
  COMMTIMEOUTS to = {};
  to.ReadIntervalTimeout        = MAXDWORD;
  to.ReadTotalTimeoutMultiplier = 0;
  to.ReadTotalTimeoutConstant   = 0;
  to.WriteTotalTimeoutConstant  = 500;
  SetCommTimeouts(h, &to);

  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
  m_h = h;
  err.clear();
  return true;
}

void ComPort::close()
{
  if (m_h) {
    CloseHandle((HANDLE)m_h);
    m_h = nullptr;
  }
}

bool ComPort::write(const u8* data, size_t len, std::string& err)
{
  if (!m_h) { err = "Port is not open."; return false; }
  DWORD written = 0;
  if (!WriteFile((HANDLE)m_h, data, DWORD(len), &written, nullptr) || written != len) {
    err = "Write failed: " + LastErrText(GetLastError());
    return false;
  }
  return true;
}

int ComPort::read(u8* buf, size_t len, std::string& err)
{
  if (!m_h) { err = "Port is not open."; return -1; }
  DWORD got = 0;
  if (!ReadFile((HANDLE)m_h, buf, DWORD(len), &got, nullptr)) {
    err = "Read failed: " + LastErrText(GetLastError());
    return -1;
  }
  return int(got);
}

std::vector<std::string> ComPort::list()
{
  std::vector<std::string> ports;

  HKEY key = nullptr;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ,
                    &key) != ERROR_SUCCESS) {
    return ports;
  }
  for (DWORD i = 0;; i++) {
    char  name[256];
    char  data[64] = {};
    DWORD nameLen = sizeof name;
    DWORD dataLen = sizeof data - 1;
    DWORD type    = 0;
    const LONG r = RegEnumValueA(key, i, name, &nameLen, nullptr, &type, (LPBYTE)data, &dataLen);
    if (r == ERROR_NO_MORE_ITEMS) break;
    if (r == ERROR_SUCCESS && type == REG_SZ) ports.push_back(data);
  }
  RegCloseKey(key);

  // COM4 before COM11: shorter names first, then alphabetical.
  std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
    return a.size() != b.size() ? a.size() < b.size() : a < b;
  });
  return ports;
}
