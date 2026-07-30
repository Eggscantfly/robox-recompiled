// sdk/robox_lua.h -- Lua scripting runtime for mods.
//
// The other mods in this tree (co-op, Mario physics, music packs) are C: to
// change one you edit sdk/, rebuild the whole recompiled game, and wait. That
// is fine for the four people building this repo and hopeless for anyone who
// just wants to make something. This is the answer to that -- mods written in
// Lua, dropped into mods/lua/, reloaded while the game is running.
//
// There are two Lua VMs in play and they are NOT the same thing:
//
//   HOST VM   (this file)  Lua 5.4, vendored in vendor/lua/, running in the
//                          port. It gets the powerful surface -- guest memory,
//                          function hooks, input, HUD drawing, the player.
//                          See MODDING.md.
//
//   GUEST VM  (robox_lua_guest.c)  The Lua 5.1 the game itself ships. Robox
//                          really does embed one -- LUA_PATH, the whole 5.1
//                          stdlib and the engine's own bindings (loadMap2,
//                          newFpg, renderQuad, getText, cargaAnimacion) are all
//                          in Robox USA.dol. The game only ever uses it for
//                          tileset tables and language strings, so it sat there
//                          doing nothing. Mods can now run code inside it.
//
// Every entry point here is a no-op when the mod is off, so the call sites in
// sdk/video.c and sdk/gx_ogl.c cost a load and a branch.
#ifndef ROBOX_LUA_H
#define ROBOX_LUA_H

#include <stdint.h>

/* Mod-registry entry point (sdk/robox_mods.c). Boots the VM and loads
 * everything under mods/lua/. */
void robox_lua_init(void);

/* 1 once the VM is up. */
int  robox_lua_active(void);

/* --- frame pump hooks ---------------------------------------------------- */

/* Once per host frame from the SDL pump in sdk/video.c. Runs hot reload,
 * edge-detects input, fires "frame" and "key". */
void robox_lua_tick(void);

/* Inside the present path in sdk/gx_ogl.c, over the finished frame. Fires
 * "draw" and paints robox.notify() toasts. */
void robox_lua_render(void);

/* Buttons a mod is holding down via robox.input.press(). OR'd into
 * video_input_hold() so injected input goes through exactly the same d-pad
 * rotation and remapping as a real key. */
uint32_t robox_lua_input_mask(void);

/* 1 while a mod holds the keyboard (robox.input.capture) -- the drop-down
 * console in mods/lua/console.lua is what this exists for. The SDL pump stands
 * its own hotkeys down while it is set: a console that cannot receive Escape,
 * or that toggles fullscreen when you type an F-key, is not a console. */
int  robox_lua_capture_active(void);

/* One SDL_TEXTINPUT chunk, UTF-8, from the pump. Fires the "text" event at
 * whichever mod holds the keyboard; ignored when nobody does. Text and not
 * scancodes because that is the only way shift, symbols and a non-US layout
 * all come out as the character the user actually typed. */
void robox_lua_text_input(const char *utf8);

/* --- guest-VM bridge (sdk/robox_io.c) ------------------------------------ */

/* Called for every path the guest opens through the content system -- which
 * is how the game's own Lua reaches its .lua files, dofile() included.
 * Returns a host path to serve instead, or NULL to load the real asset.
 *
 * Two things use it: serving a script/game.lua with the guest-side mods
 * appended, and answering robox.host() RPC reads. */
const char *robox_lua_guest_redirect(const char *guest_path);

#endif /* ROBOX_LUA_H */
