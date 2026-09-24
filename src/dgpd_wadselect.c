// Start-up WAD picker: see dgpd_wadselect.h.
#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "dgpd_wadselect.h"
#include "pd_glue.h"

// Any *.wad in the wad folder is offered; Doom identifies E1M1/MAP01
// games by contents when the name is not a standard one. These engines'
// IWADs would be misdetected as Doom, so they are skipped by name.
static const char *const unsupported_wads[] = {
    "heretic.wad", "heretic1.wad", "hexen.wad", "hexendemo.wad", "hexdd.wad",
    "strife0.wad", "strife1.wad", "voices.wad",
};

#define WAD_DIR "wad" // folder in the game bundle (Source/wad/)
#define MAX_WADS 32
#define WAD_NAME_LEN 40
#define WAD_ROWS 7 // rows visible at once in the picker

static char wad_names[MAX_WADS][WAD_NAME_LEN];
static int wad_count;
static int wad_selected;
static int wad_top; // first visible row
static char wad_path[WAD_NAME_LEN + sizeof(WAD_DIR)];

// True if the file starts with a WAD header. "PWAD" is accepted too: some
// standalone games (Chex Quest) ship that way.
static int is_wad(const char *name)
{
    PlaydateAPI *pd = pd_glue_api();
    char path[WAD_NAME_LEN + sizeof(WAD_DIR)];
    char magic[4] = {0};
    SDFile *f;
    int n;

    snprintf(path, sizeof(path), WAD_DIR "/%s", name);
    f = pd->file->open(path, kFileRead | kFileReadData);
    if (f == NULL)
        return 0;
    n = pd->file->read(f, magic, 4);
    pd->file->close(f);
    return n == 4 && (memcmp(magic, "IWAD", 4) == 0 || memcmp(magic, "PWAD", 4) == 0);
}

static void wad_found(const char *filename, void *ud)
{
    size_t i, len = strlen(filename);

    (void)ud;
    if (wad_count >= MAX_WADS || len >= WAD_NAME_LEN || len < 5
        || strcasecmp(filename + len - 4, ".wad") != 0)
        return;

    for (i = 0; i < sizeof(unsupported_wads) / sizeof(unsupported_wads[0]); ++i)
        if (strcasecmp(filename, unsupported_wads[i]) == 0)
            return;

    if (is_wad(filename))
        strcpy(wad_names[wad_count++], filename);
}

void dgpd_wad_Scan(void)
{
    int i, j;

    wad_count = 0;
    pd_glue_api()->file->listfiles(WAD_DIR, wad_found, NULL, 0);

    for (i = 1; i < wad_count; ++i) // tiny insertion sort, alphabetical
    {
        char tmp[WAD_NAME_LEN];

        strcpy(tmp, wad_names[i]);
        for (j = i - 1; j >= 0 && strcasecmp(wad_names[j], tmp) > 0; --j)
            strcpy(wad_names[j + 1], wad_names[j]);
        strcpy(wad_names[j + 1], tmp);
    }
}

static void draw_text_line(const char *msg, int y, int selected)
{
    PlaydateAPI *pd = pd_glue_api();
    int w = (int) strlen(msg) * 9 + 16;
    int x = (400 - w) / 2;

    if (selected)
    {
        pd->graphics->fillRect(x, y - 2, w, 20, kColorWhite);
        pd->graphics->setDrawMode(kDrawModeFillBlack);
    }
    else
    {
        pd->graphics->setDrawMode(kDrawModeFillWhite);
    }
    pd->graphics->drawText(msg, strlen(msg), kASCIIEncoding, x + 8, y);
}

void dgpd_wad_DrawMessage(const char *msg)
{
    pd_glue_api()->graphics->clear(kColorBlack);
    draw_text_line(msg, 112, 0);
}

static void draw_wad_menu(void)
{
    int i;
    int rows = wad_count < WAD_ROWS ? wad_count : WAD_ROWS;
    int y = 128 - rows * 12;

    pd_glue_api()->graphics->clear(kColorBlack);
    draw_text_line("Select WAD", y - 34, 0);
    if (wad_top > 0)
        draw_text_line("^", y - 14, 0);
    for (i = 0; i < rows; ++i)
        draw_text_line(wad_names[wad_top + i], y + i * 24, wad_top + i == wad_selected);
    if (wad_top + rows < wad_count)
        draw_text_line("v", y + rows * 24 - 4, 0);
}

int dgpd_wad_Update(void)
{
    PDButtons cur, pushed, released;

    if (wad_count == 0)
    {
        dgpd_wad_DrawMessage("No WAD found in wad folder");
        return 0;
    }
    if (wad_count == 1)
        return 1;

    pd_glue_api()->system->getButtonState(&cur, &pushed, &released);
    if ((pushed & kButtonUp) && wad_selected > 0)
        wad_selected--;
    if ((pushed & kButtonDown) && wad_selected < wad_count - 1)
        wad_selected++;
    if (wad_selected < wad_top)
        wad_top = wad_selected;
    if (wad_selected >= wad_top + WAD_ROWS)
        wad_top = wad_selected - WAD_ROWS + 1;
    if (pushed & kButtonA)
        return 1;

    draw_wad_menu();
    return 0;
}

const char *dgpd_wad_Path(void)
{
    snprintf(wad_path, sizeof(wad_path), WAD_DIR "/%s", wad_names[wad_selected]);
    return wad_path;
}
