/* i_sound_stub.c — no-op sound layer for VibeOS DOOM port. */
#include "i_sound.h"
#include "m_config.h"

int snd_musicdevice  = 0;
int snd_sfxdevice    = 0;
int snd_samplerate   = 22050;
int snd_maxslicetime_ms = 28;
char *snd_dmxoption  = (char *)"";

void I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void I_ShutdownSound(void) {}

int  I_GetSfxLumpNum(sfxinfo_t *e) { (void)e; return -1; }
void I_UpdateSound(void) {}
void I_UpdateSoundParams(int h, int v, int s) { (void)h;(void)v;(void)s; }
int  I_StartSound(sfxinfo_t *e, int c, int v, int s) { (void)e;(void)c;(void)v;(void)s; return -1; }
void I_StopSound(int h) { (void)h; }
boolean I_SoundIsPlaying(int h) { (void)h; return 0; }
void I_PrecacheSounds(sfxinfo_t *t, int n) { (void)t;(void)n; }

void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int v) { (void)v; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(void *d, int l) { (void)d;(void)l; return (void *)0; }
void I_UnRegisterSong(void *h) { (void)h; }
void I_PlaySong(void *h, boolean loop) { (void)h;(void)loop; }
void I_StopSong(void) {}
boolean I_MusicIsPlaying(void) { return 0; }
void I_BindSoundVariables(void) {}
