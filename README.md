# bspwmbar

![AUR Version](https://img.shields.io/aur/version/bspwmbar.svg)
![AUR License](https://img.shields.io/aur/license/bspwmbar.svg)

A lightweight status bar for bspwm.

Currently required [nerd-fonts](https://github.com/ryanoasis/nerd-fonts)
([AUR](https://aur.archlinux.org/packages/ttf-nerd-fonts-symbols/))

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
- [x] Backlight
- [x] Battery (Colorized)
- [x] Wifi (Used code from yabar)
- [x] Implements clickable label
- [x] System Tray support
- [x] Refactor code
- [x] Refactor code 2 (tabs replaced by spaces)
- [x] Restyled
- [x] OpenBSD support
- [ ] FreeBSD support
- [ ] Pulseaudio support (just enough only ALSA support?)
- [ ] Decrease memory usage

## Configure

Modify and recompile `config.h` like `dwm`, `st`.

## Install

You can install from [AUR](https://aur.archlinux.org/packages/bspwmbar/) on Arch Linux.

Or build and install by using `make` and `make install`.

## Build & Debug

```sh
./configure
make

# debug build with AddressSanitizer
make debug

# static analyze with clang
scan-build make debug
```
