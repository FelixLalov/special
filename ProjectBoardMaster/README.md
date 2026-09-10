# BoardMaster

A standalone C++ / Win32 dialog application that connects to one or more MiniGPIO
boards and reads or writes their **AI**, **DI**, **AO** and **DO** pins, one pin at
a time.

It is deliberately independent of the MaterialJet Print Engine: no PE libraries,
no Qt, no Python, one executable plus one JSON file.

```
D:\ZZ_Experiments\ProjectBoardMaster
├── bin\BoardMaster.exe        the application
├── dat\BoardMasterConfig.json the configuration file (default location)
├── src\                       sources
├── tests\                     console self-checks
└── CMakeLists.txt
```

---

## Build

Needs MSVC (Visual Studio 2022 or newer) and CMake. From a Developer prompt, or
after running `vcvars64.bat`:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable lands in `bin\BoardMaster.exe`. To run the self-checks:

```
cmake --build build --target run_tests
```

---

## The configuration file

One JSON file holds everything persistent: how many boards there are, the port
for each, and which machine-configuration directory to read pin definitions from.

**Default location** — the `dat` directory under the project root, i.e.

```
D:\ZZ_Experiments\ProjectBoardMaster\dat\BoardMasterConfig.json
```

The project root is found by walking up from the running executable until a
directory containing both `src\` and `CMakeLists.txt` is seen, so the same
default holds whether the binary sits in `bin\` or in a `build\<config>\` tree.
If the executable is copied somewhere else entirely, the app falls back to
`<exe dir>\..\dat`, which is the same directory for the normal `bin\` layout.
The Settings tab always shows the resolved path.

The directory is created on the first save. Nothing is written until you ask.

**Loading a different file** — Settings tab: type or **Browse...** to a path and
press **Load**. Or *File > Load Configuration...*. **Save** writes the file the
path box names; *File > Save Configuration As...* writes a copy elsewhere.

```json
{
  "version": 1,
  "profileDir": "...\\Config\\Pro\\CarmelPro1PH",
  "boardsNum": 2,
  "boards": [
    { "name": "Board 1", "transport": "serial", "port": "COM7",
      "baud": 115200, "host": "127.0.0.1", "tcpPort": 5010,
      "stationId": 255, "timeoutMs": 500 },
    { "name": "Board 2", "transport": "serial", "port": "COM9", "...": "..." }
  ]
}
```

The reader is lenient on purpose: `//` and `/* */` comments are allowed, key
order does not matter, and a missing key falls back to its default. `boardsNum`
wins over the length of the `boards` array, so shortening the array by hand does
not silently drop boards.

---

## The machine configuration directory

This is where the app learns what pins exist and how to address them. Point it at
a MetalJet machine-configuration directory — for example

```
...\XjetApps\MetalJet\Apps\Project\qt\Config\Pro\CarmelPro1PH
```

Settings tab: **Config dir** + **Browse...**, then **Reload**. Two files in that
directory are read (and if the directory itself does not hold them, up to three
parent directories are searched, the way the Print Engine resolves config files):

| File | What is taken from it |
|---|---|
| `Mini_GPIO_Scheme.py` | `GPIO_Scheme` — for each of AI/AO/DI/DO, connector + pin → index |
| `Mini_GPIO_IoDefs.py` | `ANALOG_SENSORS_BASE`, `EXT_DAC_BUFFER_BASE`, `DIGITAL_SENSORS_STATUS_BASE`, `ACTUATORS_REG_BASE` — the register base each type counts from |

Neither file is executed. The scheme is parsed as a Python dict literal; the
IoDefs constants are evaluated as integer arithmetic over previously defined
names, so a base written as `HEATERS_STATUS_BASE + MAX_HEATERS_NUM` resolves
correctly. Lines the evaluator cannot handle (calls, lists, f-strings) are
skipped. Type annotations such as `GPIO_Scheme: GPIOSharedUtils.IOSchemeDict = {`
are accepted.

**This has to be read per machine, not hardcoded.** Every base cascades from a
handful of root counts, so they move between board revisions and ink counts:

