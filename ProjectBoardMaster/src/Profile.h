// Board profile: everything needed to turn a wiring location (connector + pin)
// into a register access, read straight out of a MetalJet machine-configuration
// directory such as ...\Config\Pro\CarmelPro1PH.
//
// Two files in that directory carry it:
//
//   Mini_GPIO_Scheme.py   GPIO_Scheme = { 'AI': { 'J6': {11:2, ...}, ... }, ... }
//                         connector + pin  ->  index within the I/O type
//
//   Mini_GPIO_IoDefs.py   ACTUATORS_REG_BASE, DIGITAL_SENSORS_STATUS_BASE,
//                         ANALOG_SENSORS_BASE, EXT_DAC_BUFFER_BASE
//                         the register base each I/O type counts from
//
// Register address  =  base[type] + index, which is exactly what the firmware
// expects in the RegNum field of a register read/write message.
#pragma once

#include <map>
#include <string>
#include <vector>

enum class PinType { AI, AO, DI, DO };

const char* PinTypeName(PinType t);

// Register width used on the wire for a given I/O type: DI/DO are 8-bit
// registers, AI/AO are 16-bit registers.
int PinTypeRegWidth(PinType t);

class Profile {
public:
  using PinMap       = std::map<int, int>;          // pin number -> index
  using ConnectorMap = std::map<std::string, PinMap>;

  // Reads the two .py files out of dir (walking up a few parents if the
  // directory itself does not hold them, the way the Print Engine resolves
  // config files). Returns false and fills err when nothing usable was found.
  bool load(const std::string& dir, std::string& err);

  bool loaded() const { return m_loaded; }
  void clear();

  // Wiring -> register. index() and regAddr() return -1 when the pair is not
  // in the scheme for that type.
  std::vector<std::string> connectors(PinType t) const;
  std::vector<int>         pins(PinType t, const std::string& connector) const;
  int                      index(PinType t, const std::string& connector, int pin) const;
  int                      regAddr(PinType t, const std::string& connector, int pin) const;

  int base(PinType t) const;
  int pinCount(PinType t) const;   // how many (connector,pin) pairs the scheme lists

  const std::string& schemeFile() const { return m_schemeFile; }
  const std::string& ioDefsFile() const { return m_ioDefsFile; }

  // Multi-line human-readable summary for the Settings page.
  std::string summary() const;

private:
  const ConnectorMap* connectorsFor(PinType t) const;

  bool m_loaded = false;

  std::map<std::string, ConnectorMap> m_scheme;      // "AI" -> connector -> pins
  std::map<std::string, long long>    m_consts;      // every constant IoDefs defines

  std::string m_schemeFile;
  std::string m_ioDefsFile;

  int m_baseAI = -1, m_baseAO = -1, m_baseDI = -1, m_baseDO = -1;
};
