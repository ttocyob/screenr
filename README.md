# Screenr

**Screenr** — an EFL screen recorder for X11, with a live drag-to-select region.

![Screenr screenshot](https://github.com/user-attachments/assets/f4507d7a-1637-4531-a725-fdecd014ff47)

---

Screenr is a small, focused screen recorder built on the Enlightenment Foundation Libraries. Record your whole screen, or drag out a specific region on a live desktop screenshot before you start — either way, it stays out of your way: a compact control window with a timer, a record button, and audio/cursor toggles.

---

## Features

- Two recording modes: full screen, or a drag-to-select region
- Selection mode shows a real screenshot of your desktop to drag against, with resizable corner handles
- Live dimensions readout while dragging, and in the window title
- Audio (PulseAudio) and mouse cursor toggles, on by default for cursor, off by default for audio
- `Tab` toggles between the two modes; `Esc` backs out of an open selection window
- Output goes to `~/Videos/screenr/<timestamp>.webm`, never overwrites a previous recording

## Dependencies

- [EFL](https://www.enlightenment.org) — Elementary, Evas, Edje, Ecore, Ecore-X, Ecore-Evas, Eina, Eet
- [Meson](https://mesonbuild.com) build system
- [ffmpeg](https://ffmpeg.org)
- PulseAudio, if you want the audio toggle to do anything

## Building

```bash
meson setup build
ninja -C build
sudo ninja -C build install
```

## Usage

```bash
screenr
```

Screenr opens in Screen mode by default. Press the record button to start recording the full desktop.

### Selection mode

Click the selection-mode icon (or press `Tab`), then press record. A window opens showing a still screenshot of your desktop — drag the corner handles to size your region, or drag inside the fill to move it. Press **Done** to close the window and start recording that exact region immediately.

`Esc`, or the window's own close button, backs out of Selection mode without recording.

### Keybindings

| Key | Action |
|---|---|
| `Tab` | Toggle Screen / Selection mode (when Screenr's main window has focus) |
| `Esc` | Close the Selection window without recording |

### From the file manager or desktop

`ninja install` installs the `.desktop` file alongside the binary, so Screenr shows up in your Applications menu automatically.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
