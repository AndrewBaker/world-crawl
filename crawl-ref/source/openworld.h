#ifndef OPENWORLD_H
#define OPENWORLD_H

#include <stdint.h>

#include "coord.h"

struct openworld_state
{
    coord_def major_coord;
    uint32_t seed;
    bool initialized;
};

extern openworld_state openworld_state;

void openworld_init_state();
void dgn_build_openworld_level();
void maybe_shift_openworld_around_player();
void openworld_resync_floor_tiles();

#endif
