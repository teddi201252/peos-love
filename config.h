/* LÖVE's build configuration for PeOS (in place of autotools' config.h;
 * userland/ports/love/build.sh copies it beside LÖVE's src/). The modules a
 * game like Balatro uses; left out: video (Theora), physics (Box2D), enet
 * and luasocket (the network from Lua), MOD and MP3 decoding. */
#ifndef LOVE_PEOS_CONFIG_H
#define LOVE_PEOS_CONFIG_H

#define LOVE_ENABLE_LOVE
#define LOVE_ENABLE_AUDIO
#define LOVE_ENABLE_DATA
#define LOVE_ENABLE_EVENT
#define LOVE_ENABLE_FILESYSTEM
#define LOVE_ENABLE_FONT
#define LOVE_ENABLE_GRAPHICS
#define LOVE_ENABLE_IMAGE
#define LOVE_ENABLE_JOYSTICK
#define LOVE_ENABLE_KEYBOARD
#define LOVE_ENABLE_MATH
#define LOVE_ENABLE_MOUSE
#define LOVE_ENABLE_SOUND
#define LOVE_ENABLE_SYSTEM
#define LOVE_ENABLE_THREAD
#define LOVE_ENABLE_TIMER
#define LOVE_ENABLE_TOUCH
#define LOVE_ENABLE_WINDOW
#define LOVE_ENABLE_LUA53

#define LOVE_NO_MODPLUG 1
#define LOVE_NOMPG123 1

#endif
