// CD audio: null stub.  SDL2 removed the SDL_CD API entirely, and Quake CD music
// is non-essential for the miniGL/HW-rasterizer effort.  All entry points no-op;
// CDAudio_Init reports "no CD" so the engine runs silently without it.

#include "quakedef.h"

void CDAudio_Play(byte track, qboolean looping) { (void)track; (void)looping; }
void CDAudio_Stop(void) {}
void CDAudio_Pause(void) {}
void CDAudio_Resume(void) {}
void CDAudio_Update(void) {}
int  CDAudio_Init(void) { return -1; }
void CDAudio_Shutdown(void) {}
