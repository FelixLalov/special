# ChillTalker

A small console tool that opens a serial port and talks to an **SMC Thermo-con
HEF002-A6** chiller. A text menu sends the requests the chiller understands; every
frame in both directions is shown as bytes and as plain English, and so is anything
that arrives while the prompt is idle.

It also works with the **Nanotec PD1-C** motor drives that share the chiller's RS-485
bus: a scan for their Modbus slave ids, and a test that turns one of them.

```
ChillTalker\
├── ChillTalker.sln
├── src\        the console application  (ChillTalker.vcxproj)
└── tests\      console self-check       (ChillTalker.Tests.vcxproj)
```

No libraries beyond the Windows SDK and the C++ standard library.

---

## The chiller is not Modbus

The Serial Communication Manual (`Chiller_hef002_SerialCommManual.pdf`, DOC1009224-1)
does not describe Modbus. The word does not appear in it, nor in the operation manual
next to it. The chiller speaks a plain ASCII request / reply protocol, and that is what
this tool implements:

```
read         STX <addr:2> R <id:3>              ETX [BCC]
write        STX <addr:2> W <id:3> <data:5>     ETX [BCC]
store        STX <addr:2> W S T R               ETX [BCC]
read reply   STX <addr:2> ACK <id:3> <data:5>   ETX [BCC]
other reply  STX <addr:2> ACK                   ETX [BCC]
error reply  STX <addr:2> NAK <error:1>         ETX [BCC]
```

* `STX` 02h, `ETX` 03h, `ACK` 06h, `NAK` 15h; everything else is printable ASCII.
* The address is two digits, 01 to 99. The chiller ignores other addresses.
* The data is five characters with an implied decimal point: `00250` is 25.0 °C, a
  negative value has `-` first: `-0099` is -9.9 °C.
* `BCC` is the XOR of every byte from `STX` to `ETX` inclusive, present only while
  "BCC check" is enabled on the chiller.
* The chiller never speaks first, and says nothing to a request it cannot frame or that
  is addressed to someone else — a missing reply is a timeout, not an error message.

| Identifier | Meaning | Access | Data |
|---|---|---|---|
| `PV1` | measured temperature | read | -1999 to 05000 (tenths of °C) |
| `SV1` | target temperature | read / write | 00100 to 00600 |
| `PVS` | temperature offset | read / write | -0099 to 00099 |
| `STR` | store the set values to EEPROM | write | none |
| `_MD` | operation mode | read / write | 00000 RUN, 00002 STOP (RDY) |
| `_AL` | alarm flags | read | sum of 1 AL0, 2 AL1, 4 AL2 … 128 AL7 |

`_` stands for a space (20h), the way the manual writes it. Errors, in a `NAK` reply:
0 memory error or controller failure, 1 value out of range, 2 no such item, 3 data not
numeric, 4 format, 5 BCC, 6 overrun, 7 framing, 8 parity.

The decoding lives in `src\Protocol.h/.cpp`, which has no Windows code in it.

---

## Build

Visual Studio 2019 or newer. Open `ChillTalker.sln`, or from a command line:

```
msbuild ChillTalker.sln -p:Configuration=Release -p:Platform=x64
```

The executable lands in `bin\x64\Release\ChillTalker.exe`. The projects use the
default toolset of whichever Visual Studio builds them. They were built with Visual
Studio 2026 (v145) in all four configurations (Debug and Release, x86 and x64) and with
2019 (v142) for Release x64, with no warnings at `/W4`.

---

## Using it

```
ChillTalker.exe [COMx] [--baud N] [--data 7|8] [--parity N|E|O] [--stop 1|2]
                [--addr 1..99] [--bcc on|off]
```

