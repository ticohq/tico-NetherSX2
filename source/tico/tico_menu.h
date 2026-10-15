/* tico's in-game menu for the NetherSX2 host (after tico-drasticDS): the
 * ImGui overlay drawn over the frames the core presents, opened with
 * Plus + Minus. It runs on the host's input thread; the overlay draws on the
 * core's presenting thread (tico/overlay/imgui_overlay.cpp). */
#ifndef NETHERSX2_TICO_MENU_H
#define NETHERSX2_TICO_MENU_H

#include <switch.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the menu asks of the core; main.c fills it in with the NativeLibrary
 * calls. State slots are the core's (0..10). */
typedef struct {
  void (*save_state)(int slot);
  void (*load_state)(int slot);
  void (*reset)(void);
  /* the prefs changed: the core and the host re-read them */
  void (*apply_settings)(void);
  /* the game's PNACH changed */
  void (*reload_patches)(void);
  /* lets go of every button the game holds */
  void (*release_inputs)(void);
  /* frames the core has presented so far */
  int (*presented_frames)(void);
} TicoMenuCore;

/* After the renderer is known (vulkan: the core presents with Vulkan). */
void tico_menu_init(const TicoMenuCore *core, bool vulkan);
void tico_menu_shutdown(void);

/* The core's game callback (any thread): names the state and cheat files. */
void tico_menu_game_changed(const char *serial, uint32_t crc);

/* Feeds the first player's pad; true while the menu has the input (the
 * game then gets none). */
bool tico_menu_update(u64 held, u64 pressed);

/* Housekeeping, about 125 times a second. */
void tico_menu_tick(void);

bool tico_menu_is_open(void);
/* True once after Exit Game or Restart. */
bool tico_menu_take_exit_request(void);
/* True once after Restart (with the exit request). */
bool tico_menu_take_restart_request(void);

/* Saves the game into the auto slot and waits (a few seconds at most) for
 * the file; call before the game closes (Exit, Restart, HOME). */
void tico_menu_auto_save(void);

#ifdef __cplusplus
}
#endif

#endif
