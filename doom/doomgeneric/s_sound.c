/*
 * s_sound.c — Thumby Color: all sound stubbed out
 *
 * DOOM's sound system is disabled entirely for the Thumby port.
 * All public functions are no-ops. This avoids crashes from
 * uninitialized sound channels and missing music modules.
 */

#include "s_sound.h"
#include "doomtype.h"

int snd_channels = 0;
int sfxVolume = 8;
int musicVolume = 8;

void S_Init(int sfxVolume, int musicVolume) { (void)sfxVolume; (void)musicVolume; }
void S_Shutdown(void) {}
void S_Start(void) {}
void S_StartSound(void *origin, int sound_id) { (void)origin; (void)sound_id; }
void S_StopSound(mobj_t *origin) { (void)origin; }
void S_StartMusic(int music_id) { (void)music_id; }
void S_ChangeMusic(int music_id, int looping) { (void)music_id; (void)looping; }
boolean S_MusicPlaying(void) { return false; }
void S_StopMusic(void) {}
void S_PauseSound(void) {}
void S_ResumeSound(void) {}
void S_UpdateSounds(mobj_t *listener) { (void)listener; }
void S_SetMusicVolume(int volume) { (void)volume; }
void S_SetSfxVolume(int volume) { (void)volume; }