Without a port on the command line it lists the ports Windows knows about and asks for
each setting, Enter keeping the value in brackets. The defaults are the chiller's
factory settings, the underlined values in the manual, except the baud rate (9600
there, 19200 here): **19200 baud, 8 data bits, no parity, 2 stop bits, BCC off, address
1**. Set the same on the chiller (its communication setting mode) or it will not
answer. The chiller also ships set to **RS-232C**: to use its RS-485 pins, switch its
communication standard to 485 in that mode first (operation manual, "SET2:
Communication Setting Mode").

```
  1  Read measured temperature      PV1
  2  Read target temperature        SV1
  3  Set target temperature         SV1
  4  Read temperature offset        PVS
  5  Set temperature offset         PVS
  6  Read operation mode            _MD
  7  Set operation mode (RUN/STOP)  _MD
  8  Read alarms                    _AL
  9  Store the set values           STR
  C  Custom message (R/W, identifier, data)
  H  Send raw hex bytes
  W  Scan the bus for Modbus slave ids (all but the chiller's)
  T  Test a Nanotec drive: ask its slave id, turn it for a few seconds
  S  Settings (port, line, address, BCC)
  Q  Quit
```

Setting values outside the documented range is refused; **C** and **H** send anything,
which is how to see the chiller's own `NAK`s. A read looks like this:

```
choice (Enter = menu)> 1
[13:49:50.291] TX  02 30 31 52 50 56 31 03
                   <STX>01RPV1<ETX>  read PV1 (measured temperature) from 01
[13:49:50.323] RX  02 30 31 06 50 56 31 30 30 32 35 30 03
                   <STX>01<ACK>PV100250<ETX>  ACK from 01: PV1 (measured temperature) = 25.0 degC
```

Received frames that are not the answer to the pending request — an echo of the
request from a 2-wire RS-485 adapter, a reply from another address, bytes that arrive
while you are at the prompt — are shown the same way. Bytes that do not form a frame are
shown too, with a hint: for example a BCC setting that disagrees with the chiller's.
Output is coloured (TX cyan, RX green, warnings yellow) when stdout is a console, and a
redirected stdin works as a script. Ctrl+C quits.

**Store (9)** takes about 6 s: the chiller acknowledges only after writing its EEPROM
and must not be powered off before then. Values written over the line are not kept
across a power cycle until a store is sent.

**Wiring** (operation manual, 9-pin D-sub on the chiller): RS-485 on pin 1 (+) and pin 9
(−), RS-232C on pin 2 (RD) and pin 3 (SD), signal ground on pin 5. RS-485 uses a straight
cable, RS-232C a crossed one. The chiller does not talk for about 6 s after power-on.

---

## The Nanotec drives on the same bus

MetalJet's ink-tank stirrers are Nanotec PD1-C drives on the chiller's RS-485 bus. They do
speak **Modbus RTU**, and two menu entries use it. The frames, the CRC, the object numbers
and the power state machine are the ones of Nanotec's Modbus RTU manual and of MetalJet's
`NanotecDevices\NanotecLib.cpp` (`src\Nanotec.h` has the byte layouts).

* **W - scan.** Asks every slave id from 1 to 247 except the chiller's (the address set with
  **S**) for its server id, function 11h, the question MetalJet asks to see whether a drive
  is there. It lists the ids that answer, with the frames. A probe waits 120 ms, so a scan
  of a bus with nobody on it takes about 30 s; **Esc** stops it.
* **T - test.** Asks for a slave id and turns that drive for 5 s at 300 rpm, acceleration
  300, which is what MetalJet runs its stirrers at. Every frame is shown:
  1. report server id, then a read of the status word (6041): the id must be a drive and
     not in the fault state, or nothing is changed;
  2. profile velocity mode (6060 = 3), the ramps (6083, 6084, 6085) and the target
     velocity (60FF);
  3. control word (6040) 06, 07, 0F: shutdown, switch on, enable operation. The status
     word must then say "operation enabled", and the motor turns;
  4. the actual velocity (606C) is read once a second for 5 s;
  5. stop: target velocity 0, wait for standstill, control word 06, which switches the power
     stage off.

  **Esc** skips to the stop, and the stop also runs after a failure once the target speed
  has been sent. The test ends with what happened: the shaft turned (actual velocity above
  10 rpm), or the drive took every command and did not turn. If the drive does not confirm
  the switch-off the line is red: the motor may keep turning until the drive is switched
  off or T is run again.

