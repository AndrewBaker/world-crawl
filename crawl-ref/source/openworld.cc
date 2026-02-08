#include "AppHdr.h"

#include "openworld.h"

#include "branch.h"
#include "cloud.h"
#include "coordit.h"
#include "dungeon.h"
#include "externs.h"
#include "hash.h"
#include "items.h"
#include "mapmark.h"
#include "mon-death.h"
#include "player.h"
#include "random.h"
#include "shopping.h"
#include "stash.h"
#include "terrain.h"
#include "tiledef-floor.h"
#include "tileview.h"
#include "traps.h"
#include "los.h"

struct openworld_state openworld_state = { coord_def(0, 0), 0, false };

static const coord_def OPENWORLD_CENTRE(GXM / 2, GYM / 2);
static const int OPENWORLD_AREA_SHIFT_RADIUS = LOS_RADIUS + 2;
static const int OPENWORLD_CHUNK_SIZE = GXM - MAPGEN_BORDER * 2;

static tileidx_t _openworld_floor_tile_for_chunk(const coord_def &chunk)
{
    const tileidx_t min_tile = TILE_FLOOR_GREY_DIRT;
    const tileidx_t max_tile = TILE_FLOOR_MOSAIC_15;
    const uint32_t count = static_cast<uint32_t>(max_tile - min_tile + 1);

    const uint64_t h = hash3(openworld_state.seed,
                             static_cast<uint64_t>(chunk.x),
                             static_cast<uint64_t>(chunk.y));
    return static_cast<tileidx_t>(min_tile + (h % count));
}

static coord_def _openworld_chunk_for_world(const coord_def &world)
{
    return coord_def(world.x / OPENWORLD_CHUNK_SIZE,
                     world.y / OPENWORLD_CHUNK_SIZE);
}

static void _openworld_apply_floor_at(const coord_def &p)
{
    const coord_def world = openworld_state.major_coord + p;
    const coord_def chunk = _openworld_chunk_for_world(world);
    const tileidx_t base = _openworld_floor_tile_for_chunk(chunk);

    grd(p) = DNGN_FLOOR;
    env.grid_colours(p) = 0;
    tile_clear_flavour(p);
    env.tile_flv(p).floor = base;
}

static void _openworld_wipe_square_at(const coord_def &p)
{
    destroy_shop_at(p);
    destroy_trap(p);

    if (map_masked(p, MMT_VAULT))
        env.level_map_mask(p) &= ~MMT_VAULT;

    grd(p) = DNGN_UNSEEN;

    lose_item_stack(p);

    if (monster *mon = monster_at(p))
        monster_die(mon, KILL_RESET, -1, true, false);

    delete_cloud_at(p);

    env.pgrid(p) = 0;
    env.grid_colours(p) = 0;
#ifdef USE_TILE
    env.tile_bk_fg(p) = 0;
    env.tile_bk_bg(p) = 0;
    env.tile_bk_cloud(p) = 0;
#endif
    tile_clear_flavour(p);

    env.level_map_mask(p) = 0;
    env.level_map_ids(p) = INVALID_MAP_INDEX;

    remove_markers_and_listeners_at(p);

    env.map_knowledge(p).clear();
    if (env.map_forgotten.get())
        (*env.map_forgotten.get())(p).clear();
    env.map_seen.set(p, false);
    StashTrack.update_stash(p);
}

static void _openworld_wipe_unmasked_area(const map_bitmask &preserve_mask)
{
    for (rectangle_iterator ri(MAPGEN_BORDER); ri; ++ri)
        if (!preserve_mask(*ri))
            _openworld_wipe_square_at(*ri);
}

static void _openworld_move_entities_at(const coord_def &src,
                                        const coord_def &dst)
{
    dgn_move_entities_at(src, dst, true, true, true);
}

static void _openworld_identify_area_to_shift(const coord_def &source,
                                              int radius,
                                              map_bitmask *mask)
{
    mask->reset();
    for (rectangle_iterator ri(source, radius); ri; ++ri)
    {
        if (!map_bounds_with_margin(*ri, MAPGEN_BORDER))
            continue;
        mask->set(*ri);
    }
}

