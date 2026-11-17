# Building LÖVE and balatro.app

The port builds against a **PeOS checkout**: its cross toolchain
(`x86_64-peos-gcc`), its C library and the libraries already ported there
(Mesa for OpenGL, FreeType, zlib, OpenAL).

**PeOS's own repository is not public yet.** The C library and its
headers are, in the [Starlight App Kit](https://github.com/teddi201252/starlight-app-kit)
(`sdk/`), and so is the toolchain's recipe (in
[peos-d2d](https://github.com/teddi201252/peos-d2d), `toolchain/`); the
ported libraries this port links (Mesa, FreeType, OpenAL) will come with
the kit too ("the ports' SDK", planned). Until then this repository is
mostly to read: how a Linux program with SDL2, OpenGL, OpenAL and a JIT
becomes a PeOS app. With a PeOS checkout:

## 1. PeOS

```
cd peos
make                                # the toolchain, the kernel and the system
bash userland/ports/mesa/build.sh   # Mesa (OpenGL), FreeType and the sysroot
```

Inside PeOS this port is `userland/ports/love`. This repository has the same
files at its top: next to a PeOS checkout (`../peos`), or with `PEOS_DIR`:

```
PEOS_DIR=/path/to/peos ./build.sh
```

## 2. LÖVE

```
./build.sh fetch    # LuaJIT (fixed commit), LÖVE 11.5, libogg, libvorbis, SDL2's headers; patched
./build.sh luajit   # LuaJIT 2.1, static, the JIT on
./build.sh vorbis   # libogg and libvorbis
./build.sh love     # LÖVE's 258 objects: liblove.a
./build.sh link     # SDL2 on PeOS, OpenAL into the sysroot, the program: love
./build.sh app      # balatro.app (LÖVE and its setup; no game in it)
```

or `./build.sh` for all of them. The work goes to `build/love` in the PeOS
checkout (`LOVE_WORK` to choose another folder); nothing is written here.

## 3. Trying it

In PeOS (`make` in its folder; QEMU with virgl comes from UTM: see PeOS's
`AGENTS.md`):

```
make test-luajit    # LuaJIT: the same work with and without the JIT
make test-love      # tests/lovecheck: shader, canvas, font, threads, saves, sound, Ogg
./build.sh stick /path/to/Balatro.love
make test-balatro   # balatro.app from the dock: the copy from the stick, then the game
```

`stick` puts your game on a FAT32 image in `build/love/stick`, which stays
on your computer: it is never in git, nor in an image or a package.

## Changing a patch

`fetch` unpacks a clean LÖVE into `build/love/love-11.5` and applies
`patches/love/*.patch`. To change one, keep a clean copy, edit, and write
the patch again:

```
cp -R build/love/love-11.5 build/love/love-11.5.orig   # right after fetch
# ... edit build/love/love-11.5/src ...
cd build/love && diff -ruN love-11.5.orig/src love-11.5/src | \
  sed 's|^--- love-11.5.orig/|--- a/|; s|^+++ love-11.5/|+++ b/|' > /path/to/patches/love/0001-peos-platform.patch
```
