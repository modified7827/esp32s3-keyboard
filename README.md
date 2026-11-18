# ESP32-S3 Keyboard Emulator

Turn an ESP32-S3 into a **USB flash drive + keyboard** combo device.
Drop a `payload.txt` onto the drive, press the onboard **BOOT** button,
and it types the file's contents as keystrokes — one character at a time.

- Nothing happens on plug-in: it just enumerates as a disk. Typing only
  starts when you press BOOT.
- The payload is re-read before every run — edit `payload.txt`, hit save,
  press BOOT. No re-flashing, no unplugging.
- Works over USB OTG on Android too: edit the file in a file manager,
  focus an input field, press BOOT.

## Markup syntax

Plain ASCII text is typed as-is. `{...}` markers fire special keys
(case-insensitive):

| Marker | What it does |
|---|---|
| `{ENTER}` `{TAB}` `{ESC}` `{SPACE}` | Common keys |
| `{BACKSPACE}` `{DEL}` `{INS}` `{HOME}` `{END}` `{PGUP}` `{PGDN}` | Editing keys |
| `{UP}` `{DOWN}` `{LEFT}` `{RIGHT}` | Arrow keys |
| `{F1}`…`{F24}` `{CAPS}` `{NUM}` `{PRTSC}` `{PAUSE}` | Function keys |
| `{CTRL}` `{ALT}` `{SHIFT}` `{GUI}` | Modifiers on their own |
| `{CTRL+C}` `{ALT+F4}` `{CTRL+ALT+DEL}` `{GUI+R}` `{SHIFT+TAB}` | Combos, joined with `+` |
| `{DELAY 1000}` or `{DELAY=1000}` | Wait 1000 ms |
| `{{` `}}` | Literal braces |

A few rules:

- Newlines in the file are typed as Enter.
- **ASCII only** — the HID keyboard protocol can't send Unicode, so
  non-ASCII characters (e.g. CJK) are skipped.
- Unknown `{...}` markers are typed literally, braces included.
- Max payload size is ~60 KB.

## Example

```
{DELAY 1000}sh /sdcard/start.sh{ENTER}
```

Plug into a phone over OTG → focus a terminal input field → press BOOT:
waits one second, types the command, hits Enter.

## Status LED (GPIO48 onboard LED)

| Pattern | Meaning |
|---|---|
| 2 blinks at boot | Initialized OK |
| Solid on | Typing in progress |
| Off | Idle |

## Build & flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash
```

**Heads-up:** once flashed, the USB serial console is gone (the port
becomes the composite USB device). To re-flash, enter download mode
manually:

> Hold BOOT → tap RST → release BOOT → the COM port reappears, then flash.

If the board seems stuck in download mode (`waiting for download`),
tap RST once.

## Files

```
main/main.c            Everything: HID descriptor, ASCII→HID keymap,
                       {} marker parser, MSC storage, BOOT trigger
main/Kconfig.projbuild BOOT pin / key timing / LED pin (configurable)
main/payload.txt       Syntax demo (the firmware actually reads the
                       payload.txt on the USB drive, not this file)
partitions.csv         factory 1M + FAT storage 2M
sdkconfig.defaults     UART0 console + 4MB flash + TinyUSB HID/MSC
```

## Disclaimer

This project is published for learning and technical research
(a TinyUSB HID/MSC composite-device example).

- By design it does **not** type on plug-in — it only fires when you
  physically press the BOOT button.
- Use it only on devices and hosts you own or are explicitly
  authorized to use.
- Do not use it for unauthorized keystroke injection or anything
  unlawful in your jurisdiction.

The software is provided "as is" (see [LICENSE](LICENSE)). The authors
accept no liability for any consequences of using or misusing it; you
are solely responsible for what you do with it.
