# Logitech WingMan Force J-UA9 driver for Windows 11

This is a custom KMDF USB function driver plus a user-mode bridge that lets a
Logitech WingMan Force J-UA9 joystick (`USB\VID_046D&PID_C281`) work on
Windows 11, replacing the original Windows XP-era driver. It was
reverse-engineered against the Linux kernel's `iforce` driver
(`drivers/input/joystick/iforce/`), since Immersion/Logitech never released a
protocol spec and no modern Windows driver exists for this device.

## Status

- **Input (axes/buttons/POV hat) is fully working** end-to-end: KMDF driver
  reads the device's interrupt IN reports and a bridge app forwards them to a
  [vJoy](https://sourceforge.net/projects/vjoystick/) virtual device, so any
  Windows game/app that reads vJoy sees the stick normally.
- **Driver load/unload/hot-unplug is stable.** Earlier crashes on device
  replacement/removal were fixed by using
  `WdfUsbTargetPipeConfigContinuousReader` and cleaning up pending I/O
  correctly.
- **Force feedback (rumble & autocenter) is working.** The interrupt-OUT
  command protocol was verified against raw hardware USB traffic captured
  from the original Windows XP Logitech driver:
  - Startup handshake: EP0 queries `'N'` (`4E 0A`), `'B'` (`42 C8 00`)
  - Profile setup: `40 05 00 04` on EP1 (sets hardware spring/autocenter)
  - Motor enable: `42 01` on EP1 (engages H-bridge motor drivers)
  - Hardware optical grip sensor: reports `02 03` when hand is detected on
    grip, allowing motors to safely fire.
  - Custom effect upload and playback: tested and verified via `JUa9Bridge.exe --rumble`
    and `JUa9Bridge.exe --autocenter`.

## Project layout

- `JUa9TestDriver.c` / `JUa9TestDriver.inf` / `JUa9TestDriver.vcxproj` — the
  KMDF USB function driver. Selects USB configuration 1, opens interrupt IN
  endpoint `0x82` and interrupt OUT endpoint `0x01`, and exposes a device
  interface with IOCTLs for reading the last report, sending a raw I-Force
  command frame, and issuing vendor "query ID" control transfers.
- `protocol.c` / `protocol.h` — shared frame/command definitions.
- `JUa9Bridge.cpp` / `JUa9Bridge.vcxproj` — a user-mode console app with two
  modes:
  - Default: reads reports from the driver and publishes axes/buttons to a
    vJoy device.
  - `--rumble`: a guarded diagnostic that walks through the full I-Force
    force-feedback command sequence (enable, gain, autocenter, effect
    upload, play, stop, disable) for manual testing.

## Protocol notes (I-Force over USB)

The J-UA9 uses Immersion's I-Force command protocol encapsulated directly in
USB interrupt packets (not USB HID PID). The canonical reference is the Linux
kernel's `iforce-usb.c` / `iforce-packets.c` / `iforce-main.c` /
`iforce-ff.c`, which explicitly lists this device
(`{ 0x046d, 0xc281, "Logitech WingMan Force", ... }`).

Key details validated against a real device:

- USB packets on the wire are `[opcode byte][payload bytes...]` — there is
  **no length byte and no checksum** on the native USB transport (the
  serial/RS-232 variant of I-Force used a different, checksummed framing;
  the USB variant does not).
- Device identification uses vendor control transfers
  (`bRequest = 'O' | 'M' | 'P' | 'B' | 'N'`, `IN`, recipient = interface) that
  return ready state, vendor ID, product ID, effect-memory size, and
  effect-slot count respectively. These have been confirmed to return this
  device's real vendor/product ID (`046D:C281`), 200 bytes of effect memory,
  and 10 effect slots.
- Force-feedback enable/disable, gain, autocenter, and effect
  upload/play/stop packet formats all match `iforce_send_packet` call sites
  in the Linux source exactly (opcodes `0x01`–`0x05`, `0x40`–`0x43`).

## Build prerequisites

- Visual Studio with the Desktop C++ workload
- Windows 11 SDK
- Windows Driver Kit matching the SDK
- [vJoy](https://sourceforge.net/projects/vjoystick/) installed (for the
  input bridge)

Open `JUa9TestDriver.vcxproj` in Visual Studio, select `Debug|x64`, and
build. The resulting package must be test-signed before installation. Do not
enable test signing on a production machine.

## Install for development

Use an elevated command prompt on a test installation:

```text
bcdedit /set testsigning on
```

After reboot, install `JUa9TestDriver.inf` through Device Manager using
**Have Disk**, or via:

```text
pnputil /add-driver JUa9TestDriver.inf /install
```

Revert test signing when finished:

```text
bcdedit /set testsigning off
```

## Running the input bridge

`JUa9Bridge.exe` reads reports from the driver's device interface and
publishes axes/buttons to vJoy device 1. Configure one vJoy device with at
least X, Y, throttle, rudder, and 8 buttons before running it.

To run the guarded force-feedback diagnostic instead, stop the normal bridge
first and run:

```text
JUa9Bridge.exe --rumble
```

