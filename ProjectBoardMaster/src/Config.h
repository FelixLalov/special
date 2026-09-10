// Persistent application configuration: one JSON file holding the board count,
// the port for each board, and the machine-configuration directory to read pin
// definitions from.
#pragma once

#include <string>
#include <vector>

enum class Transport { Serial, Tcp };

struct BoardConfig {
  std::string name      = "Board 1";
  Transport   transport = Transport::Serial;
  std::string port      = "COM1";        // serial: "COM7"
  int         baud      = 115200;
  std::string host      = "127.0.0.1";   // tcp: board simulator on loopback
  int         tcpPort    = 5010;
  int         stationId = 0xFF;          // single-drop; the PE uses -1 == 0xFF
  int         timeoutMs = 500;
};

struct AppConfig {
  int                      version   = 1;
  std::string              profileDir;      // e.g. ...\Config\Pro\CarmelPro1PH
  std::vector<BoardConfig> boards;

  AppConfig() { boards.resize(1); }

  int boardsNum() const { return int(boards.size()); }
  void setBoardsNum(int n);

  // Serialise / restore. Both report a human-readable error.
  bool load(const std::string& path, std::string& err);
  bool save(const std::string& path, std::string& err) const;
};

// The default configuration file: "<project root>\dat\BoardMasterConfig.json".
// The project root is located by walking up from the running executable looking
// for the directory that holds src\ and CMakeLists.txt; if that is not found
// (a copied-out binary), "<exe dir>\..\dat" is used, which is the same place
// for the normal bin\ layout.
std::string DefaultConfigPath();

// Directory the default config lives in, created on demand by save().
std::string DefaultConfigDir();

// Helpers shared with the UI.
std::string ExeDir();
std::string TransportToString(Transport t);
Transport   TransportFromString(const std::string& s);
