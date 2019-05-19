#!/bin/sh

PKGCONFIG='pkg-config'
DEPS='x11 xft xrandr xext fontconfig'
MODS='cpu memory disk thermal datetime'

DCFLAGS="${CFLAGS:-} -g"
DLDFLAGS="${LDFLAGS}"
# release flags
RCFLAGS="${CFLAGS:--Os -DNDEBUG}"
RLDFLAGS="${LDFLAGS:--s}"

case $(uname -s) in
  Linux)
    DEPS="${DEPS} alsa"
    MODS="${MODS} alsa"
    DCFLAGS="${DCFLAGS} -fsanitize=address -fno-omit-frame-pointer"
    DLDFLAGS="${DLDFLAGS} -fsanitize=address"
    ;;
  OpenBSD)
    MODS="${MODS} volume"
    ;;
esac

if [ "${1:-}" = 'debug' ]; then
  CFLAGS="${DCFLAGS}"
  LDFLAGS="${DLDFLAGS}"
else
  CFLAGS="${RCFLAGS}"
  LDFLAGS="${RLDFLAGS}"
fi

CFLAGS="$("${PKGCONFIG}" --cflags "${DEPS}") ${CFLAGS}"
LDFLAGS="$("${PKGCONFIG}" --libs "${DEPS}") ${LDFLAGS}"

export CFLAGS LDFLAGS MODS
make
