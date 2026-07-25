// sdk/robox_discord.h -- Discord Rich Presence for the Robox port.
//
// Registered as the "discord" mod (sdk/robox_mods.c), so it is togglable from
// mods/mods.cfg or ROBOX_MOD_DISCORD=0 like everything else. Compiles to
// no-ops on Android and the web, which have no Discord IPC endpoint.
#ifndef ROBOX_DISCORD_H
#define ROBOX_DISCORD_H

/* Mod entry point. Cheap: it only arms the state machine -- the first
 * connection attempt happens on the first tick. */
void robox_discord_init(void);

/* Drives connect / handshake / presence updates. Safe (and nearly free) to
 * call every frame; the work inside is time-gated. Called from
 * video_present(). No-op if the mod is off. */
void robox_discord_tick(void);

/* The two lines Discord shows under "Playing ROBOX Recompiled". Either may be
 * NULL to leave that line alone. Takes a copy; the next tick pushes it. */
void robox_discord_set_status(const char *details, const char *state);

/* Clears the presence and closes the pipe. Registered with atexit() by
 * robox_discord_init(), so nothing else needs to call it. */
void robox_discord_shutdown(void);

#endif /* ROBOX_DISCORD_H */
