# Talking to the board over serial

The AtomS3R has **no USB-serial bridge**. It enumerates over the ESP32-S3's
built-in **USB-Serial-JTAG** peripheral as `/dev/cu.usbmodem*`. That changes how
the serial port behaves in ways that cost real time during bring-up, so they are
collected here.

Applies to any ESP32-S3/C3/C6/H2 board wired for native USB rather than a CP2102
or CH9102 — not just this one.

## TL;DR

| I want to… | Do this |
|---|---|
| watch the log | `make monitor` |
| **type a console command** (`start`, `vol +8`, `wifi …`) | `make monitor`, **by hand** — cannot be scripted |
| capture a log to a file | `script -q /dev/null bash -c 'timeout 120 make monitor' > log.txt` |
| flash | `make flash` (esptool handles the reset dance itself) |

## The console cannot be driven programmatically

Two independent blockers, either of which alone is fatal:

**1. Opening the port resets the chip.** macOS asserts DTR/RTS when a serial port
is opened, and on USB-Serial-JTAG that combination *is* the reset strobe. The
chip reboots and re-enumerates on USB, which invalidates the file descriptor you
just opened — reads die mid-session with:

```
serial.serialutil.SerialException: read failed: [Errno 6] Device not configured
```

Setting `dtr=False` / `rts=False` before `open()` does **not** prevent it; pyserial
applies line state after the OS has already opened the tty. The practical
consequence is that anything written shortly after opening lands in the
*bootloader*, not the REPL — the command is silently swallowed and you see no
echo and no error.

`stty -hupcl` does not help either, and if the device happens to be
re-enumerating at that moment you get `stty: No such file or directory` on a port
that existed a second ago.

**2. `idf.py monitor` ignores piped stdin.** It *does* handle the reset and
re-enumeration correctly — that is why it is the right tool for reading — but it
only forwards keystrokes from a real interactive terminal. Piping input:

```bash
( sleep 14; printf 'start\r\n' ) | script -q /dev/null bash -c 'make monitor'   # does nothing
```

…produces no echo and no effect. The command never reaches the device.

**So: sending a console command requires a human at an interactive
`make monitor`.** Automate the *reading*, not the *typing*. Everything the
console exposes is also on the front button (see the README's gesture table),
which is usually the easier path anyway.

## Capturing a log to a file

`make monitor` needs a pty, so redirecting it directly gives nothing useful.
Wrap it in `script`:

```bash
script -q /dev/null bash -c 'timeout 120 make monitor' > /tmp/board.log 2>&1
```

Then grep for the per-call markers listed in [`PORTING.md`](PORTING.md#markers-for-diagnosing-a-silent-call).
The log carries ANSI colour codes, so use `grep -a` and expect `[0;32m` noise.

Note this *also* resets the board when the port opens, so the capture always
starts from a fresh boot. That is usually convenient, but it means you cannot
attach to an already-running session to catch a state you got the device into by
hand — reproduce the sequence after the capture starts.

## Port auto-detect can pick the wrong device

`usbmodem` is not unique to ESP32s. On this desk an LG monitor enumerates as
`/dev/cu.usbmodem412NTVS6R4632`, right next to the board's
`/dev/cu.usbmodem1101`. The `Makefile` globs `/dev/cu.usbmodem*` first (correct
for this board, since it has no USB-serial bridge), so it can grab the monitor.

Identify the board by elimination:

```bash
ls /dev/cu.usbmodem*     # unplugged
ls /dev/cu.usbmodem*     # plugged — the new entry is the board
```

or ask the chip directly, which also confirms you have an AtomS3R and not a
PSRAM-less AtomS3:

```bash
esptool.py --port /dev/cu.usbmodem1101 chip_id
# Chip is ESP32-S3-PICO-1 (LGA56) (revision v0.2)
# Features: WiFi, BLE, Embedded Flash 8MB (GD), Embedded PSRAM 8MB (AP_3v3)
```

Override when needed: `make PORT=/dev/cu.usbmodem1101 run`.

## The board can vanish from `/dev` entirely

It happened once mid-session: no `/dev/cu.usbmodem1101`, and no Espressif entry
in `ioreg -p IOUSB -w0 -l | grep Espressif`. Reseating the cable brought it back
with no lasting damage.

Worth knowing that this board is often plugged into a monitor's or dock's USB
hub, and a hub or monitor power event drops it. Before debugging firmware,
confirm the device is actually enumerated:

```bash
ioreg -p IOUSB -w0 -l | grep -A3 Espressif    # nothing = it is not on the bus
```

## Silence is not a crash

The firmware only logs on events. Once it prints
`WiFi connected — tap the button to start a call`, an idle board emits **nothing**
until you press the button. A capture that returns zero bytes usually means a
healthy, idle device — not a hang. Reset it (unplug/replug, or reopen the port)
if you want to see the boot banner again.
