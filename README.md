# bspwmbar

A lightweight status bar for bspwm.

![bspwmbar.png](docs/bspwmbar.png)

## Features and TODO

- [x] Support multiple monitors (Xrandr)
- [x] Render text
- [x] Bspwm workspaces
- [x] Active window title
- [x] Datetime
- [x] CPU temperature
- [x] Disk usage
- [x] ALSA volume
- [x] Memory usage
- [x] CPU usage per core
- [x] Implements clickable label
- [x] System Tray support
- [ ] Pulseaudio support
- [ ] GitHub notification
- [ ] Refactor code
- [ ] Decrease memory usage
- [ ] BSD support

## Configure

Modify and recompile `config.h` like `dwm`, `st`.

## Build & Debug

```sh
# or `make optimized`
make

# debug build with AddressSanitizer
make debug

# static analyze with clang
scan-build make debug
```
