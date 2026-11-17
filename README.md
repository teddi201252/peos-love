# LÖVE on PeOS (and Balatro)

This is the port of [LÖVE 11.5](https://love2d.org), the 2D game engine in
Lua, to [PeOS](https://github.com/teddi201252): the code that turns LÖVE
into a PeOS program, and **balatro.app**, the app that plays *Balatro* with
**your own** copy of the game.

It is also meant as an example: how a program written for Linux, with SDL2,
OpenGL, OpenAL and a JIT compiler, becomes a PeOS app. If you want to port
something yourself, the files here are small and commented.

| I want to... | Go to |
| --- | --- |
| **Play** Balatro on PeOS | [Play Balatro](#play-balatro) |
| **Rebuild** LÖVE and balatro.app | [docs/BUILDING.md](docs/BUILDING.md) (needs a PeOS checkout: not public yet) |
| **Port** a program of my own | [How the port is made](#how-the-port-is-made) |

## Play Balatro

You need:
- PeOS **0.18.0** or later, with OpenGL: a MacBook or PC with an Intel GPU,
  or a virtual machine with a 3D GPU (UTM with `virtio-vga-gl`, its
  renderer on "Apple Core OpenGL");
- **Balatro**, bought on Steam. Nothing of the game comes with this
  repository: you give PeOS your copy.

1. Find the game's file on your computer:
   - **macOS**: `Balatro.love`, inside the app: in Steam, right-click
     Balatro, *Manage*, *Browse local files*; then right-click `Balatro.app`,
     *Show Package Contents*, `Contents/Resources/Balatro.love`;
   - **Windows**: `Balatro.exe`, in the same *Browse local files* folder.
2. Copy it to a USB stick (FAT32 or exFAT), or into `/home` on PeOS.
3. In PeOS, open **Balatro**. The first time it looks for the file on the
   stick (PeOS mounts it on `/media1` by itself), copies the game (only the
   game's own zip, about 56 MB) and starts it. From then on Balatro starts
   straight away; the stick is no longer needed.

Your saves are in `/starlight/config/love/Balatro`, next to the copy.

## How the port is made

| File or folder | What it is |
| --- | --- |
| `build.sh` | downloads LÖVE, LuaJIT, libogg, libvorbis and SDL2's headers (checked by SHA-256 or a fixed commit), patches and builds them |
| `config.h` | LÖVE's modules for PeOS: all but video (Theora), physics (Box2D) and the network from Lua |
| `patches/luajit/` | LuaJIT knows PeOS as a POSIX-like system; its random seed from `getentropy` |
| `patches/love/` | LÖVE knows PeOS: its name, where saves go, modules left out load as nothing, `--first` (below) |
| `sdl2/sdl2_peos.c` | the 149 SDL2 calls LÖVE uses, written over PeOS: the window, OpenGL, keys, mouse, clipboard, threads |
| `balatro/` | balatro.app: its `app.ini`, the setup screen (a small LÖVE game), its texts and icon |
| `tests/` | `lovecheck`, a LÖVE game that checks every part Balatro uses; `luajit_check.lua`; `mkogg.c`, a test sound |

What PeOS gives the port, and how:
- **The window** is a Starlight window whose pixels are the app's own;
  OpenGL draws with Mesa (`peos_gl`: the Intel GPU, or virgl in a VM) and
  each frame is copied into the window. Waiting for input is waiting on the
  window's channel: no work while nothing happens.
- **The JIT**: PeOS never lets a page be writable and executable at once.
  LuaJIT writes its machine code with the page read-write, then turns it
  read-execute (`mprotect`, the capability `jit` in `app.ini`).
- **Sound**: OpenAL 1.1 written for PeOS (in PeOS, `userland/ports/openal`),
  mixed by the kernel with the other programs.
- **Capabilities**: an app asks only for what it uses (`gpu`, `jit`, `shm`,
  `audio`, `fs.read`, `fs.write`) and the paths it reads or writes.

`--first A B` (PeOS only) starts the first game of the list that exists:
balatro.app's `args` are the copied game, then the setup, so the setup
runs until the game is copied, then restarts LÖVE into it.

Any other LÖVE 11.5 game runs the same way: `love /path/to/game.love`, or a
bundle like balatro.app with its own `args`.

## License

The port (these files) is under the **zlib license**, as LÖVE and SDL: see
[LICENSE](LICENSE). The projects it downloads and builds keep their own
licenses: LÖVE (zlib), LuaJIT (MIT), libogg and libvorbis (BSD), SDL2's
headers (zlib), and the libraries inside LÖVE (see its `license.txt`).

Balatro is a game by LocalThunk, published by Playstack. This project is
not affiliated with or endorsed by them, and contains none of the game's
files.
