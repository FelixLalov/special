// Standalone check of the profile parser against real machine config dirs.
#include <cstdio>
#include <string>
#include "Profile.h"

static int failures = 0;

// mustParse: a real machine config that has to keep working.
static void dumpOne(const char* dir, bool mustParse = false) {
  Profile     p;
  std::string err;
  std::printf("=== %s\n", dir);
  if (!p.load(dir, err)) {
    std::printf("  %s: %s\n\n", mustParse ? "FAIL" : "expected failure", err.c_str());
    if (mustParse) ++failures;
    return;
  }
  if (p.base(PinType::AI) < 0 || p.base(PinType::DI) < 0 || p.base(PinType::DO) < 0 ||
      p.base(PinType::AO) < 0 || p.pinCount(PinType::DI) == 0) {
    std::printf("  FAIL: parsed but incomplete\n");
    ++failures;
  }
  std::printf("%s\n", p.summary().c_str());

  const PinType types[] = {PinType::AI, PinType::DI, PinType::DO, PinType::AO};
  for (PinType t : types) {
    auto conns = p.connectors(t);
    std::printf("  %s connectors:", PinTypeName(t));
    for (auto& c : conns) std::printf(" %s", c.c_str());
    std::printf("\n");
    if (!conns.empty()) {
      const std::string& c0 = conns.front();
      auto               pins = p.pins(t, c0);
      std::printf("    %s pins:", c0.c_str());
      for (int pin : pins) {
        std::printf(" %d->idx%d/reg%d", pin, p.index(t, c0, pin), p.regAddr(t, c0, pin));
      }
      std::printf("\n");
    }
  }
  std::printf("\n");
}

int main() {
  const char* root =
      "D:/ZZ_Experiments/ProjectH/MetalJet/XjetApps/MetalJet/Apps/Project/qt/Config/";
  dumpOne((std::string(root) + "Pro/CarmelPro1PH").c_str(), true);
  dumpOne((std::string(root) + "Carmel5000/C25").c_str(), true);
  dumpOne((std::string(root) + "Aurora4").c_str(), true);
  dumpOne((std::string(root) + "Carmel1400/Metal/STST316L").c_str(), true);
  dumpOne("D:/ZZ_Experiments/ProjectBoardMaster");   // expect a clean failure

  std::printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed", failures,
              failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
