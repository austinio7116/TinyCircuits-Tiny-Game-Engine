/*
 * doom_config.h — Thumby Color memory reduction overrides
 *
 * Reduces static array sizes to fit in RP2350's limited SRAM.
 * Include this BEFORE any doomgeneric headers that define these.
 */

#ifndef DOOM_CONFIG_H
#define DOOM_CONFIG_H

/* Reduce BACKUPTICS from 128 to 12 — saves ~15KB BSS
 * Only need a few tics for single-player */
#undef BACKUPTICS
#define BACKUPTICS 12

/* Reduce MAXDRAWSEGS from 256 to 64 — saves ~9KB BSS
 * 64 is plenty for 128px wide screen */
#undef MAXDRAWSEGS
#define MAXDRAWSEGS 64

/* Reduce MAXVISSPRITES from 128 to 32 — saves ~5.7KB BSS */
#undef MAXVISSPRITES
#define MAXVISSPRITES 32

/* Reduce MAXOPENINGS from SCREENWIDTH*64 to SCREENWIDTH*16 — saves ~12KB BSS */
#undef MAXOPENINGS
#define MAXOPENINGS (SCREENWIDTH * 16)

/* Reduce MAXSEGS from 32 to 16 — small savings */
#undef MAXSEGS
#define MAXSEGS 16

#endif /* DOOM_CONFIG_H */
