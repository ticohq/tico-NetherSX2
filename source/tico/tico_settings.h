/* tico's settings for this core, applied to the host's prefs.
 *
 * tico draws the settings screen from the module's settings.json and stores
 * the values in sdmc:/tico/config/cores/nethersx2.jsonc, plus a game's own in
 * sdmc:/tico/config/games/nethersx2/<game>.jsonc. Option keys are the
 * nethersx2.ini keys, so each value goes to the pref of the same name. */
#ifndef NETHERSX2_TICO_SETTINGS_H
#define NETHERSX2_TICO_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Copies every option (the game's value, else the core's, else the default)
 * into prefs, then derives what the upstream launcher derives at launch.
 * rom is the game as tico named it; NULL for none. */
void tico_settings_apply(const char *rom);

#ifdef __cplusplus
}
#endif

#endif