**One line setting for the whole bus.** A drive answers only at its own line setting. The
PD1-C ships at **19200 baud, 8 data bits, even parity, 1 stop bit**, which is also how
MetalJet opens the drives, so the chiller (its communication setting mode offers 2.4 to
38.4, parity and stop bits) and this tool have to be set the same way, for example
`ChillTalker COM4 --baud 19200 --parity E --stop 1 --bcc off`. When nothing answers, W and
T say what the port is open at.

---

## If the chiller does not answer

A missing reply is how every mismatch shows up, so work down this list:

1. **The chiller's own settings.** Hold **[MODE]** for 2 s, pick **02** (communication
   setting mode) with **[▲]/[▼]**, and step through the items with **[MODE]**:
   communication standard (`232C` or `485`, factory `232C`), the four-digit line
   parameter (read from the right: stop bits 1/2, parity, data length 7/8, BCC check),
   speed (2.4 to 38.4), address (1 to 99) and response delay (0 to 250 ms). Hold
   **[MODE]** for 2 s to leave. Make this tool's settings (**S**) equal to them. The RS-485
   pins do nothing until the standard is `485`; a power cycle may be needed after changing
   it, and the chiller is silent for about 6 s after power-on.
2. **RS-485 wiring.** + on pin 1, - on pin 9, signal ground on pin 5 of the chiller's
   connector. Adapter terminals are labelled A/B or D+/D- differently by different makers,
   so if nothing happens swap the two wires. A 4-wire RS-422/485 adapter used as 2-wire
   RS-485 usually needs its transmit and receive pairs joined (see the adapter's manual).
3. **Try RS-232C first.** It is the factory setting. A crossed cable from pins 2 (RD),
   3 (SD) and 5 (SG) to a PC serial port needs no change on the chiller and proves the
   settings and this tool without RS-485 in the way.
4. **Adapter direction.** An adapter that does not switch between sending and receiving by
   itself will not work here (see below).
5. **BCC.** If the chiller has BCC check on, turn it on here too. **H** sends the request
   with or without the BCC byte, whichever the chiller turns out to want.

---

## Known limits

* **Tried on one real setup only.** It works against a real chiller over RS-485 through an
  ATEN USB-to-RS-422/485 adapter, at 9600 baud, 1 stop bit and BCC check off. So with BCC
  check off the chiller does answer requests that carry no BCC byte, as the tool assumes.
  BCC check on has not been verified on the real chiller. The manual's four worked
  examples are checked byte for byte, and the whole menu against a simulated chiller.
* **W and T have only met a simulated drive.** Their frames equal the worked examples of
  Nanotec's manual byte for byte, and both run against a simulated bus (a drive with a power
  state machine, an echoing adapter, a slow drive, one in fault, one that refuses, one that
  goes silent), but no real drive has been asked yet. Velocity and acceleration are in the
  drive's user units, rpm in the factory setup.
* **RS-485 direction is not controlled.** The port is plain 2-wire half duplex with DTR
  and RTS held asserted; the adapter has to switch between sending and receiving by
  itself (most USB and PCIe RS-485 adapters do, and the ATEN one it was tried with does).
* One request at a time, one thread; a reply that arrives after the timeout is shown but
  not matched to anything.
* ANSI console, ASCII only.

---

## Self-check

`ChillTalker.Tests.exe` (built with the solution) checks the protocol layer: the manual's
examples from section 3.7 (request and reply, read and write, BCC 65h, 06h, 51h, 06h),
data formatting, the NAK and alarm decoding, and the frame assembler (a byte at a time,
two frames in one read, a BCC that equals STX, a cut-short frame, a missing BCC). For the
Nanotec drives it checks the worked examples of Nanotec's N5 Modbus RTU manual (section
8.6: report server id, reading and writing objects, the control word sequence, the error
reply, all with their CRCs), frames of our own, the decoding of the status word and of the
errors, and the Modbus frame assembler (the echo of a request followed by its reply, noise
in front, a cut-short frame). It prints the number of checks and exits non-zero on any
failure.
