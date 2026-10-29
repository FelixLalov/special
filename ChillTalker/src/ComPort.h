// A Win32 serial port that is polled: read() never blocks, so one thread can serve
// the keyboard and the line together.
#pragma once

#include <string>
#include <vector>

#include "Protocol.h"

struct LineSettings
{
  int  baud     = 19200;   // chiller options: 2400, 4800, 9600, 19200, 38400
  int  dataBits = 8;      // 7 or 8
  //char parity   = 'N';    // N, E or O
  char parity = 'E';    // N, E or O
  int  stopBits = 1;      // 1 or 2
};

class ComPort
{
public:
  ComPort() = default;
  ComPort(const ComPort&) = delete;
  ComPort& operator=(const ComPort&) = delete;

  ~ComPort()
  {
    close();
  }

  bool open(const std::string& name, const LineSettings& line, std::string& err);
  void close();

  bool isOpen() const
  {
    return m_h != nullptr;
  }

  bool write(const u8* data, size_t len, std::string& err);
  // Returns the bytes that have arrived (up to len), 0 when there are none, -1 on error.
  int  read(u8* buf, size_t len, std::string& err);

  // The COM ports Windows currently knows about, in numeric order.
  static std::vector<std::string> list();

private:
  void* m_h = nullptr;   // HANDLE
};
