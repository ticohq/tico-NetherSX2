/* Boost mode (the Wrapper/BoostMode setting), after tico-dolphin's: the CPU at
 * 1785 MHz and the GPU at 768 MHz while the game runs, through a clock
 * manager (hoc-clk, sys-clk-OC, sys-clk) when one is running, else set
 * directly and kept; a clock already faster is left as it is. */
#ifndef NETHERSX2_TICO_CLOCKS_H
#define NETHERSX2_TICO_CLOCKS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Applies Boost mode when the setting is on; true when it boosts. */
bool tico_clocks_start(void);
/* About once a second: Horizon puts its clocks back on dock changes and
 * after sleep. */
void tico_clocks_keep(void);
/* Puts the clocks back as they were. */
void tico_clocks_restore(void);

#ifdef __cplusplus
}
#endif

#endif
