#include "Config.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "Json.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------- paths

std::string ExeDir() {
  char buf[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::error_code ec;
  fs::path p = fs::path(buf).parent_path();
  return p.string();
}

// Walk up from the executable looking for the project root, i.e. the first
// ancestor that contains both src\ and CMakeLists.txt. bin\ (our output dir)
// and build\<cfg>\ layouts are both found this way.
static bool FindProjectRoot(fs::path& out) {
  std::error_code ec;
  fs::path        dir = fs::path(ExeDir());
  for (int up = 0; up < 8 && !dir.empty(); ++up) {
    if (fs::exists(dir / "CMakeLists.txt", ec) && fs::is_directory(dir / "src", ec)) {
      out = dir;
      return true;
    }
    fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return false;
}

std::string DefaultConfigDir() {
  std::error_code ec;
  fs::path        root;
  fs::path        base;
  if (FindProjectRoot(root)) {
    base = root / "dat";   // "dat" under the project root
  } else {
    // Copied-out binary: bin\ sits under the root, so one level up plus dat
    // lands in the same place.
    base = fs::path(ExeDir()) / ".." / "dat";
  }
  fs::path norm = fs::weakly_canonical(base, ec);
  return (ec ? base : norm).string();
}

std::string DefaultConfigPath() {
  return (fs::path(DefaultConfigDir()) / "BoardMasterConfig.json").string();
}

// ---------------------------------------------------------------- enums

std::string TransportToString(Transport t) {
  return t == Transport::Tcp ? "tcp" : "serial";
}

Transport TransportFromString(const std::string& s) {
  return (s == "tcp" || s == "TCP" || s == "simulator") ? Transport::Tcp : Transport::Serial;
}

// ---------------------------------------------------------------- AppConfig

void AppConfig::setBoardsNum(int n) {
  if (n < 1) n = 1;
  if (n > 16) n = 16;
  const int old = int(boards.size());
  boards.resize(size_t(n));
  for (int i = old; i < n; ++i) {
    BoardConfig b;
    b.name    = "Board " + std::to_string(i + 1);
    b.port    = "COM1";
    b.tcpPort = 5010 + i;
    boards[size_t(i)] = b;
  }
}

bool AppConfig::load(const std::string& path, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "Cannot open \"" + path + "\".";
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  json::Value root;
  std::string jerr;
  if (!json::parse(ss.str(), root, jerr)) {
    err = "Malformed JSON in \"" + path + "\": " + jerr;
    return false;
  }
  if (!root.isObject()) {
    err = "The configuration file must contain a JSON object.";
    return false;
  }

  version    = root["version"].asInt(1);
  profileDir = root["profileDir"].asString();

  std::vector<BoardConfig> parsed;
  const json::Value& arr = root["boards"];
  if (arr.isArray()) {
    for (const json::Value& bv : arr.arr()) {
      BoardConfig b;
      b.name      = bv["name"].asString(b.name);
      b.transport = TransportFromString(bv["transport"].asString("serial"));
      b.port      = bv["port"].asString(b.port);
      b.baud      = bv["baud"].asInt(b.baud);
      b.host      = bv["host"].asString(b.host);
      b.tcpPort   = bv["tcpPort"].asInt(b.tcpPort);
      b.stationId = bv["stationId"].asInt(b.stationId);
      b.timeoutMs = bv["timeoutMs"].asInt(b.timeoutMs);
      parsed.push_back(b);
    }
  }

  // "boardsNum" wins when present, so the file stays authoritative even if the
  // boards array was hand-edited to a different length.
  const int wanted = root["boardsNum"].asInt(parsed.empty() ? 1 : int(parsed.size()));
  boards = parsed;
  if (boards.empty()) boards.resize(1);
  setBoardsNum(wanted);

  for (size_t i = 0; i < boards.size(); ++i) {
    if (boards[i].name.empty()) boards[i].name = "Board " + std::to_string(i + 1);
  }
  err.clear();
  return true;
}

bool AppConfig::save(const std::string& path, std::string& err) const {
  std::error_code ec;
  fs::path        p(path);
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);   // makes ..\dat on first save
    if (ec) {
      err = "Cannot create \"" + p.parent_path().string() + "\": " + ec.message();
      return false;
    }
  }

  json::Value root;
  root.set("version", json::Value(version));
  root.set("profileDir", json::Value(profileDir));
  root.set("boardsNum", json::Value(int(boards.size())));

  json::Array arr;
  for (const BoardConfig& b : boards) {
    json::Value bv;
    bv.set("name", json::Value(b.name));
    bv.set("transport", json::Value(TransportToString(b.transport)));
    bv.set("port", json::Value(b.port));
    bv.set("baud", json::Value(b.baud));
    bv.set("host", json::Value(b.host));
    bv.set("tcpPort", json::Value(b.tcpPort));
    bv.set("stationId", json::Value(b.stationId));
    bv.set("timeoutMs", json::Value(b.timeoutMs));
    arr.push_back(bv);
  }
  root.set("boards", json::Value(arr));

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = "Cannot write \"" + path + "\".";
    return false;
  }
  out << root.dump(2) << "\n";
  if (!out) {
    err = "Write to \"" + path + "\" failed.";
    return false;
  }
  err.clear();
  return true;
}
