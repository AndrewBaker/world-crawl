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
#include "perlin.h"
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

static const double OPENWORLD_HEIGHT_SCALE = 0.010;
static const double OPENWORLD_BIOME_SCALE = 0.003;

static const double OPENWORLD_DEEP_WATER = -0.40;
static const double OPENWORLD_SHALLOW_WATER = -0.10;
static const double OPENWORLD_SAND = 0.10;

static const int OPENWORLD_FOREST_LIGHT_DENSITY = 12;
static const int OPENWORLD_FOREST_HEAVY_DENSITY = 28;

enum openworld_biome
{
    OW_BIOME_GRASSLAND,
    OW_BIOME_DESERT,
    OW_BIOME_FOREST_LIGHT,
    OW_BIOME_FOREST_HEAVY,
};

static double _openworld_noise(double scale, const coord_def &world, double z)
{
    const double x = world.x * scale;
    const double y = world.y * scale;
    return perlin::noise(x, y, z);
}

static double _openworld_height_at(const coord_def &world)
{
    const double z = static_cast<double>(openworld_state.seed) * 0.001;
    return _openworld_noise(OPENWORLD_HEIGHT_SCALE, world, z);
}

static openworld_biome _openworld_biome_for_chunk(const coord_def &chunk)
{
    const double z = static_cast<double>(openworld_state.seed ^ 0x5A5A5A5A)
                     * 0.001;
    const coord_def world(chunk.x * OPENWORLD_CHUNK_SIZE,
                          chunk.y * OPENWORLD_CHUNK_SIZE);
    const double n = _openworld_noise(OPENWORLD_BIOME_SCALE, world, z);

    if (n < -0.25)
        return OW_BIOME_DESERT;
    if (n < 0.10)
        return OW_BIOME_GRASSLAND;
    if (n < 0.35)
        return OW_BIOME_FOREST_LIGHT;
    return OW_BIOME_FOREST_HEAVY;
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
    const openworld_biome biome = _openworld_biome_for_chunk(chunk);
    const double height = _openworld_height_at(world);

    const bool is_deep = height < OPENWORLD_DEEP_WATER;
    const bool is_shallow = height < OPENWORLD_SHALLOW_WATER && !is_deep;
    const bool is_sand = height < OPENWORLD_SAND && !is_deep && !is_shallow;

    tile_clear_flavour(p);
    env.grid_colours(p) = 0;

    if (is_deep)
    {
        grd(p) = DNGN_DEEP_WATER;
        return;
    }

    if (is_shallow)
    {
        grd(p) = DNGN_SHALLOW_WATER;
        return;
    }

    const bool desert = biome == OW_BIOME_DESERT;
    const bool sandy = desert || is_sand;
    const tileidx_t base = sandy ? TILE_FLOOR_SAND : TILE_FLOOR_GRASS;

    bool place_tree = false;
    if (!sandy && (biome == OW_BIOME_FOREST_LIGHT || biome == OW_BIOME_FOREST_HEAVY))
    {
        const int density = biome == OW_BIOME_FOREST_HEAVY
                            ? OPENWORLD_FOREST_HEAVY_DENSITY
                            : OPENWORLD_FOREST_LIGHT_DENSITY;
        const uint64_t h = hash3(openworld_state.seed ^ 0x1BADD00D,
                                 static_cast<uint64_t>(world.x),
                                 static_cast<uint64_t>(world.y));
        place_tree = static_cast<int>(h % 100) < density;
    }

    if (place_tree && p != OPENWORLD_CENTRE)
        grd(p) = DNGN_TREE;
    else
        grd(p) = DNGN_FLOOR;

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
        const dungeon_feature_type feat = grd(*ri);
        if (feat != DNGN_FLOOR && feat != DNGN_SHALLOW_WATER
            && feat != DNGN_DEEP_WATER && feat != DNGN_TREE)
        {
            continue;
        }

        _openworld_apply_floor_at(*ri);
        tile_init_flavour(*ri);
    }
}
