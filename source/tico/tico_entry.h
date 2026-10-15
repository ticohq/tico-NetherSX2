/* tico frontend entry for the NetherSX2 host (after tico-drasticDS).
 *
 * tico chainloads tico-nethersx2.nro with the module's launch template:
 *   "<nro>" "<rom>" "<title>" "--saves=<saves>" "--states=<states>"
 * where saves and states are the playing account's PS2 folders. On exit the
 * host chainloads back to tico with --resume. */
#ifndef NETHERSX2_TICO_ENTRY_H
#define NETHERSX2_TICO_ENTRY_H

#include <stdbool.h>

#define TICO_LAUNCHER_PATH "sdmc:/switch/tico/tico.nro"
#define TICO_LAUNCHER_ROOT_PATH "sdmc:/switch/tico.nro"

/* Reads the launch arguments and points the host's prefs at the game, the
 * bundled core and tico's folders. Call right after prefs_init(). Returns
 * false when started without a game (nothing to run). */
bool tico_prepare(int argc, char **argv);

const char *tico_display_title(void);

/* The game as tico named it (sdmc:/... or usb://...): names its per-game
 * settings and its cheat file. */
const char *tico_rom_path(void);

/* The playing account (32 hex digits) when tico's saves folder names it
 * (.../users/<id>/ps2/), else "". */
const char *tico_account_id(void);

/* True when this launch is a Restart: the game starts over instead of
 * offering to continue from the auto save. */
bool tico_was_restarted(void);

/* Queues this NRO with the same game for when this process exits. */
bool tico_queue_restart(void);

/* Queues tico with --resume for when this process exits. */
bool tico_queue_return(void);

/* Appends to TICO_LOG_PATH. */
void tico_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define TICO_LOG_PATH DATA_ROOT "/tico.log"

#endif
