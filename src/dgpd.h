#ifndef DGPD_H
#define DGPD_H

// Playdate platform layer for doomgeneric (implemented in doomgeneric_playdate.c).
void dgpd_PollInput(void);
void dgpd_OpenMenu(void);
void dgpd_ToggleAutomap(void);
void dgpd_SetAlwaysRun(int on);
int dgpd_AutomapActive(void);

#endif
