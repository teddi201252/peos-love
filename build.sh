#!/usr/bin/env bash
# LÖVE 11.5 for PeOS, and Balatro on it: docs/game-ports-plan.md (section 1).
# The sources are downloaded and checked (SHA-256, or a fixed commit) under
# build/love/ (never in git); the games' files are the user's own and never
# come here (Balatro's from the Steam copy: Balatro.love on a Mac, the zip
# inside Balatro.exe on Windows).
#
#   userland/ports/love/build.sh fetch     LuaJIT, LÖVE 11.5, libogg, libvorbis, PeOS's patches
#   userland/ports/love/build.sh luajit    LuaJIT 2.1 (the JIT on: pages RW then RX, capability "jit")
#   userland/ports/love/build.sh vorbis    libogg and libvorbis
#   userland/ports/love/build.sh love      LÖVE's objects and liblove.a
#   userland/ports/love/build.sh link      OpenAL in the sysroot, the love program
#   userland/ports/love/build.sh app       the bundle build/love/balatro.app (LÖVE and the setup, no game)
#   userland/ports/love/build.sh all       all of it
#   userland/ports/love/build.sh stick <Balatro.love|Balatro.exe>   the user's
#       game on a FAT32 stick image (build/love/stick/disk.img: an MBR, one
#       partition) for make test-balatro; it stays on this machine
#
# Needs: the x86_64-peos toolchain, and in the sysroot Mesa's libGL-peos.a,
# FreeType, zlib and OpenAL (userland/ports/*).
#
# The same script is the public copy's (the peos-love repository, the port's
# files at its top): there PEOS_DIR names a PeOS checkout (by default
# ../peos beside it), whose toolchain and ports it builds with; the work
# goes to LOVE_WORK (by default build/love in that checkout).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "${PEOS_DIR:-}" ]; then
    ROOT="$(cd "$PEOS_DIR" && pwd)"
elif [ -f "$HERE/../../../kernel/main.c" ]; then   # inside PeOS: userland/ports/love
    ROOT="$(cd "$HERE/../../.." && pwd)"
else                                                # the public copy, beside a PeOS checkout
    ROOT="$(cd "$HERE/../peos" 2>/dev/null && pwd)" ||
        { echo "love: no PeOS checkout: set PEOS_DIR"; exit 1; }
fi
[ -f "$ROOT/kernel/main.c" ] || { echo "love: $ROOT is not a PeOS checkout (PEOS_DIR)"; exit 1; }
W="${LOVE_WORK:-$ROOT/build/love}"
PREFIX="$ROOT/build/toolchain/peos"
SYSROOT="$ROOT/build/peos-sysroot"
TARGET=x86_64-peos
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

LUAJIT_URL=https://github.com/LuaJIT/LuaJIT.git
LUAJIT_COMMIT=63c8f1a9b479ba5518cf6a170ff7729178bc3cfd   # v2.1, 2026-11-11
LOVE=love-11.5
LOVE_SHA256=066e0843f71aa9fd28b8eaf27d41abb74bfaef7556153ac2e3cf08eafc874c39   # GitHub's release
OGG=libogg-1.3.5
OGG_SHA256=c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705
VORBIS=libvorbis-1.3.7
VORBIS_SHA256=b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b

sha256_check() {
    if command -v shasum >/dev/null; then shasum -a 256 -c -; else sha256sum -c -; fi
}

fetch_one() {   # url sha256: downloaded whole or not at all, dropped if it does not match
    local f; f="$(basename "$1")"
    [ -f "$W/dl/$f" ] || { curl -sSLf "$1" -o "$W/dl/$f.part" && mv "$W/dl/$f.part" "$W/dl/$f"; }
    echo "$2  $W/dl/$f" | sha256_check >/dev/null || { rm -f "$W/dl/$f"; echo "love: $f: wrong SHA-256 (removed)"; exit 1; }
}

