#ifndef DGPD_WADSELECT_H
#define DGPD_WADSELECT_H

// Start-up WAD picker: finds the WADs bundled in the game's wad/ folder and
// lets the player choose one (implemented in dgpd_wadselect.c).

// Scans wad/ for usable WADs. Call once at init.
void dgpd_wad_Scan(void);

// Runs one frame of the picker (input + drawing). Returns 1 once a WAD has
// been chosen (immediately if there is exactly one), else 0. If there are no
// WADs it shows an error and keeps returning 0.
int dgpd_wad_Update(void);

// Path of the chosen WAD, e.g. "wad/doom1.wad", valid for the rest of the
// session (Doom keeps pointers into its argv).
const char *dgpd_wad_Path(void);

// Clears the screen and shows a centred message in the picker's style.
void dgpd_wad_DrawMessage(const char *msg);

#endif