| Config | AI base | DI base | DO base | AO base |
|---|---|---|---|---|
| `Pro\CarmelPro1PH` | 16 | 88 | 26 | 260 |
| `Carmel5000\C25` | 8 | 80 | 18 | 188 |
| `Aurora4` | 8 | 80 | 18 | 188 |
| `Carmel1400\Metal\STST316L` | 4 | 76 | 14 | 176 |

---

## How a pin becomes a message

```
register address = base[type] + scheme[type][connector][pin]
```

| Type | Register class | Read | Write |
|---|---|---|---|
| AI | IR16 | `UC_REG16_READ` (45) → `HERE_IS_UC_REG16_VALUE` (36) | — |
| DI | IR8 | `UC_REG8_READ` (42) → `HERE_IS_UC_REG8_VALUE` (35) | — |
| DO | IR8 | `UC_REG8_READ` (42) | `UC_REG8_WRITE` (41) → ACK |
| AO | IR16 | — | `UC_REG16_WRITE` (44) → ACK |

The address goes in the message's `RegNum` field. The 16-bit register offset in
the firmware header is a cache index only and is **not** part of the frame.

Frame layout, as the Print Engine and the board firmware both build it:

```
0x55 0x33 <station> <len lo> <len hi> <payload ...> <checksum>
```

* `0x55` inside station/length/payload is escaped as `0x55 0xAA`
* `0x55 0x33` is a resynchronise marker and never data
* the checksum is the 8-bit sum of station + length + payload, taken before
  escaping, and is written **unescaped** — so a checksum that happens to be
  `0x55` is a literal checksum byte, not an escape lead-in
* 115200 8-N-1, station `0xFF`, one board per port

---

## Using it

**Settings** — set the number of boards (1–16), pick a row, set its transport and
port, **Apply to board**. `Serial` uses the COM port; `Simulator` connects to
`host:tcpPort`, which is how the `Mini_GPIO_V*_Sim.exe` board simulator is
reached. **Connect All** opens every port and identifies each board: a MiniGPIO
answers with card id 7 and its firmware version, and the Status column says so.

**I/O** — four sections. AI and DI have a **Read** button; AO and DO have a value
box and a **Write** button. Pick a connector and a pin and the resolved register
address is shown before you act on it. AO values are 12-bit counts, 0–4095,
mapping to 0–10 V; DO is 0 or 1.

Writes are refused until **Enable writes (outputs armed)** is ticked. DO and AO
pins drive valves, pumps and heaters, so the app starts read-only every time.

**Log** — every frame in both directions, bytes as they went on the wire, with
the decoded message name, timings, retries and errors.

---

## Known limits

* **One master per port.** The link is single-drop. If the Print Engine holds a
  COM port, opening it here fails with that explanation rather than corrupting
  both sides. Do not run this against a board the Print Engine is using.
* **No polling.** A Read is one round trip on the UI thread, with a 500 ms
  timeout and one retry. There is no background refresh, so no worker thread.
  A read that times out blocks the window for up to a second.
* **No value conversion.** Raw register values are shown (plus hex, and
  high/low for digital pins). Converting counts to °C, kPa or grams would mean
  reading `AnalogSensorsConversions.ini` as well, which is not done here.
* **The AO write sequence is unconfirmed on hardware.** The app writes the DAC
  buffer register. Whether that is live on its own, or needs the load-all
  command and the on/off register, still needs checking on a real board.
* **The watchdog is untouched.** The board's output watchdog is left exactly as
  found.
* ANSI paths only; a configuration path with non-ASCII characters is not handled.

---

## Self-checks

`cmake --build build --target run_tests` runs three console programs:

| Test | What it covers |
|---|---|
| `test_config` | JSON round-trip, board count growth and shrink, hand-edited files with comments, malformed input rejected |
| `test_comm` | frame encode/decode including `0x55` escaping and a `0x55` checksum, bad checksums rejected, then identify / read / write against a fake board over TCP loopback |
| `test_profile` | parses four real machine-configuration directories and checks all four bases resolve |

`test_profile` reads the MetalJet tree at
`D:\ZZ_Experiments\ProjectH\MetalJet\...`; adjust the paths at the top of
`tests\test_profile.cpp` if that tree moves.