unpack() {   # tarball dir patches: a clean tree with PeOS's patches
    rm -rf "$W/$2"
    tar -xf "$W/dl/$1" -C "$W"
    if [ -d "$3" ]; then
        for p in "$3"/*.patch; do
            [ -f "$p" ] && { patch -d "$W/$2" -p1 --quiet < "$p" || { echo "love: $p does not apply"; exit 1; }; }
        done
    fi
}

fetch() {
    mkdir -p "$W/dl"
    fetch_one "https://github.com/love2d/love/releases/download/${LOVE#love-}/$LOVE-linux-src.tar.gz" "$LOVE_SHA256"
    fetch_one "https://downloads.xiph.org/releases/ogg/$OGG.tar.xz" "$OGG_SHA256"
    fetch_one "https://downloads.xiph.org/releases/vorbis/$VORBIS.tar.xz" "$VORBIS_SHA256"
    [ -d "$W/luajit/.git" ] || git clone -q "$LUAJIT_URL" "$W/luajit"
    git -C "$W/luajit" cat-file -e "$LUAJIT_COMMIT^{commit}" 2>/dev/null || git -C "$W/luajit" fetch -q origin
    git -C "$W/luajit" -c advice.detachedHead=false checkout -q --detach "$LUAJIT_COMMIT"
    git -C "$W/luajit" reset -q --hard && git -C "$W/luajit" clean -qfdx
    for p in "$HERE"/patches/luajit/*.patch; do git -C "$W/luajit" apply "$p" || { echo "love: $p does not apply"; exit 1; }; done
    unpack "$LOVE-linux-src.tar.gz" "$LOVE" "$HERE/patches/love"
    unpack "$OGG.tar.xz" "$OGG" ""
    unpack "$VORBIS.tar.xz" "$VORBIS" ""
    echo "love: sources in $W (LuaJIT ${LUAJIT_COMMIT:0:12}, $LOVE, $OGG, $VORBIS)"
}

luajit_make() {
    make -C "$W/luajit/src" HOST_CC="cc" CROSS="$PREFIX/bin/$TARGET-" TARGET_SYS=PeOS BUILDMODE=static \
        XCFLAGS="-DLUAJIT_DISABLE_PROFILE" TARGET_CFLAGS="-funwind-tables -I$SYSROOT/usr/include" "$@"
}

luajit() {   # the library for LÖVE, and the luajit program (make test-luajit)
    luajit_make clean >/dev/null
    luajit_make -j"$JOBS" libluajit.a >/dev/null
    luajit_make luajit >/dev/null
    echo "love: LuaJIT built ($W/luajit/src/libluajit.a, luajit)"
}

vorbis() {   # libogg and libvorbis (libvorbisfile in it), C only
    local out="$W/vorbis-peos" cc="$PREFIX/bin/$TARGET-gcc"
    rm -rf "$out" && mkdir -p "$out/obj" "$out/include/ogg"
    cat > "$out/include/ogg/config_types.h" <<'EOF'
#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#include <stdint.h>
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#endif
EOF
    cp "$W/$OGG"/include/ogg/ogg.h "$W/$OGG"/include/ogg/os_types.h "$out/include/ogg/"
    cp -R "$W/$VORBIS/include/vorbis" "$out/include/"
    local f
    for f in "$W/$OGG"/src/{bitwise,framing}.c; do
        "$cc" -O2 -I"$out/include" -c "$f" -o "$out/obj/ogg_$(basename "$f" .c).o"
    done
    for f in "$W/$VORBIS"/lib/*.c; do
        case "$(basename "$f")" in barkmel.c|psytune.c|tone.c|misc.c) continue ;; esac
        "$cc" -O2 -w -I"$out/include" -I"$W/$VORBIS/lib" -c "$f" -o "$out/obj/vorbis_$(basename "$f" .c).o"
    done
    rm -f "$out/libvorbis-all.a"
    "$PREFIX/bin/$TARGET-ar" rcs "$out/libvorbis-all.a" "$out"/obj/*.o
    echo "love: libogg and libvorbis ($out/libvorbis-all.a)"
}

love() {   # LÖVE's sources with PeOS's config.h: what Balatro needs (no video decoding, physics, enet, luasocket)
    local src="$W/$LOVE" out="$W/love-peos" cxx="$PREFIX/bin/$TARGET-g++" cc="$PREFIX/bin/$TARGET-gcc"
    mkdir -p "$out/obj"
    cp "$HERE/config.h" "$src/config.h"
    local inc=(-DHAVE_CONFIG_H -I"$src/src" -I"$src/src/modules" -I"$src/src/libraries" -I"$src/src/libraries/physfs"
        -I"$src/src/libraries/glslang" -I"$W/luajit/src" -I"$SYSROOT/usr/include/SDL2"
        -I"$W/vorbis-peos/include" -I"$SYSROOT/usr/include/freetype2" -I"$SYSROOT/usr/include"
        -I"$ROOT/userland/ports/openal/include")
    local files
    files="$(cd "$src/src" && find common modules libraries love.cpp -name '*.cpp' -o -name '*.c' | sort |
        grep -v -e '^modules/video/theora/' -e '^modules/video/wrap_Video\.cpp' -e '^modules/physics/' -e '^libraries/Box2D/' -e '^libraries/enet/' \
            -e '^libraries/luasocket/' -e '^common/android' -e '^common/ios' -e '^common/macosx' \
            -e 'glslang/OSDependent/Windows' -e '^libraries/tinyexr/.*example')"
    : > "$out/errors.log"
    printf '%s\n' $files | xargs -P "$JOBS" -I{} sh -c '
        f="$1"; shift; o="$OUT/obj/$(echo "$f" | tr / _).o"
        [ "$o" -nt "$SRC/src/$f" ] && [ "$o" -nt "$SRC/config.h" ] && exit 0
        case "$f" in
            *.c) "$CC" -O2 -w "$@" -c "$SRC/src/$f" -o "$o" ;;
            *) "$CXX" -std=gnu++14 -O2 -w -fpermissive "$@" -c "$SRC/src/$f" -o "$o" ;;
        esac 2>>"$OUT/errors.log" || echo "FAILED $f" >> "$OUT/errors.log"
    ' _ {} "${inc[@]}"
    if grep -q '^FAILED' "$out/errors.log"; then
        grep -c '^FAILED' "$out/errors.log" | xargs echo "love: objects that do not compile:"
        grep '^FAILED' "$out/errors.log" | head -20
        exit 1
    fi
    rm -f "$out/liblove.a"   # main (love.cpp) stays out: link puts it first
    "$PREFIX/bin/$TARGET-ar" rcs "$out/liblove.a" $(ls "$out"/obj/*.o | grep -v '/love\.cpp\.o$')
    echo "love: $(ls "$out"/obj | wc -l | tr -d ' ') objects, $out/liblove.a"
}

link() {   # the love program: LÖVE, LuaJIT, Vorbis, SDL2 (userland/ports/sdl2), OpenAL, Mesa
    local out="$W/love-peos" cc="$PREFIX/bin/$TARGET-gcc"
    # OpenAL 1.1 on PeOS (userland/ports/openal), the sysroot's copy brought up to date
    rm -rf "$SYSROOT/usr/include/AL" && cp -R "$ROOT/userland/ports/openal/include/AL" "$SYSROOT/usr/include/"
    "$cc" -O2 -I"$SYSROOT/usr/include" -c "$ROOT/userland/ports/openal/al_peos.c" -o "$out/al_peos.o"
    rm -f "$SYSROOT/usr/lib/libopenal.a" && "$PREFIX/bin/$TARGET-ar" rcs "$SYSROOT/usr/lib/libopenal.a" "$out/al_peos.o"
    [ -f "$SYSROOT/usr/lib/libSDL2.a" ] || { echo "love: no SDL2 in the sysroot: userland/ports/sdl2/build.sh"; exit 1; }
    # -u peos_gl_create: SDL2's video driver reaches Mesa through weak
    # symbols; LÖVE draws with OpenGL, so Mesa comes in
    "$PREFIX/bin/$TARGET-g++" -o "$out/love.elf" "$out/obj/love.cpp.o" -Wl,-u,peos_gl_create \
        -Wl,--whole-archive "$out/liblove.a" -Wl,--no-whole-archive \
        "$W/luajit/src/libluajit.a" "$W/vorbis-peos/libvorbis-all.a" \
        -lSDL2 -lopenal -lfreetype -lGL-peos -ldrm -lz -lpthread -lm
    "$PREFIX/bin/$TARGET-strip" -o "$out/love" "$out/love.elf"
    echo "love: $out/love ($(du -h "$out/love" | cut -f1 | tr -d ' '))"
}

app() {   # balatro.app: LÖVE, the setup that copies the user's game, the texts and the icon
    local app="$W/balatro.app"
    [ -f "$W/love-peos/love" ] || { echo "love: no $W/love-peos/love (build.sh link)"; exit 1; }
    rm -rf "$app" && mkdir -p "$app/setup"
    cp "$W/love-peos/love" "$app/app.bin"
    cp "$HERE/balatro/app.ini" "$app/app.ini"
    cp "$HERE"/balatro/setup/*.lua "$app/setup/"
    cp -R "$HERE/balatro/resources" "$app/resources"
    find "$app" -name .DS_Store -delete
    echo "love: $app ($(du -sh "$app" | cut -f1 | tr -d ' '))"
}

stick() {   # the user's game on a stick image, never anywhere else (build/ is not in git)
    local src="${1:?Balatro.love or Balatro.exe}" out="$W/stick" part=1048576 mb
    [ -f "$src" ] || { echo "love: no $src"; exit 1; }
    command -v mkfs.fat >/dev/null && command -v mcopy >/dev/null ||
        { echo "love: stick needs mkfs.fat and mcopy (dosfstools, mtools)"; exit 1; }
    mkdir -p "$out"
    mb=$(( $(du -sm "$src" | cut -f1) + 64 ))
    rm -f "$out/disk.img" "$out/part.img"
    dd if=/dev/zero of="$out/part.img" bs=1048576 count=0 seek="$mb" 2>/dev/null   # 1m is macOS's only, 1M Linux's only
    mkfs.fat -F 32 -h $((part / 512)) -n GAMES "$out/part.img" >/dev/null
    mcopy -i "$out/part.img" "$src" "::/$(basename "$src")"
    dd if=/dev/zero of="$out/disk.img" bs="$part" count=1 2>/dev/null
    cat "$out/part.img" >> "$out/disk.img"
    rm -f "$out/part.img"
    python3 - "$out/disk.img" "$part" <<'PY'
import struct, sys
path, start = sys.argv[1], int(sys.argv[2])
with open(path, "r+b") as f:
    f.seek(0, 2)
    sectors = (f.tell() - start) // 512
    entry = bytes([0x00, 0xFE, 0xFF, 0xFF, 0x0C, 0xFE, 0xFF, 0xFF]) + struct.pack("<II", start // 512, sectors)
    f.seek(446)
    f.write(entry + bytes(48) + b"\x55\xaa")
PY
    echo "love: $out/disk.img ($(basename "$src") on a FAT32 stick)"
}

export OUT="$W/love-peos" SRC="$W/$LOVE" CC="$PREFIX/bin/$TARGET-gcc" CXX="$PREFIX/bin/$TARGET-g++"
case "${1:-all}" in
    fetch) fetch ;;
    luajit) luajit ;;
    vorbis) vorbis ;;
    love) love ;;
    link) link ;;
    app) app ;;
    stick) stick "${2:-}" ;;
    all) fetch; luajit; vorbis; love; link; app ;;
    *) echo "usage: $0 fetch|luajit|vorbis|love|link|app|all|stick <Balatro.love|Balatro.exe>"; exit 2 ;;
esac
