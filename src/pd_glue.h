#ifndef PD_GLUE_H
#define PD_GLUE_H

#include "pd_api.h"

// The PlaydateAPI handed to eventHandler, for the Doom platform layer.
PlaydateAPI *pd_glue_api(void);

#endif
