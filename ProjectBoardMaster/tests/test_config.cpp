// Standalone check of the JSON config: default path, save, reload, hand-edit.
#include <cstdio>
#include <fstream>
#include <string>
#include "Config.h"
#include "Json.h"

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++failures;
}

int main() {
  std::printf("ExeDir            : %s\n", ExeDir().c_str());
  std::printf("DefaultConfigDir  : %s\n", DefaultConfigDir().c_str());
  std::printf("DefaultConfigPath : %s\n\n", DefaultConfigPath().c_str());

  const std::string tmp = "test_roundtrip.json";

  AppConfig a;
  a.setBoardsNum(3);
  a.profileDir = "D:\\ZZ_Experiments\\ProjectH\\MetalJet\\XjetApps\\MetalJet\\Apps\\"
                 "Project\\qt\\Config\\Pro\\CarmelPro1PH";
  a.boards[0].port = "COM7";
  a.boards[1].port = "COM9";
  a.boards[2].transport = Transport::Tcp;
  a.boards[2].tcpPort   = 5011;

  std::string err;
  check(a.save(tmp, err), "save");
  if (!err.empty()) std::printf("      err: %s\n", err.c_str());

  AppConfig b;
  check(b.load(tmp, err), "load");
  if (!err.empty()) std::printf("      err: %s\n", err.c_str());

  check(b.boardsNum() == 3, "board count survives");
  check(b.profileDir == a.profileDir, "profileDir survives (backslashes intact)");
  check(b.boards[0].port == "COM7", "board 1 port");
  check(b.boards[1].port == "COM9", "board 2 port");
  check(b.boards[2].transport == Transport::Tcp, "board 3 transport");
  check(b.boards[2].tcpPort == 5011, "board 3 tcp port");
  check(b.boards[2].stationId == 0xFF, "station id default");

  // Growing and shrinking the board count.
  b.setBoardsNum(5);
  check(b.boardsNum() == 5 && b.boards[4].name == "Board 5", "grow to 5 names new boards");
  b.setBoardsNum(2);
  check(b.boardsNum() == 2 && b.boards[0].port == "COM7", "shrink keeps the first boards");

  // A hand-edited file: comments, different key order, missing keys.
  {
    std::ofstream o("test_handedit.json", std::ios::binary);
    o << "{\n  // a comment the operator left\n  \"boardsNum\": 2,\n"
         "  \"boards\": [ { \"port\": \"COM3\" }, { \"transport\": \"tcp\" } ],\n"
         "  \"profileDir\": \"C:\\\\cfg\\\\Pro\\\\CarmelPro1PH\"\n}\n";
  }
  AppConfig c;
  check(c.load("test_handedit.json", err), "load hand-edited file with comments");
  if (!err.empty()) std::printf("      err: %s\n", err.c_str());
  check(c.boardsNum() == 2, "hand-edited board count");
  check(c.boards[0].port == "COM3", "hand-edited port");
  check(c.boards[0].baud == 115200, "missing key falls back to the default");
  check(c.boards[1].transport == Transport::Tcp, "hand-edited transport");
  check(c.profileDir == "C:\\cfg\\Pro\\CarmelPro1PH", "hand-edited profileDir unescaped");

  // Malformed input must fail cleanly, not crash.
  {
    std::ofstream o("test_bad.json", std::ios::binary);
    o << "{ \"boardsNum\": 2, ";
  }
  AppConfig d;
  check(!d.load("test_bad.json", err), "malformed JSON is rejected");
  std::printf("      message: %s\n", err.c_str());
  check(!d.load("no_such_file_here.json", err), "missing file is rejected");

  std::printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed", failures,
              failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