static void _openworld_move_entities(const coord_def &target_centre,
                                     map_bitmask *shift_area_mask)
{
    const coord_def source_centre = you.pos();
    const coord_def delta = (target_centre - source_centre).sgn();
    coord_def direction = -delta;

    if (!direction.x)
        direction.x = 1;
    if (!direction.y)
        direction.y = 1;

    coord_def start(MAPGEN_BORDER, MAPGEN_BORDER);
    coord_def end(GXM - 1 - MAPGEN_BORDER, GYM - 1 - MAPGEN_BORDER);

    if (direction.x == -1)
        swap(start.x, end.x);
    if (direction.y == -1)
        swap(start.y, end.y);

    end += direction;

    for (int y = start.y; y != end.y; y += direction.y)
    {
        for (int x = start.x; x != end.x; x += direction.x)
        {
            const coord_def src(x, y);
            if (!shift_area_mask->get(src))
                continue;

            shift_area_mask->set(src, false);

            const coord_def dst = src - source_centre + target_centre;
            if (map_bounds_with_margin(dst, MAPGEN_BORDER))
            {
                shift_area_mask->set(dst);
                _openworld_wipe_square_at(dst);
                _openworld_move_entities_at(src, dst);
            }
            else
                _openworld_wipe_square_at(src);
        }
    }
}

static void _openworld_shift_level_contents_around_player(
    int radius,
    const coord_def &target_centre,
    map_bitmask &genlevel_mask)
{
    const coord_def source_centre = you.pos();

    openworld_state.major_coord += (source_centre - OPENWORLD_CENTRE);

    _openworld_identify_area_to_shift(source_centre, radius, &genlevel_mask);

    _openworld_wipe_unmasked_area(genlevel_mask);
    _openworld_move_entities(target_centre, &genlevel_mask);
    _openworld_wipe_unmasked_area(genlevel_mask);

    for (rectangle_iterator ri(0); ri; ++ri)
        genlevel_mask.set(*ri, !genlevel_mask.get(*ri));
}

static void _openworld_generate_area(const map_bitmask &genlevel_mask)
{
    for (rectangle_iterator ri(MAPGEN_BORDER); ri; ++ri)
        if (genlevel_mask(*ri))
            _openworld_apply_floor_at(*ri);

    tile_init_flavour();
}

static void _openworld_area_shift()
{
    if (you.pos() == OPENWORLD_CENTRE)
        return;

    map_bitmask genlevel_mask;
    _openworld_shift_level_contents_around_player(
        OPENWORLD_AREA_SHIFT_RADIUS, OPENWORLD_CENTRE, genlevel_mask);

    _openworld_generate_area(genlevel_mask);
    los_changed();
}

void openworld_init_state()
{
    if (openworld_state.initialized)
        return;

    openworld_state.major_coord.x = get_uint32() & 0x7FFFFFFF;
    openworld_state.major_coord.y = get_uint32() & 0x7FFFFFFF;
    openworld_state.seed = get_uint32() & 0x7FFFFFFF;
    openworld_state.initialized = true;
}

void dgn_build_openworld_level()
{
    openworld_init_state();

    for (rectangle_iterator ri(MAPGEN_BORDER); ri; ++ri)
        _openworld_apply_floor_at(*ri);

    grd(OPENWORLD_CENTRE) = DNGN_STONE_STAIRS_DOWN_I;

    env.spawn_random_rate = 0;
    env.density = 0;
}

void maybe_shift_openworld_around_player()
{
    if (!player_in_branch(BRANCH_OPENWORLD))
        return;

    if (map_bounds_with_margin(you.pos(),
                               MAPGEN_BORDER + OPENWORLD_AREA_SHIFT_RADIUS + 1))
        return;

    _openworld_area_shift();
}

void openworld_resync_floor_tiles()
{
    if (!player_in_branch(BRANCH_OPENWORLD))
        return;

    if (!openworld_state.initialized)
        openworld_init_state();

    for (rectangle_iterator ri(MAPGEN_BORDER); ri; ++ri)
    {
        if (grd(*ri) != DNGN_FLOOR)
            continue;

        tile_clear_flavour(*ri);
        const coord_def world = openworld_state.major_coord + *ri;
        const coord_def chunk = _openworld_chunk_for_world(world);
        const tileidx_t base = _openworld_floor_tile_for_chunk(chunk);
        env.tile_flv(*ri).floor = base;
        tile_init_flavour(*ri);
    }
}
