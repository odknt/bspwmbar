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

## Install

You can install from [AUR](https://aur.archlinux.org/packages/bspwmbar/) on Arch Linux.

Or build and install by using `make` and `make install`.

## Build & Debug

```sh
# or `make optimized`
make

# debug build with AddressSanitizer
make debug

# static analyze with clang
scan-build make debug
```
