// Island Skies, a mod for Snap64 Recomp.
//
// The courses under an open sky -- the Beach, the River, the Volcano and the
// Valley -- after dark, and the Tunnel's mouths with them. (The Cave, lit by
// its own lights, has no sky to turn.)
//
// These courses are drawn the way most of the game is: every surface is a
// texture multiplied by a shade, and the shade is either a color painted on
// the vertex or the scene's one light falling on it. The fog, the color
// behind everything and the sky belong to the course's own set-up. So night
// is made here by changing those few things as a course is set up, and
// nothing else: the game then draws its own night, on the ride and again
// whenever it redraws one of the course's photos for Professor Oak or the
// album.
//
//   - the course's set-up record (fog, background) is changed before the
//     game reads it;
//   - the scene's light is dimmed toward the night's own light while the
//     course's world exists, and put back when it is taken down;
//   - the colors painted on vertices are multiplied by the same light, once
//     per model, as each model is first drawn;
//   - the sky's small picture is recolored in place: the same clouds, by
//     night;
//   - the sky's list is made to call one more list, the mod's own: stars
//     between the clouds and, where the night has one, a moon
//     (night_data.c, written by tools/gen_night.py). The stars twinkle by
//     the course's own clock, and a photo keeps the twinkle of its instant.
//
// Everything is done to the course's data as it sits in memory. That data
// is read from the cartridge again each time a course is loaded, so the
// change never outlives the course.

#include "modding.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "snap64.h"
#include "common.h"
#include "world/world.h"
#include "night_data.h"

// The scene light every course shares (app_render, always in memory) and
// the four display-list cursors.
#define COURSE_LIGHT  ((volatile u8*) 0x800AEBD0)
#define MAIN_GFX_POS  ((Gfx**) 0x8004A890)
#define SKY_TEXELS    (64 * 32)
// The frame being drawn (sys/gtl.c): its context, whose command buffers and
// heap are its own until the renderer has finished with it, and the count of
// frames drawn, which moves on once a frame's draw is done.
#define GTL_CONTEXT   (*(volatile s32*) 0x8004A910u)     // gtlContextId
#define GTL_DRAWN     (*(volatile s32*) 0x80040CF8u)     // gtlDrawnFrameCounter
#define FRAME_CTX     2                                  // the courses' contexts (their scene setups' numContexts)
#define FRAME_SLOTS   8                                  // the slots a frame context has for matrices written more than once a frame
// The list every camera's frame calls to load the scene light (app_render/4D400.c:
// a pipe sync, lighting on, one light, the light from 0x800AEBD8, the ambient from
// 0x800AEBD0, two dither modes, the end).
#define CAMERA_LIGHTS 0x800AEC18u

// What the cartridge loads together with a course. Only vertices and
// commands inside these spans are ever touched, because only these are read
// fresh from the cartridge with the course:
//   LOW   on the ride and for a photo: the course's own data and the
//         Pokémon data loaded beside it (Magikarp, Pikachu, Zubat,
//         Bulbasaur);
//   RIDE  on the ride only: the ride's own models of those four;
//   LEVEL on the ride only: the data of the level code every course shares
//         (the Zero-One and its rider, the things thrown);
//   PHOTO for a photo only: the close-up models of those four, which sit
//         where the level code's memory ends.
#define LOW_LO    0x800F5D90u
#define LOW_HI    0x801D6840u
#define RIDE_LO   0x8033F6C0u
#define RIDE_HI   0x80350200u
#define LEVEL_LO  0x80366BA0u
#define LEVEL_HI  0x803B5000u
#define PHOTO_LO  0x803A71B0u
#define LOW_UNITS   ((LOW_HI - LOW_LO) >> 3)
#define RIDE_UNITS  ((RIDE_HI - RIDE_LO) >> 3)
#define LEVEL_UNITS ((LEVEL_HI - LEVEL_LO) >> 3)
#define MAP_BYTES   ((LOW_UNITS + RIDE_UNITS + LEVEL_UNITS) / 8 + 1)

// A photo's world is built with this function of the photo code as its
// "a Pokemon enters" callback; the ride's world with the course's own.
#define PHOTO_WORLD_CALLBACK 0x8009D0B4u

// The scene light by day, as the cartridge has it.
#define DAY_AMBIENT 100
#define DAY_DIFFUSE 180
// At night the ambient share rises and the diffuse falls, as moonlight
// models softly; the two still add up to the day's.
#define NIGHT_AMBIENT 125
#define NIGHT_DIFFUSE 155

#define G_LIGHTING_BIT 0x00020000u

// Written into the empty last byte of a course's set-up record once its
// load has been made night.
#define DONE_MARK 0xA5

// One course's night. Colors are red, green, blue; lights are parts of 256.
typedef struct Night {
    u32 setup;              // the course's set-up record
    const char* option;     // the mod's setting for this course
    const char* weather;    // its setting for this course's weather, or none
    u32 spare;              // the pipe sync in the sky's list that gives its place to our call
    u32 pics[2];            // the sky's pictures, 64x32
    const u8* rows[2];      // each picture row's height: 0 at the horizon, 255 overhead
    u32 sky_vtx[2];         // the sky box's vertex arrays
    u8 sky_vtx_n[2];
    u8 r_lo, r_scale;       // cloud = (a texel's red, 5 bits, minus r_lo) * r_scale, of 256
    u32 world_lo, world_hi; // where the course's ground and scenery vertices lie
    u8 world[3];            // the light on the scenery
    u8 body[3];             // the light on Pokémon and objects, and the scene light
    u8 flow[3];             // the light on what flows: the water, or the Volcano's lava
    u8 pic_sky[3], pic_cloud[3];            // the night sky's picture, kept bright: open sky, and cloud
    u8 tint_lo[3], tint_hi[3];              // the box's corners: the tint at the horizon, and at the top
    u16 fog_min, fog_max;
    Gfx* sky;               // the mod's list for this sky
    Gfx (*twinkle)[2];      // its brightness commands
    u8 twinkles;
    s8 key[3];              // where the night's key light comes from: the moon, or the lava below
    u8 glint;               // the moon's glint on the water: the courses that have water
    u8 embers;              // the Volcano: embers over the lava, its sky pulsing with it, ash for weather
    Gfx* const* fireflies;  // the fireflies, a list a dot, where there are any
    s8 sun[3];              // where the sunset's sun stands
    Gfx* sun_list;          // the sun and its haze, for the sky at sunset
    s8 dawn_sun[3];         // where the dawn's sun stands
    Gfx* dawn_list;         // the dawn's sun, for the sky at dawn
    Vtx* band;              // the glow band at the horizon toward the sunset's sun (night_data.h)
    Vtx* dawn_band;         // and toward the dawn's
    f32 roof_z;             // a roofed course: where its roof begins over the cart, z in the first block
    u32 code_lo, code_hi;   // where the course's code lies: the ride's Pokémon (pokemonsnap/splat.yaml, <course>_code)
    u32 extra_lo, extra_hi; // and its extra models: the photos' Pokémon (<course>_extra, the model_hd lists)
} Night;

#define NIGHT_TUNNEL_SETUP 0x8011E6CCu  // tunnel_WorldSetup: no sky box; a flat sky color where it opens
// The Tunnel's open air: the first block's floor from the course's far end to
// the cave's mouth, where a roof first stands over the cart (z 1380 of the
// block's own frame, from the block's triangles: every face over the cart's
// path that is not a wall, Oct 15 2026; the roof runs on into the next
// block, which begins at z 1880). The weather is night_data's volume set:
// a box of sheets NIGHT_VOLUME_HALF_X by NIGHT_VOLUME_HALF_Z each way, as
// dense as the ring around the camera, ending at the mouth, drawn in the
// first block and the next.
#define NIGHT_TUNNEL_ROOF_Z (1380.0f)
// Its blocks' ceilings over a grid, from the decompilation's meshes: which
// corners are under the roof, in every block (tools/gen_tunnel_roof.py).
#include "tunnel_roof.inc"
#define NIGHT_TUNNEL_OPEN_Y (1000.0f)    // the volume's middle height: its sheets reach 1300 above it and 1700 below, over a floor near 100 to 400
#define NIGHT_TUNNEL_VOICE_Z (900.0f)    // the rain's voice fades to nothing this far under the roof
#define NIGHT_TUNNEL_CAVE_Z (400.0f)     // the fog and the scene light are the hour's own this far under the roof
#define NIGHT_TUNNEL_EAVE (400.0f)       // the weather still falls this far under the roof's edge, as under any eave (the arch is high to z 1800)
// The far end: the second tunnel's roof (600 to 980 high over the floor) ends
// in an arch rising from 800 to 1500 that opens at x 1400 of the last block
// (its vertex array's faces, Oct 16 2026: the eye trace left the roof there);
// the path runs along +x at z about -500, the floor near 100. The last block's
// origin in the course's frame is (8000, 0, 12000) (src/tunnel/world/blocks.c).
#define NIGHT_TUNNEL_EXIT_X (1400.0f)
#define NIGHT_TUNNEL_EXIT_Z (-500.0f)
#define NIGHT_TUNNEL_EXIT_ORIGIN_X (8000.0f)
#define NIGHT_TUNNEL_EXIT_ORIGIN_Z (12000.0f)

static const Night nights[] = {
    {   // The Beach: a clear night, a full moon over the sea.
        NIGHT_BEACH_SETUP, "beach", "beach_weather", NIGHT_BEACH_SPARE, NIGHT_BEACH_PICS, NIGHT_BEACH_ROWS,
        NIGHT_BEACH_SKY_VTX, NIGHT_BEACH_SKY_VTX_N, NIGHT_BEACH_R_LO, NIGHT_BEACH_R_SCALE,
        0x8011B050u, 0x80135BD4u, { 92, 123, 205 }, { 108, 141, 220 }, { 80, 112, 190 },
        { 104, 120, 184 }, { 196, 200, 212 }, { 48, 56, 88 }, { 16, 20, 48 }, 990, 1000,
        night_beach_sky, night_beach_twinkle, NIGHT_BEACH_TWINKLES, NIGHT_BEACH_KEY, 1, 0, NULL, NIGHT_BEACH_SUN, night_beach_sun, NIGHT_BEACH_DAWN, night_beach_dawn, night_beach_band, night_beach_dawn_band, 0.0f,
        0x802C40A0u, 0x803476A0u, 0x801B0310u, 0x801D6840u },
    {   // The River: broken cloud, a mist over the water.
        NIGHT_RIVER_SETUP, "river", "river_weather", NIGHT_RIVER_SPARE, NIGHT_RIVER_PICS, NIGHT_RIVER_ROWS,
        NIGHT_RIVER_SKY_VTX, NIGHT_RIVER_SKY_VTX_N, NIGHT_RIVER_R_LO, NIGHT_RIVER_R_SCALE,
        0x8012A0C0u, 0x801416F0u, { 86, 124, 196 }, { 104, 142, 214 }, { 66, 104, 156 },
        { 104, 120, 176 }, { 188, 192, 208 }, { 48, 56, 80 }, { 16, 20, 44 }, 988, 1000,
        night_river_sky, night_river_twinkle, NIGHT_RIVER_TWINKLES, NIGHT_RIVER_KEY, 1, 0, night_firefly_dots, NIGHT_RIVER_SUN, night_river_sun, NIGHT_RIVER_DAWN, night_river_dawn, night_river_band, night_river_dawn_band, 0.0f,
        0x802D8800u, 0x8033F6B0u, 0x8019AEE0u, 0x801B6B60u },
    {   // The Volcano: no moon. The lava keeps its own light and lights the smoke from below.
        NIGHT_VOLCANO_SETUP, "volcano", "volcano_weather", NIGHT_VOLCANO_SPARE, NIGHT_VOLCANO_PICS, NIGHT_VOLCANO_ROWS,
        NIGHT_VOLCANO_SKY_VTX, NIGHT_VOLCANO_SKY_VTX_N, NIGHT_VOLCANO_R_LO, NIGHT_VOLCANO_R_SCALE,
        0x800FFF90u, 0x80112260u, { 132, 96, 92 }, { 172, 132, 122 }, { 255, 240, 228 },
        { 72, 56, 64 }, { 255, 210, 150 }, { 108, 60, 44 }, { 24, 20, 40 }, 986, 1000,
        night_volcano_sky, night_volcano_twinkle, NIGHT_VOLCANO_TWINKLES, { 0, -127, 0 }, 0, 1, NULL, NIGHT_VOLCANO_SUN, night_volcano_sun, NIGHT_VOLCANO_DAWN, night_volcano_dawn, night_volcano_band, night_volcano_dawn_band, 0.0f,
        0x802D60E0u, 0x8034E120u, 0x801A9900u, 0x801CF770u },
    {   // The Valley: a high, clear mountain night.
        NIGHT_VALLEY_SETUP, "valley", "valley_weather", NIGHT_VALLEY_SPARE, NIGHT_VALLEY_PICS, NIGHT_VALLEY_ROWS,
        NIGHT_VALLEY_SKY_VTX, NIGHT_VALLEY_SKY_VTX_N, NIGHT_VALLEY_R_LO, NIGHT_VALLEY_R_SCALE,
        0x800FFF90u, 0x80116A70u, { 84, 112, 190 }, { 104, 136, 220 }, { 46, 72, 126 },
        { 104, 120, 184 }, { 196, 200, 212 }, { 48, 56, 88 }, { 12, 16, 44 }, 992, 1000,
        night_valley_sky, night_valley_twinkle, NIGHT_VALLEY_TWINKLES, NIGHT_VALLEY_KEY, 1, 0, NULL, NIGHT_VALLEY_SUN, night_valley_sun, NIGHT_VALLEY_DAWN, night_valley_dawn, night_valley_band, night_valley_dawn_band, 0.0f,
        0x802C5C20u, 0x8034E120u, 0x80186B10u, 0x801B1D40u },
    {   // The Tunnel: no sky drum, but an open-air start and end under two painted
        // 64x32 pictures, which take the hour and the weather as the other courses'
        // sky pictures do; the weather falls there until the mouth (roof_z), and
        // inside only the mouths' color and the light carry it. Its stars, moon and
        // suns are the generator's as every course's, drawn around the eye after
        // the land (roof_weather), there being no drum to hang them on. The
        // pictures: 801565B8 is the sky of both ends; 801575C0 a second, smaller
        // painted box of pale clouds at the far end, beside the track (block 5,
        // x -2384..972, z 929..3860), found in the block lists Oct 17 2026 and
        // not yet seen in a frame; both take the sky's rule and the stars
        // (roof_sky_depth). The texture log of Oct 11 had named 80101FB8, which
        // is in no block list.
        NIGHT_TUNNEL_SETUP, "tunnel", "tunnel_weather", 0, { 0x801565B8u, 0x801575C0u }, { 0, 0 },
        { 0, 0 }, { 0, 0 }, 0, 0,
        0x8011CB50u, 0x8018BC30u, { 118, 130, 178 }, { 126, 140, 196 }, { 100, 120, 170 },
        { 196, 200, 212 }, { 196, 200, 212 }, { 48, 56, 88 }, { 16, 20, 48 }, 996, 1000,
        night_tunnel_sky, night_tunnel_twinkle, NIGHT_TUNNEL_TWINKLES, NIGHT_TUNNEL_KEY, 0, 0, NULL, NIGHT_TUNNEL_SUN, night_tunnel_sun, NIGHT_TUNNEL_DAWN, night_tunnel_dawn, night_tunnel_band, night_tunnel_dawn_band, NIGHT_TUNNEL_ROOF_Z,
        0x802E2500u, 0x80344770u, 0x8018BC50u, 0x801AFD30u },
};

extern f32 GlobalTimer;         // the world's clock: it advances by the set-up's sky speed each tick

static const Night* night;       // the course whose world exists and is at night, or none
static u8 on_the_ride;           // that world is the ride's, not a photo's
static u8 raining;               // that night is a rainy one
static u8 storming;              // and the rain has lightning in it
static u8 snowing;               // or that night is a snowy one
static u8 ashing;                // or, on the Volcano, ash is falling
static u8 scoring;               // the game is measuring a photo, not showing anything
static u8 fx_lit = 1;            // the settings: effects take the night's light (Night-lit, the default),
static u8 glows_on = 1;          // the water glows are drawn,
static u8 moonlight_on = 1;      // the key light leans toward the moon
static u8 night_sounds_on = 1;   // the night's voices are heard
static u8 lightning = 1;         // a storm's lightning: 0 full, 1 gentle (the flash dimmed to two fifths), 2 none
static u8 mist_on = 1;           // the dawn's mist is drawn (Dawn Mist, on unless Custom says)
static u8 shower;                // a meteor shower tonight: the ride's draw (shower_draw)
static u8 shower_for[8], shower_drawn[8];   // each course's draw, kept for its photos' worlds
static f32 shower_az, shower_el; // the radiant: where the streaks run from, around and up
static u32 shower_span = 240;    // ticks between the shower's streaks, by the shower's strength
static const char* shower_name;  // the shower's name, for the log and the page
static u8 aurora;                // an aurora tonight: the ride's draw (aurora_draw), or a stamped photo's note
static u8 aurora_for[8], aurora_drawn[8];   // each course's draw, kept for its photos' worlds
static f32 moon_light = 1.0f;    // how much of the moon is lit: 0.3 at new to 1 at full, for its glint
static u8 sunset;                // the time chosen is the sunset, not the night
static u8 dawn;                  // and that sunset is a dawn (the sun across the sky)
static u8 dusk;                  // or that the sun sets during the ride
static u8 misty;                 // a fixed dawn on a course with a mist: the fog alone moves as the ride goes (mist_now)
static u8 cycle;                 // or that the player's clock drives the hour, day and night
static u8 daytime;               // or that the time chosen is Day: the cycle's day hour, held at midday
static u32 day_weather;          // the weather night_of drew for a Day load, for the load to take
static u8 day_weather_set;
static f32 nightness = 1.0f;     // how far from the cartridge's day the scene is: 0 the day itself, 1 the night's own colors
static f32 ride_minutes = -1.0f;     // the clock's minute the ride began at, for its photos' worlds (none yet: -1)
static s32 ride_day = -1, ride_month;    // and its date: days since 2000, and the month
static f32 cycle_minute = -1.0f;     // the minute last applied
static u16 steep_shade[3] = { 110, 118, 160 };   // the steep faces' shade, 256 by day
static const u8 snow_rock[3] = { 208, 212, 236 };   // a steep face under snow, over its dusting: a little darker and cooler (176, 184, 212 to Oct 20 2026, when it held no dusting)
static u16 sky_orig_pic[2][64 * 32];  // the sky pictures as the cartridge has them (the day end of the blend)
static u32 sky_orig_cn[2][64];        // the sky box's corner colors as the cartridge has them
static u8 sky_orig_cn_kept[2];
static u8 day_fog[3], day_bg[3];      // the course's own fog and background colors,
static u16 day_fog_min, day_fog_max;  // its own fog distances,
static u8 day_ambient[3], day_diffuse[3];    // its own scene light,
static u8 live_weather[8];            // Random's draw for each course this session
static u8 live_weather_drawn[8];
// Under Live, the course's weather as the island's clock has it: what it
// was, what it is turning to, how far the turn has come, and the minute of
// the day the turn begins (-1: none this hour); the shares the drawing
// takes from them (weather_shares_apply). One weather for the ride
// otherwise: both the same, the share 1.
static u32 wx_from, wx_to;            // 0 clear, 1 rain (the Volcano: ash), 2 storm, 3 snow
static f32 wx_share = 1.0f;
static f32 wx_turn_minute = -1.0f;
static f32 pal_share = 1.0f;          // the palettes: 0 the first weather's, 1 the second's
static f32 overcast_share;            // how gray the day's sky picture goes
static f32 rain_share, snow_share, ash_share;    // how much falls, of the full fall
static f32 clear_share = 1.0f;        // how much of the clear sky's stars, moon and glint show
static u8 snow_cover;                 // snow lies on the land: it was snowing as the ride loaded
static u8 snow_relit;                 // the record lit again once the water's height is known (the surf line)
// A roofed course (the Tunnel, with no sky list of its own) has an open-air
// start under a painted sky, which is drawn every frame, inside too. Its
// weather is a thing of the world, not of the cart: one volume of it stands
// over the open stretch of the first block, drawn whenever that block is
// current; the rock hides it where the cart is under the roof, and the
// mouth shows it from inside (JackandBeans, Oct 15 2026: rain seen through
// the openings, none under the roof).
static u8 roof_volume;           // this frame: 1 the entry's volume stands (the first block or the next is current), 2 the exit's (the last two), 0 none
static f32 roof_shift[3];        // the first block's origin in the current block's frame (the game moves its origin at each block)
static f32 roof_past;            // how far the camera is under the roof, in the first block's frame; 0 under the open sky
static f32 cave_now;             // how far the camera is in the cave's own light: 0 under the open sky, 1 under the roof
static u8 roof_outside;          // this frame: the camera is under the open sky, before the roof
static GObj* roof_cam;           // the camera that drew the world this frame, whose matrices the weather takes
static u8 roof_blink;            // the main camera's blink frame (the shutter's black): nothing is drawn on it
static Gfx* roof_sky;            // the sky list in force (the night's, the sunset's or the dawn's; spare_point)
static Mtx roof_sky_mtx[FRAME_CTX]; // its place around the eye, for each frame context
static GObj* photo_sky_cam;      // a photo's camera that drew the roofed course's land in this object pass
static Mtx photo_sky_mtx[FRAME_CTX * FRAME_SLOTS];   // the sky's place around that camera's eye (more than one may draw in a frame)
// A roofed course (the Tunnel): its sky is painted faces of the level, not a
// drum, so the cave is told from the open air (open_air) and its stars, moon
// and suns are drawn after the land (roof_weather).
#define ROOFED(by) ((by)->roof_z > 0.0f)
static u8 tint_away[3];          // the corners' tint at the horizon away from the sun (tint_lo's twin by night)
static u8 scene_ambient[3], scene_diffuse[3];   // the scene light's colors in force
static u8 land_diff[3];          // what the sun (or the moon) adds to a painted face that faces it
static f32 relight_dir[3];       // toward the sun, or the moon, in the world's axes
static u16 fog_near, fog_far;    // the night's fog, to come back to after a flash
static Mtx storm_mtx[FRAME_CTX * FRAME_SLOTS][2];   // the flash's and the bolt's matrices, a pair a slot
static Mtx streak_mtx[FRAME_CTX * FRAME_SLOTS];   // the shooting star's, a slot each time one is drawn
static u32 streak_logged;        // the last star the log named
static f32 view_fwd[2];          // where the view faces, level, a unit; from the game's light each frame
static u8 light_world[3];        // the lights in force: on the scenery,
static u8 light_body[3];         // on Pokémon and objects (and the scene light),
static u8 light_flow[3];         // and on what flows
static u8 sky_horizon[3], sky_top[3];    // the sky's colors in force: at the horizon (the fog's) and at the top (the background's)
static u8 pic_sky[3], pic_cloud[3];      // the sky picture's colors in force
static u8 tint_lo[3], tint_hi[3];        // the box's corner tints in force
static f32 sky_sin_lo, sky_sin_hi;       // how low and how high the box's corners reach (sines of elevation)
static u8 cloud_top_fade;                // how far the clouds thin toward the drum's lid: full at sunset, none by night
static u8 sky_mask[SKY_TEXELS];          // the picture's luminance, then its softened cloud mask
static u8 sky_blur[SKY_TEXELS];
static Mtx rain_mtx[FRAME_CTX * FRAME_SLOTS][NIGHT_RAIN_SHEETS * 2];   // each sheet's two matrices, a set a slot: the frame's, and photos drawn in one frame (as many as six)
static Gfx* const* rain_sheets_now = night_rain_sheets;   // the sheets the next rain takes: the ring's, or the Tunnel's volume
static Gfx* const* snow_sheets_now = night_snow_sheets;
static Mtx glow_mtx[FRAME_CTX * FRAME_SLOTS][1 + NIGHT_FIREFLY_GROUPS];   // the moon path's and the fireflies' matrices, a set a slot
static u8 mist_rgb[3];           // the mist's color: the fog's in force, a little lighter (fog_apply),
static u8 mist_fog_rgb[3];       // the fog's own, and its near and far on the fog's scale, as the mod last set them
static u16 mist_fog_min, mist_fog_max;
static u8 mist_said, mist_said_frame;   // the ride's first mist said in the log, and a whole frame of it
static f32 mist_share;           // the mist at this instant, for the draw (mist_now)
static u16 mist_parts_frame;     // the parts of the record that carried mist this frame,
static u32 mist_corners_frame;   // and their misted corners
static f32 flow_y_sum;           // the water's height: the mean, the lowest and the highest of the vertices that flow
static f32 flow_y_min, flow_y_max;
static u32 flow_y_n;
static u32 water_model;          // the model whose lists hold the water (its first list), once walked
static u8 drawing_water;         // that model is being drawn this frame

// The moon's glint on the water. The water's own triangles are copied as the
// course is first read -- each vertex load and each triangle of the lists
// that draw with the flowing material, node by node -- and drawn again right
// after the water every frame, with an adding blend and the soft dot as the
// moon's reflection. Each corner's place in the dot (its s and t) is set from
// how far the half-way direction between the eye and the moon tilts off the
// water's upright, split along the moon's bearing and across it, so the
// highlight lands where the reflection would, stretched toward the moon as
// the glitter path on real water is, and the dot is read per pixel between
// the corners, so even the Beach's wide sea triangles carry a compact light.
// The copies stand under the game's own matrices, read off the commands it
// wrote for the water's nodes this frame.
#define GLINT_VTX  12288        // a load op keeps its start in 14 bits (bits 12..25): room for 16383
#define GLINT_OPS  24576
#define GLINT_SEGS 128
#define GLINT_LOAD 0x80000000u   // an op that loads copies: start << 12 | count << 6 | first slot
#define GLINT_USED 0x40000000u   // a load a triangle of the water draws from
#define GLINT_FLOW 0x20000000u   // a triangle of the water (else of the land)
#define GLINT_LAND 0x10000000u   // a load a triangle of the land draws from
typedef struct GlintSeg {
    u32 lists[4];           // the node's lists, as the game calls them
    u32 model;              // the model it belongs to (its first list)
    u16 op0, op_n;          // its loads and triangles
    u8 lists_n;
    u8 flow;                // whether any of its triangles are the water's
} GlintSeg;
static Vtx glint_vtx[FRAME_CTX][GLINT_VTX];    // the copies, a set for each frame context (the same corners in each)
static u32 glint_op[GLINT_OPS];
static GlintSeg glint_seg[GLINT_SEGS];
static Mtx glint_mtx[FRAME_CTX][GLINT_SEGS];
static u16 glint_vtx_n, glint_op_n, glint_tris, glint_land;
static u32 glint_load_src[GLINT_OPS];    // where a load's vertices lie in the game's memory (the dusk relights them)
static u32 glint_orig_cn[GLINT_VTX];     // each copy's day colors, as the course has them
static u16 glint_face[GLINT_VTX];        // each copy's lighting: bit 15 lit, 14 steep, 13 water, low nine bits toward
static u8 glint_snow[GLINT_VTX];         // each copy's share of snow, 0 to 255, from its face's slope
static s16 glint_world_y[GLINT_VTX];     // each copy's height in the world, for the surf line at a relight
static s16 glint_world_x[GLINT_VTX], glint_world_z[GLINT_VTX];   // and its place across, for the mist's floors
static u8 sky_mask_pic[2][SKY_TEXELS];   // each sky picture's cloud mask, kept for the dusk
static u8 sky_repaint_due;               // the Valley's pictures to be painted again after this load's first draw (night_sky_ends)
static u8 pal2_a[11][3], pal2_b[11][3], pal2_d[11][3];   // the three palettes for the weather the island turns to
static u8 mix_a[11][3], mix_b[11][3], mix_d[11][3];      // the two weathers' palettes mixed, as far as the turn has come
static u8 palc_a[11][3], palc_b[11][3], palc_d[11][3];   // the three palettes under a clear sky, for a roofed course's cave
static u8 cave_palette[11][3];   // the live colors the cave takes: the same moment under a clear sky
static u8 pal_a[11][3], pal_b[11][3];    // the sunset's palette and the night's,
static u8 pal_d[11][3];                  // and the dawn's: the cycle's three ends (the fourth, the day, is the cartridge)
static f32 dir_a[3], dir_b[3], dir_d[3];
static u8 star_fade = 255;               // how far the stars have come up
static u32 dusk_last;
static u32 sky_paint_last = 0xFFFFFFFFu;     // the ride's tick the sky's pictures were last painted at, in a dusk
static u8 sky_paint_skip;                    // this step leaves the pictures as they are
static u8 glint_seg_n, glint_recording, glint_said, glint_said_drawn;
static u8 drawing_glint;              // the model being drawn has segments of the glint
static u8 drawing_parts;              // the model being drawn has parts in the record at all
static u32 glint_model;               // the model being walked
static u8 glint_slot_ok[64];           // the walker's slots that hold copies,
static u16 glint_slot_op[64];          // and the load that filled each
static u32 glint_top[4];               // the lists a node calls, while it is walked
static s32 glint_top_n;
static u8 glint_drawn[GLINT_SEGS];
static u8 ember_n;                      // the embers seeded so far (the Volcano)
static u8 ember_seeded[GLINT_SEGS];     // the parts whose embers are seeded
static void glint_reset(void) {
    glint_vtx_n = glint_op_n = glint_tris = glint_land = 0;
    glint_seg_n = 0;
    ember_n = 0;
    {
        s32 k;

        for (k = 0; k < GLINT_SEGS; k++) {
            ember_seeded[k] = 0;
        }
    }
    glint_said = 0;
    glint_said_drawn = 0;
}
static f32 walk_node_y;          // the height in the world of the node being walked (its blocks are placed),
static f32 walk_node_x, walk_node_z;   // and its place across
static u8 lit;                   // the walker's reading of G_LIGHTING
static u8 flowing;               // the walker's reading of the material: the one that flows
static u32 slots[64];            // the walker's vertex buffer: where each painted vertex came from
static f32 photo_clock;          // the world's clock as a photo recorded it
static u32 photo_rounds;         // how many times that clock had started over, by the photo's stamp (stamp_of)
static u32 ride_rounds;          // how many times the clock has started over on this ride (clock_rounds)
static f32 ride_clock_last;      // and the clock when that was last looked at
static u16 photo_stamp;          // the sky the photo being drawn was taken under (stamp_of), 0 when it has none
static u16 world_stamp;          // the stamp the world now standing was built under
static u8 stamped;               // that world is a stamped photo's: its weather holds, whatever the rows say
static u32 stamp_weather;        // the stamp's weather, for the world being built
static u8 stamp_weather_set;
static u32 world_weather;        // the weather the ride's world was built under, for its photos' stamps
#define PHOTO_LEVEL 0x800AC0F0u  // the course the game's photo world stands for (D_800AC0F0), -1 for none
static u8 tinted[MAP_BYTES];     // one bit per 8 bytes: this vertex or command is done
static u8 walked[MAP_BYTES];     // one bit per 8 bytes: the model that starts here is done
static Gfx* entry_pos[4];        // where the four command lists stood as a model began
static u32 tex_addr;             // the walker's reading of the picture set to load (G_SETTIMG),
static u32 tex_wide;             // the bytes a unit of its load counts (4 for 32-bit pictures),
static u32 tex_bytes;            // the size of the last block load,
static u32 tlut_addr;            // the palette last loaded and its entries,
static u32 tlut_n;
static u8 tile_fmt, tile_siz;    // and the drawing tile's picture format

static s32 unit_of(u32 addr) {
    if (addr >= LOW_LO && addr < LOW_HI) {
        return (s32) ((addr - LOW_LO) >> 3);
    }
    if (on_the_ride) {
        if (addr >= RIDE_LO && addr < RIDE_HI) {
            return (s32) (LOW_UNITS + ((addr - RIDE_LO) >> 3));
        }
        if (addr >= LEVEL_LO && addr < LEVEL_HI) {
            return (s32) (LOW_UNITS + RIDE_UNITS + ((addr - LEVEL_LO) >> 3));
        }
    } else if (addr >= PHOTO_LO && addr < LEVEL_HI) {
        return (s32) (LOW_UNITS + RIDE_UNITS + ((addr - LEVEL_LO) >> 3));
    }
    return -1;
}

// Marks the address in a map. 1 when it was not marked before; 0 when it
// was, or when the address is outside what a course loads.
static s32 mark(u8* map, u32 addr) {
    s32 unit = unit_of(addr);
    u8 bit;

    if (unit < 0) {
        return 0;
    }
    bit = (u8) (1 << (unit & 7));
    if (map[unit >> 3] & bit) {
        return 0;
    }
    map[unit >> 3] |= bit;
    return 1;
}

static s32 marked(u8* map, u32 addr) {
    s32 unit = unit_of(addr);

    return unit >= 0 && (map[unit >> 3] & (1 << (unit & 7))) != 0;
}

static void clear_maps(void) {
    volatile u8* a = tinted;
    volatile u8* b = walked;
    u32 i;

    for (i = 0; i < MAP_BYTES; i++) {
        a[i] = 0;
        b[i] = 0;
    }
}

static u32 part(u32 value, u32 of256) {
    return (value * of256) >> 8;
}

// A light color as the display list carries it: red, green, blue in the
// top three bytes.
// A light of 255 is the whole: 256 parts, so a day color comes back exact.
static u32 whole(u32 light) {
    return light >= 255 ? 256 : light;
}

static u32 night_word(u32 word) {
    return (part(word >> 24, whole(light_body[0])) << 24) | (part((word >> 16) & 0xFF, whole(light_body[1])) << 16) |
           (part((word >> 8) & 0xFF, whole(light_body[2])) << 8) | (word & 0xFF);
}

// The night's light falls on one painted vertex, once: the scenery's, a
// body's, or -- under the material that flows -- the water's or the lava's.
static u32 snow_lie(u32 share, f32 y);
static void snow_paint(volatile u8* color, s32 toward, u32 share, f32 y);
static u32 ticks_of(const Night* by);
static u32 strike_flash;        // this frame: how strong the flash is, of 255
static u32 strike_bolt;         // how strong the bolt is, of 255
static void strike_at(u32 ticks);
static void storm_light(void);
static void thunder(u32 ticks);
static void rain_sound(void);
static void rain_quiet(void);
static void rain_at(Gfx** list, f32 ex, f32 ey, f32 ez);
static void snow_at(Gfx** list, f32 ex, f32 ey, f32 ez);
static void roof_weather(void);
static void roof_stars(Gfx** list, u32 oc, Mtx* mtx);
static void roof_sky_draw(Gfx** list, u32 oc, Mtx* mtx);
static void roof_volume_draw(Gfx** list, s32 heard);
static void storm_sky_at(Gfx** list, f32 ex, f32 ey, f32 ez);
static void shooting_star_at(Gfx** list, f32 ex, f32 ey, f32 ez);
static f32 clock_minutes(void);
static f32 sky_sin(f32 x);
static f32 sky_cos(f32 x);
static u32 setup_of(void);
static u8 simple_setup(void);
static void aurora_frame(void);

#define SNOW_R 236
#define SNOW_G 240
#define SNOW_B 252

// The Tunnel's painted sky: the faces that show the sky's pictures. Their
// corners take the sky drums' rule at every hour (sky_corner_color): the
// cartridge's paint, white under the overcast, toward the night's tints by
// height; no world light, no sun across them, no steep shade, no snow.
// (JackandBeans's pictures, Oct 15 and 16 2026: the sky in panels of three
// tints under snow, and a lighter panel with a hard edge at dusk.)
static u8 sky_face;

// How high a painted sky's corner stands, 0 at the sky's lowest corner and
// 1 at its lid, from its block's record; below 0 for a corner that is no
// block's (tunnel_roof.inc).
static f32 sky_up(u32 at) {
    s32 b;

    for (b = 0; b < TUNNEL_ROOF_BLOCKS; b++) {
        const TunnelRoof* roof = &tunnel_roofs[b];

        if (at >= roof->vtx_lo && at < roof->vtx_hi) {
            f32 span = (f32) (roof->sky_y_hi - roof->sky_y_lo);
            f32 up;

            if (!roof->open_sky || span < 1.0f) {
                return -1.0f;
            }
            up = ((f32) ((const volatile s16*) at)[1] - (f32) roof->sky_y_lo) / span;
            return up < 0.0f ? 0.0f : up > 1.0f ? 1.0f : up;
        }
    }
    return -1.0f;
}

// A sky corner's color: the cartridge's paint, white under the overcast,
// toward the night's tints by its height, as the sky drums' corners go
// (sky_corners); nightness and overcast_share as they stand.
static void sky_corner_color(volatile u8* color, const u8* paint, f32 up) {
    s32 k;

    for (k = 0; k < 3; k++) {
        f32 day = (f32) paint[k] + (255.0f - (f32) paint[k]) * overcast_share;
        f32 tint = (f32) tint_lo[k] + ((f32) tint_hi[k] - (f32) tint_lo[k]) * up;
        f32 value = day + (tint - day) * nightness;

        color[k] = (u8) (value < 0.0f ? 0 : value > 255.0f ? 255 : (s32) value);
    }
}
// Its cave: the corners under the roof, lit by the hour alone (the cave
// palette), the weather kept to the open air. (JackandBeans, Oct 15 2026:
// the snow's light reached inside the cave.)
static u8 cave_face;
static const u8 no_light[3] = { 0, 0, 0 };     // the cave's directional term: none (JackandBeans's picture, Oct 16 2026: the dusk's sun on an inner wall)

// A roofed course's sky faces, as the walker comes to them (the fine sky).
static u8 fine_open;
static u8 fine_new;
static void fine_open_at(u32 list);
static void fine_face(u32 a, u32 b, u32 c);
static void fine_close(u32 list);
static void fine_roof_refresh(void);

static void light_vertex(u32 at, s32 steep, s32 toward, u32 snow) {
    if (at != 0 && mark(tinted, at)) {
        volatile u8* color = (volatile u8*) (at + 12);
        if (sky_face) {
            f32 up = sky_up(at);

            if (up >= 0.0f) {
                u8 paint[3] = { color[0], color[1], color[2] };

                sky_corner_color(color, paint, up);
                return;
            }
        }
        const u8* light = flowing ? light_flow
                        : (at >= night->world_lo && at < night->world_hi) ? (cave_face ? cave_palette[0] : light_world) : light_body;
        const u8* diff = cave_face ? no_light : land_diff;     // no sun nor moon reaches in

        // The water's height in the world: the vertex's, plus its block's place.
        // Only the course's own water counts: the Zero-One has parts of the
        // same two-picture material.
        if (flowing && at >= night->world_lo && at < night->world_hi) {
            f32 y = (f32) ((volatile s16*) at)[1] + walk_node_y;

            flow_y_sum += y;
            if (flow_y_n == 0 || y > flow_y_max) {
                flow_y_max = y;
            }
            if (flow_y_n == 0 || y < flow_y_min) {
                flow_y_min = y;
            }
            flow_y_n++;
        }

        // Under snow, the land's painted colors (much of the land's color is
        // painted, over gray pictures) go toward snow as far as the face's
        // slope allows, a dusting where it stands steep: there the rock
        // shows through, a little darker and colder.
        if (snow_cover && !flowing && snow > 0) {
            snow_paint(color, toward, snow, (f32) ((volatile s16*) at)[1] + walk_node_y);
        }
        {
            s32 k;

            for (k = 0; k < 3; k++) {
                u32 lit = light[k] + (flowing ? 0 : ((diff[k] * (u32) toward) >> 8));

                color[k] = (u8) part(color[k], whole(lit));
            }
        }
        if (steep) {
            color[0] = (u8) part(color[0], steep_shade[0]);
            color[1] = (u8) part(color[1], steep_shade[1]);
            color[2] = (u8) part(color[2], steep_shade[2]);
            if (snow_cover && !cave_face) {
                // Rock in snow weather: a little darker and bluer at any
                // hour, so the white above it reads as snow and not as paint.
                color[0] = (u8) part(color[0], snow_rock[0]);
                color[1] = (u8) part(color[1], snow_rock[1]);
                color[2] = (u8) part(color[2], snow_rock[2]);
            }
        }
    }
}

// Snow lies on the land. Under snow, the land's own pictures are painted
// over white where they lie, once each, as the first triangle of the land
// that shows them is met: a picture drawn through a palette has the palette
// painted; one of plain colors, or of intensities, has its texels painted.
// The Pokémon and the things that move are lit by the scene's light, not
// painted, so their pictures are never touched; nor is the water's, nor
// the sky's.

// Where the snow lies thinner. On the Volcano a dusting only, the ground
// warm: the share is held under half, so the rock shows through. On a flat
// water's shore, the sand within SURF units above the water stays wet and
// bare, the surf line (the Beach's sea; a river that falls has no one
// height, and keeps its banks). The water's height is known once its first
// vertex is lit, and the record is lit again then (snow_relit).
#define SURF 14.0f
static u32 snow_lie(u32 share, f32 y) {
    if (night->embers && share > 112) {
        share = 112;
    }
    if (flow_y_n > 0 && (flow_y_max - flow_y_min) < 12.0f) {
        f32 above = y - flow_y_max;

        if (above < SURF) {
            share = (above <= 2.0f) ? 0 : (u32) ((f32) share * (above - 2.0f) / (SURF - 2.0f));
        }
    }
    return share;
}

// A painted color under snow: it goes toward the snow's color as far as the
// share says. The snow's own color turns with the face, cool and a little
// darker away from the light, warm and bright toward it, which is what gives
// a blanket its form under a flat sky (by day the cartridge's light leaves
// the painted colors as they are, so nothing else would). The painted
// color's brightness keeps a small say, so a blanket is not one flat white.
static void snow_paint(volatile u8* color, s32 toward, u32 share, f32 y) {
    static const u8 cool[3] = { 196, 206, 236 };
    static const u8 warm[3] = { 250, 250, 254 };
    u32 lum = (color[0] * 77 + color[1] * 151 + color[2] * 28) >> 8;
    u32 bright = 216 + (40 * lum) / 255;
    s32 k;

    share = snow_lie(share, y);
    if (share > 256) {
        share = 256;
    }
    for (k = 0; k < 3; k++) {
        u32 snow = (((u32) cool[k] + (((u32) warm[k] - cool[k]) * (u32) toward) / 256) * bright) >> 8;
        u32 value = ((u32) color[k] * (256 - share) + snow * share) >> 8;

        color[k] = (u8) (value > 255 ? 255 : value);
    }
}

// The land's pictures under snow: their range of brightness is lifted and
// compressed, not painted white, so the same picture reads as snow under a
// white vertex and as rock under a steep face's darker one (a picture is
// shared by every face that shows it). A quarter of the color stays.
static u32 snow_lift(u32 channel, u32 lum) {
    u32 lift = 128 + (lum * 112) / 255;

    return (channel * 64 + lift * 192) >> 8;
}

static void snow_colors(volatile u16* texel, u32 count) {
    u32 i;

    for (i = 0; i < count; i++) {
        u32 t = texel[i];
        u32 r = (((t >> 11) & 31) * 255) / 31;
        u32 g = (((t >> 6) & 31) * 255) / 31;
        u32 b = (((t >> 1) & 31) * 255) / 31;
        u32 lum = (r * 77 + g * 151 + b * 28) >> 8;

        r = snow_lift(r, lum);
        g = snow_lift(g, lum);
        b = snow_lift(b, lum);
        texel[i] = (u16) (((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (t & 1));
    }
}

static void snow_intensities(volatile u8* value, u32 count, u32 step) {
    u32 i;

    for (i = 0; i < count; i++) {
        value[i * step] = (u8) snow_lift(value[i * step], value[i * step]);
    }
}

static void snow_picture(void) {
    if (tex_addr == night->pics[0] || tex_addr == night->pics[1]) {
        return;
    }
    // A roofed course: a picture the cave's faces show too (the rock's, shared
    // by the walls inside and the cliffs outside) stays as it is, or the cave
    // would brighten with the snow it never sees.
    if (ROOFED(night)) {
        u32 key = (tile_fmt == 2) ? tlut_addr : tex_addr;
        s32 i;

        for (i = 0; i < TUNNEL_CAVE_PICTURES; i++) {
            if (tunnel_cave_pictures[i] == key) {
                return;
            }
        }
    }
    switch (tile_fmt) {
        case 2:     // color index: the palette
            if (tlut_addr != 0 && mark(tinted, tlut_addr)) {
                snow_colors((volatile u16*) tlut_addr, tlut_n);
            }
            break;
        case 0:     // red, green, blue, alpha
            if (tile_siz == 2 && mark(tinted, tex_addr)) {
                snow_colors((volatile u16*) tex_addr, tex_bytes / 2);
            }
            break;
        case 3:     // intensity and alpha
            if (tile_siz == 2 && mark(tinted, tex_addr)) {
                snow_intensities((volatile u8*) tex_addr, tex_bytes / 2, 2);
            }
            break;
        case 4:     // intensity
            if (tile_siz == 1 && mark(tinted, tex_addr)) {
                snow_intensities((volatile u8*) tex_addr, tex_bytes, 1);
            }
            break;
        default:
            break;
    }
}

// A face that stands steep holds no snow: its corners keep a darker, colder
// shade. 1 when less than a third of the face's normal stands upright.
static s32 steep_face(u32 a, u32 b, u32 c) {
    const volatile s16* A = (const volatile s16*) a;
    const volatile s16* B = (const volatile s16*) b;
    const volatile s16* C = (const volatile s16*) c;
    f32 ux = (f32) (B[0] - A[0]), uy = (f32) (B[1] - A[1]), uz = (f32) (B[2] - A[2]);
    f32 vx = (f32) (C[0] - A[0]), vy = (f32) (C[1] - A[1]), vz = (f32) (C[2] - A[2]);
    f32 nx = uy * vz - uz * vy;
    f32 ny = uz * vx - ux * vz;
    f32 nz = ux * vy - uy * vx;
    f32 all = nx * nx + ny * ny + nz * nz;

    return all > 1.0f && ny * ny < 0.12f * all;
}

// How much snow a face holds, 0 to 256, by its slope: all of it on ground
// within 32 degrees of flat, a dusting past 66 degrees, and between the two
// a snow line, where a hillside goes from white to its dusted rock. The
// dusting is what a wall holds in a snowfall, rime and the snow on its
// ledges and in its cracks: it stays its own rock, lighter and cooler. (To
// Oct 20 2026 a face past 66 degrees held none.)
#define SNOW_DUST 92            // the dusting, of 256
static u32 snow_share_face(u32 a, u32 b, u32 c) {
    const volatile s16* A = (const volatile s16*) a;
    const volatile s16* B = (const volatile s16*) b;
    const volatile s16* C = (const volatile s16*) c;
    f32 ux = (f32) (B[0] - A[0]), uy = (f32) (B[1] - A[1]), uz = (f32) (B[2] - A[2]);
    f32 vx = (f32) (C[0] - A[0]), vy = (f32) (C[1] - A[1]), vz = (f32) (C[2] - A[2]);
    f32 nx = uy * vz - uz * vy;
    f32 ny = uz * vx - ux * vz;
    f32 nz = ux * vy - uy * vx;
    f32 all = nx * nx + ny * ny + nz * nz;
    f32 up, share;

    if (all <= 1.0f) {
        return 256;
    }
    up = __builtin_sqrtf((ny * ny) / all);
    share = (up - 0.40f) / 0.45f;
    share = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
    return SNOW_DUST + (u32) (share * (f32) (256 - SNOW_DUST));
}

// On a roofed course (the Tunnel), a block's corner is under the open sky
// where its block's ceiling map has no ceiling over its cell, or where it
// stands above the ceiling there (the hill over the cave); the cave is the
// rest, the track with it. What is not a block's (the scenery set about)
// is told from the rock by its painting: the things outside are painted
// bright, the cave's dark, so a corner is open where every channel of its
// paint reads 120 or more. (The paint alone was the rule to Oct 15 2026,
// and some cave walls are painted bright: snow lay on them.)
static u8 open_air(u32 at) {
    const volatile s16* v = (const volatile s16*) at;
    volatile u8* c = (volatile u8*) (at + 12);
    s32 b;

    if (!ROOFED(night)) {
        return 1;
    }
    for (b = 0; b < TUNNEL_ROOF_BLOCKS; b++) {
        const TunnelRoof* roof = &tunnel_roofs[b];

        if (at >= roof->vtx_lo && at < roof->vtx_hi) {
            s32 cx, cz;
            u32 top;

            if (!roof->open_sky) {
                return 0;       // no painted sky over this block: the hall that draws no ceiling is cave too
            }
            if (v[0] < roof->x0 || v[2] < roof->z0) {
                return 1;
            }
            cx = ((s32) v[0] - roof->x0) / TUNNEL_ROOF_CELL;
            cz = ((s32) v[2] - roof->z0) / TUNNEL_ROOF_CELL;
            if (cx >= roof->w || cz >= roof->h) {
                return 1;
            }
            top = roof->cells[cz * roof->w + cx];
            return top == 0 || (s32) v[1] > (s32) top * 16 + 16;
        }
    }
    return c[0] >= 120 && c[1] >= 120 && c[2] >= 120;
}

// How much a face faces the light (the sun, or the moon), 0 to 256: the
// face's normal from its corners' winding against relight_dir.
static s32 face_toward(u32 a, u32 b, u32 c) {
    const volatile s16* A = (const volatile s16*) a;
    const volatile s16* B = (const volatile s16*) b;
    const volatile s16* C = (const volatile s16*) c;
    f32 ux = (f32) (B[0] - A[0]), uy = (f32) (B[1] - A[1]), uz = (f32) (B[2] - A[2]);
    f32 vx = (f32) (C[0] - A[0]), vy = (f32) (C[1] - A[1]), vz = (f32) (C[2] - A[2]);
    f32 nx = uy * vz - uz * vy;
    f32 ny = uz * vx - ux * vz;
    f32 nz = ux * vy - uy * vx;
    f32 len = __builtin_sqrtf(nx * nx + ny * ny + nz * nz);
    f32 d;

    if (len < 1.0f) {
        return 0;
    }
    d = (nx * relight_dir[0] + ny * relight_dir[1] + nz * relight_dir[2]) / len;
    if (d <= 0.0f) {
        return 0;
    }
    return (s32) (d * 256.0f);
}

// A corner's lighting, kept with its copy for the dusk: the first face that
// lights it decides, as light_vertex does.
static void glint_face_set(u32 slot, s32 steep, s32 toward, u32 snow) {
    u32 o = glint_op[glint_slot_op[slot]];
    u32 copy = ((o >> 12) & 0x3FFF) + (slot - (o & 63));

    if (copy < GLINT_VTX && glint_face[copy] == 0) {
        glint_face[copy] = (u16) (0x8000u | (steep ? 0x4000u : 0u) | (flowing ? 0x2000u : 0u) | (cave_face ? 0x1000u : 0u)
                                  | (sky_face ? 0x0800u : 0u) | ((u32) toward & 0x1FFu));
        glint_snow[copy] = (u8) (snow > 255 ? 255 : snow);
        glint_world_y[copy] = (s16) (((volatile s16*) slots[slot])[1] + walk_node_y);
        glint_world_x[copy] = (s16) (((volatile s16*) slots[slot])[0] + walk_node_x);
        glint_world_z[copy] = (s16) (((volatile s16*) slots[slot])[2] + walk_node_z);
    }
}

// The three corners of a triangle command's word. Under snow, a triangle of
// the land also brings the picture it shows under the snow.
static void light_corners(u32 word) {
    u32 a = ((word >> 16) & 0xFF) >> 1;
    u32 b = ((word >> 8) & 0xFF) >> 1;
    u32 c = (word & 0xFF) >> 1;

    if (a < 64 && b < 64 && c < 64) {
        s32 steep = 0;
        s32 toward = 0;
        u32 snow = 0;

        sky_face = (ROOFED(night) && tex_addr != 0 && (tex_addr == night->pics[0] || tex_addr == night->pics[1]));
        cave_face = (ROOFED(night) && !sky_face && slots[a] != 0 && !open_air(slots[a]));
        if (sky_face && fine_open) {
            fine_face(slots[a], slots[b], slots[c]);    // before they are lit: the first corner's paint is the day's
        }
        if (!flowing && slots[a] != 0 && slots[b] != 0 && slots[c] != 0) {
            if (snow_cover && !sky_face && open_air(slots[a])) {
                if (tex_addr != 0) {
                    snow_picture();
                }
                steep = steep_face(slots[a], slots[b], slots[c]);
                snow = snow_share_face(slots[a], slots[b], slots[c]);
            }
            toward = face_toward(slots[a], slots[b], slots[c]);
        }
        if (glint_recording && glint_slot_ok[a] && glint_slot_ok[b] && glint_slot_ok[c] && glint_op_n < GLINT_OPS) {
            u32 used = flowing ? GLINT_USED : GLINT_LAND;

            glint_op[glint_op_n++] = (a << 16) | (b << 8) | c | (flowing ? GLINT_FLOW : 0);
            glint_face_set(a, steep, toward, snow);
            glint_face_set(b, steep, toward, snow);
            glint_face_set(c, steep, toward, snow);
            glint_op[glint_slot_op[a]] |= used;
            glint_op[glint_slot_op[b]] |= used;
            glint_op[glint_slot_op[c]] |= used;
            if (flowing) {
                glint_tris++;
            } else {
                glint_land++;
            }
        }
        light_vertex(slots[a], steep, toward, snow);
        light_vertex(slots[b], steep, toward, snow);
        light_vertex(slots[c], steep, toward, snow);
        sky_face = 0;
        cave_face = 0;
    }
}

// The two-picture material is the courses' moving water (and the Volcano's
// lava), and also the River's three shimmering panels of rock and grass, where
// Porygon hides: set into the walls, they take the same combiner and a palette
// of their own, loaded in the list after it. The water's palettes are blue,
// or it has none (its pictures are full color); the lava's is red. A palette
// that is neither, loaded under that combiner, is earth: the panel is land,
// lit as the walls around it and snowed on as they are. (To Oct 20 2026 the
// panels were water to the mod: no snow lay on them, and the rock panel
// stood bare and brown in a white wall.) Read through all seven courses'
// data: only those three panels pass.
static s32 earthen_palette(u32 at, u32 n) {
    u32 r = 0, g = 0, b = 0;
    u32 i;

    if ((at & 1) || n == 0 || n > 256 || unit_of(at) < 0 || unit_of(at + 2 * (n - 1)) < 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        u32 t = ((const volatile u16*) at)[i];

        r += (((t >> 11) & 31) * 255) / 31;
        g += (((t >> 6) & 31) * 255) / 31;
        b += (((t >> 1) & 31) * 255) / 31;
    }
    return b < g && r < 2 * g;
}

// Reads a display list of the course's own data and gives the night's light
// to what colors it: the vertices loaded while lighting is off (their fourth
// word is a color then, a normal otherwise), each as a triangle first uses
// it, and the light colors a list sets for itself. The material in force is
// followed by its combiner: the one that blends two pictures is the one the
// courses use for what flows. Lists reached by a call or by the near/far
// branch are read too.
static void walk(u32 list, s32 depth) {
    u32 half = 0;
    s32 budget = 20000;

    if (depth > 12 || (list & 7) || unit_of(list) < 0) {
        return;
    }
    while (budget-- > 0 && unit_of(list) >= 0) {
        volatile u32* cmd = (volatile u32*) list;
        u32 w0 = cmd[0];
        u32 w1 = cmd[1];

        // A run of a sky's faces ends at the first command that is not geometry.
        if (fine_open && (w0 >> 24) != 0x01 && (w0 >> 24) != 0x05 && (w0 >> 24) != 0x06 && (w0 >> 24) != 0x07) {
            fine_close(list);
        }
        switch (w0 >> 24) {
            case 0x01: {    // G_VTX: count, and one past the last slot filled
                u32 count = (w0 >> 12) & 0xFF;
                u32 first = ((w0 >> 1) & 0x7F) - count;
                u32 i;

                if (!fine_open && ROOFED(night) && tex_addr != 0 && (tex_addr == night->pics[0] || tex_addr == night->pics[1])) {
                    fine_open_at(list);
                }
                for (i = 0; i < count && first + i < 64; i++) {
                    slots[first + i] = (lit || (w1 & 7)) ? 0 : w1 + 16 * i;
                }
                if (glint_recording && !lit && (w1 & 7) == 0 && w1 >= night->world_lo && w1 < night->world_hi
                    && count <= 32 && glint_vtx_n + count <= GLINT_VTX && glint_op_n < GLINT_OPS) {
                    const volatile u32* src = (const volatile u32*) w1;
                    u32* dst0 = (u32*) &glint_vtx[0][glint_vtx_n];
                    u32* dst1 = (u32*) &glint_vtx[1][glint_vtx_n];

                    for (i = 0; i < count * 4; i++) {
                        dst0[i] = dst1[i] = src[i];
                    }
                    for (i = 0; i < count && first + i < 64; i++) {
                        glint_slot_ok[first + i] = 1;
                        glint_slot_op[first + i] = glint_op_n;
                    }
                    glint_load_src[glint_op_n] = w1;
                    for (i = 0; i < count; i++) {
                        glint_orig_cn[glint_vtx_n + i] = ((u32*) &glint_vtx[0][glint_vtx_n + i])[3];
                        glint_face[glint_vtx_n + i] = 0;
                    }
                    glint_op[glint_op_n++] = GLINT_LOAD | ((u32) glint_vtx_n << 12) | (count << 6) | (first & 63);
                    glint_vtx_n = (u16) (glint_vtx_n + count);
                } else {
                    for (i = 0; i < count && first + i < 64; i++) {
                        glint_slot_ok[first + i] = 0;
                    }
                }
                break;
            }
            case 0x05:      // G_TRI1
                light_corners(w0);
                break;
            case 0x06:      // G_TRI2
            case 0x07:      // G_QUAD
                light_corners(w0);
                light_corners(w1);
                break;
            case 0xFC:      // G_SETCOMBINE
                flowing = ((w0 & 0x00FFFFFF) == 0x272C04 && w1 == 0x1F0C93FF);
                break;
            case 0xFD:      // G_SETTIMG: the picture to load next
                tex_addr = w1;
                tex_wide = (((w0 >> 19) & 3) == 3) ? 4 : 2;
                break;
            case 0xF3:      // G_LOADBLOCK: the load's size, in its own units
                tex_bytes = (((w1 >> 12) & 0xFFF) + 1) * tex_wide;
                break;
            case 0xF0:      // G_LOADTLUT: a palette, and its entries
                tlut_addr = tex_addr;
                tlut_n = ((w1 >> 14) & 0x3FF) + 1;
                if (flowing && earthen_palette(tlut_addr, tlut_n)) {
                    flowing = 0;        // a panel of rock or grass, not water
                }
                break;
            case 0xF5:      // G_SETTILE, the drawing tile: the picture's format
                if (((w1 >> 24) & 7) == 0) {
                    tile_fmt = (u8) ((w0 >> 21) & 7);
                    tile_siz = (u8) ((w0 >> 19) & 3);
                }
                break;
            case 0xD9:      // G_GEOMETRYMODE: clear, then set
                if (~w0 & G_LIGHTING_BIT) {
                    lit = 0;
                }
                if (w1 & G_LIGHTING_BIT) {
                    lit = 1;
                }
                break;
            case 0xDB:      // G_MOVEWORD, light color
                if (((w0 >> 16) & 0xFF) == 0x0A && mark(tinted, list)) {
                    cmd[1] = night_word(w1);
                }
                break;
            case 0xE1:      // G_RDPHALF_1: the target of the branch that follows
                half = w1;
                break;
            case 0x04:      // G_BRANCH_Z: the near or far list
                walk(half, depth + 1);
                break;
            case 0xDE:      // G_DL
                walk(w1, depth + 1);
                if ((w0 >> 16) & 0xFF) {
                    return;
                }
                break;
            case 0xDF:      // G_ENDDL
                return;
            default:
                break;
        }
        list += 8;
    }
}

static DObj* after(DObj* node) {
    if (node->firstChild != NULL) {
        return node->firstChild;
    }
    while (node != NULL && (u32) node != 1) {
        if (node->next != NULL) {
            return node->next;
        }
        node = node->parent;
    }
    return NULL;
}

// A list a node calls: remembered, so the glint can find the node's matrix
// among the commands the game writes, then read.
static void walk_top(u32 list) {
    if (glint_top_n < 4) {
        glint_top[glint_top_n++] = list;
    }
    walk(list, 0);
}

static void walk_payload(DObj* node, s32 kind) {
    void* payload = node->payload.any;
    DObj* up = node;
    s32 guard = 8;
    u16 op_before, tris_before, land_before;

    if (payload == NULL) {
        return;
    }
    walk_node_x = walk_node_y = walk_node_z = 0.0f;
    while (up != NULL && (u32) up != 1 && guard-- > 0) {
        walk_node_x += up->position.v.x;
        walk_node_y += up->position.v.y;
        walk_node_z += up->position.v.z;
        up = up->parent;
    }
    glint_top_n = 0;
    op_before = glint_op_n;
    tris_before = glint_tris;
    land_before = glint_land;
    {
        s32 k;

        for (k = 0; k < 64; k++) {
            glint_slot_ok[k] = 0;
        }
    }
    switch (kind) {
        case 'A':
        case 'B':
            walk_top((u32) node->payload.dlist);
            break;
        case 'C':
        case 'D': {
            DObjPayloadTypeC* entry = node->payload.typeC;
            s32 guard = 8;

            while (unit_of((u32) entry) >= 0 && entry->dlistID != 4 && guard-- > 0) {
                walk_top((u32) entry->dlist);
                entry++;
            }
            break;
        }
        case 'I': {
            DObjPayloadTypeI* pair = node->payload.typeI;

            if (unit_of((u32) pair) >= 0) {
                walk_top((u32) pair->unk_00);
                walk_top((u32) pair->unk_04);
            }
            break;
        }
        case 'J': {
            DObjPayloadTypeJ* entry = node->payload.typeJ;
            s32 guard = 8;

            while (unit_of((u32) entry) >= 0 && entry->dlistID != 4 && guard-- > 0) {
                walk_top((u32) entry->unk_04);
                walk_top((u32) entry->unk_08);
                entry++;
            }
            break;
        }
    }
    // The node drew water: its loads and triangles are a segment of the glint.
    if (glint_recording && (glint_tris > tris_before || glint_land > land_before) && glint_seg_n < GLINT_SEGS) {
        GlintSeg* seg = &glint_seg[glint_seg_n++];
        s32 k;

        for (k = 0; k < glint_top_n; k++) {
            seg->lists[k] = glint_top[k];
        }
        seg->lists_n = (u8) glint_top_n;
        seg->model = glint_model;
        seg->op0 = op_before;
        seg->op_n = (u16) (glint_op_n - op_before);
        seg->flow = (glint_tris > tris_before);
    }
}

static void set_light(const Night* by);
static void fog_apply(void);
static u8 roof_placed;           // this frame's camera has been placed against the roof
static u8 roof_photo_said;       // a photo world's place said in the log

// A roofed course's storm strikes only while the camera is under the open sky.
static void roof_strike(void) {
    if (!storming) {
        return;
    }
    if (roof_outside) {
        strike_at(ticks_of(night));
    } else {
        strike_flash = strike_bolt = 0;
    }
}

// The camera against a roofed course's roof: which volume of weather stands
// (roof_volume), the first block's origin in the current block's frame
// (roof_shift), how far under the roof the eye stands (roof_past, the lesser
// of how far it is in past the entry's roof line and how far short of the
// exit's), and whether it is under the open sky (roof_outside).
static void roof_place(GObj* cam) {
    volatile f32* eye = (volatile f32*) ((u32) cam->data.cam + 0x3C);
    u32 block = *(volatile u32*) 0x800E6AD0u;          // CurrentWorldBlock (world/block.c); its index first
    s32 index = (block != 0) ? *(volatile s32*) block : 0;
    f32 cx, cz, entry_past, exit_past;

    roof_placed = 1;
    roof_cam = cam;
    roof_volume = (index <= 1) ? 1 : (index >= TUNNEL_ROOF_BLOCKS - 2) ? 2 : 0;
    roof_shift[0] = roof_shift[1] = roof_shift[2] = 0.0f;
    if (block != 0) {
        u32 desc = *(volatile u32*) (block + 4);        // its descriptor: worldPos at +4, in hundredths

        if (desc != 0) {
            roof_shift[0] = -*(volatile f32*) (desc + 4) * 100.0f;
            roof_shift[1] = -*(volatile f32*) (desc + 8) * 100.0f;
            roof_shift[2] = -*(volatile f32*) (desc + 12) * 100.0f;
        }
    }
    cx = eye[0] - roof_shift[0];
    cz = eye[2] - roof_shift[2];
    entry_past = cz - night->roof_z;
    exit_past = NIGHT_TUNNEL_EXIT_ORIGIN_X + NIGHT_TUNNEL_EXIT_X - cx;
    roof_outside = ((index == 0) && (entry_past < 0.0f)) || (exit_past < 0.0f);
    roof_past = (entry_past < exit_past) ? entry_past : exit_past;
    if (roof_past < 0.0f) {
        roof_past = 0.0f;
    }
}


// ---- The Pokémon outside Day ----
// The courses' Pokémon are drawn from their own lists: the ride's in the course's
// code, a photo's from the course's extra models. Outside Day four things went wrong
// in them (Nov 7 2026; JackandBeans's pictures of Moltres at night, half of him dark):
// - Moltres and Rapidash are drawn without the game's fog wrapper, every other
//   Pokémon with it. Moltres, lit by nothing but his own fire, has parts that switch
//   fog on and none off, so how much of him fogged hung on what was drawn just before,
//   and under the night's dark fog the fogged half went dark: the dark wing was the
//   bright one taken 83 parts in 100 toward the fog's color. Outside Day his switches
//   switch nothing and he is drawn unfogged from the start: fire stays bright at any
//   distance. Rapidash's body is lit as any Pokémon's: outside Day it takes the
//   game's own fog wrapper, as the others do, and only its flames, which turn the fog
//   off themselves, stay clear. What was drawn before never decides their fog. A
//   fade the game draws with a fog of its own (app_level/pokemon.c) is left to it.
// - A part may set the lights' colors for itself (a flame, a spark, Electrode's
//   material) and leave them set: every lit thing drawn after it in the frame took
//   those gray day colors, a day's key over a fill of a sixth, instead of the hour's,
//   until the camera loaded the lights again. Outside Day the hour's lights are
//   loaded again after such a part.
// - A flame lit by the key went dark on its far side: the Volcano's flames' own
//   colors are a fill nearly whole outside Day, so a flame is even all round.
// - The parts the cartridge paints rather than lights kept the day's colors, or were
//   lit once for the hour the course loaded under: now they take the hour's light as
//   the land's painted corners do, and what glows keeps its color (fire, sparks,
//   Starmie's gem).
// At Day nothing here writes a word: the cartridge's lists, colors and commands are
// as it has them.
#define FOG_BIT 0x00010000u
#define FIRE_MOLTRES 0x8032A5E8u        // the first payloads of the model drawn as fire: the ride's and the photo's
#define FIRE_MOLTRES_HD 0x801B2680u
#define GLOW_RAPIDASH 0x80330568u       // the model whose painted parts glow: Rapidash's flames
#define GLOW_RAPIDASH_HD 0x801B6638u    // (the two ids once kept for Electrode's blast name no model: no word of the cartridge holds them)
#define FOG_SET_LIST 0x800AEA80u        // the game's fog wrapper (DListRMFogOpaSet, app_render/4CDB0.c), its fog
#define FOG_CLEAR_LIST 0x800AEAB8u      // brought up to date by setFogColor and setFogDistance; its end (DListRMFogOpaClear)
#define NOFOG_SET_LIST 0x800AEB40u      // and the game's own unfogged wrapper (DListRMNoFogOpaSet)
#define STARMIE_GEM 0x80171858u         // the Valley's Starmie: its gem's picture, drawn unlit (it glows)
#define FIRE_KEY_DAY 0x9F9F9F00u        // a Volcano flame's own key and fill, as the cartridge has them,
#define FIRE_FILL_DAY 0x26262600u
#define FIRE_KEY_NIGHT 0x37373700u      // and outside Day: a fill nearly whole and a little key
#define FIRE_FILL_NIGHT 0xC8C8C800u
#define ACTOR_WORDS 160
#define ACTOR_PAINTS 384
#define ACTOR_MODELS 64
#define ACTOR_LEAKS 96

typedef struct ActorWord {
    u32 at;         // a list's second word
    u32 day;        // as the cartridge has it
    u8 kind;        // what the hour makes of it
} ActorWord;
#define AW_FOG 1    // a fire model's fog switch: outside Day it switches nothing
#define AW_KEY 2    // a flame's own key color
#define AW_FILL 3   // a flame's own fill

typedef struct ActorPaint {
    u32 at;         // a painted corner's color
    u8 day[3];      // as the cartridge has it
    s8 n[3];        // its face's facing, in the model's own frame
} ActorPaint;

static ActorWord actor_words[ACTOR_WORDS];
static s32 actor_word_n;
static ActorPaint actor_paints[ACTOR_PAINTS];
static s32 actor_paint_n;
static u32 actor_models[ACTOR_MODELS];     // the models read this load, by their first payload
static s32 actor_model_n;
static u32 actor_leaks[ACTOR_LEAKS];       // lists that set the lights' colors
static s32 actor_leak_n;
static u8 actor_full_said;
static u8 actor_fire;                      // the model being read or drawn is fire
static u8 actor_blast;                     // its painted parts glow
static u8 actor_lit, actor_fog;            // the reader's lighting and fog bits
static u32 actor_tex;                      // the picture the reader saw loaded last
static u32 actor_slots[64];                // the corners loaded, while lighting is off
static u8 actor_on;                        // a Pokémon is being drawn outside Day
static u8 actor_wrapped;                   // and the mod put the game's fog wrapper around it
static u8 actor_rapidash;                  // the model is Rapidash, drawn without the wrapper and lit
static Gfx* actor_seen[4];                 // how far each of the frame's lists has been looked at

static s32 actor_list(u32 at) {
    if (night == NULL || (at & 3)) {
        return 0;
    }
    return (at >= night->code_lo && at < night->code_hi) || (at >= night->extra_lo && at < night->extra_hi);
}

static void actor_word_add(u32 at, u32 day, u8 kind) {
    s32 i;

    for (i = 0; i < actor_word_n; i++) {
        if (actor_words[i].at == at) {
            return;
        }
    }
    if (actor_word_n >= ACTOR_WORDS) {
        actor_full_said |= 1;
        return;
    }
    actor_words[actor_word_n].at = at;
    actor_words[actor_word_n].day = day;
    actor_words[actor_word_n].kind = kind;
    actor_word_n++;
}

static void actor_paint_add(u32 at, const f32* n) {
    volatile u8* color = (volatile u8*) (at + 12);
    ActorPaint* p;
    s32 i;

    for (i = 0; i < actor_paint_n; i++) {
        if (actor_paints[i].at == at) {
            return;
        }
    }
    if (actor_paint_n >= ACTOR_PAINTS) {
        actor_full_said |= 2;
        return;
    }
    p = &actor_paints[actor_paint_n++];
    p->at = at;
    p->day[0] = color[0];
    p->day[1] = color[1];
    p->day[2] = color[2];
    p->n[0] = (s8) (n[0] * 127.0f);
    p->n[1] = (s8) (n[1] * 127.0f);
    p->n[2] = (s8) (n[2] * 127.0f);
}

static void actor_leak_add(u32 list) {
    s32 i;

    for (i = 0; i < actor_leak_n; i++) {
        if (actor_leaks[i] == list) {
            return;
        }
    }
    if (actor_leak_n >= ACTOR_LEAKS) {
        actor_full_said |= 4;
        return;
    }
    actor_leaks[actor_leak_n++] = list;
}

// A painted triangle: its corners take the hour's light, unless what it shows glows.
static void actor_face(u32 word) {
    u32 a = ((word >> 16) & 0xFF) >> 1;
    u32 b = ((word >> 8) & 0xFF) >> 1;
    u32 c = (word & 0xFF) >> 1;

    if (actor_lit || a >= 64 || b >= 64 || c >= 64 || actor_slots[a] == 0 || actor_slots[b] == 0 || actor_slots[c] == 0) {
        return;
    }
    // Fire, a blast, a material the cartridge draws with the fog off as well (a spark,
    // an effect), and Starmie's gem keep their own color.
    if (actor_fire || actor_blast || !actor_fog || (night->setup == NIGHT_VALLEY_SETUP && actor_tex == STARMIE_GEM)) {
        return;
    }
    {
        const volatile s16* A = (const volatile s16*) actor_slots[a];
        const volatile s16* B = (const volatile s16*) actor_slots[b];
        const volatile s16* C = (const volatile s16*) actor_slots[c];
        f32 ux = (f32) (B[0] - A[0]), uy = (f32) (B[1] - A[1]), uz = (f32) (B[2] - A[2]);
        f32 vx = (f32) (C[0] - A[0]), vy = (f32) (C[1] - A[1]), vz = (f32) (C[2] - A[2]);
        f32 n[3];
        f32 len;

        n[0] = uy * vz - uz * vy;
        n[1] = uz * vx - ux * vz;
        n[2] = ux * vy - uy * vx;
        len = __builtin_sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1.0f) {
            n[0] = n[2] = 0.0f;
            n[1] = 1.0f;
        } else {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        }
        actor_paint_add(actor_slots[a], n);
        actor_paint_add(actor_slots[b], n);
        actor_paint_add(actor_slots[c], n);
    }
}

// Reads a Pokémon's list, and what it calls, writing nothing: its fire's fog switches,
// its flames' own light colors, its painted corners. Says whether it sets the lights'
// colors itself, and keeps such lists.
static s32 actor_read(u32 list, s32 depth) {
    u32 start = list;
    u32 half = 0;
    s32 budget = 4000;
    s32 sets = 0;
    s32 i;

    if (depth > 10 || (list & 7) || !actor_list(list)) {
        return 0;
    }
    for (i = 0; i < actor_leak_n; i++) {
        if (actor_leaks[i] == start) {
            return 1;
        }
    }
    // Plain tests, not a switch: a switch compiles to a jump table, and the runtime's mod
    // recompiler did not resolve one in cut 34b ("Failed to find function at 0x8100EA78",
    // JackandBeans's crash, Nov 8 2026).
    while (budget-- > 0 && actor_list(list)) {
        volatile u32* cmd = (volatile u32*) list;
        u32 w0 = cmd[0];
        u32 w1 = cmd[1];
        u32 op = w0 >> 24;

        if (op == 0xDF) {
            break;
        }
        if (op == 0x01) {           // G_VTX
            u32 count = (w0 >> 12) & 0xFF;
            u32 first = ((w0 >> 1) & 0x7F) - count;

            for (i = 0; i < (s32) count && first + i < 64; i++) {
                actor_slots[first + i] = (actor_lit || !actor_list(w1)) ? 0 : w1 + 16 * i;
            }
        } else if (op == 0x05) {
            actor_face(w0);
        } else if (op == 0x06 || op == 0x07) {
            actor_face(w0);
            actor_face(w1);
        } else if (op == 0xFD) {
            actor_tex = w1;
        } else if (op == 0xD9) {    // G_GEOMETRYMODE: clear, then set
            u32 clear = ~w0 & 0x00FFFFFFu;

            if (clear & G_LIGHTING_BIT) {
                actor_lit = 0;
            }
            if (w1 & G_LIGHTING_BIT) {
                actor_lit = 1;
            }
            if (clear & FOG_BIT) {
                actor_fog = 0;
            }
            if (w1 & FOG_BIT) {
                actor_fog = 1;
                if (actor_fire) {
                    actor_word_add((u32) &cmd[1], w1, AW_FOG);
                }
            }
        } else if (op == 0xDB) {    // G_MOVEWORD: a light's color
            if (((w0 >> 16) & 0xFF) == 0x0A) {
                sets = 1;
                if (night->embers && (w1 == FIRE_KEY_DAY || w1 == FIRE_FILL_DAY)) {
                    actor_word_add((u32) &cmd[1], w1, ((w0 & 0xFFFF) < 0x18) ? AW_KEY : AW_FILL);
                }
            }
        } else if (op == 0xE1) {    // G_RDPHALF_1: the target of the branch that follows
            half = w1;
        } else if (op == 0x04) {    // G_BRANCH_Z: the near or far list
            sets |= actor_read(half, depth + 1);
        } else if (op == 0xDE) {    // G_DL
            sets |= actor_read(w1, depth + 1);
            if ((w0 >> 16) & 0xFF) {
                break;
            }
        }
        list += 8;
    }
    if (sets) {
        actor_leak_add(start);
    }
    return sets;
}

// Plain tests, not a switch, for the same reason as actor_read's.
static void actor_payload(DObj* node, s32 kind) {
    if (node->payload.any == NULL) {
        return;
    }
    if (kind == 'A' || kind == 'B') {
        actor_read((u32) node->payload.dlist, 0);
    } else if (kind == 'C' || kind == 'D') {
        DObjPayloadTypeC* entry = node->payload.typeC;
        s32 guard = 8;

        while (actor_list((u32) entry) && entry->dlistID != 4 && guard-- > 0) {
            actor_read((u32) entry->dlist, 0);
            entry++;
        }
    } else if (kind == 'I') {
        DObjPayloadTypeI* pair = node->payload.typeI;

        if (actor_list((u32) pair)) {
            actor_read((u32) pair->unk_00, 0);
            actor_read((u32) pair->unk_04, 0);
        }
    } else if (kind == 'J') {
        DObjPayloadTypeJ* entry = node->payload.typeJ;
        s32 guard = 8;

        while (actor_list((u32) entry) && entry->dlistID != 4 && guard-- > 0) {
            actor_read((u32) entry->unk_04, 0);
            actor_read((u32) entry->unk_08, 0);
            entry++;
        }
    }
}

static u32 blend_light(u32 day, u32 hour, s32 share) {
    u32 out = day & 0xFF;
    s32 k;

    for (k = 1; k < 4; k++) {
        s32 d = (s32) ((day >> (8 * k)) & 0xFF);
        s32 h = (s32) ((hour >> (8 * k)) & 0xFF);

        out |= (u32) (d + ((h - d) * share) / 256) << (8 * k);
    }
    return out;
}

// The hour on the Pokémon's words and painted corners: the cartridge's at Day.
static void actors_hour(void) {
    s32 share = (night != NULL) ? (s32) (nightness * 256.0f) : 0;
    s32 i, k;

    for (i = 0; i < actor_word_n; i++) {
        const ActorWord* w = &actor_words[i];
        u32 v = w->day;

        if (share > 0) {
            v = (w->kind == AW_FOG) ? (w->day & ~FOG_BIT)
              : (w->kind == AW_KEY) ? blend_light(w->day, FIRE_KEY_NIGHT, share)
              : blend_light(w->day, FIRE_FILL_NIGHT, share);
        }
        *(volatile u32*) w->at = v;
    }
    for (i = 0; i < actor_paint_n; i++) {
        const ActorPaint* p = &actor_paints[i];
        volatile u8* color = (volatile u8*) (p->at + 12);

        if (share <= 0) {
            color[0] = p->day[0];
            color[1] = p->day[1];
            color[2] = p->day[2];
            continue;
        }
        {
            f32 d = ((f32) p->n[0] * relight_dir[0] + (f32) p->n[1] * relight_dir[1] + (f32) p->n[2] * relight_dir[2]) / 127.0f;
            u32 toward = d > 0.0f ? (u32) (d * 256.0f) : 0;

            for (k = 0; k < 3; k++) {
                u32 lit = light_body[k] + ((land_diff[k] * toward) >> 8);

                color[k] = (u8) part(p->day[k], whole(lit));
            }
        }
    }
}

// The tables for a new load: the cartridge's words and colors given back first.
static void actor_reset(void) {
    if (actor_word_n != 0 || actor_paint_n != 0) {
        recomp_printf("[island skies] the Pokémon's hour: %d models, %d words, %d painted corners, %d lists that set their lights%s\n",
                      actor_model_n, actor_word_n, actor_paint_n, actor_leak_n, actor_full_said ? "; a table FULL" : "");
    }
    {
        s32 i;

        for (i = 0; i < actor_word_n; i++) {
            *(volatile u32*) actor_words[i].at = actor_words[i].day;
        }
        for (i = 0; i < actor_paint_n; i++) {
            volatile u8* color = (volatile u8*) (actor_paints[i].at + 12);

            color[0] = actor_paints[i].day[0];
            color[1] = actor_paints[i].day[1];
            color[2] = actor_paints[i].day[2];
        }
    }
    actor_word_n = actor_paint_n = actor_model_n = actor_leak_n = 0;
    actor_full_said = 0;
    actor_on = 0;
    actor_wrapped = 0;
}

static void actor_emit(s32 i, u32 w0, u32 w1) {
    Gfx* g = MAIN_GFX_POS[i];

    g->words.w0 = w0;
    g->words.w1 = w1;
    MAIN_GFX_POS[i] = g + 1;
}

// Whether a list the frame calls sets the lights' colors: one the reader kept.
static s32 actor_sets_lights(u32 target) {
    s32 i;

    for (i = 0; i < actor_leak_n; i++) {
        if (actor_leaks[i] == target) {
            return 1;
        }
    }
    return 0;
}

// A part's materials as the game writes them each frame (renLoadTextures): a branch
// for each, then their lists. Whether any sets the lights' colors (Electrode's does).
static s32 actor_materials_set_lights(u32 base) {
    volatile u32* entry = (volatile u32*) base;
    s32 n;

    if ((base >> 24) != 0x80 || (base & 7)) {
        return 0;
    }
    for (n = 0; n < 8 && (entry[0] >> 24) == 0xDE && ((entry[0] >> 16) & 0xFF) != 0; n++, entry += 2) {
        volatile u32* cmd = (volatile u32*) entry[1];
        s32 budget = 48;

        if ((entry[1] >> 24) != 0x80 || (entry[1] & 7)) {
            return 0;
        }
        while (budget-- > 0 && (cmd[0] >> 24) != 0xDF) {
            if ((cmd[0] >> 24) == 0xDB && ((cmd[0] >> 16) & 0xFF) == 0x0A) {
                return 1;
            }
            cmd += 2;
        }
    }
    return 0;
}

// ---- The frame's own light ----
// The renderer reads a frame's commands on a thread of its own, after the game has
// gone on: the game's next update writes its own light direction into the one record
// every camera loads (behind the camera, app_level/player.c), and the mod's lean
// toward the sun or the moon came only with the next frame's sky. A frame read late
// was lit from behind the camera, and the scene flickered (JackandBeans, Nov 8 2026).
// So outside Day each frame carries its own light, in its own command buffer, which is
// the frame context's own until the renderer has done with it (sys/gtl.c): the light
// list's call is pointed at a copy of that list, written past a branch beside it,
// whose two loads read a copy of the record written just after it. The record itself
// holds only what the game writes. At Day nothing of this is written.
static u8 light_now[16];                   // the scene light's colors in force: ambient twice, diffuse twice, as the record has them
static u8 light_set;                       // set_light has given them for this load
static Gfx** prerender_at;                 // the list being written by the light list's call (func_800A1A50)
static volatile u8* light_block;           // this frame's copy of the record, in its command buffer
static s32 light_block_drawn;              // the frame it belongs to (GTL_DRAWN while it is drawn)
static u8 light_block_said;
static void light_store(void);

// The frame being drawn still owns its copy: written from the light list's call on,
// until the draw is done and the frame goes to the renderer.
static s32 light_block_ok(void) {
    return light_block != NULL && GTL_DRAWN == light_block_drawn;
}

// The frame context being drawn: what the mod writes for one frame and points that
// frame's commands at is kept in sets by it, so the game draws the next frame into
// another set, or into this one only once the renderer has finished with it.
static u8 frame_ctx_said;

static s32 frame_ctx(void) {
    s32 c = GTL_CONTEXT;

    if (c < 0 || c >= FRAME_CTX) {
        if (!frame_ctx_said) {
            frame_ctx_said = 1;
            recomp_printf("[island skies] a frame context %d past the mod's %d sets\n", c, FRAME_CTX);
        }
        return 0;
    }
    return c;
}

// Matrices written more than once in a frame (the rain for each photo drawn with
// it, the storm's, a roofed course's photo skies) take the next of their context's
// slots; the slots start again with each frame (night_light_list_begins).
static u8 rain_slot_n, storm_slot_n, photo_slot_n, glow_slot_n, streak_slot_n;

static s32 frame_slot(u8* n, s32 slots) {
    s32 s = frame_ctx() * slots + (s32) (*n % slots);

    (*n)++;
    return s;
}

// ---- The frame buffers' room ----
// The game draws a frame into four buffers of its frame context (gtlDLBuffers[2][4], each a start and a
// length: sys/gtl.c:81), and stops the game when one runs past its end ("DLBuffer over flow",
// gtlCheckBuffers, gtl.c:176-186). The photo review's main buffer runs as full as 95.7% (Nov 8 2026).
// So the mod keeps what it draws in its own memory, a set for each frame context as the sky's copies are:
// the light list with the frame's copy of the record, the parts' reloads, and every extra (the weather, the
// mist, the glint, the fireflies, a roofed course's sky, the storm's flash, a streak). In the game's buffers
// it puts only the calls into those lists, each inside a window of the extension's addresses of its own (the
// renderer reaches the mod's memory only through one: rt64_rsp.cpp, maskPhysicalAddress); so does the call the
// mod leaves in the sky's own list (spare_point), so no call depends on another's window or on the plan. What each costs a buffer: the light 24 bytes at the frame's start (the extension's enable,
// which the port gives only at each camera's matrices, later in the frame, in the cartridge's call's place;
// the window's two ends; the call), a part's reload 24, an extra 24, the sky's
// window 16, Moltres's fog 8, Rapidash's fog wrapper 16.
// It adds even that only by plan: the game's own fill of the buffer in the last frame drawn (at omDrawAll's
// end), plus all the mod may add to that buffer in a frame (gfx_budget), must leave the game a margin of it (a
// 32nd, and never under 512 bytes), and so must the room left at the moment of each addition. A new scene's
// first frame has no frame before it, and is planned from a seed instead: the fullest the game's own fill of
// such a buffer was ever measured (gfx_seed). At a scene's end the log gives the most the mod added to each
// buffer in one frame, and what it left out for room.
#define GTL_DL_BUFFERS ((volatile u32*) 0x8004A850u)
#define GFX_STAGE_N   8192      // commands an extra is written into first: more than the photo review's whole buffer holds
#define GFX_ARENA_N   16384     // commands of the mod's own lists each frame context holds
#define EXT_ENABLE_W0 0xE0525464u     // the extension's enable (rt64_extended_gbi.h gEXEnable: G_SPNOOP with RT64's mark)
#define EXT_ENABLE_W1 0x10000064u
#define EXT_WINDOW_W0 0x6400002Cu     // the window of the extension's addresses (G_EX_SETRDRAMEXTENDED), 1 open, 0 shut

static Gfx gfx_stage[GFX_STAGE_N];
static Gfx* gfx_stage_at;              // the extra being written: its next command
static Gfx gfx_arena[FRAME_CTX][GFX_ARENA_N];
static s32 gfx_arena_used[FRAME_CTX];
static Gfx light_lists[FRAME_CTX][11];      // the camera's light list's copy (8 commands), then the frame's copy of the record (24 bytes)
static Gfx light_reloads[FRAME_CTX][3];     // the light and the ambient loaded again from that copy, and the end
static const u32 gfx_budget[4] = { 1536, 256, 128, 128 };  // bytes the mod may add to each of a frame's buffers
static u32 gfx_mod_bytes[4];           // what the mod has added to each buffer in this frame
static u32 gfx_most[4];                // the most it added to each in one frame, this scene
static u32 gfx_plan_game[4];           // the game's own fill of each buffer in the last frame drawn
static u32 gfx_plan_length[4];         // that buffer's length then; 0 while there is nothing to go by
static s32 gfx_begun = -1;             // the drawn frame whose lists were begun
static u32 gfx_short_n;                // additions left out for room, this scene
static u8 gfx_short_said;

// The buffer a pointer is in (context * 4 + its kind), the room left there in commands, the margin and the
// buffer's length; -1 for a pointer in none of them.
static s32 gfx_buffer(const Gfx* at, s32* room, s32* margin, u32* length) {
    s32 i;

    for (i = 0; i < 8; i++) {
        u32 start = GTL_DL_BUFFERS[i * 2];
        u32 len = GTL_DL_BUFFERS[i * 2 + 1];

        if (len != 0 && (u32) at >= start && (u32) at <= start + len) {
            u32 keep = len / 32;

            *margin = (s32) ((keep > 512 ? keep : 512) / sizeof(Gfx));
            *room = (s32) ((start + len - (u32) at) / sizeof(Gfx));
            *length = len;
            return i;
        }
    }
    return -1;
}

// Whether the mod may add n commands at `at` now. Its share of a buffer in a frame is what the last frame's own
// fill left above the margin, and never more than its budget; and the room left at this moment must keep the
// margin too. So the mod can only be what crosses the end in a frame whose own fill grew by more than the
// margin since the frame before.
static s32 gfx_may(const Gfx* at, s32 n) {
    s32 room, margin, b, k;
    u32 length, why, keep, share;

    if (at == NULL) {
        return 0;
    }
    b = gfx_buffer(at, &room, &margin, &length);
    if (b < 0) {
        return 1;
    }
    k = b & 3;
    keep = (u32) margin * sizeof(Gfx);
    share = (gfx_plan_game[k] + keep >= length) ? 0 : length - keep - gfx_plan_game[k];
    if (share > gfx_budget[k]) {
        share = gfx_budget[k];
    }
    why = (gfx_plan_length[k] != length) ? 1                                    // nothing to go by: the buffer is not the plan's
        : (gfx_mod_bytes[k] + (u32) n * sizeof(Gfx) > share) ? 2               // past the share the last frame left
        : (room - n < margin) ? 3 : 0;                                         // too little left now
    if (why == 0) {
        gfx_mod_bytes[k] += (u32) n * sizeof(Gfx);
        if (gfx_mod_bytes[k] > gfx_most[k]) {
            gfx_most[k] = gfx_mod_bytes[k];
        }
        return 1;
    }
    if (why != 1) {
        gfx_short_n++;
        if (!gfx_short_said) {
            gfx_short_said = 1;
            recomp_printf("[island skies] buffer %d of the frame: %s, so the mod leaves out what does not fit (%d commands left, the last frame's own fill %u of %u)\n",
                          k, (why == 2 && share == gfx_budget[k]) ? "the mod's budget for a frame is spent" : "nearly full",
                          room, gfx_plan_game[k], length);
        }
    }
    return 0;
}

// The seed of a buffer's plan for a scene's first frame: the fullest the game's own fill of such a buffer was
// measured. The photo check's main buffer (53,248 bytes) reached 50,864 on its own (Nov 8 2026, the eval tape,
// at the release folder's settings and at JackandBeans's). No other buffer the mod draws into, in the five
// courses or the photo check, ever passed 90% of its length: the port says so from 90% up ([SNAP-DL],
// src/dl_budget.cpp), and no log of a ride or a run has such a line for one (Nov 2026).
static u32 gfx_seed(s32 k, u32 length) {
    u32 seed = length / 10 * 9;

    if (k == 0 && length == 53248 && seed < 50864) {
        seed = 50864;
    }
    return seed;
}

// A frame's draw begins (the light list's call, or omDrawAll's start, whichever comes first): its context's
// lists start again, and so do the mod's shares. The drawn-frame count starts again with each scene, whose
// first frame is planned from the seeds.
static void gfx_frame_begins(void) {
    s32 drawn = GTL_DRAWN, k;

    if (drawn == gfx_begun) {
        return;
    }
    if (drawn < gfx_begun || gfx_begun < 0) {
        s32 c = frame_ctx();

        for (k = 0; k < 4; k++) {
            u32 length = GTL_DL_BUFFERS[(c * 4 + k) * 2 + 1];

            gfx_plan_length[k] = length;
            gfx_plan_game[k] = gfx_seed(k, length);
        }
    }
    gfx_begun = drawn;
    gfx_arena_used[frame_ctx()] = 0;
    for (k = 0; k < 4; k++) {
        gfx_mod_bytes[k] = 0;
    }
}

// The draw has ended (omDrawAll's return): the game's own fill of each buffer, the plan for the next frame.
static void gfx_frame_ends(void) {
    s32 c = frame_ctx(), k;

    for (k = 0; k < 4; k++) {
        u32 start = GTL_DL_BUFFERS[(c * 4 + k) * 2];
        u32 length = GTL_DL_BUFFERS[(c * 4 + k) * 2 + 1];
        u32 at = (u32) MAIN_GFX_POS[k];

        if (length == 0 || at < start || at - start > length) {
            gfx_plan_length[k] = 0;
            continue;
        }
        gfx_plan_game[k] = (at - start) - ((gfx_mod_bytes[k] < at - start) ? gfx_mod_bytes[k] : (at - start));
        gfx_plan_length[k] = length;
    }
}

// An extra whose length the frame decides is written into the stage first. Closed, it is copied whole into its
// frame context's own lists, ended, and called from the game's buffer inside a window of the extension's
// addresses (or with the call alone, `windowed`, for a caller already inside a window: none since cut 35c,
// whose storm's flash and streak take windows of their own); or, without room or plan, it
// is left out whole. A writer handed the stage itself (a roofed course's sky and weather hand theirs to the
// rain, the snow, the storm and the streak) writes on into it, and the extra it is part of goes, or not, as one.
static Gfx** gfx_stage_open(Gfx** game) {
    if (game == &gfx_stage_at) {
        return game;
    }
    gfx_stage_at = gfx_stage;
    return &gfx_stage_at;
}

static void gfx_stage_close(Gfx** game, s32 windowed) {
    s32 c, n, i;
    Gfx* list;

    if (game == &gfx_stage_at) {
        return;
    }
    n = (s32) (gfx_stage_at - gfx_stage);
    gfx_stage_at = gfx_stage;
    c = frame_ctx();
    if (n <= 0 || n > GFX_STAGE_N || *game == NULL || gfx_arena_used[c] + n + 1 > GFX_ARENA_N || !gfx_may(*game, windowed ? 1 : 3)) {
        return;
    }
    list = &gfx_arena[c][gfx_arena_used[c]];
    for (i = 0; i < n; i++) {
        list[i] = gfx_stage[i];
    }
    list[n].words.w0 = 0xDF000000u;
    list[n].words.w1 = 0;
    gfx_arena_used[c] += n + 1;
    if (!windowed) {
        (*game)->words.w0 = EXT_WINDOW_W0;
        (*game)->words.w1 = 1;
        (*game)++;
    }
    (*game)->words.w0 = 0xDE000000u;
    (*game)->words.w1 = (u32) list;
    (*game)++;
    if (!windowed) {
        (*game)->words.w0 = EXT_WINDOW_W0;
        (*game)->words.w1 = 0;
        (*game)++;
    }
}

// After a part: where it set the lights' colors for itself, the hour's lights are
// loaded again in that list, from the frame's own copy of the record (which holds
// the storm's flash too).
static void actor_look(void) {
    s32 i;

    for (i = 0; i < 4; i++) {
        volatile u32* cmd = (volatile u32*) actor_seen[i];
        volatile u32* end = (volatile u32*) MAIN_GFX_POS[i];
        s32 reload = 0;

        if (cmd != NULL && end >= cmd && end - cmd < 4096) {
            while (cmd < end) {
                if (cmd[0] == 0xDB060038u) {        // the segment 0x0E: the part's materials
                    reload |= actor_materials_set_lights(cmd[1]);
                } else if ((cmd[0] >> 24) == 0xDE && actor_sets_lights(cmd[1])) {
                    reload = 1;
                }
                cmd += 2;
            }
        }
        if (reload && light_block_ok() && gfx_may(MAIN_GFX_POS[i], 3)) {
            actor_emit(i, EXT_WINDOW_W0, 1);                                  // the frame's lists are in the mod's memory:
            actor_emit(i, 0xDE000000u, (u32) light_reloads[frame_ctx()]);     // the light and the ambient, as the camera's list loads them
            actor_emit(i, EXT_WINDOW_W0, 0);
        }
        actor_seen[i] = MAIN_GFX_POS[i];
    }
}

// A Pokémon about to be drawn: read once a load; outside Day, followed part by part.
static void actor_enters(DObj* first, s32 kind) {
    u32 id = (u32) first->payload.any;
    DObj* node;
    s32 guard = 400;
    s32 i;

    actor_fire = (id == FIRE_MOLTRES || id == FIRE_MOLTRES_HD);
    actor_rapidash = (id == GLOW_RAPIDASH || id == GLOW_RAPIDASH_HD);
    actor_blast = actor_rapidash;
    for (i = 0; i < actor_model_n; i++) {
        if (actor_models[i] == id) {
            break;
        }
    }
    if (i == actor_model_n && actor_model_n < ACTOR_MODELS) {
        actor_models[actor_model_n++] = id;
        actor_lit = 1;
        actor_fog = !actor_fire;        // the others are drawn inside the game's fog wrapper
        actor_tex = 0;
        for (i = 0; i < 64; i++) {
            actor_slots[i] = 0;
        }
        for (node = first; node != NULL && guard-- > 0; node = after(node)) {
            actor_payload(node, kind);
        }
        actors_hour();
    }
    actor_on = (nightness > 0.0f);
    if (!actor_on) {
        return;
    }
    for (i = 0; i < 4; i++) {
        actor_seen[i] = MAIN_GFX_POS[i];
    }
    {
        Gfx* last = MAIN_GFX_POS[0] - 1;
        s32 fading = (last->words.w0 >> 24) == 0xF8;       // the game's fade has just set its own fog color

        if (actor_fire && !fading && gfx_may(MAIN_GFX_POS[0], 1)) {
            actor_emit(0, 0xD9FEFFFFu, 0);      // fog off from the first part: what came before cannot fog it
        } else if (actor_rapidash && !fading) {
            // The game's own wrapper, so the fog is on from the first part whatever
            // came before; the lists are checked to be the wrapper's before they
            // are called, and not called where the game has already wrapped it.
            volatile u32* set = (volatile u32*) FOG_SET_LIST;
            volatile u32* end = (volatile u32*) FOG_CLEAR_LIST;
            s32 by_game = (last->words.w0 == 0xDE000000u && (last->words.w1 == FOG_SET_LIST || last->words.w1 == NOFOG_SET_LIST));

            // Room for both ends of the wrapper, asked here: the end goes in whenever the start did.
            if (!by_game && set[0] == 0xE7000000u && set[10] == 0xD9FFFFFFu && set[11] == FOG_BIT && end[0] == 0xE7000000u && end[6] == 0xD9FEFFFFu
                && gfx_may(MAIN_GFX_POS[0], 2)) {
                actor_emit(0, 0xDE000000u, FOG_SET_LIST);
                actor_wrapped = 1;
            }
        }
    }
    for (i = 0; i < 4; i++) {
        actor_seen[i] = MAIN_GFX_POS[i];
    }
}

// A model about to be drawn. The first time each model of the course comes
// by, its lists are read and given the night's light; after that the model
// is known by its first list and passed over.
static void model_enters(GObj* obj, s32 kind) {
    DObj* node;
    DObj* first = NULL;
    s32 guard = 400;
    s32 i;

    for (i = 0; i < 4; i++) {
        entry_pos[i] = MAIN_GFX_POS[i];
    }
    actor_on = 0;
    if (night == NULL || obj == NULL || obj->type != 1) {
        return;
    }
    for (node = obj->data.dobj; node != NULL && guard-- > 0; node = after(node)) {
        if (node->payload.any != NULL) {
            first = node;
            break;
        }
    }
    if (first != NULL && actor_list((u32) first->payload.any)) {
        actor_enters(first, kind);      // the course's Pokémon: not the walker's
        return;
    }
    if (first == NULL || unit_of((u32) first->payload.any) < 0) {
        return;
    }
    // A roofed course's open-air start: whether the first block or the next is current,
    // and whether the camera is still under the open sky, noted once a frame at the
    // first world model's drawing with the camera drawing it; the weather is drawn
    // as the UI camera begins (night_ui_begins). First found by JackandBeans's
    // picture, Oct 11 2026: rain in the tunnel, and a hang where it met
    // Electrode's explosion in the same list.
    if (ROOFED(night) && on_the_ride && !scoring && !roof_placed
        && (u32) first->payload.any >= night->world_lo && (u32) first->payload.any < night->world_hi) {
        GObj* cam = *(GObj* volatile*) 0x8004AC48u;

        if (cam != NULL && cam->data.cam != NULL) {
            roof_place(cam);
            roof_strike();
            light_store();          // the drum's hook (night_sky_begins) never runs here: the strike's lean
        }
    }
    // A photo of a roofed course: the camera drawing its land, whose object
    // pass ends with the sky and the weather (night_objects_end). The photo
    // takes the cave's own light and fog where its camera stands under the
    // roof, as the ride did at that place (roof_tick runs on the ride only).
    if (ROOFED(night) && !on_the_ride && !scoring && photo_sky_cam == NULL
        && (u32) first->payload.any >= night->world_lo && (u32) first->payload.any < night->world_hi) {
        GObj* cam = *(GObj* volatile*) 0x8004AC48u;

        if (cam != NULL && cam->data.cam != NULL) {
            f32 f;

            photo_sky_cam = cam;
            roof_place(cam);
            f = roof_past / NIGHT_TUNNEL_CAVE_Z;
            cave_now = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
            set_light(night);
            fog_apply();
            roof_strike();
            if (storming && roof_outside) {
                storm_light();      // the flash's veil, as a drum's photo has it
            }
            light_store();
            if (!roof_photo_said) {
                roof_photo_said = 1;
                recomp_printf("[island skies] a photo of the tunnel: weather volume %d, %d units under the roof, %s\n",
                              (s32) roof_volume, (s32) roof_past, roof_outside ? "under the open sky" : "under the roof");
            }
        }
    }
    drawing_water = (water_model != 0 && (u32) first->payload.any == water_model);
    drawing_glint = 0;
    drawing_parts = 0;
    {
        s32 k;

        for (k = 0; k < glint_seg_n; k++) {
            if (glint_seg[k].model == (u32) first->payload.any) {
                drawing_parts = 1;
                drawing_glint |= glint_seg[k].flow;
            }
        }
    }
    if (marked(walked, (u32) first->payload.any)) {
        return;
    }
    mark(walked, (u32) first->payload.any);
    lit = 1;
    flowing = 0;
    tex_addr = 0;
    tlut_addr = 0;
    tile_fmt = 0xFF;
    for (i = 0; i < 64; i++) {
        slots[i] = 0;
    }
    guard = 400;
    {
        u32 water_before = flow_y_n;
        u16 vtx_before = glint_vtx_n, op_before = glint_op_n, tris_before = glint_tris, land_before = glint_land;
        u8 seg_before = glint_seg_n;

        glint_model = (u32) first->payload.any;
        glint_recording = (glint_seg_n < GLINT_SEGS);
        for (node = first; node != NULL && guard-- > 0; node = after(node)) {
            walk_payload(node, kind);
        }
        glint_recording = 0;
        if (fine_new) {
            fine_new = 0;
            fine_roof_refresh();
        }
        if (glint_tris == tris_before && glint_land == land_before) {
            // Nothing of the world in this model: what it loaded is dropped again.
            glint_vtx_n = vtx_before;
            glint_op_n = op_before;
            glint_seg_n = seg_before;
        } else {
            drawing_glint = (glint_tris > tris_before);
            drawing_parts = (glint_seg_n > seg_before);
        }
        if (flow_y_n > water_before && water_model == 0) {
            water_model = (u32) first->payload.any;
            drawing_water = 1;
        }
    }
}

static void moon_glint(void);
static void embers(void);
static void cycle_tick(void);
static void status_tick(void);
static u16 shutter_stamp(void);
static void roof_tick(void);
static u32 weather_live(const Night* by);
static void weather_flags_set(u32 code);
static void weather_shares_apply(s32 say);
static void relight_record(void);
static u32 weather_of(const Night* by);
static void weather_palettes_keep(const Night* by);
static void sun_hours_apply(void);
static s32 sun_latitude(void);
static void fireflies(GObj* sky);
static void mist_scan(void);
static void mist_weather(void);
static void mist_photo(GObj* cam);
static f32 mist_now(void);
static GObj* sky_now;           // the sky being drawn, for the camera's place

// A model just drawn. A material may carry light colors of its own, which
// the game writes into the frame's command lists as it draws; those written
// for this model are given the night's light here, every frame, since the
// game writes them anew every frame.
static void model_leaves(void) {
    s32 i;

    if (actor_on) {
        actor_look();
        actor_on = 0;
    }
    if (actor_wrapped) {
        actor_emit(0, 0xDE000000u, FOG_CLEAR_LIST);     // the wrapper's end, as the game's own
        actor_wrapped = 0;
    }
    if (night == NULL) {
        return;
    }
    for (i = 0; i < 4; i++) {
        volatile u32* cmd = (volatile u32*) entry_pos[i];
        volatile u32* end = (volatile u32*) MAIN_GFX_POS[i];
        s32 budget = 4096;

        if (cmd == NULL || end < cmd) {
            continue;
        }
        while (cmd < end && budget-- > 0) {
            if ((cmd[0] >> 16) == 0xDB0A) {
                cmd[1] = night_word(cmd[1]);
            }
            cmd += 2;
        }
    }
    if (drawing_glint && night->glint && glows_on && !sunset && !scoring && clear_share > 0.0f && sky_now != NULL) {
        drawing_glint = 0;
        moon_glint();
    } else if (drawing_glint && night->embers && !scoring && sky_now != NULL) {
        drawing_glint = 0;
        embers();
    }
    // The water was just drawn: what glows on it is drawn now, so that the
    // water, which is drawn among the see-through things and writes no
    // depth, does not paint over it.
    if (drawing_water && !scoring && sky_now != NULL && glows_on && !sunset && clear_share > 0.0f) {
        drawing_water = 0;
        if (night->fireflies != NULL) {
            fireflies(sky_now);
        }
    }
    // A model with parts of the record was just drawn: the dawn's mist on
    // its water and its shore is drawn now, for the same reason.
    if (drawing_parts && !scoring && sky_now != NULL) {
        drawing_parts = 0;
        mist_share = mist_now();
        if (mist_share > 0.0f) {
            mist_scan();
        }
    }
}

RECOMP_HOOK("renRenderModelTypeA") void night_model_a(GObj* obj) { model_enters(obj, 'A'); }
RECOMP_HOOK("renRenderModelTypeB") void night_model_b(GObj* obj) { model_enters(obj, 'B'); }
RECOMP_HOOK("renRenderModelTypeC") void night_model_c(GObj* obj) { model_enters(obj, 'C'); }
RECOMP_HOOK("renRenderModelTypeD") void night_model_d(GObj* obj) { model_enters(obj, 'D'); }
RECOMP_HOOK("renRenderModelTypeI") void night_model_i(GObj* obj) { model_enters(obj, 'I'); }
RECOMP_HOOK("renRenderModelTypeJ") void night_model_j(GObj* obj) { model_enters(obj, 'J'); }

RECOMP_HOOK_RETURN("renRenderModelTypeA") void night_model_a_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeB") void night_model_b_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeC") void night_model_c_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeD") void night_model_d_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeI") void night_model_i_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeJ") void night_model_j_done(void) { model_leaves(); }

// A Pokémon's part about to be drawn: what the part before it left is looked at first.
RECOMP_HOOK("renRenderModelNodeTypeB") void night_node_b(DObj* dobj) { if (actor_on) { actor_look(); } }
RECOMP_HOOK("renRenderModelNodeTypeD") void night_node_d(DObj* dobj) { if (actor_on) { actor_look(); } }
RECOMP_HOOK("renRenderModelNodeTypeI") void night_node_i(DObj* dobj) { if (actor_on) { actor_look(); } }
RECOMP_HOOK("renRenderModelNodeTypeJ") void night_node_j(DObj* dobj) { if (actor_on) { actor_look(); } }

// The scene light: ambient at 0 and diffuse at 8, each twice, as the game's light
// record lays them out; kept by the mod and carried by each frame (light_store),
// the record itself left as the game has it.
static void set_light(const Night* by) {
    volatile u8* light = light_now;
    s32 i;

    for (i = 0; i < 3; i++) {
        s32 day_a = (day_ambient[i] != 0 || day_diffuse[i] != 0) ? day_ambient[i] : DAY_AMBIENT;
        s32 day_d = (day_ambient[i] != 0 || day_diffuse[i] != 0) ? day_diffuse[i] : DAY_DIFFUSE;
        s32 share = (s32) (nightness * 256.0f);
        s32 amb = scene_ambient[i] + (s32) ((f32) ((s32) cave_palette[9][i] - scene_ambient[i]) * cave_now);     // in the cave the hour's own
        s32 dif = scene_diffuse[i] + (s32) ((f32) ((s32) cave_palette[10][i] - scene_diffuse[i]) * cave_now);
        s32 ambient = (by != NULL) ? day_a + ((amb - day_a) * share) / 256 : day_a;
        s32 diffuse = (by != NULL) ? day_d + ((dif - day_d) * share) / 256 : day_d;

        light[i] = light[4 + i] = (u8) ambient;
        light[8 + i] = light[12 + i] = (u8) diffuse;
    }
    light[3] = light[7] = light[11] = light[15] = 0;
    light_set = (by != NULL);
    actors_hour();
    light_store();
}

// A sky picture, recolored where it lies. The night keeps the day picture's
// own light and shade -- its luminance, stretched between the darkest open
// sky and the brightest cloud and softened a little -- and paints it again
// as night sky under night cloud, both kept BRIGHT. The dark comes from the
// box's corners (sky_corners), which blend in eight bits across the dome, as
// the game's own corners carry the day's blue above and haze below. A
// picture of five bits a channel cannot hold a dark gradient without flecks;
// a bright one can.
// A sky picture painted from its cloud mask with the colors in force.
// A texel of the cartridge's picture as the day end of the blend: itself, or
// under rain, snow or ash, grayed and a little darker (an overcast day).
// The sky over a day under weather: a flat overcast, pale under snow, gray
// under rain, dark in a storm, dun under ash, with a little of the picture's
// own light left in it so the clouds still show. (1.0.0 dimmed and grayed the
// cartridge's picture instead, and the Valley's deep blue dimmed was a
// night-dark sky over a snowy day: JackandBeans, Oct 11 2026.)
static void overcast_base(u8 out[3]) {
    static const u8 snow_sky[3] = { 208, 210, 216 };
    static const u8 rain_sky[3] = { 150, 153, 160 };
    static const u8 storm_sky[3] = { 96, 100, 110 };
    static const u8 ash_sky[3] = { 156, 146, 136 };
    const u8* b = snowing ? snow_sky : storming ? storm_sky : ashing ? ash_sky : rain_sky;
    s32 k;

    for (k = 0; k < 3; k++) {
        out[k] = b[k];
    }
}

static u32 day_texel(s32 which, s32 i, s32 k) {
    u16 t = sky_orig_pic[which][i];
    u32 c[3];

    c[0] = ((t >> 11) & 31) * 255 / 31;
    c[1] = ((t >> 6) & 31) * 255 / 31;
    c[2] = ((t >> 1) & 31) * 255 / 31;
    if (overcast_share > 0.0f) {
        u8 base[3];
        s32 gray = (s32) ((c[0] * 77 + c[1] * 151 + c[2] * 28) >> 8);
        s32 flat;

        overcast_base(base);
        flat = (s32) base[k] + ((gray - 150) * 12) / 100;
        flat = flat < 0 ? 0 : flat > 255 ? 255 : flat;
        return (u32) ((s32) c[k] + (s32) (((f32) flat - (f32) c[k]) * overcast_share));
    }
    return c[k];
}

// The haze of the Beach, the River and the Volcano. Their pictures hold a haze
// as well as clouds, a floor under each row of the mask that climbs to 250 under
// the River's lid and from 17 to 252 down the Volcano's sky. Five bits a channel
// cannot hold it: the top fade took it down in whole rows, each a step of the
// picture all the way round, and the drum's six-sided rim and rings stood out.
// So each row of the picture is divided by its own open sky and painted from a
// constant (sky_paint), and the corners carry that sky (sky_corners). Open sky is
// then one color of the picture at every height and every hour, so it never
// crosses a step, and a cloud keeps its contrast against the sky around it. The
// Tunnel's first picture is the Beach's, with the same haze; its heights are
// reckoned by the fine sky. The shares are of 255, at the low ring and at the top ring; keep
// is the rows left alone at the bottom; base is the constant, red, green, blue:
// high enough to keep the corners under 255 with the lava's glow on them, low
// enough to leave the clouds room above it (Nov 6 2026).
typedef struct SkyHaze {
    u32 setup;
    s16 low_y, top_y;
    u8 low, top, keep;
    u8 base[3];
    u8 night_base[3];           // the night's: its clouds stand further above their sky, and want the room
} SkyHaze;

static const SkyHaze sky_hazes[] = {
    { NIGHT_BEACH_SETUP, -153, 687, 88, 0, 0, { 165, 173, 213 }, { 140, 156, 196 } },
    { NIGHT_RIVER_SETUP, 0, 1000, 48, 51, 2, { 173, 172, 204 }, { 172, 172, 204 } },
    { NIGHT_VOLCANO_SETUP, 0, 1000, 230, 15, 0, { 190, 198, 196 }, { 140, 140, 164 } },
    { NIGHT_TUNNEL_SETUP, 0, 1000, 88, 0, 0, { 165, 173, 213 }, { 140, 156, 196 } },
};
static u8 sky_floor[32];        // each row's floor; nothing on the other courses
static f32 sky_gain[3] = { 1.0f, 1.0f, 1.0f };     // what five bits lost or gained of the painted picture, for the corners to give back

// How far the hour has come into the Night's way of doing this: none at Sunset, at
// Dawn, or with any of the Day in the blend.
static f32 sky_night_share(void) {
    return nightness >= 1.0f ? 1.0f - (f32) cloud_top_fade / 255.0f : 0.0f;
}

// The constant in force: the day's end's, going to the night's as far as the dusk has
// come, picture and corners alike at every moment, so the open sky does not move. It
// moves on the picture's own steps of eight and stands halfway up one, so a faint
// cloud rounds the same way at every moment and each move is a whole step, which the
// corners take back. None of the night's with any Day in the blend: picture and
// corners would not cancel.
static s32 sky_base(const SkyHaze* haze, s32 k) {
    f32 share = sky_night_share();
    f32 b = (f32) haze->base[k] + (f32) ((s32) haze->night_base[k] - (s32) haze->base[k]) * share;

    if (share <= 0.0f) {
        return haze->base[k];
    }
    return (s32) ((b - 4.0f) / 8.0f + 0.5f) * 8 + 4;
}

// At Night the open sky climbs the drum on one smooth line, from the low rows' haze to
// the top row's, by the share of the top ring the tints themselves take, so it is level
// where it meets the lid and has no corner at the middle ring. The corners carry the
// line; the picture's clouds stand on it by their difference from their row's open sky,
// so they keep the height above their sky they have on the cartridge (Nov 7 2026).
static f32 sky_night_line(const SkyHaze* haze, s32 k, f32 up) {
    f32 top_now = (f32) haze->top + ((f32) sky_floor[31] - (f32) haze->top) * (f32) (255 - (s32) cloud_top_fade) / 255.0f;
    f32 h = ((f32) haze->low + (top_now - (f32) haze->low) * up) / 255.0f;

    return (f32) pic_sky[k] + (f32) ((s32) pic_cloud[k] - (s32) pic_sky[k]) * h;
}

// That share at a row of the picture, which runs from the middle ring to the top one.
// A picture is painted before the drum is first read, so the rings are from the record;
// the drums' lowest ring stands at -400 on all three courses.
static f32 sky_row_up(const SkyHaze* haze, s32 row) {
    f32 a = (f32) row / 31.0f;

    return a * a * (3.0f - 2.0f * a) + a * (1.0f - a) * (1.0f - a) * (f32) (haze->top_y - haze->low_y) / (f32) (haze->top_y + 400);
}

static const SkyHaze* sky_haze_of(const Night* by) {
    u32 i;

    for (i = 0; i < sizeof(sky_hazes) / sizeof(sky_hazes[0]); i++) {
        if (sky_hazes[i].setup == by->setup) {
            return &sky_hazes[i];
        }
    }
    return NULL;
}

static void sky_paint(u32 pic, const u8* mask, s32 which) {
    s32 share = (s32) (nightness * 256.0f);
    volatile u16* texel = (volatile u16*) pic;
    s32 lid = !(which == 1 && night->setup == NIGHT_VALLEY_SETUP);      // the Valley's second picture stands under the first
    const SkyHaze* haze = (which == 0) ? sky_haze_of(night) : NULL;     // the Tunnel's second picture has no eye under it
    f32 by_line = (haze != NULL && night->setup != NIGHT_TUNNEL_SETUP) ? sky_night_share() : 0.0f;
    f32 line[3] = { 0.0f, 0.0f, 0.0f };
    u32 kept[3], drawn[3];
    s32 i;

    for (i = 0; i < 3; i++) {
        kept[i] = drawn[i] = 0;
    }
    for (i = 0; i < SKY_TEXELS; i++) {
        s32 row = i >> 6;
        s32 open = sky_floor[row];
        s32 m = (lid && haze != NULL && row == 31) ? open : mask[i];     // the lid's row: open sky, one color at every hour
        u32 c[3];
        s32 k;

        if (by_line > 0.0f && (i & 63) == 0) {
            for (k = 0; k < 3; k++) {
                line[k] = sky_night_line(haze, k, sky_row_up(haze, row));
            }
        }
        if (lid && cloud_top_fade != 0 && row > 12) {
            // At sunset the clouds thin away toward the top of the drum, so
            // its flat lid (the picture's top row, one color) meets the sides
            // without a rim, and the picture's six repeats around the sky
            // crowd less where they are closest. Not on a picture that meets
            // no lid: the Valley's lower picture lost its clouds right up to
            // the upper's, and the two met in a straight line across the sky
            // (JackandBeans, Nov 5 2026).
            s32 f = ((row - 12) * 255) / 19;

            m -= ((((m * f) / 255) * f) / 255) * cloud_top_fade / 255;
            open -= ((((open * f) / 255) * f) / 255) * cloud_top_fade / 255;
        }
        for (k = 0; k < 3; k++) {
            c[k] = (u32) (pic_sky[k] + (((s32) pic_cloud[k] - pic_sky[k]) * m) / 255);
            if (haze != NULL) {
                // This row's open sky comes to the constant, and the clouds with it.
                s32 sky = (s32) pic_sky[k] + (((s32) pic_cloud[k] - pic_sky[k]) * open) / 255;
                s32 base = sky_base(haze, k);
                s32 v;

                if (by_line > 0.0f) {
                    v = base + (s32) ((f32) (((s32) c[k] - sky) * base) / ((f32) sky + (line[k] - (f32) sky) * by_line));
                } else {
                    v = ((s32) c[k] * base) / sky;
                }
                c[k] = (u32) (v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
        if (share < 256) {
            for (k = 0; k < 3; k++) {
                u32 day = day_texel(which, i, k);

                c[k] = day + (((s32) c[k] - (s32) day) * share) / 256;
            }
        }
        if (haze != NULL) {
            for (k = 0; k < 3; k++) {
                u32 five = c[k] >> 3;

                kept[k] += c[k];
                drawn[k] += (five << 3) | (five >> 2);
            }
        }
        texel[i] = (u16) (((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | 1);
    }
    if (haze != NULL) {
        for (i = 0; i < 3; i++) {
            sky_gain[i] = (f32) kept[i] / (f32) drawn[i];
        }
    }
}

// The cartridge's picture meets itself at the drum's joins with a step, as
// the mask does (sky_at_night); the kept copy takes the same dissolve the
// port gives the picture it draws, so the day end of the blend has none.
static void dissolve_orig(s32 which) {
    u16* pic = sky_orig_pic[which];
    u32 best = 0xFFFFFFFFu;
    s32 best_o = 10, o, x, y, d, k;
    u16 keep[64];

    for (o = 10; o + 20 <= 54; o++) {
        u32 light = 0;

        for (y = 0; y < 32; y++) {
            for (x = o; x < o + 20; x++) {
                u16 t = pic[y * 64 + x];

                light += ((t >> 11) & 31) + ((t >> 6) & 31) + ((t >> 1) & 31);
            }
        }
        if (light < best) {
            best = light;
            best_o = o;
        }
    }
    for (y = 0; y < 32; y++) {
        u16* row = &pic[y * 64];

        for (x = 0; x < 64; x++) {
            keep[x] = row[x];
        }
        for (d = 0; d < 10; d++) {
            s32 wq = ((10 - d) * 256) / 10;
            s32 side;

            wq = (wq * wq * (3 * 256 - 2 * wq)) / (256 * 256);
            for (side = 0; side < 2; side++) {
                s32 at = side ? (63 - d) : d;
                s32 from = side ? (best_o + 10 - d) : (best_o + 10 + d);
                u16 t0 = keep[at], t1 = keep[from];
                s32 c0[3], c1[3], c[3];

                c0[0] = (t0 >> 11) & 31; c0[1] = (t0 >> 6) & 31; c0[2] = (t0 >> 1) & 31;
                c1[0] = (t1 >> 11) & 31; c1[1] = (t1 >> 6) & 31; c1[2] = (t1 >> 1) & 31;
                for (k = 0; k < 3; k++) {
                    c[k] = c0[k] + (((c1[k] - c0[k]) * wq + 128) >> 8);
                    c[k] = c[k] < 0 ? 0 : c[k] > 31 ? 31 : c[k];
                }
                row[at] = (u16) ((c[0] << 11) | (c[1] << 6) | (c[2] << 1) | (t0 & 1));
            }
        }
    }
}

static void sky_at_night(u32 pic, s32 which) {
    volatile u16* texel = (volatile u16*) pic;
    u32 bins[32];
    s32 lo = 0;
    s32 hi = 255;
    s32 count;
    s32 i;

    // The picture as the cartridge has it, kept for the day end of every blend.
    for (i = 0; i < SKY_TEXELS; i++) {
        sky_orig_pic[which][i] = texel[i];
    }
    dissolve_orig(which);
    for (i = 0; i < 32; i++) {
        bins[i] = 0;
    }
    for (i = 0; i < SKY_TEXELS; i++) {
        u32 t = texel[i];
        u32 r = (((t >> 11) & 31) * 255) / 31;
        u32 g = (((t >> 6) & 31) * 255) / 31;
        u32 b = (((t >> 1) & 31) * 255) / 31;
        u32 lum = (r * 77 + g * 151 + b * 28) >> 8;

        sky_mask[i] = (u8) lum;
        bins[lum >> 3]++;
    }
    // The stretch runs from the 3rd to the 97th hundredth of the texels.
    count = 0;
    for (i = 0; i < 32; i++) {
        count += bins[i];
        if (count >= SKY_TEXELS * 3 / 100) {
            lo = i * 8;
            break;
        }
    }
    count = 0;
    for (i = 31; i >= 0; i--) {
        count += bins[i];
        if (count >= SKY_TEXELS * 3 / 100) {
            hi = i * 8 + 7;
            break;
        }
    }
    if (hi - lo < 16) {
        hi = lo + 16;
    }
    for (i = 0; i < SKY_TEXELS; i++) {
        s32 v = (((s32) sky_mask[i] - lo) * 255) / (hi - lo);

        sky_blur[i] = (u8) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    // Softened: each texel with its neighbors, 1-2-1 both ways.
    for (i = 0; i < SKY_TEXELS; i++) {
        s32 x = i & 63;
        s32 y = i >> 6;
        s32 xl = x > 0 ? x - 1 : x;
        s32 xr = x < 63 ? x + 1 : x;
        s32 yu = y > 0 ? y - 1 : y;
        s32 yd = y < 31 ? y + 1 : y;
        u32 sum = 4 * sky_blur[y * 64 + x]
                + 2 * (sky_blur[y * 64 + xl] + sky_blur[y * 64 + xr] + sky_blur[yu * 64 + x] + sky_blur[yd * 64 + x])
                + sky_blur[yu * 64 + xl] + sky_blur[yu * 64 + xr] + sky_blur[yd * 64 + xl] + sky_blur[yd * 64 + xr];

        sky_mask[i] = (u8) (sum >> 4);
    }
    // The picture meets itself at each of the drum's six edges, its last
    // column against its first, and the two do not agree (a cloud begins at
    // the first). The ten columns either side of the join dissolve into the
    // mask's clearest stretch of twenty, laid across the join so the cloud
    // runs on, as the port does to the day's picture (patches/src/sky_patch.c).
    {
        u32 best = 0xFFFFFFFFu;
        s32 best_o = 10, o, x, d;
        u8 keep[64];

        for (o = 10; o + 20 <= 54; o++) {
            u32 cloud = 0;

            for (i = 0; i < 32; i++) {
                for (x = o; x < o + 20; x++) {
                    cloud += sky_mask[i * 64 + x];
                }
            }
            if (cloud < best) {
                best = cloud;
                best_o = o;
            }
        }
        for (i = 0; i < 32; i++) {
            u8* row = &sky_mask[i * 64];

            for (x = 0; x < 64; x++) {
                keep[x] = row[x];
            }
            for (d = 0; d < 10; d++) {
                s32 w = ((10 - d) * 256) / 10;

                w = (w * w * (3 * 256 - 2 * w)) / (256 * 256);
                row[d] = (u8) (keep[d] + ((((s32) keep[best_o + 10 + d] - keep[d]) * w + 128) >> 8));
                row[63 - d] = (u8) (keep[63 - d] + ((((s32) keep[best_o + 10 - d] - keep[63 - d]) * w + 128) >> 8));
            }
        }
    }
}

// The Valley's sky is one picture cut in two, ten degrees up: the lower
// picture's last row is the upper's first, texel for texel, and by day no join
// shows. Each mask is stretched and dissolved by its own picture, though, so
// along the join the two disagree. Each column's two edge values meet at their
// mean and the difference eases away over six rows either side, so a cloud
// runs through the join.
// First the masks' floor comes off. Open sky in a mask was never nothing: the
// day's picture has a haze of its own, a floor of 10 to 40 that changes from
// row to row. The thinning toward the lid took that floor across one five-bit
// step of the picture in a single row all the way round, a line three steps of
// eight bits tall on every face, and the drum's six-sided rim stood out as a
// box's ceiling (JackandBeans, Nov 5 2026). With no floor, open sky is one
// color of the picture in every row, and only the corners carry its height.
#define SKY_MASK_FLOOR 40
static void sky_masks_lift(void) {
    s32 which, i;

    for (which = 0; which < 2; which++) {
        for (i = 0; i < SKY_TEXELS; i++) {
            s32 v = sky_mask_pic[which][i];

            sky_mask_pic[which][i] = (u8) (v <= SKY_MASK_FLOOR ? 0 : ((v - SKY_MASK_FLOOR) * 255) / (255 - SKY_MASK_FLOOR));
        }
    }
}

static void sky_masks_meet(void) {
    static const s32 weight[6] = { 256, 237, 190, 128, 66, 19 };
    s32 x, r;

    for (x = 0; x < 64; x++) {
        s32 gap = (s32) sky_mask_pic[1][31 * 64 + x] - (s32) sky_mask_pic[0][x];

        for (r = 0; r < 6; r++) {
            s32 upper = (gap * weight[r]) / 512;
            s32 lower = upper - (gap * weight[r]) / 256;     // the rest of the gap, so the two edges come out equal
            s32 a = (s32) sky_mask_pic[0][r * 64 + x] + upper;
            s32 b = (s32) sky_mask_pic[1][(31 - r) * 64 + x] + lower;

            sky_mask_pic[0][r * 64 + x] = (u8) (a < 0 ? 0 : a > 255 ? 255 : a);
            sky_mask_pic[1][(31 - r) * 64 + x] = (u8) (b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
}

// Each row's floor under the mask, a little smoothed between rows and never
// under the row's own least, so that open sky comes to nothing (sky_hazes).
static void sky_floor_make(const Night* by) {
    const SkyHaze* haze = sky_haze_of(by);
    u8 least[32];
    s32 r, x;

    for (r = 0; r < 32; r++) {
        sky_floor[r] = 0;
    }
    if (haze == NULL) {
        return;
    }
    for (r = 0; r < 32; r++) {
        u8 m = 255;

        for (x = 0; x < 64; x++) {
            if (sky_mask_pic[0][r * 64 + x] < m) {
                m = sky_mask_pic[0][r * 64 + x];
            }
        }
        least[r] = m;
    }
    for (r = 0; r < haze->keep; r++) {
        sky_floor[r] = haze->low;       // their open sky is the corners', so they come out as they were
    }
    for (r = haze->keep; r < 32; r++) {
        s32 below = (r - 1 < haze->keep) ? r : r - 1;
        s32 above = (r + 1 > 31) ? 31 : r + 1;
        s32 smooth = ((s32) least[below] + 2 * (s32) least[r] + (s32) least[above]) / 4;

        sky_floor[r] = (u8) (smooth > (s32) least[r] ? smooth : (s32) least[r]);
    }
}

// How low and how high the sky box's corners reach, as sines of elevation,
// so that the lowest ring takes the horizon's tint and the highest the top's.
// The top ring's corners are whole numbers, and their sines differ in the fourth
// place: the least of them is the top, so that every one of them is all the way
// up and they take one color (the lid's seams, Nov 6 2026).
static void sky_extent(const Night* by) {
    s32 top_y = -32768;
    s32 i, j;

    sky_sin_lo = 1.0f;
    sky_sin_hi = 1.0f;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < by->sky_vtx_n[i]; j++) {
            s32 y = ((volatile s16*) (by->sky_vtx[i] + 16 * j))[1];

            if (y > top_y) {
                top_y = y;
            }
        }
    }
    for (i = 0; i < 2; i++) {
        for (j = 0; j < by->sky_vtx_n[i]; j++) {
            volatile s16* pos = (volatile s16*) (by->sky_vtx[i] + 16 * j);
            f32 x = pos[0];
            f32 y = pos[1];
            f32 z = pos[2];
            f32 len = __builtin_sqrtf(x * x + y * y + z * z);

            if (len > 1.0f) {
                f32 sine = y / len;

                if (sine < sky_sin_lo) {
                    sky_sin_lo = sine;
                }
                if (pos[1] >= top_y - 2 && sine < sky_sin_hi) {
                    sky_sin_hi = sine;
                }
            }
        }
    }
    if (sky_sin_hi - sky_sin_lo < 0.1f) {
        sky_sin_hi = sky_sin_lo + 0.1f;
    }
}

// The sky box's corners carry the night: each corner's color is the tint at
// the horizon blended toward the tint at the top by the corner's height, as
// the game's own corners carry the day's blue above and haze below. The
// walker is told these are done, so the scenery's light does not fall on
// the sky. At the sunset's and the dawn's hours the Valley's sky is lighter
// toward the sun, the more the higher, so the colors near the lid do not all
// run along the drum's rings. No deeper away from it: the ring would stand
// darker than the lid above it. Near the top the warmth evens out around the
// sky, to what it comes to on the whole, so the top ring and the lid are one
// color (Nov 6 2026): around is its share as a corner faces the sun, even its
// share whichever way the corner faces.
static const u8 sky_sun_side[3] = { 14, 8, 4 };
static u8 sky_lo_now[3];        // tint_lo as the corners last took it: the lava's glow is on it for that moment only

static void sky_warm_shares(f32 up, f32* around, f32* even) {
    f32 f = (up - 0.55f) / 0.45f;
    f32 s = f <= 0.0f ? 0.0f : f >= 1.0f ? 1.0f : f * f * (3.0f - 2.0f * f);

    *around = up * (1.0f - s);
    *even = up * s * 0.31831f;      // one over pi: the mean around a circle of a facing that stops at nothing
}

static void sky_corners(u32 addr, s32 count, s32 which) {
    f32 life = (night->setup == NIGHT_VALLEY_SETUP) ? ((f32) cloud_top_fade / 255.0f) * clear_share : 0.0f;
    const SkyHaze* haze = sky_haze_of(night);
    f32 top_now = 0.0f;
    s32 i;

    if (haze != NULL) {
        // The top fade took the top ring's haze; as it goes, the haze comes back.
        top_now = (f32) haze->top + ((f32) sky_floor[31] - (f32) haze->top) * (f32) (255 - (s32) cloud_top_fade) / 255.0f;
    }
    sky_lo_now[0] = tint_lo[0];
    sky_lo_now[1] = tint_lo[1];
    sky_lo_now[2] = tint_lo[2];
    for (i = 0; i < count; i++) {
        volatile s16* pos = (volatile s16*) (addr + 16 * i);
        volatile u8* color = (volatile u8*) (addr + 16 * i + 12);
        f32 x = pos[0];
        f32 y = pos[1];
        f32 z = pos[2];
        f32 len = __builtin_sqrtf(x * x + y * y + z * z);
        f32 up = len > 1.0f ? (y / len - sky_sin_lo) / (sky_sin_hi - sky_sin_lo) : 0.0f;
        f32 flat = __builtin_sqrtf(x * x + z * z);
        f32 facing = flat > 1.0f ? (x * relight_dir[0] + z * relight_dir[2]) / flat : 0.0f;
        f32 around, even;
        s32 share;
        s32 toward;
        s32 k;

        if (up < 0.0f) {
            up = 0.0f;
        }
        if (up > 1.0f) {
            up = 1.0f;
        }
        sky_warm_shares(up, &around, &even);
        share = (s32) (up * 256.0f);        // the height's share, horizon to top
        toward = (s32) ((0.5f + 0.5f * facing) * 256.0f);
        if (!sky_orig_cn_kept[which] && i < 64) {
            sky_orig_cn[which][i] = ((u32) color[0] << 16) | ((u32) color[1] << 8) | color[2];
        }
        for (k = 0; k < 3; k++) {
            s32 low = tint_away[k] + (((s32) tint_lo[k] - tint_away[k]) * toward) / 256;
            s32 tint = low + (((s32) tint_hi[k] - low) * share) / 256 + (s32) (life * (f32) sky_sun_side[k] * ((facing > 0.0f ? facing : 0.0f) * around + even));
            s32 day;

            if (haze != NULL) {
                // The open sky the picture no longer holds at this height, as far as it has left it,
                // and what five bits made of the picture.
                f32 a = (y - (f32) haze->low_y) / (f32) (haze->top_y - haze->low_y);
                f32 h, fit;

                a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
                h = ((f32) haze->low + (top_now - (f32) haze->low) * a) / 255.0f;
                fit = (f32) pic_sky[k] + (f32) ((s32) pic_cloud[k] - (s32) pic_sky[k]) * h;
                tint = (s32) ((f32) tint * fit / (f32) sky_base(haze, k) * sky_gain[k]);
                tint = tint > 255 ? 255 : tint;
            }
            day = (i < 64) ? (s32) ((sky_orig_cn[which][i] >> (16 - 8 * k)) & 0xFF) : tint;

            if (i < 64 && overcast_share > 0.0f) {
                day += (s32) ((f32) (255 - day) * overcast_share);     // a day under weather: the drum untinted, the overcast flat
            }
            color[k] = (u8) (day + ((tint - day) * (s32) (nightness * 256.0f)) / 256);
        }
        mark(tinted, addr + 16 * i);
    }
}

// The world's clock: its own on the ride, the clock a photo recorded when it
// is a photo's world. It advances by the set-up's sky speed each tick (a
// sixtieth of a second: the game takes two steps for each frame it draws),
// is kept to thousandths, and starts over at 10000 of its units (block.c).
static f32 world_clock(void) {
    return on_the_ride ? GlobalTimer : photo_clock;
}

// The clock's advance each tick: the set-up's sky speed.
static f32 clock_speed(const Night* by) {
    WorldSetup* setup = (WorldSetup*) by->setup;
    f32 speed = setup->unk_10;

    return speed < 0.05f ? 1.0f : speed;
}

// The course's time in whole ticks since the clock's start, rounded: the
// clock's quotient by the speed can come out a hair under the whole tick,
// and cut down it lost a tick now and then, so that on the Beach and the
// Volcano (0.4) a frame stood still and the next stepped two, as often as
// one frame in seven.
static u32 ticks_of(const Night* by) {
    return (u32) (world_clock() / clock_speed(by) + 0.5f);
}

// How many times the clock has started over: on the ride, counted as it
// happens; in a photo's world, what the photo's stamp says (stamp_of). Only
// a fall of more than half a round counts: a stopped clock can lose a
// thousandth to its own rounding (world/block.c), and the sky's speed is
// set to nothing in the course's cut-scenes.
static u32 clock_rounds(void) {
    if (!on_the_ride) {
        return photo_rounds;
    }
    if (GlobalTimer < ride_clock_last - 5000.0f) {
        ride_rounds++;
    }
    ride_clock_last = GlobalTimer;
    return ride_rounds;
}

// The ride's time in whole ticks since its start, for what happens once over
// a ride or is timed ahead: it does not start over with the clock, which
// keeps what was over as it starts over, so it runs on without a step. With
// no rounds it is ticks_of to the tick.
static u32 ride_ticks_of(const Night* by) {
    f32 rounds = (f32) clock_rounds();

    return (u32) ((rounds * 10000.0f + world_clock()) / clock_speed(by) + 0.5f);
}

// The stars twinkle. Each group's brightness is its base, plus two slow
// waves of different lengths so that it never quite repeats, plus -- for
// some -- a brief swell now and then. Most groups are calm, some breathe, a
// few shimmer; none flashes. The clock is the world's own (a tick is a
// sixtieth of a second) on the ride, and the clock a photo recorded when
// it is a photo's sky, so a photo keeps the twinkle of its instant.
static void twinkle(const Night* by) {
    u32 ticks = ticks_of(by);
    s32 i;

    for (i = 0; i < by->twinkles && i < NIGHT_GROUPS_MAX; i++) {
        const NightTwinkle* group = &night_twinkles[i];
        s32 one = night_sine[((group->phase1 + ticks * group->rate1) >> 8) & 0xFF];
        s32 two = night_sine[((group->phase2 + ticks * group->rate2) >> 8) & 0xFF];
        s32 value = group->base + (group->swing * (3 * one + 2 * two)) / (5 * 127);

        if (group->swell != 0) {
            s32 at = (s32) ((group->phase3 + ticks * group->rate3) & 0xFFFF);

            if (at < 0x0C00) {
                value += (group->swell * (at < 0x0600 ? at : 0x0C00 - at)) / 0x0600;
            }
        }
        if (value < 40) {
            value = 40;
        }
        if (value > 255) {
            value = 255;
        }
        value = (value * (s32) star_fade) / 255;
        by->twinkle[i][0].words.w1 = 0xFFFFFF00u | (u32) value;
    }
    // The bright stars' steady hearts, one command after the groups'.
    by->twinkle[by->twinkles][0].words.w1 = 0xFFFFFF00u | (u32) ((235 * (s32) star_fade) / 255);
}

// The mod's own lists live in the mod's own memory, above the console's. The
// renderer reads an address up there only between these two commands of its
// extended set (rt64_extended_gbi.h: opcode 0x64, G_EX_SETRDRAMEXTENDED_V1 =
// 0x2C). Every call the mod puts into the game's lists carries a pair of its
// own (the light, a part's reload, every extra, the sky's spare pair, a roofed
// course's finer faces); this pair goes around the sky's drawing for the fine
// sky alone, the one call that takes the drum's own place.
static u8 ext_open;     // the sky's window of the mod's own addresses is open in this frame's main list

// The window opens only with room for both of its ends; the fine sky goes in only while it is open (and
// otherwise the drum stays the game's own), and it closes only if it opened.
static void extended_addresses(s32 on) {
    Gfx* cmd;

    if (on) {
        ext_open = (u8) gfx_may(MAIN_GFX_POS[0], 2);
        if (!ext_open) {
            return;
        }
    } else {
        if (!ext_open) {
            return;
        }
        ext_open = 0;
    }
    cmd = MAIN_GFX_POS[0]++;
    cmd->words.w0 = (0x64u << 24) | 0x00002Cu;
    cmd->words.w1 = on ? 1u : 0u;
}

static void command(Gfx** list, u32 w0, u32 w1) {
    Gfx* cmd = (*list)++;

    cmd->words.w0 = w0;
    cmd->words.w1 = w1;
}

// ---- The fine sky ----
// At every hour but the cartridge's own Day the sky is drawn from here, the same
// pictures in the same places. Three things drew a six-sided line across it at
// the port's resolution (JackandBeans's pictures, Nov 6 2026): the top ring's
// corners rounded apart, and the lid showed straight seams; the color went by
// height on six flat faces, so its level lines were six-sided and its slope broke
// along them; and the last rounding to eight bits made steps of those lines. So a
// much finer drum stands in for the port's, each corner colored by its direction
// from the eye: the lid and the sky around it one color, the level lines round,
// and the slope never breaking at a face. The rings' colors, the curve between
// them and the corners' law are the port's and sky_corners'; the height they are
// read at is the height the corner's elevation has on the drum, around the ring.
// A corner's color has eight bits, and on a drum this fine their rounding stands
// out as lumps: so the faces are drawn twice, the second time at an eighth, the
// same picture under corners that make up what the first lost. The sum is the
// picture times a color good to an eighth of a level, in each channel.
// And the sky's own draw adds half a level of noise either way before the
// rounding, new at each pixel and each frame, so the rounding makes no steps.
// The day's end of every blend is the port's own colors, laid on its own cuts,
// so nothing moves as an hour leaves Day; at Day itself the port's drum is drawn.
#define FINE_CUT 12             // each of the game's triangles is cut in twelve along each side: the port's four, by three
#define FINE_CUT_PORT 4
#define FINE_TRI_VTX (((FINE_CUT + 1) * (FINE_CUT + 2)) / 2)
#define FINE_PER (FINE_CUT / FINE_CUT_PORT)
#define FINE_TRI_CMDS (FINE_CUT + (FINE_CUT * (FINE_CUT + 1)) / 2)     // a load a row, its triangles two to a command
#define FINE_TRIS 80            // a drum has 28; the Tunnel's three painted boxes 74
#define FINE_VTX (FINE_TRIS * FINE_TRI_VTX)
#define FINE_CMDS (FINE_TRIS * FINE_TRI_CMDS * 2 + 256)
#define FINE_EIGHTH 32          // the second pass's share, of 255: its corners' alpha
#define FINE_PARTS 4
#define FINE_PATCH 4            // the commands a roofed course's call takes in its list
#define FINE_RDRAM 0x6400002Cu  // the renderer's extended addresses (extended_addresses)
#define FINE_PLAIN_W0 0xFC121824u   // the cartridge's sky: TEXEL0 * SHADE
#define FINE_PLAIN_W1 0xFF33FFFFu
#define FINE_NOISE_W0 0xFC1218EFu   // TEXEL0 * SHADE, then (NOISE - K4) * K5 + COMBINED
#define FINE_NOISE_W1 0xF7FFFE38u
#define FINE_KEYS 0x00010001u       // K4 128 of 255, the noise's middle; K5 1 of 255, its size: a level, end to end
#define FINE_ZETA 0.91f             // a six-sided ring's mean radius, of its corners'

typedef struct FineCorner {
    f32 w[3];           // its share of each ring's color
    f32 hx, hz;         // its way around the sky, as a unit
    f32 warm;           // its share of the sun's warmth, as it faces the sun
    f32 even;           // and all round, whichever way it faces
    f32 a;              // on a roofed course: how far up its picture it stands, for the picture's haze
    u16 day[3];         // the day's color at it, in 256ths of a level
} FineCorner;

typedef struct FinePart {       // a roofed course's run of sky faces
    u32 from, to;               // the commands it stands in for
    u32 saved[FINE_PATCH * 2];  // the words its call covers
    f32 up_lo, up_hi;           // how high its lowest and highest corners stand (sky_up)
    u16 vtx0, vtx_n, list0;
    u8 paint[3];                // its corners' paint
    u8 tris, on, bad, turn, hazed;
} FinePart;

static Vtx fine_vtx[FRAME_CTX][2][FINE_VTX];    // each frame context's set: the first pass's corners, the second's
static Gfx fine_list[FRAME_CTX][FINE_CMDS];
static Gfx fine_entry[FRAME_CTX][2];
static u32 fine_rest;                   // the game's own sky list, which the port's drum branches to after it
static u32 sky_tail_for(s32 c);
static FineCorner fine_corner[FINE_VTX];
static FinePart fine_part[FINE_PARTS];
static s16 fine_tri[FINE_TRIS][3][5];   // the drum's triangles: each corner's x, y, z, s, t
static s32 fine_parts, fine_vtx_n, fine_cmd_n;
static u8 fine_turn, fine_ready, fine_failed, fine_full;
static u32 fine_sum[FRAME_CTX];
static f32 fine_ring_y[3], fine_ring_up[3];
static f32 fine_node[3][8][3];          // the rings' corners' day colors, around
static f32 fine_mean_top[3];
static s32 fine_nodes;
static f32 fine_az0, fine_step, fine_top_sin2;

// A course's load starts the fine sky over.
static void fine_reset(void) {
    fine_parts = 0;
    fine_vtx_n = 0;
    fine_cmd_n = 0;
    fine_ready = 0;
    fine_failed = 0;
    fine_full = 0;
    fine_open = 0;
    fine_new = 0;
    fine_sum[0] = fine_sum[1] = 0;
}

static void fine_emit(u32 w0, u32 w1) {
    s32 k;

    if (fine_cmd_n >= FINE_CMDS) {
        fine_full = 1;
        return;
    }
    for (k = 0; k < FRAME_CTX; k++) {
        fine_list[k][fine_cmd_n].words.w0 = w0;
        fine_list[k][fine_cmd_n].words.w1 = w1;
    }
    fine_cmd_n++;
}

// A point of the cut triangle, in whole numbers and the same from either side of
// a shared edge, so two faces meet without a crack.
static s32 fine_mix(s32 a, s32 b, s32 c, s32 i, s32 j) {
    s32 sum = a * (FINE_CUT - i - j) + b * i + c * j;

    return sum >= 0 ? (sum + FINE_CUT / 2) / FINE_CUT : -((-sum + FINE_CUT / 2) / FINE_CUT);
}

// A triangle's commands for one pass: a load for each row and the next, the
// triangles between them wound as the game's, two to a command.
static void fine_rows(s32 base, s32 pass) {
    s32 row = base;
    s32 i, j, k;

    for (j = 0; j < FINE_CUT; j++) {
        s32 across = FINE_CUT + 1 - j;
        s32 count = across + across - 1;
        u32 first = 0;
        s32 have = 0;

        for (k = 0; k < FRAME_CTX; k++) {
            fine_list[k][fine_cmd_n].words.w0 = (0x01u << 24) | ((u32) count << 12) | ((u32) count << 1);
            fine_list[k][fine_cmd_n].words.w1 = (u32) &fine_vtx[k][pass][row];
        }
        fine_cmd_n++;
        for (i = 0; i < across - 1; i++) {
            s32 t;

            for (t = 0; t < ((i < across - 2) ? 2 : 1); t++) {
                u32 p = (u32) (t == 0 ? i : i + 1);
                u32 q = (u32) (t == 0 ? i + 1 : across + i + 1);
                u32 r = (u32) (across + i);
                u32 word = (p * 2) << 16 | (q * 2) << 8 | (r * 2);

                if (have) {
                    fine_emit((0x06u << 24) | first, word);
                    have = 0;
                } else {
                    first = word;
                    have = 1;
                }
            }
        }
        if (have) {
            fine_emit((0x05u << 24) | first, 0);
        }
        row += across;
    }
}

// Cuts one triangle, its corners five numbers each (x, y, z and the picture's s,
// t), into every set of corners, row by row, and its first pass into both lists.
static s32 fine_cut_face(const s16* a, const s16* b, const s16* c) {
    s32 base = fine_vtx_n;
    s32 n = base;
    s32 i, j, k;

    if (fine_vtx_n + FINE_TRI_VTX > FINE_VTX || fine_cmd_n + FINE_TRI_CMDS * 2 + 32 > FINE_CMDS) {
        fine_full = 1;
        return -1;
    }
    for (j = 0; j <= FINE_CUT; j++) {
        for (i = 0; i + j <= FINE_CUT; i++) {
            for (k = 0; k < 4; k++) {
                Vtx* p = &fine_vtx[k >> 1][k & 1][n];

                p->v.ob[0] = (s16) fine_mix(a[0], b[0], c[0], i, j);
                p->v.ob[1] = (s16) fine_mix(a[1], b[1], c[1], i, j);
                p->v.ob[2] = (s16) fine_mix(a[2], b[2], c[2], i, j);
                p->v.flag = 0;
                p->v.tc[0] = (s16) fine_mix(a[3], b[3], c[3], i, j);
                p->v.tc[1] = (s16) fine_mix(a[4], b[4], c[4], i, j);
                p->v.cn[0] = p->v.cn[1] = p->v.cn[2] = 0;
                p->v.cn[3] = (k & 1) ? FINE_EIGHTH : 255;
            }
            n++;
        }
    }
    fine_vtx_n = n;
    fine_rows(base, 0);
    return base;
}

// The azimuth as the port reckons it (patches/src/sky_patch.c), degrees.
static f32 fine_atan_unit(f32 z) {
    f32 z2 = z * z;

    return z * (0.99997726f + z2 * (-0.33262347f + z2 * (0.19354346f + z2 * (-0.11643287f + z2 * (0.05265332f + z2 * -0.01172120f)))));
}

static f32 fine_azimuth(f32 x, f32 z) {
    f32 ax = x < 0.0f ? -x : x;
    f32 az = z < 0.0f ? -z : z;
    f32 a;

    if (ax < 0.001f && az < 0.001f) {
        return 0.0f;
    }
    if (az >= ax) {
        a = fine_atan_unit(ax / az);
    } else {
        a = 1.57079633f - fine_atan_unit(az / ax);
    }
    if (z < 0.0f) {
        a = 3.14159265f - a;
    }
    if (x < 0.0f) {
        a = -a;
    }
    return a * (180.0f / 3.14159265f);
}

// The port's curve between the rings, as each ring's share of the color.
static void fine_shares(f32 y, f32* w) {
    f32 y0 = fine_ring_y[0], y1 = fine_ring_y[1], y2 = fine_ring_y[2];

    if (y >= y2) {
        w[0] = 0.0f;
        w[1] = 0.0f;
        w[2] = 1.0f;
    } else if (y <= y0) {
        w[0] = 1.0f;
        w[1] = 0.0f;
        w[2] = 0.0f;
    } else {
        s32 low = y < y1;
        f32 span = low ? y1 - y0 : y2 - y1;
        f32 u = (y - (low ? y0 : y1)) / span;
        f32 u2 = u * u;
        f32 u3 = u2 * u;
        f32 h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
        f32 h10 = u3 - 2.0f * u2 + u;
        f32 h01 = -2.0f * u3 + 3.0f * u2;
        f32 h11 = u3 - u2;
        f32 m = span / (y2 - y0);

        if (low) {
            w[0] = h00 - h11 * m;
            w[1] = h01;
            w[2] = h11 * m;
        } else {
            w[0] = -h10 * m;
            w[1] = h00;
            w[2] = h01 + h10 * m;
        }
    }
}

// The day's color at a point of the port's cuts: its sky_paint, from the game's
// corners as the cartridge has them.
static void fine_port_color(f32 x, f32 y, f32 z, f32* out) {
    f32 ring[3][3];
    f32 f = (fine_azimuth(x, z) - fine_az0) / fine_step;
    s32 seg = (s32) f;
    f32 t, t2, t3, w[4];
    s32 n[4];
    s32 r, k;

    if ((f32) seg > f) {
        seg--;
    }
    t = f - (f32) seg;
    t2 = t * t;
    t3 = t2 * t;
    w[0] = (-t3 + 2.0f * t2 - t) * 0.5f;
    w[1] = (3.0f * t3 - 5.0f * t2 + 2.0f) * 0.5f;
    w[2] = (-3.0f * t3 + 4.0f * t2 + t) * 0.5f;
    w[3] = (t3 - t2) * 0.5f;
    for (k = 0; k < 4; k++) {
        s32 q = (seg - 1 + k) % fine_nodes;

        n[k] = q < 0 ? q + fine_nodes : q;
    }
    for (r = 0; r < 3; r++) {
        for (k = 0; k < 3; k++) {
            ring[r][k] = w[0] * fine_node[r][n[0]][k] + w[1] * fine_node[r][n[1]][k] + w[2] * fine_node[r][n[2]][k] + w[3] * fine_node[r][n[3]][k];
        }
    }
    if (y >= fine_ring_y[2] - 1.0f) {
        f32 len2 = x * x + y * y + z * z;
        f32 sin2 = (len2 > 1.0f) ? (y * y) / len2 : 0.0f;
        f32 dome = (fine_top_sin2 < 0.999f) ? (sin2 - fine_top_sin2) / (1.0f - fine_top_sin2) : 0.0f;

        dome = dome < 0.0f ? 0.0f : dome > 1.0f ? 1.0f : dome;
        dome = dome * dome * (3.0f - 2.0f * dome);
        for (k = 0; k < 3; k++) {
            out[k] = ring[2][k] + (fine_mean_top[k] - ring[2][k]) * dome;
        }
    } else if (y <= fine_ring_y[0] + 1.0f) {
        for (k = 0; k < 3; k++) {
            out[k] = ring[0][k];
        }
    } else {
        f32 s[3];

        fine_shares(y, s);
        for (k = 0; k < 3; k++) {
            out[k] = s[0] * ring[0][k] + s[1] * ring[1][k] + s[2] * ring[2][k];
        }
    }
    for (k = 0; k < 3; k++) {
        f32 c = out[k] + 0.5f;

        out[k] = (f32) (c < 0.0f ? 0 : c > 255.0f ? 255 : (s32) c);
    }
}

// A triangle's day colors: the port's at its own cuts, laid straight across them
// as the port's triangles lay them, so the sky at the day's end is the port's.
static void fine_day_face(s32 base, const s16* a, const s16* b, const s16* c) {
    f32 col[FINE_CUT_PORT + 1][FINE_CUT_PORT + 1][3];
    s32 n = base;
    s32 i, j, k;

    for (j = 0; j <= FINE_CUT_PORT; j++) {
        for (i = 0; i + j <= FINE_CUT_PORT; i++) {
            f32 u = (f32) i / (f32) FINE_CUT_PORT;
            f32 v = (f32) j / (f32) FINE_CUT_PORT;
            f32 w = 1.0f - u - v;

            fine_port_color(w * a[0] + u * b[0] + v * c[0], w * a[1] + u * b[1] + v * c[1], w * a[2] + u * b[2] + v * c[2], col[i][j]);
        }
    }
    for (j = 0; j <= FINE_CUT; j++) {
        for (i = 0; i + j <= FINE_CUT; i++) {
            s32 ia = i / FINE_PER, ja = j / FINE_PER, fa = i % FINE_PER, fb = j % FINE_PER;
            FineCorner* corner = &fine_corner[n++];

            for (k = 0; k < 3; k++) {
                f32 v;

                if (fa + fb <= FINE_PER) {
                    v = col[ia][ja][k] * (f32) (FINE_PER - fa - fb);
                    if (fa > 0) {
                        v += col[ia + 1][ja][k] * (f32) fa;
                    }
                    if (fb > 0) {
                        v += col[ia][ja + 1][k] * (f32) fb;
                    }
                } else {
                    v = col[ia + 1][ja][k] * (f32) (FINE_PER - fb) + col[ia][ja + 1][k] * (f32) (FINE_PER - fa) + col[ia + 1][ja + 1][k] * (f32) (fa + fb - FINE_PER);
                }
                corner->day[k] = (u16) (v * (256.0f / (f32) FINE_PER) + 0.5f);
            }
        }
    }
}

// Each corner's shares by its direction. The height is what its elevation has on
// the drum, reckoned at the ring's mean radius low down and at its corners' at
// the top, so the whole top ring is up at the elevation of its lowest corner.
static void fine_drum_shares(f32 r1, f32 rt, f32 t_cap) {
    f32 y1 = fine_ring_y[1], y2 = fine_ring_y[2];
    f32 slant = (rt - r1) / (y2 - y1);
    f32 reach = r1 - slant * y1;
    f32 t1 = y1 / r1;
    s32 i;

    for (i = 0; i < fine_vtx_n; i++) {
        FineCorner* corner = &fine_corner[i];
        f32 x = fine_vtx[0][0][i].v.ob[0];
        f32 y = fine_vtx[0][0][i].v.ob[1];
        f32 z = fine_vtx[0][0][i].v.ob[2];
        f32 rho = __builtin_sqrtf(x * x + z * z);
        f32 len = __builtin_sqrtf(rho * rho + y * y);
        f32 up = len > 1.0f ? (y / len - sky_sin_lo) / (sky_sin_hi - sky_sin_lo) : 1.0f;

        corner->hx = rho > 1.0f ? x / rho : 0.0f;
        corner->hz = rho > 1.0f ? z / rho : 0.0f;
        sky_warm_shares(up < 0.0f ? 0.0f : up > 1.0f ? 1.0f : up, &corner->warm, &corner->even);
        if (y >= y2 - 1.0f || rho < 1.0f || y / rho >= t_cap) {
            corner->w[0] = 0.0f;
            corner->w[1] = 0.0f;
            corner->w[2] = 1.0f;
            sky_warm_shares(1.0f, &corner->warm, &corner->even);
        } else {
            f32 t = y / rho;
            f32 tt = t * (FINE_ZETA + (1.0f - FINE_ZETA) * (t > 0.0f ? t / t_cap : 0.0f));

            fine_shares(tt >= t1 ? tt * reach / (1.0f - tt * slant) : tt * r1, corner->w);
        }
    }
}

// The drum, from the fine drum the port made of the game's list: the state in its
// order, each of its cut triangles cut finer, the sky's combiner with the noise
// added, two cycles for it, and all as it was at the end. The game's list still
// runs after it, with what the mod has added there.
static s32 fine_drum_build(u32 entry) {
    volatile u32* e = (volatile u32*) entry;
    volatile u32* list;
    f32 ring_at[3][8][3];
    s32 tris = 0;
    s32 rings = 0;
    s32 n, i, c, r, k, m;
    u32 w0;

    if (e[0] != 0xDE000000u || e[2] != 0xDE010000u) {
        return 0;
    }
    list = (volatile u32*) e[1];
    for (n = 0; n < 2048; n++) {
        w0 = list[n * 2];
        if ((w0 >> 24) == 0xDF) {
            break;
        }
        if ((w0 >> 24) == 0x01) {
            volatile s16* v = (volatile s16*) list[n * 2 + 1];

            if (((w0 >> 12) & 0xFF) != 15 || tris >= FINE_TRIS) {
                return 0;
            }
            for (c = 0; c < 3; c++) {
                volatile s16* at = v + (c == 0 ? 0 : c == 1 ? 4 : 14) * 8;

                fine_tri[tris][c][0] = at[0];
                fine_tri[tris][c][1] = at[1];
                fine_tri[tris][c][2] = at[2];
                fine_tri[tris][c][3] = at[4];
                fine_tri[tris][c][4] = at[5];
            }
            tris++;
        }
    }
    if (n >= 2048 || tris == 0) {
        return 0;
    }
    // The rings, lowest first, and each one's corners around, as the port finds them.
    for (i = 0; i < tris; i++) {
        for (c = 0; c < 3; c++) {
            f32 y = fine_tri[i][c][1];

            for (r = 0; r < rings; r++) {
                if (y > fine_ring_y[r] - 2.0f && y < fine_ring_y[r] + 2.0f) {
                    break;
                }
            }
            if (r == rings) {
                if (rings >= 3) {
                    return 0;
                }
                fine_ring_y[rings++] = y;
            }
        }
    }
    if (rings != 3) {
        return 0;
    }
    for (r = 0; r < 3; r++) {
        for (m = r + 1; m < 3; m++) {
            if (fine_ring_y[m] < fine_ring_y[r]) {
                f32 t = fine_ring_y[r];

                fine_ring_y[r] = fine_ring_y[m];
                fine_ring_y[m] = t;
            }
        }
    }
    for (r = 0; r < 3; r++) {
        f32 az[8];
        s32 count = 0;

        for (i = 0; i < tris; i++) {
            for (c = 0; c < 3; c++) {
                f32 x = fine_tri[i][c][0], y = fine_tri[i][c][1], z = fine_tri[i][c][2];
                f32 a;

                if (!(y > fine_ring_y[r] - 2.0f && y < fine_ring_y[r] + 2.0f)) {
                    continue;
                }
                a = fine_azimuth(x, z);
                for (m = 0; m < count; m++) {
                    f32 d = a - az[m];

                    d = d < 0.0f ? -d : d;
                    if (d < 2.0f || d > 358.0f) {
                        break;
                    }
                }
                if (m == count) {
                    if (count >= 8) {
                        return 0;
                    }
                    az[count] = a;
                    ring_at[r][count][0] = x;
                    ring_at[r][count][1] = y;
                    ring_at[r][count][2] = z;
                    count++;
                }
            }
        }
        if (count < 4 || (r > 0 && count != fine_nodes)) {
            return 0;
        }
        for (m = 0; m < count; m++) {
            for (k = m + 1; k < count; k++) {
                if (az[k] < az[m]) {
                    f32 t = az[m];

                    az[m] = az[k];
                    az[k] = t;
                    for (c = 0; c < 3; c++) {
                        t = ring_at[r][m][c];
                        ring_at[r][m][c] = ring_at[r][k][c];
                        ring_at[r][k][c] = t;
                    }
                }
            }
        }
        if (r == 0) {
            fine_nodes = count;
            fine_az0 = az[0];
            fine_step = 360.0f / (f32) count;
        }
    }
    // The corners' day colors, as the cartridge has them, by place.
    for (r = 0; r < 3; r++) {
        for (m = 0; m < fine_nodes; m++) {
            s32 found = 0;
            s32 at, j;

            for (at = 0; at < 2 && !found; at++) {
                for (j = 0; j < night->sky_vtx_n[at] && j < 64 && !found; j++) {
                    volatile s16* pos = (volatile s16*) (night->sky_vtx[at] + 16 * j);

                    if ((f32) pos[0] == ring_at[r][m][0] && (f32) pos[1] == ring_at[r][m][1] && (f32) pos[2] == ring_at[r][m][2]) {
                        for (k = 0; k < 3; k++) {
                            fine_node[r][m][k] = (f32) ((sky_orig_cn[at][j] >> (16 - 8 * k)) & 0xFF);
                        }
                        found = 1;
                    }
                }
            }
            if (!found) {
                return 0;
            }
        }
    }
    {
        f32 t_cap = 1000.0f;
        f32 x = ring_at[2][0][0], y = ring_at[2][0][1], z = ring_at[2][0][2];
        f32 len2 = x * x + y * y + z * z;

        fine_top_sin2 = (len2 > 1.0f) ? (y * y) / len2 : 1.0f;
        for (k = 0; k < 3; k++) {
            f32 s = 0.0f;

            for (m = 0; m < fine_nodes; m++) {
                s += fine_node[2][m][k];
            }
            fine_mean_top[k] = s / (f32) fine_nodes;
        }
        for (r = 0; r < 3; r++) {
            f32 s = 0.0f;

            for (m = 0; m < fine_nodes; m++) {
                f32 ax = ring_at[r][m][0], ay = ring_at[r][m][1], az = ring_at[r][m][2];
                f32 rho = __builtin_sqrtf(ax * ax + az * az);

                s += ay / __builtin_sqrtf(rho * rho + ay * ay);
                if (r == 2 && ay / rho < t_cap) {
                    t_cap = ay / rho;
                }
            }
            s = (s / (f32) fine_nodes - sky_sin_lo) / (sky_sin_hi - sky_sin_lo);
            fine_ring_up[r] = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
        }
        fine_ring_up[2] = 1.0f;
        // The lists. The rectangle first: the renderer seeds its noise with the
        // draw's number times the pixel's place, and the frame's first draw is
        // number nought, which would give every pixel of it one value. Under
        // the sky, it never shows.
        fine_vtx_n = 0;
        fine_cmd_n = 0;
        fine_emit(0xE7000000u, 0);
        fine_emit(0xFCFFFFFFu, 0xFFFFFFFFu);
        fine_emit(0xF6004004u, 0);
        fine_emit(0xE7000000u, 0);
        fine_emit(0xE3000A01u, 0x00100000u);                    // two cycles
        fine_emit(0xE200001Cu, G_RM_PASS | G_RM_AA_OPA_SURF2);  // as drawSkyBox2Cycle has them
        fine_emit(0xEC000000u, FINE_KEYS);
        i = 0;
        c = 0;      // where the run of triangles not yet given their second pass begins
        for (n = 0; n < 2048; n++) {
            u32 w1;

            w0 = list[n * 2];
            w1 = list[n * 2 + 1];
            if ((w0 >> 24) == 0x01) {
                s32 base = fine_cut_face(fine_tri[i][0], fine_tri[i][1], fine_tri[i][2]);

                if (base < 0) {
                    return 0;
                }
                fine_day_face(base, fine_tri[i][0], fine_tri[i][1], fine_tri[i][2]);
                i++;
            } else if ((w0 >> 24) != 0x05 && (w0 >> 24) != 0x06 && (w0 >> 24) != 0x07) {
                if (c < fine_vtx_n) {
                    fine_emit(0xE7000000u, 0);
                    fine_emit(0xE200001Cu, G_RM_PASS | G_RM_XLU_SURF2);
                    for (; c < fine_vtx_n; c += FINE_TRI_VTX) {
                        fine_rows(c, 1);
                    }
                    fine_emit(0xE7000000u, 0);
                    fine_emit(0xE200001Cu, G_RM_PASS | G_RM_AA_OPA_SURF2);
                }
                if ((w0 >> 24) == 0xDF) {
                    break;
                }
                if (w0 == FINE_PLAIN_W0 && w1 == FINE_PLAIN_W1) {
                    w0 = FINE_NOISE_W0;
                    w1 = FINE_NOISE_W1;
                }
                fine_emit(w0, w1);
            }
        }
        fine_emit(0xE7000000u, 0);
        fine_emit(0xE3000A01u, 0);                              // one cycle again, as drawSkyBox1Cycle set it
        fine_emit(0xE200001Cu, G_RM_AA_OPA_SURF | G_RM_AA_OPA_SURF2);
        fine_emit(0xEC000000u, 0);
        fine_emit(0xDF000000u, 0);
        if (fine_full) {
            return 0;
        }
        fine_rest = e[3];
        for (k = 0; k < FRAME_CTX; k++) {
            fine_entry[k][0].words.w0 = 0xDE000000u;
            fine_entry[k][0].words.w1 = (u32) &fine_list[k][0];
            fine_entry[k][1].words.w0 = 0xDE010000u;
            fine_entry[k][1].words.w1 = e[3];       // the game's own list goes on (each frame: its context's copy, sky_tail_for)
        }
        fine_drum_shares(__builtin_sqrtf(ring_at[1][0][0] * ring_at[1][0][0] + ring_at[1][0][2] * ring_at[1][0][2]),
                         __builtin_sqrtf(ring_at[2][0][0] * ring_at[2][0][0] + ring_at[2][0][2] * ring_at[2][0][2]), t_cap);
    }
    return 1;
}

// One stretch of corners painted: each ring's color at the corner's way around, by
// the corner's shares, and the day's color as far as the hour has left it.
static void fine_paint(s32 set, s32 from, s32 count, f32 a[3][3], f32 b[3][3], f32 life, const SkyHaze* haze, f32 plain[3][3], f32 plain_b[3][3], const SkyHaze* rows) {
    f32 dx = relight_dir[0], dz = relight_dir[2];
    f32 top_now = 0.0f;
    f32 by_line = rows != NULL ? sky_night_share() : 0.0f;   // Sunset's and Dawn's sky is the rings' products; Night's, the curve of the tints times the line
    s32 i, k;

    if (haze != NULL) {
        top_now = (f32) haze->top + ((f32) sky_floor[31] - (f32) haze->top) * (f32) (255 - (s32) cloud_top_fade) / 255.0f;
    }

    for (i = from; i < from + count; i++) {
        const FineCorner* corner = &fine_corner[i];
        u8* color = fine_vtx[set][0][i].v.cn;
        u8* extra = fine_vtx[set][1][i].v.cn;
        f32 facing = corner->hx * dx + corner->hz * dz;
        f32 toward = 0.5f + 0.5f * facing;
        f32 warm = life * ((facing > 0.0f ? facing * corner->warm : 0.0f) + corner->even);

        for (k = 0; k < 3; k++) {
            f32 tint = corner->w[0] * (a[0][k] + b[0][k] * toward) + corner->w[1] * (a[1][k] + b[1][k] * toward)
                     + corner->w[2] * (a[2][k] + b[2][k] * toward) + warm * (f32) sky_sun_side[k];
            f32 day = (f32) corner->day[k] / 256.0f;
            f32 v;
            s32 whole;

            if (haze != NULL) {
                f32 h = ((f32) haze->low + (top_now - (f32) haze->low) * corner->a) / 255.0f;
                f32 fit = (f32) pic_sky[k] + (f32) ((s32) pic_cloud[k] - (s32) pic_sky[k]) * h;

                tint *= sky_gain[k] * fit / (f32) sky_base(haze, k);
            }
            if (by_line > 0.0f) {
                f32 own = corner->w[0] * (plain[0][k] + plain_b[0][k] * toward) + corner->w[1] * (plain[1][k] + plain_b[1][k] * toward)
                        + corner->w[2] * (plain[2][k] + plain_b[2][k] * toward);

                tint += (own * sky_gain[k] * sky_night_line(rows, k, corner->w[2]) / (f32) sky_base(rows, k) - tint) * by_line;
            }
            day += (255.0f - day) * overcast_share;
            v = day + (tint - day) * nightness;
            v = v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v;
            whole = (s32) (v + 0.5f);
            color[k] = (u8) whole;
            // The second pass, at an eighth, makes up what the rounding lost.
            v += (v - (f32) whole) * ((255.0f - (f32) FINE_EIGHTH) / (f32) FINE_EIGHTH) + 0.5f;
            extra[k] = (u8) (v < 0.0f ? 0 : v > 255.0f ? 255 : (s32) v);
        }
    }
}

// The rings' colors at this moment, by sky_corners' law: at each ring, a part that
// is the same all round and a part that goes by how far the corner faces the sun.
static void fine_rings(f32 a[3][3], f32 b[3][3], f32 plain[3][3], f32 plain_b[3][3]) {
    const SkyHaze* haze = sky_haze_of(night);
    f32 top_now = 0.0f;
    s32 r, k;

    if (haze != NULL) {
        top_now = (f32) haze->top + ((f32) sky_floor[31] - (f32) haze->top) * (f32) (255 - (s32) cloud_top_fade) / 255.0f;
    }
    for (r = 0; r < 3; r++) {
        f32 up = fine_ring_up[r];

        for (k = 0; k < 3; k++) {
            f32 lo = sky_lo_now[k], away = tint_away[k], hi = tint_hi[k];

            a[r][k] = away * (1.0f - up) + hi * up;
            b[r][k] = (lo - away) * (1.0f - up);
            plain[r][k] = a[r][k];
            plain_b[r][k] = b[r][k];
            if (haze != NULL) {
                f32 at = (fine_ring_y[r] - (f32) haze->low_y) / (f32) (haze->top_y - haze->low_y);
                f32 h, fit, gain;

                at = at < 0.0f ? 0.0f : at > 1.0f ? 1.0f : at;
                h = ((f32) haze->low + (top_now - (f32) haze->low) * at) / 255.0f;
                fit = (f32) pic_sky[k] + (f32) ((s32) pic_cloud[k] - (s32) pic_sky[k]) * h;
                gain = sky_gain[k] * fit / (f32) sky_base(haze, k);
                a[r][k] *= gain;
                b[r][k] *= gain;
            }
        }
    }
}

static u32 fine_bits(f32 f) {
    union {
        f32 f;
        u32 u;
    } v;

    v.f = f;
    return v.u;
}

// What the drum's colors are made from, as one number: they are painted again when it changes.
static u32 fine_inputs(void) {
    u32 sum = (u32) cloud_top_fade + ((u32) sky_floor[31] << 8);
    s32 k;

    sum = sum * 31u + fine_bits(nightness);
    sum = sum * 31u + fine_bits(overcast_share);
    sum = sum * 31u + fine_bits(clear_share);
    for (k = 0; k < 3; k++) {
        sum = sum * 31u + (((u32) sky_lo_now[k] << 16) | ((u32) tint_away[k] << 8) | tint_hi[k]);
        sum = sum * 31u + (((u32) pic_sky[k] << 8) | pic_cloud[k]);
        sum = sum * 31u + fine_bits(relight_dir[k]);
        sum = sum * 31u + fine_bits(sky_gain[k]);
    }
    return sum | 1u;
}

// The sky's draw has just gone into the frame's list: its call to the port's drum
// goes to the fine one instead, unless the hour is the cartridge's Day or the game
// is measuring a photo. A sky the port left as the cartridge has it stays so.
static void sky_fine_draw(void) {
    f32 a[3][3], b[3][3], plain[3][3], plain_b[3][3];
    volatile u32* cmd;
    u32 entry, sum;
    s32 back;

    if (night == NULL || scoring || nightness <= 0.0f || fine_failed || sky_now == NULL || sky_now->data.dobj == NULL) {
        return;
    }
    entry = (u32) sky_now->data.dobj->payload.dlist;
    if (!fine_ready) {
        if (!fine_drum_build(entry)) {
            fine_failed = 1;
            recomp_printf("[island skies] the fine sky: left to the port's drum\n");
            return;
        }
        fine_ready = 1;
        recomp_printf("[island skies] the fine sky: %d corners, %d commands\n", fine_vtx_n, fine_cmd_n);
    }
    cmd = (volatile u32*) MAIN_GFX_POS[0];
    for (back = 1; back <= 32; back++) {
        if (cmd[-2 * back] == 0xDE000000u && cmd[-2 * back + 1] == entry) {
            break;
        }
    }
    if (back > 32) {
        return;
    }
    fine_turn = (u8) frame_ctx();      // the frame context's set
    sum = fine_inputs();
    if (fine_sum[fine_turn] != sum) {
        fine_sum[fine_turn] = sum;
        fine_rings(a, b, plain, plain_b);
        fine_paint(fine_turn, 0, fine_vtx_n, a, b,
                   (night->setup == NIGHT_VALLEY_SETUP) ? ((f32) cloud_top_fade / 255.0f) * clear_share : 0.0f, NULL, plain, plain_b, sky_haze_of(night));
    }
    fine_entry[fine_turn][1].words.w1 = sky_tail_for(fine_turn);       // the rest of the sky, as this frame has it
    cmd[-2 * back + 1] = (u32) &fine_entry[fine_turn][0];
}

// A roofed course's sky is faces in its land's lists, the same material and the
// same six-sided boxes, but the eye moves about under them. The walker finds each
// run of them; a call to its finer faces goes in at the run's start and a branch
// past the rest, while the hour is not Day. They are colored by their direction
// from the box's middle, from the foot of its walls: a color by the direction from
// the eye itself would crowd its whole slope into the few rows of the nearest wall.
static void fine_open_at(u32 list) {
    FinePart* p;
    s32 i;

    if (fine_parts >= FINE_PARTS) {
        return;
    }
    for (i = 0; i < fine_parts; i++) {
        if (fine_part[i].from == list) {
            return;
        }
    }
    p = &fine_part[fine_parts];
    p->from = list;
    p->to = 0;
    p->vtx0 = (u16) fine_vtx_n;
    p->vtx_n = 0;
    p->list0 = (u16) fine_cmd_n;
    p->tris = 0;
    p->on = 0;
    p->bad = 0;
    p->up_lo = 1.0f;
    p->up_hi = 0.0f;
    p->turn = 0;
    p->hazed = (tex_addr == night->pics[0]);        // the pale box's is the second picture, which keeps its haze
    fine_emit(0xE7000000u, 0);
    fine_emit(0xE3000A01u, 0x00100000u);
    fine_emit(0xE200001Cu, (G_RM_PASS | G_RM_AA_ZB_OPA_SURF2) & ~(u32) Z_UPD);     // without Z_UPD, as roof_sky_depth has the material
    fine_emit(0xEC000000u, FINE_KEYS);
    fine_emit(FINE_NOISE_W0, FINE_NOISE_W1);
    fine_open = 1;
}

static void fine_face(u32 a, u32 b, u32 c) {
    FinePart* p = &fine_part[fine_parts];
    u32 at[3];
    s16 corner[3][5];
    s32 i;

    if (a == 0 || b == 0 || c == 0) {
        p->bad = 1;
        return;
    }
    at[0] = a;
    at[1] = b;
    at[2] = c;
    for (i = 0; i < 3; i++) {
        volatile s16* v = (volatile s16*) at[i];
        f32 up = sky_up(at[i]);

        corner[i][0] = v[0];
        corner[i][1] = v[1];
        corner[i][2] = v[2];
        corner[i][3] = v[4];
        corner[i][4] = v[5];
        if (up < p->up_lo) {
            p->up_lo = up;
        }
        if (up > p->up_hi) {
            p->up_hi = up;
        }
    }
    if (p->tris == 0) {
        volatile u8* paint = (volatile u8*) (a + 12);

        p->paint[0] = paint[0];
        p->paint[1] = paint[1];
        p->paint[2] = paint[2];
    }
    if (fine_cut_face(corner[0], corner[1], corner[2]) < 0 || p->tris >= 40) {
        p->bad = 1;
        return;
    }
    p->tris++;
}

static void fine_close(u32 list) {
    FinePart* p = &fine_part[fine_parts];
    f32 y_lo = 32767.0f, y_hi = -32768.0f;
    f32 x_lo = 32767.0f, x_hi = -32768.0f, z_lo = 32767.0f, z_hi = -32768.0f;
    f32 mx, mz, far2 = 0.0f;
    s32 i, k;

    fine_open = 0;
    fine_new = 1;
    fine_emit(0xE7000000u, 0);
    fine_emit(0xE200001Cu, G_RM_PASS | G_RM_ZB_XLU_SURF2);
    for (i = p->vtx0; i < fine_vtx_n; i += FINE_TRI_VTX) {
        fine_rows(i, 1);
    }
    fine_emit(0xE7000000u, 0);
    fine_emit(0xE3000A01u, 0);
    fine_emit(0xE200001Cu, (G_RM_AA_ZB_OPA_SURF | G_RM_AA_ZB_OPA_SURF2) & ~(u32) Z_UPD);
    fine_emit(0xEC000000u, 0);
    fine_emit(FINE_PLAIN_W0, FINE_PLAIN_W1);
    fine_emit(0xDF000000u, 0);
    p->to = list;
    p->vtx_n = (u16) (fine_vtx_n - p->vtx0);
    fine_parts++;
    if (fine_full || p->tris == 0 || (list - p->from) / 8 < FINE_PATCH) {
        p->bad = 1;
        return;
    }
    for (i = p->vtx0; i < p->vtx0 + p->vtx_n; i++) {
        f32 y = fine_vtx[0][0][i].v.ob[1];

        y_lo = y < y_lo ? y : y_lo;
        y_hi = y > y_hi ? y : y_hi;
    }
    for (i = p->vtx0; i < p->vtx0 + p->vtx_n; i++) {
        f32 x = fine_vtx[0][0][i].v.ob[0], y = fine_vtx[0][0][i].v.ob[1], z = fine_vtx[0][0][i].v.ob[2];

        if (y >= y_hi - 2.0f) {
            x_lo = x < x_lo ? x : x_lo;
            x_hi = x > x_hi ? x : x_hi;
            z_lo = z < z_lo ? z : z_lo;
            z_hi = z > z_hi ? z : z_hi;
        }
    }
    mx = (x_lo + x_hi) * 0.5f;
    mz = (z_lo + z_hi) * 0.5f;
    for (i = p->vtx0; i < p->vtx0 + p->vtx_n; i++) {
        f32 dx = fine_vtx[0][0][i].v.ob[0] - mx, y = fine_vtx[0][0][i].v.ob[1], dz = fine_vtx[0][0][i].v.ob[2] - mz;

        if (y >= y_hi - 2.0f && dx * dx + dz * dz > far2) {
            far2 = dx * dx + dz * dz;
        }
    }
    {
        f32 far = __builtin_sqrtf(far2 > 1.0f ? far2 : 1.0f);
        f32 y_mid = y_lo;
        f32 t_cap = (y_hi - y_lo) / far;
        f32 t_mid;

        for (i = p->vtx0; i < p->vtx0 + p->vtx_n; i += FINE_TRI_VTX) {
            s32 c;

            // A triangle's three corners stand first, at the end of its first row, and last.
            for (c = 0; c < 3; c++) {
                f32 y = fine_vtx[0][0][i + (c == 0 ? 0 : c == 1 ? FINE_CUT : FINE_TRI_VTX - 1)].v.ob[1];

                if (y < y_hi - 2.0f && y > y_mid) {
                    y_mid = y;
                }
            }
        }
        t_mid = (y_mid - y_lo) / (FINE_ZETA * far);
        for (i = p->vtx0; i < p->vtx0 + p->vtx_n; i++) {
            FineCorner* corner = &fine_corner[i];
            f32 dx = fine_vtx[0][0][i].v.ob[0] - mx, y = fine_vtx[0][0][i].v.ob[1], dz = fine_vtx[0][0][i].v.ob[2] - mz;
            f32 rho = __builtin_sqrtf(dx * dx + dz * dz);
            f32 tan = (y >= y_hi - 2.0f || rho < 1.0f) ? t_cap : (y - y_lo) / rho;
            f32 u = tan / t_cap;
            f32 a = (tan - t_mid) / (t_cap - t_mid);
            f32 up;

            u = u > 1.0f ? 1.0f : u;
            a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
            up = p->up_lo + (p->up_hi - p->up_lo) * u * (0.5f + u * (2.0f - 1.5f * u));     // flat at the top, nearly straight below
            corner->a = a * a * (3.0f - 2.0f * a);
            corner->w[0] = 1.0f - up;
            corner->w[1] = 0.0f;
            corner->w[2] = up;
            corner->hx = 0.0f;
            corner->hz = 0.0f;
            corner->warm = 0.0f;
            corner->even = 0.0f;
            for (k = 0; k < 3; k++) {
                corner->day[k] = (u16) ((u32) p->paint[k] << 8);
            }
        }
    }
}

// A part's spare set of corners painted and its call pointed at it.
static void fine_part_show(FinePart* p) {
    f32 a[3][3], b[3][3];
    volatile u32* at = (volatile u32*) p->from;
    s32 k;

    if (p->bad) {
        return;
    }
    for (k = 0; k < 3; k++) {
        a[0][k] = tint_lo[k];
        a[1][k] = 0.0f;
        a[2][k] = tint_hi[k];
        b[0][k] = b[1][k] = b[2][k] = 0.0f;
    }
    p->turn ^= 1;
    fine_paint(p->turn, p->vtx0, p->vtx_n, a, b, 0.0f, p->hazed ? sky_haze_of(night) : NULL, NULL, NULL, NULL);
    if (!p->on) {
        for (k = 0; k < FINE_PATCH * 2; k++) {
            p->saved[k] = at[k];
        }
        p->on = 1;
    }
    at[0] = FINE_RDRAM;
    at[1] = 1;
    at[2] = 0xDE000000u;
    at[3] = (u32) &fine_list[p->turn][p->list0];
    at[4] = FINE_RDRAM;
    at[5] = 0;
    at[6] = 0xDE010000u;
    at[7] = p->to;
}

// The hour has changed, or the walker has found more faces: every part shown
// anew; at Day the lists as the cartridge has them.
static void fine_roof_refresh(void) {
    s32 i, k;

    if (night == NULL || !ROOFED(night) || fine_parts == 0) {
        return;
    }
    if (nightness <= 0.0f) {
        for (i = 0; i < fine_parts; i++) {
            FinePart* p = &fine_part[i];

            if (p->on) {
                volatile u32* at = (volatile u32*) p->from;

                for (k = 0; k < FINE_PATCH * 2; k++) {
                    at[k] = p->saved[k];
                }
                p->on = 0;
            }
        }
        return;
    }
    for (i = 0; i < fine_parts; i++) {
        fine_part_show(&fine_part[i]);
    }
}

// ---- The fine sky's end ----

// A matrix that moves a thing to a place: ones on the diagonal, with the
// place in its last row. The record holds sixteen whole parts, then their
// sixteen fractions.
static void place(Mtx* mtx, f32 x, f32 y, f32 z) {
    volatile u16* half = (volatile u16*) mtx;
    s32 at[3];
    s32 i;

    at[0] = (s32) (x * 65536.0f);
    at[1] = (s32) (y * 65536.0f);
    at[2] = (s32) (z * 65536.0f);
    for (i = 0; i < 32; i++) {
        half[i] = 0;
    }
    half[0] = half[5] = half[10] = half[15] = 1;
    for (i = 0; i < 3; i++) {
        half[12 + i] = (u16) (at[i] >> 16);
        half[28 + i] = (u16) (at[i] & 0xFFFF);
    }
}

// The rain: sheets of drops around the camera, each sheet drawn twice, one
// copy above the other, and moved down by a matrix as the clock runs. A copy
// that has fallen a sheet's height is gone and a new one has come in at the
// top, so every copy only ever falls; each is named to the renderer
// (rt64_extended_gbi.h, a matrix group: opcode 0x64, command 0x0C, closed by
// 0x0D) so that it can place the copy between two ticks when the frame rate
// is raised. The camera's place is the sky's own: the sky box follows the
// camera on the ride and is set to a photo's camera for a photo. The lists
// go among the see-through things, which are drawn after the solid world.
static void rain_at(Gfx** game, f32 ex, f32 ey, f32 ez) {
    u32 ticks = ticks_of(night);
    Mtx* set;
    u32 sheet;
    u32 copy;
    Gfx** list;

    if (*game == NULL) {
        return;
    }
    list = gfx_stage_open(game);
    set = rain_mtx[frame_slot(&rain_slot_n, FRAME_SLOTS)];
    command(list, (0x64u << 24) | 0x00002Cu, 1);
    gSPDisplayList((*list)++, night_rain_begin);
    gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (u8) (132.0f * rain_share));     // the rain's own alpha, as far as it has come
    // The wind: the sheets drift sideways on two slow waves, a gust every six
    // seconds or so with a quicker shiver on it, each sheet a little apart.
    for (sheet = 0; sheet < NIGHT_RAIN_SHEETS; sheet++) {
        u32 fallen = ticks * night_rain_speed[sheet];
        u32 turn = fallen / NIGHT_RAIN_HEIGHT;
        f32 gust = (f32) night_sine[((ticks * 23) >> 5) + 11 * sheet & 0xFF] * (90.0f / 127.0f)
                 + (f32) night_sine[(((ticks * 61) >> 5) + 70 + 29 * sheet) & 0xFF] * (36.0f / 127.0f);

        for (copy = 0; copy < 2; copy++) {
            u32 which = turn + copy;
            s32 below = (s32) fallen - (s32) (which * NIGHT_RAIN_HEIGHT);     // from -HEIGHT up to HEIGHT
            Mtx* mtx = &set[sheet * 2 + copy];

            place(mtx, ex + NIGHT_RAIN_SLANT_X * (f32) below + gust * 0.9f,
                  ey - (f32) (NIGHT_RAIN_HEIGHT / 2) - (f32) below,
                  ez + NIGHT_RAIN_SLANT_Z * (f32) below + gust * 0.4f);
            // The group: this id, pushed; place, turn, perspective and vertices followed between ticks.
            command(list, (0x64u << 24) | 0x00000Cu, 0x52410000u | (sheet << 8) | (which & 0xFF));
            command(list, 0x00002829u, 0);
            gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
            gSPDisplayList((*list)++, rain_sheets_now[sheet]);
            gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
            command(list, (0x64u << 24) | 0x00000Du, 1);
        }
    }
    gSPDisplayList((*list)++, night_rain_end);
    command(list, (0x64u << 24) | 0x00002Cu, 0);
    gfx_stage_close(game, 0);
}

// The snow: made and moved as the rain is, but slowly (its paces are in
// eighths of a unit a tick), and each sheet sways from side to side on a
// slow wave of its own as it falls.
static void snow_at(Gfx** game, f32 ex, f32 ey, f32 ez) {
    u32 ticks = ticks_of(night);
    Mtx* set;
    u32 sheet;
    u32 copy;
    u8 ash = (ash_share > 0.0f && snow_share <= 0.0f);      // the Volcano's ash, unless snow falls with it
    Gfx** list;

    if (*game == NULL) {
        return;
    }
    list = gfx_stage_open(game);
    set = rain_mtx[frame_slot(&rain_slot_n, FRAME_SLOTS)];
    command(list, (0x64u << 24) | 0x00002Cu, 1);
    gSPDisplayList((*list)++, night_rain_begin);
    gSPDisplayList((*list)++, night_snow_alpha);
    gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (u8) (215.0f * snow_share));     // the snow's own alpha, as far as it has come
    if (ash) {
        // Ash: the flakes take the primitive's gray (the snow's material colors
        // them from their corners alone, which are white), a little see-through.
        gDPSetCombineLERP((*list)++, SHADE, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0,
                          SHADE, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0);
        gDPSetPrimColor((*list)++, 0, 0, 112, 104, 100, (u8) (220.0f * ash_share));
    }
    for (sheet = 0; sheet < NIGHT_SNOW_SHEETS; sheet++) {
        u32 fallen = (ticks * night_snow_speed[sheet]) >> (ash ? 4 : 3);
        u32 turn = fallen / NIGHT_SNOW_HEIGHT;
        f32 sway = (f32) (night_sine[(((ticks * (90 + 17 * sheet)) >> 8) + 37 * sheet) & 0xFF] * 40) / 127.0f;
        f32 sway_across = (f32) (night_sine[(((ticks * (70 + 13 * sheet)) >> 8) + 91 * sheet) & 0xFF] * 28) / 127.0f;

        for (copy = 0; copy < 2; copy++) {
            u32 which = turn + copy;
            s32 below = (s32) fallen - (s32) (which * NIGHT_SNOW_HEIGHT);
            Mtx* mtx = &set[sheet * 2 + copy];

            place(mtx, ex + sway, ey - (f32) (NIGHT_SNOW_HEIGHT / 2) - (f32) below, ez + sway_across);
            command(list, (0x64u << 24) | 0x00000Cu, 0x534E0000u | (sheet << 8) | (which & 0xFF));
            command(list, 0x00002829u, 0);
            gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
            gSPDisplayList((*list)++, snow_sheets_now[sheet]);
            gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
            command(list, (0x64u << 24) | 0x00000Du, 1);
        }
    }
    gSPDisplayList((*list)++, night_rain_end);
    command(list, (0x64u << 24) | 0x00002Cu, 0);
    gfx_stage_close(game, 0);
}

// The moon's path on the sea: a strip of soft light from near the camera
// toward the moon, a hair above the water (whose height is the mean of the
// vertices that flow, gathered as the walker meets them), moving with the
// camera as a real one does, and adding to the water rather than covering
// it. Where the land stands before the water it hides the path.
// The game's matrix at an address, as floats; its product with another; a
// float matrix packed as the game's.
static void read_mtx(f32 m[4][4], u32 addr) {
    const volatile s16* hi = (const volatile s16*) addr;
    const volatile u16* lo = (const volatile u16*) (addr + 32);
    s32 r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            m[r][c] = (f32) hi[r * 4 + c] + (f32) lo[r * 4 + c] / 65536.0f;
        }
    }
}

static void mul_mtx(f32 out[4][4], f32 a[4][4], f32 b[4][4]) {
    s32 r, c, k;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            f32 sum = 0.0f;

            for (k = 0; k < 4; k++) {
                sum += a[r][k] * b[k][c];
            }
            out[r][c] = sum;
        }
    }
}

static void copy_mtx(f32 out[4][4], f32 in[4][4]) {
    s32 r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            out[r][c] = in[r][c];
        }
    }
}

static void pack_mtx(Mtx* mtx, f32 m[4][4]) {
    volatile u16* half = (volatile u16*) mtx;
    s32 r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            s32 v = (s32) (m[r][c] * 65536.0f);

            half[r * 4 + c] = (u16) (v >> 16);
            half[16 + r * 4 + c] = (u16) (v & 0xFFFF);
        }
    }
}

// One corner of the water, in the world through its node's matrix: its place
// in the dot from the tilt of the half-way direction between the eye and the
// moon, along the moon's bearing (gently) and across it (sharply), and its
// color, the moon's, fading into the far fog.
#define GLINT_ALONG 480.0f
#define GLINT_ACROSS 1200.0f

static void glint_corner(Vtx* v, f32 m[4][4], const f32* eye, const f32* la, const f32* breath) {
    f32 x = v->v.ob[0], y = v->v.ob[1], z = v->v.ob[2];
    f32 px = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
    f32 py = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
    f32 pz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
    f32 vx = eye[0] - px, vy = eye[1] - py, vz = eye[2] - pz;
    f32 d = __builtin_sqrtf(vx * vx + vy * vy + vz * vz);
    f32 hx, hy, hz, hl, along, across, k;
    s32 st;

    if (d < 1.0f) {
        v->v.tc[0] = v->v.tc[1] = 8000;
        v->v.cn[0] = v->v.cn[1] = v->v.cn[2] = 0;
        return;
    }
    hx = vx / d + relight_dir[0];
    hy = vy / d + relight_dir[1];
    hz = vz / d + relight_dir[2];
    hl = __builtin_sqrtf(hx * hx + hy * hy + hz * hz);
    if (hl < 0.01f) {
        hl = 0.01f;
    }
    hx /= hl;
    hz /= hl;
    along = hx * la[0] + hz * la[1] + breath[0];
    across = -hx * la[1] + hz * la[0] + breath[1];
    st = 256 + (s32) (along * GLINT_ALONG);
    v->v.tc[0] = (s16) (st < -8000 ? -8000 : st > 8000 ? 8000 : st);
    st = 256 + (s32) (across * GLINT_ACROSS);
    v->v.tc[1] = (s16) (st < -8000 ? -8000 : st > 8000 ? 8000 : st);
    k = (d < 3500.0f) ? 1.0f : (7000.0f - d) / 3500.0f;
    if (k < 0.0f) {
        k = 0.0f;
    }
    v->v.cn[0] = (u8) (150.0f * k);
    v->v.cn[1] = (u8) (172.0f * k);
    v->v.cn[2] = (u8) (235.0f * k);
    v->v.cn[3] = 255;
}

// A segment of the water under the matrix the game drew it with: its corners
// set for this frame, then its loads and triangles, in a matrix group of its
// own so the port moves it with the water between the game's frames.
static void glint_segment(s32 k, f32 m[4][4], const f32* eye, const f32* la, const f32* breath, Gfx** list,
                          s32* opened) {
    const GlintSeg* seg = &glint_seg[k];
    Vtx* set = glint_vtx[frame_ctx()];          // the frame's own set: its context's
    Mtx* mtx = &glint_mtx[frame_ctx()][k];
    u32 j, o, pending = 0xFFFFFFFFu;

    if (!seg->flow) {
        return;
    }
    if (!*opened) {
        *opened = 1;
        command(list, (0x64u << 24) | 0x00002Cu, 1);
        gSPDisplayList((*list)++, night_glow_begin);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (u8) (140.0f * moon_light * clear_share));
    }
    for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
        o = glint_op[j];
        if ((o & GLINT_LOAD) && (o & GLINT_USED)) {
            u32 start = (o >> 12) & 0x3FFF, count = (o >> 6) & 0x3F, v;

            for (v = 0; v < count; v++) {
                glint_corner(&set[start + v], m, eye, la, breath);
            }
        }
    }
    {
        // Two units above the water: a surface that writes depth refuses a draw
        // on its own plane, and the lift keeps the glint under the land still.
        f32 lifted[4][4];

        copy_mtx(lifted, m);
        lifted[3][1] += 2.0f;
        pack_mtx(mtx, lifted);
    }
    command(list, (0x64u << 24) | 0x00000Cu, 0x474C0000u | (u32) k);
    command(list, 0x00002829u, 0);
    gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
        o = glint_op[j];
        if (o & GLINT_LOAD) {
            if (pending != 0xFFFFFFFFu) {
                gSP1Triangle((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0);
                pending = 0xFFFFFFFFu;
            }
            if (o & GLINT_USED) {
                gSPVertex((*list)++, &set[(o >> 12) & 0x3FFF], (o >> 6) & 0x3F, o & 63);
            }
        } else if (!(o & GLINT_FLOW)) {
            continue;       // the land's
        } else if (pending == 0xFFFFFFFFu) {
            pending = o;
        } else {
            gSP2Triangles((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0,
                          (o >> 16) & 0xFF, (o >> 8) & 0xFF, o & 0xFF, 0);
            pending = 0xFFFFFFFFu;
        }
    }
    if (pending != 0xFFFFFFFFu) {
        gSP1Triangle((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0);
    }
    gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    command(list, (0x64u << 24) | 0x00000Du, 1);
}

// Embers over the lava. The lava is the Volcano's "water" to the glint's
// record (it draws with the flowing material), so when a part of it is first
// drawn, a few dozen points on its triangles become the embers' homes. Every
// frame each ember stands above its home by its age -- rising, swaying,
// cooling from yellow to a dull red, fading -- as a small soft dot with the
// adding blend, facing the view, under the matrix the game drew that part
// with. Their pace is the world's clock, so a photo keeps its instant's.
#define EMBERS 48
#define EMBER_HALF 7.0f
typedef struct Ember {
    f32 home[3];            // on the lava, in its part's own space
    u16 period, phase;      // the ticks of one rise, and where in it this one began
    u8 seg, sway;           // the part it belongs to, and its sway's phase
} Ember;
static Ember ember[EMBERS];
static Vtx ember_vtx[FRAME_CTX][EMBERS * 4];   // each frame context's corners
static u32 ember_drawn;                 // the quads written so far this frame

static void ember_seed(s32 k) {
    const GlintSeg* seg = &glint_seg[k];
    u16 slot_copy[64];
    u32 j, o, tris = 0, want, made = 0;

    ember_seeded[k] = 1;
    for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
        if ((glint_op[j] & (GLINT_LOAD | GLINT_FLOW)) == GLINT_FLOW) {
            tris++;
        }
    }
    if (tris == 0) {
        return;
    }
    want = EMBERS / (glint_seg_n > 0 ? glint_seg_n : 1);
    if (want < 8) {
        want = 8;
    }
    while (ember_n < EMBERS && made < want) {
        Ember* e = &ember[ember_n];
        u32 mix = ((u32) ember_n * 2654435761u) ^ 0x5A17E000u;
        u32 pick, t = 0;
        f32 u, v;

        mix ^= mix >> 13;
        mix *= 0x85EBCA77u;
        mix ^= mix >> 16;
        pick = (mix >> 8) % tris;
        for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
            o = glint_op[j];
            if (o & GLINT_LOAD) {
                u32 start = (o >> 12) & 0x3FFF, count = (o >> 6) & 0x3F, first = o & 63, i;

                for (i = 0; i < count && first + i < 64; i++) {
                    slot_copy[first + i] = (u16) (start + i);
                }
            } else if (!(o & GLINT_FLOW)) {
                continue;
            } else if (t++ == pick) {
                const Vtx* A = &glint_vtx[0][slot_copy[(o >> 16) & 63]];
                const Vtx* B = &glint_vtx[0][slot_copy[(o >> 8) & 63]];
                const Vtx* D = &glint_vtx[0][slot_copy[o & 63]];
                s32 c;

                u = (f32) (mix & 0xFF) / 255.0f;
                v = (f32) ((mix >> 16) & 0xFF) / 255.0f;
                if (u + v > 1.0f) {
                    u = 1.0f - u;
                    v = 1.0f - v;
                }
                for (c = 0; c < 3; c++) {
                    e->home[c] = (f32) A->v.ob[c] + u * (f32) (B->v.ob[c] - A->v.ob[c]) + v * (f32) (D->v.ob[c] - A->v.ob[c]);
                }
                break;
            }
        }
        e->period = (u16) (200 + ((mix >> 24) % 120));
        e->phase = (u16) ((mix >> 4) % e->period);
        e->seg = (u8) k;
        e->sway = (u8) (mix & 0xFF);
        ember_n++;
        made++;
    }
}

static void ember_segment(s32 k, f32 m[4][4], Gfx** list, s32* opened) {
    u32 ticks = ticks_of(night);
    Vtx* set = ember_vtx[frame_ctx()];
    f32 rx = view_fwd[1], rz = -view_fwd[0];      // level, across the view
    u32 first_quad = ember_drawn;
    s32 e;

    if (!ember_seeded[k]) {
        ember_seed(k);
    }
    if (rx == 0.0f && rz == 0.0f) {
        rx = 1.0f;
    }
    for (e = 0; e < ember_n && ember_drawn < EMBERS; e++) {
        const Ember* em = &ember[e];
        u32 age;
        f32 a, x, y, z, half, bright, r, g, b;
        Vtx* q;
        s32 c;

        if (em->seg != k) {
            continue;
        }
        age = (ticks + em->phase) % em->period;
        a = (f32) age / (f32) em->period;
        x = em->home[0] * m[0][0] + em->home[1] * m[1][0] + em->home[2] * m[2][0] + m[3][0];
        y = em->home[0] * m[0][1] + em->home[1] * m[1][1] + em->home[2] * m[2][1] + m[3][1];
        z = em->home[0] * m[0][2] + em->home[1] * m[1][2] + em->home[2] * m[2][2] + m[3][2];
        y += a * (110.0f + 0.5f * (f32) em->period);
        x += (f32) night_sine[((ticks * 2) + em->sway * 4) & 0xFF] * (14.0f / 127.0f) * a;
        z += (f32) night_sine[((ticks * 3) + em->sway * 7 + 85) & 0xFF] * (10.0f / 127.0f) * a;
        bright = (a < 0.15f) ? a / 0.15f : 1.0f - (a - 0.15f) / 0.85f;
        r = (255.0f + (170.0f - 255.0f) * a) * bright;
        g = (180.0f + (40.0f - 180.0f) * a) * bright;
        b = (80.0f + (10.0f - 80.0f) * a) * bright;
        half = EMBER_HALF * (0.7f + 0.5f * a);
        q = &set[ember_drawn * 4];
        for (c = 0; c < 4; c++) {
            f32 sx = (c == 0 || c == 3) ? -1.0f : 1.0f;
            f32 sy = (c < 2) ? -1.0f : 1.0f;

            q[c].v.ob[0] = (s16) (x + sx * rx * half);
            q[c].v.ob[1] = (s16) (y + sy * half);
            q[c].v.ob[2] = (s16) (z + sx * rz * half);
            q[c].v.flag = 0;
            q[c].v.tc[0] = (c == 0 || c == 3) ? 0 : 512;
            q[c].v.tc[1] = (c < 2) ? 512 : 0;
            q[c].v.cn[0] = (u8) r;
            q[c].v.cn[1] = (u8) g;
            q[c].v.cn[2] = (u8) b;
            q[c].v.cn[3] = 255;
        }
        ember_drawn++;
    }
    if (ember_drawn == first_quad) {
        return;
    }
    if (!*opened) {
        *opened = 1;
        command(list, (0x64u << 24) | 0x00002Cu, 1);
        gSPDisplayList((*list)++, night_glow_begin);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, 255);
    }
    // In the world's own space, under the view: eight quads a load.
    {
        u32 qd = first_quad;

        while (qd < ember_drawn) {
            u32 n = ember_drawn - qd;
            u32 i;

            if (n > 8) {
                n = 8;
            }
            gSPVertex((*list)++, &set[qd * 4], n * 4, 0);
            for (i = 0; i < n; i++) {
                gSP2Triangles((*list)++, i * 4, i * 4 + 1, i * 4 + 2, 0, i * 4, i * 4 + 2, i * 4 + 3, 0);
            }
            qd += n;
        }
    }
}

// The dawn's mist on the water and the shore: the record's own triangles
// drawn again, each corner's thickness from its height over the water near
// it and from its distance to the eye, under the picture of wisps laid on
// the world by the corners' own places, with a slow drift. The copies are
// set in the world's own space as each model with parts of the record is
// drawn (mist_segment), and drawn all at once after the whole scene
// (mist_draw: at the UI camera's start on the ride, at the photo camera's
// end for a photo), under the scene camera's own matrices, the way the
// Tunnel's weather is drawn: a draw in the middle of the scene left the
// mist's modes to the models after it, and the land went wrong (JackandBeans,
// Oct 19 2026, "extremely glitchy"). Each corner takes the game's fog as the
// land does, from the camera's projection and view and the fog in force,
// so the far mist fades into the fog and a block coming into view brings no
// bright veil with it; the water's corners stand two units over the water,
// so the bed under it does not refuse them and so they run through no wall
// the water runs under (pulled toward the eye instead, they came through
// the low grass by the water as the view moved and lay as a line along the
// walls' feet, Oct 20 2026); the shore's are the land's own corners, drawn
// under the land's own matrices at the land's own depth, which the depth
// test lets through as equal, so they stand nowhere but on the land; a
// face steeper than 65 degrees carries none;
// and the triangles are drawn without the edge smoothing, which drew a
// bright seam along every shared edge of a see-through surface. Four cuts
// of a sheet riding with the eye went before (Oct 18 and 19 2026): its flat
// plane cut the sloping banks in a straight edge, and its picture, written
// anew at each frame while the port glided the sheet between frames, moved
// against the water.
#define MIST_NEAR 60.0f         // units from the eye within which the mist is nothing,
#define MIST_FULL 520.0f        // and at which it is full
#define MIST_FAR0 700.0f        // where it begins to thin again (1400 was "too much from a distance", Oct 20 2026; 800 "still a little too much"),
#define MIST_FAR1 1500.0f       // and where it is gone (2600, then 1700, before)
#define MIST_FLOOR 3.0f         // units over the water near it up to which a corner is fully misted,
#define MIST_RISE 26.0f         // and over how many more it clears
#define MIST_REACH0 160.0f      // units across from the nearest water corner within which the shore is fully misted,
#define MIST_REACH1 420.0f      // and at which it is bare
#define MIST_LIFT 2.0f          // how far over the water the water's mist stands
#define MIST_CHAIN 8            // matrices a part is drawn under at most
#define MIST_TRIS 2048          // the triangles a frame's draw tells apart
#define MIST_ALPHA 90.0f        // the mist's alpha at full mist, where its corners and its picture are full (100 to cut 12)
#define MIST_DRIFT_S 14400u     // ticks of one tile's drift along s (four minutes),
#define MIST_DRIFT_T 18000u     // and along t (five)
#define MIST_POOL 1024          // the water corners the floors are taken from
#define MIST_UNKNOWN 0x7FFF
static Vtx mist_vtx[FRAME_CTX][GLINT_VTX];      // the copies, a set for each frame context, as the glint's
static s16 mist_floor[GLINT_VTX];       // the water's height near each copy, once found,
static u8 mist_near[GLINT_VTX];         // and how far across that water lies, in fours of units
static s16 mist_pool[MIST_POOL][3];     // the water's corners in the world
static u16 mist_pool_n;
static u16 mist_floor_n;                // the copies whose floors are reset and whose water is in the pool
static u32 mist_corners;                // corners with any mist in the last scan
static u8 mist_ready[GLINT_SEGS];       // the parts whose copies carry mist this frame, to draw
static f32 mist_clip[4][4];             // the camera's view and projection as one, for the fog
static u8 mist_clip_ok;                 // read this frame
static GObj* mist_cam;                  // the camera whose models the copies were set under
static Mtx mist_world_mtx;              // the world's own frame, the copies' space
static u8 mist_world_ok;
static u16 mist_parts_last;             // the last drawn frame's parts and corners, for the log
static u32 mist_corners_last;
static u8 mist_sky_said;
static u32 mist_part_chain[FRAME_CTX][GLINT_SEGS][MIST_CHAIN][2];   // the matrix commands each part was drawn under, for its shore copies
static u8 mist_part_chain_n[FRAME_CTX][GLINT_SEGS];
static u16 mist_seen_head[1024];        // the triangles drawn so far this frame, by their corners' places in the world
static u16 mist_seen_next[MIST_TRIS];
static u16 mist_seen_copy[MIST_TRIS][3];
static u16 mist_seen_n;

// The record grows as the course's models are first drawn: the copies that
// came since the last misty draw have their floors reset, their water corners
// join the pool, and if any did, every floor is found again.
static void mist_floors_keep(void) {
    u32 i, grew = 0;

    for (i = mist_floor_n; i < glint_vtx_n && i < GLINT_VTX; i++) {
        mist_floor[i] = MIST_UNKNOWN;
        if ((glint_face[i] & 0xA000u) == 0xA000u && mist_pool_n < MIST_POOL) {
            mist_pool[mist_pool_n][0] = glint_world_x[i];
            mist_pool[mist_pool_n][1] = glint_world_y[i];
            mist_pool[mist_pool_n][2] = glint_world_z[i];
            mist_pool_n++;
            grew = 1;
        }
    }
    if (grew) {
        // Only the copies far from any water known so far look again: the
        // rest keep their floors, so a block's water coming in moves no
        // mist already lying and costs no frame.
        for (i = 0; i < mist_floor_n; i++) {
            if (mist_near[i] > 60) {
                mist_floor[i] = MIST_UNKNOWN;
            }
        }
    }
    mist_floor_n = (u16) (glint_vtx_n < GLINT_VTX ? glint_vtx_n : GLINT_VTX);
}

// The water nearest a copy, across: its height, and how far it lies. Found
// once, at the copy's first misty draw; where the record has no water, the
// water's recorded top stands for it.
static void mist_floor_find(u32 i, f32 px, f32 pz) {
    f32 best = 1.0e18f, y = flow_y_max;
    u32 n;

    for (n = 0; n < mist_pool_n; n++) {
        f32 dx = (f32) mist_pool[n][0] - px, dz = (f32) mist_pool[n][2] - pz;
        f32 d = dx * dx + dz * dz;

        if (d < best) {
            best = d;
            y = (f32) mist_pool[n][1];
        }
    }
    mist_floor[i] = (s16) y;
    best = (mist_pool_n > 0) ? __builtin_sqrtf(best) / 4.0f : 0.0f;
    mist_near[i] = (u8) (best > 255.0f ? 255.0f : best);
}

// The camera's projection and view, read from the game's own sub-list for
// them (ren_D_8004B248 + 1), as one matrix from the world to the clip
// space, for each corner's fog. The game loads its perspective on the
// projection stack and multiplies its look-at into it (sys/render.c,
// renPrepareCameraMatrix: G_MTX_MUL | G_MTX_PROJECTION); a camera of the
// other kinds loads its view on the model stack instead. Either is read.
// The sub-list holds the port's own group packet before them.
static void mist_clip_read(void) {
    u32 at = *(volatile u32*) 0x8004B248u;
    f32 proj[4][4], view[4][4], m[4][4], out[4][4];
    s32 have_p = 0, have_v = 0, n;

    mist_clip_ok = 0;
    if (at == 0) {
        return;
    }
    for (n = 1; n < 16; n++) {
        volatile u32* cmd = (volatile u32*) (at + 8 * n);
        u32 w0 = cmd[0], w1 = cmd[1];

        if ((w0 >> 24) == 0xDF) {
            break;
        }
        if ((w0 >> 24) != 0xDA) {
            continue;
        }
        if (w1 < 0x00800000u) {
            w1 |= 0x80000000u;      // the same memory by its physical address
        } else if (w1 < 0x80000000u) {
            continue;
        }
        read_mtx(m, w1);
        if (w0 & 4) {               // the projection: loaded, or multiplied into the one loaded
            if (w0 & 2) {
                copy_mtx(proj, m);
                have_p = 1;
            } else if (have_p) {
                mul_mtx(out, m, proj);
                copy_mtx(proj, out);
            }
        } else if (w0 & 2) {        // a view loaded on the model stack
            copy_mtx(view, m);
            have_v = 1;
        }
    }
    if (have_p) {
        if (have_v) {
            mul_mtx(mist_clip, view, proj);
        } else {
            copy_mtx(mist_clip, proj);
        }
        mist_clip_ok = 1;
    }
}

// How far into the game's fog a place in the world lies, 0 to 1, as the
// land there is fogged: its depth on the fog's scale of 0 (the near plane)
// to 1000 (the far), between the fog's near and far in force.
static f32 mist_fog_at(f32 px, f32 py, f32 pz) {
    f32 zc = px * mist_clip[0][2] + py * mist_clip[1][2] + pz * mist_clip[2][2] + mist_clip[3][2];
    f32 wc = px * mist_clip[0][3] + py * mist_clip[1][3] + pz * mist_clip[2][3] + mist_clip[3][3];
    f32 p, f;

    if (!mist_clip_ok || wc < 1.0f || mist_fog_max <= mist_fog_min) {
        return 0.0f;
    }
    p = 500.0f * (1.0f + zc / wc);
    f = (p - (f32) mist_fog_min) / (f32) (mist_fog_max - mist_fog_min);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

// One copy for the mist, from its record and its part's matrix: a water
// corner's place in the world raised a little, a shore corner's own place
// in its part's space (its mist lies at the land's own depth); its s and t
// from its place in the world; its alpha by the eye's distance and by its
// height over the water near it, less the fog there; and its color the
// mist's gone into the fog's as far as the fog has it.
static u32 mist_corner(u32 i, f32 m[4][4], const f32* eye, f32 base_s, f32 base_t, f32 drift_s, f32 drift_t, f32 per) {
    const Vtx* src = &glint_vtx[0][i];
    Vtx* dst = &mist_vtx[frame_ctx()][i];
    f32 x = src->v.ob[0], y = src->v.ob[1], z = src->v.ob[2];
    f32 px = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
    f32 py = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
    f32 pz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
    u32 water = glint_face[i] & 0x2000u;
    f32 vx, vy, vz, d, a, g, h, fog;
    s32 k;

    if (mist_floor[i] == MIST_UNKNOWN) {
        mist_floor_find(i, px, pz);
    }
    vx = eye[0] - px;
    vy = eye[1] - py;
    vz = eye[2] - pz;
    d = __builtin_sqrtf(vx * vx + vy * vy + vz * vz);
    if (d <= MIST_NEAR) {
        a = 0.0f;
    } else if (d < MIST_FULL) {
        a = (d - MIST_NEAR) / (MIST_FULL - MIST_NEAR);
    } else if (d < MIST_FAR0) {
        a = 1.0f;
    } else if (d < MIST_FAR1) {
        a = (MIST_FAR1 - d) / (MIST_FAR1 - MIST_FAR0);
    } else {
        a = 0.0f;
    }
    h = py - (f32) mist_floor[i];
    g = 1.0f - (h - MIST_FLOOR) / MIST_RISE;
    g = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
    h = 1.0f - ((f32) mist_near[i] * 4.0f - MIST_REACH0) / (MIST_REACH1 - MIST_REACH0);
    g *= h < 0.0f ? 0.0f : h > 1.0f ? 1.0f : h;
    fog = (a * g > 0.0f) ? mist_fog_at(px, py, pz) : 0.0f;
    a *= g * (1.0f - fog);
    if (water) {
        // The water writes no depth, so its mist is compared with the bed's
        // and must stand clear of it: a little over the water.
        dst->v.ob[0] = (s16) px;
        dst->v.ob[1] = (s16) (py + MIST_LIFT);
        dst->v.ob[2] = (s16) pz;
    } else {
        // The land's own corner, drawn under the part's own matrices at the
        // land's own depth.
        dst->v.ob[0] = src->v.ob[0];
        dst->v.ob[1] = src->v.ob[1];
        dst->v.ob[2] = src->v.ob[2];
    }
    dst->v.flag = 0;
    dst->v.tc[0] = (s16) ((px - base_s) * per + drift_s);
    dst->v.tc[1] = (s16) ((pz - base_t) * per + drift_t);
    for (k = 0; k < 3; k++) {
        dst->v.cn[k] = (u8) (mist_rgb[k] + (s32) ((f32) ((s32) mist_fog_rgb[k] - mist_rgb[k]) * fog));
    }
    dst->v.cn[3] = (u8) (a * 255.0f);
    return dst->v.cn[3] != 0;
}

// Whether a triangle of the copies is drawn: not when none of its corners
// carries mist, nor when its face is steeper than 65 degrees.
static s32 mist_tri(const Vtx* set, const u16* slot_copy, u32 a, u32 b, u32 c) {
    const Vtx *A, *B, *C;
    f32 ux, uy, uz, vx, vy, vz, nx, ny, nz, all;

    if (a >= 64 || b >= 64 || c >= 64 || slot_copy[a] == 0xFFFF || slot_copy[b] == 0xFFFF || slot_copy[c] == 0xFFFF) {
        return 0;
    }
    A = &set[slot_copy[a]];
    B = &set[slot_copy[b]];
    C = &set[slot_copy[c]];
    if (A->v.cn[3] == 0 && B->v.cn[3] == 0 && C->v.cn[3] == 0) {
        return 0;
    }
    ux = (f32) (B->v.ob[0] - A->v.ob[0]);
    uy = (f32) (B->v.ob[1] - A->v.ob[1]);
    uz = (f32) (B->v.ob[2] - A->v.ob[2]);
    vx = (f32) (C->v.ob[0] - A->v.ob[0]);
    vy = (f32) (C->v.ob[1] - A->v.ob[1]);
    vz = (f32) (C->v.ob[2] - A->v.ob[2]);
    nx = uy * vz - uz * vy;
    ny = uz * vx - ux * vz;
    nz = ux * vy - uy * vx;
    all = nx * nx + ny * ny + nz * nz;
    return all > 1.0f && ny * ny >= 0.179f * all;
}

// A part of the record under the matrix the game drew it with: its copies
// set for this frame, and if any carries mist the part marked for the draw
// and the game's own matrix commands for it kept for the shore's pass, so
// the renderer works the shore's copies through the very matrices the land
// was drawn through and their depths come out the same.
static void mist_segment(s32 k, f32 m[4][4], const f32* eye, u32 chain[][2], u32 chain_n) {
    const GlintSeg* seg = &glint_seg[k];
    u32 ticks = cycle ? ticks_of(night) : ride_ticks_of(night);     // at a fixed Dawn the drift runs on when the clock starts over; a photo under Live has no rounds to draw it by
    f32 per = 32.0f * 32.0f / NIGHT_MIST_TILE;      // a corner's s and t (a texel is 32) a unit of the world
    // Whole tiles off the part's own place, so s and t fit their sixteen bits
    // and the picture still joins from part to part; and the drift, a tile
    // in some minutes, wrapping on the tile.
    f32 base_s = (f32) ((s32) (m[3][0] / NIGHT_MIST_TILE)) * NIGHT_MIST_TILE;
    f32 base_t = (f32) ((s32) (m[3][2] / NIGHT_MIST_TILE)) * NIGHT_MIST_TILE;
    f32 drift_s = (f32) (ticks % MIST_DRIFT_S) * (1024.0f / (f32) MIST_DRIFT_S);
    f32 drift_t = (f32) (ticks % MIST_DRIFT_T) * (1024.0f / (f32) MIST_DRIFT_T);
    u32 j, o, any = 0;

    for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
        o = glint_op[j];
        if ((o & GLINT_LOAD) && (o & (GLINT_USED | GLINT_LAND))) {
            u32 start = (o >> 12) & 0x3FFF, count = (o >> 6) & 0x3F, v;

            for (v = 0; v < count; v++) {
                any += mist_corner(start + v, m, eye, base_s, base_t, drift_s, drift_t, per);
            }
        }
    }
    if (any == 0) {
        return;
    }
    mist_ready[k] = 1;
    for (j = 0; j < chain_n && j < MIST_CHAIN; j++) {
        mist_part_chain[frame_ctx()][k][j][0] = chain[j][0];
        mist_part_chain[frame_ctx()][k][j][1] = chain[j][1];
    }
    mist_part_chain_n[frame_ctx()][k] = (u8) (chain_n < MIST_CHAIN ? chain_n : MIST_CHAIN);
    mist_corners += any;
    mist_corners_frame += any;
    mist_parts_frame++;
}

// A copy's place in the world, as one number, and whether a triangle with
// these copies' places was drawn already this frame: the game draws a model
// twice at times (the River's first stretch is in the record as two parts),
// and its mist would lie twice.
static u32 mist_place_key(u32 i) {
    return (u32) (u16) glint_world_x[i] * 73856093u ^ (u32) (u16) glint_world_y[i] * 19349663u
           ^ (u32) (u16) glint_world_z[i] * 83492791u;
}

static s32 mist_same_place(u32 i, u32 j) {
    return glint_world_x[i] == glint_world_x[j] && glint_world_y[i] == glint_world_y[j] && glint_world_z[i] == glint_world_z[j];
}

static s32 mist_seen(u32 a, u32 b, u32 c) {
    u32 h = (mist_place_key(a) + mist_place_key(b) + mist_place_key(c)) & 1023;
    u32 e;

    for (e = mist_seen_head[h]; e != 0xFFFF; e = mist_seen_next[e]) {
        const u16* t = mist_seen_copy[e];

        if ((mist_same_place(a, t[0]) || mist_same_place(a, t[1]) || mist_same_place(a, t[2]))
            && (mist_same_place(b, t[0]) || mist_same_place(b, t[1]) || mist_same_place(b, t[2]))
            && (mist_same_place(c, t[0]) || mist_same_place(c, t[1]) || mist_same_place(c, t[2]))
            && (mist_same_place(t[0], a) || mist_same_place(t[0], b) || mist_same_place(t[0], c))
            && (mist_same_place(t[1], a) || mist_same_place(t[1], b) || mist_same_place(t[1], c))
            && (mist_same_place(t[2], a) || mist_same_place(t[2], b) || mist_same_place(t[2], c))) {
            return 1;
        }
    }
    if (mist_seen_n < MIST_TRIS) {
        e = mist_seen_n++;
        mist_seen_copy[e][0] = (u16) a;
        mist_seen_copy[e][1] = (u16) b;
        mist_seen_copy[e][2] = (u16) c;
        mist_seen_next[e] = mist_seen_head[h];
        mist_seen_head[h] = (u16) e;
    }
    return 0;
}

// One pass of the draw over the parts marked this frame: each part's loads
// and the triangles that carry mist, the water's or the shore's. The water's
// pass goes first, in the world, and writes its depth; the shore's follows
// with each part's copies under the game's own matrix commands for that
// part, loaded over the world's frame, and a depth test that lets an equal
// depth through, so the shore's mist lies on the land itself and nowhere
// nearer stands: not under the water's mist, nor under the water (its bed
// lies deeper), nor over anything standing on the bank. (A decal, which
// the renderer draws only within a tolerance of the depth found, was
// refused everywhere, Oct 20 2026.) A triangle drawn once this frame
// already is passed over.
static void mist_pass(Gfx** list, Vtx* set, u32 water) {
    u16 slot_copy[64];
    u32 j, o, pending;
    s32 k, s;

    for (k = 0; k < glint_seg_n; k++) {
        const GlintSeg* seg = &glint_seg[k];

        if (!mist_ready[k]) {
            continue;
        }
        for (s = 0; s < 64; s++) {
            slot_copy[s] = 0xFFFF;
        }
        if (!water) {
            u32 n = mist_part_chain_n[frame_ctx()][k], c;

            if (n == 0) {
                gSPMatrix((*list)++, &mist_world_mtx, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            }
            for (c = 0; c < n; c++) {
                gSPMatrix((*list)++, (Mtx*) mist_part_chain[frame_ctx()][k][c][1],
                          (c == 0 ? G_MTX_PUSH | G_MTX_LOAD : G_MTX_NOPUSH | G_MTX_MUL) | G_MTX_MODELVIEW);
            }
        }
        pending = 0xFFFFFFFFu;
        for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
            o = glint_op[j];
            if (o & GLINT_LOAD) {
                u32 start = (o >> 12) & 0x3FFF, count = (o >> 6) & 0x3F, first = o & 63, v;
                u32 wanted = o & (water ? GLINT_USED : GLINT_LAND);

                if (pending != 0xFFFFFFFFu) {
                    gSP1Triangle((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0);
                    pending = 0xFFFFFFFFu;
                }
                for (v = 0; v < count && first + v < 64; v++) {
                    slot_copy[first + v] = wanted ? (u16) (start + v) : 0xFFFF;
                }
                if (wanted) {
                    gSPVertex((*list)++, &set[start], count, first);
                }
            } else if (((o & GLINT_FLOW) != 0) != (water != 0)) {
                continue;
            } else if (!mist_tri(set, slot_copy, (o >> 16) & 0xFF, (o >> 8) & 0xFF, o & 0xFF)) {
                continue;
            } else if (mist_seen(slot_copy[(o >> 16) & 0xFF], slot_copy[(o >> 8) & 0xFF], slot_copy[o & 0xFF])) {
                continue;
            } else if (pending == 0xFFFFFFFFu) {
                pending = o;
            } else {
                gSP2Triangles((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0,
                              (o >> 16) & 0xFF, (o >> 8) & 0xFF, o & 0xFF, 0);
                pending = 0xFFFFFFFFu;
            }
        }
        if (pending != 0xFFFFFFFFu) {
            gSP1Triangle((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0);
        }
        if (!water) {
            gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
        }
    }
}

// The parts marked this frame, drawn from their copies under the camera's
// view in force: the mist's modes, then the water's pass in the world's own
// space and the shore's in each part's.
static void mist_draw(Gfx** list) {
    Vtx* set = mist_vtx[frame_ctx()];
    s32 k, any = 0;

    for (k = 0; k < glint_seg_n; k++) {
        if (mist_ready[k]) {
            any = 1;
        }
    }
    if (any) {
        mist_seen_n = 0;
        for (k = 0; k < 1024; k++) {
            mist_seen_head[k] = 0xFFFF;
        }
        command(list, (0x64u << 24) | 0x00002Cu, 1);
        // The copies stand in the world, so the world's own frame is loaded
        // over whatever the scene's last object left on the stack, in a
        // matrix group of the mist's own that the port leaves still between
        // the game's frames, as the bolt's; under another object's group
        // the mist went where that object's matrix glided.
        if (!mist_world_ok) {
            place(&mist_world_mtx, 0.0f, 0.0f, 0.0f);
            mist_world_ok = 1;
        }
        command(list, (0x64u << 24) | 0x00000Cu, 0);
        command(list, 0x00000001u, 0);
        gSPMatrix((*list)++, &mist_world_mtx, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList((*list)++, night_mist_begin);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (u32) (MIST_ALPHA * mist_share));
        mist_pass(list, set, 1);
        gDPPipeSync((*list)++);
        // The shore's: at the land's own depth, which passes as equal (the
        // see-through mode's test lets only a nearer depth through), no depth written.
        gDPSetRenderMode((*list)++, G_RM_PASS,
                         Z_CMP | IM_RD | CVG_DST_FULL | ZMODE_OPA | FORCE_BL | GBL_c2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA));
        mist_pass(list, set, 0);
        for (k = 0; k < glint_seg_n; k++) {
            mist_ready[k] = 0;
        }
        gSPDisplayList((*list)++, night_mist_end);
        gDPSetAlphaCompare((*list)++, G_AC_NONE);     // as the game leaves it after its own cutouts
        gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
        command(list, (0x64u << 24) | 0x00000Du, 1);
        command(list, (0x64u << 24) | 0x00002Cu, 0);
    }
    // The frame's books close with its draw, whatever the sky's place in the
    // frame's order: the camera is read again at the next
    // frame's first misty model, and the count is kept for the log.
    mist_parts_last = mist_parts_frame;
    mist_corners_last = mist_corners_frame;
    mist_parts_frame = 0;
    mist_corners_frame = 0;
    mist_clip_ok = 0;
}

// The water was just drawn: its commands, from where this model began, hold
// the matrices its nodes were drawn with and the calls of their lists. Each
// call that is a segment's is answered with that segment, under the product
// of the matrices in force there.
static void glint_scan(s32 mode) {
    DObj* eye = sky_now->data.dobj;
    f32 stack[8][4][4];
    u32 chain[8][MIST_CHAIN][2];    // the commands that built each level of the stack, for the mist
    u32 chain_n[8];
    f32 at[3], la[2], breath[2], len;
    u32 ticks;
    s32 i, k;

    if (eye == NULL || glint_seg_n == 0) {
        return;
    }
    at[0] = eye->position.v.x;
    at[1] = eye->position.v.y;
    at[2] = eye->position.v.z;
    len = __builtin_sqrtf(relight_dir[0] * relight_dir[0] + relight_dir[2] * relight_dir[2]);
    if (mode == 0 && len < 0.01f) {
        return;         // the moon overhead: no bearing to stretch along
    }
    la[0] = (len > 0.01f) ? relight_dir[0] / len : 1.0f;
    la[1] = (len > 0.01f) ? relight_dir[2] / len : 0.0f;
    // The lobe breathes: a slow sway along the bearing and a quicker, smaller one across.
    ticks = ticks_of(night);
    breath[0] = (f32) night_sine[(ticks * 2) & 0xFF] * (0.012f / 127.0f);
    breath[1] = (f32) night_sine[((ticks * 3) + 80) & 0xFF] * (0.006f / 127.0f);
    for (k = 0; k < GLINT_SEGS; k++) {
        glint_drawn[k] = 0;
    }
    if (!glint_said) {
        glint_said = 1;
        recomp_printf("[island skies] the record so far: %d corners, %d triangles of water, %d of land, %d parts\n",
                      (s32) glint_vtx_n, (s32) glint_tris, (s32) glint_land, (s32) glint_seg_n);
    }
    for (i = 0; i < 4; i++) {
        volatile u32* cmd = (volatile u32*) entry_pos[i];
        volatile u32* end = (volatile u32*) MAIN_GFX_POS[i];
        Gfx** game = &MAIN_GFX_POS[i];
        Gfx** list = gfx_stage_open(game);      // what the walk adds: moved into the buffer whole, or left out
        s32 sp = 0, budget = (mode == 2) ? 16384 : 4096, opened = 0, r, c;

        if (cmd == NULL || end < cmd) {
            continue;
        }
        for (r = 0; r < 4; r++) {
            for (c = 0; c < 4; c++) {
                stack[0][r][c] = (r == c) ? 1.0f : 0.0f;
            }
        }
        chain_n[0] = 0;
        while (cmd < end && budget-- > 0) {
            u32 w0 = cmd[0], w1 = cmd[1];

            switch (w0 >> 24) {
                case 0xDA:      // a matrix: pushed unless the low bit says not, loaded or multiplied
                    if (!(w0 & 4) && w1 >= 0x80000000u) {
                        f32 m[4][4];

                        read_mtx(m, w1);
                        if ((w0 & 1) == 0 && sp < 7) {
                            u32 j;

                            copy_mtx(stack[sp + 1], stack[sp]);
                            for (j = 0; j < chain_n[sp]; j++) {
                                chain[sp + 1][j][0] = chain[sp][j][0];
                                chain[sp + 1][j][1] = chain[sp][j][1];
                            }
                            chain_n[sp + 1] = chain_n[sp];
                            sp++;
                        }
                        if (w0 & 2) {
                            copy_mtx(stack[sp], m);
                            chain[sp][0][0] = w0;
                            chain[sp][0][1] = w1;
                            chain_n[sp] = 1;
                        } else {
                            f32 out[4][4];

                            mul_mtx(out, m, stack[sp]);
                            copy_mtx(stack[sp], out);
                            if (chain_n[sp] < MIST_CHAIN) {
                                chain[sp][chain_n[sp]][0] = w0;
                                chain[sp][chain_n[sp]][1] = w1;
                                chain_n[sp]++;
                            }
                        }
                    }
                    break;
                case 0xD8: {    // matrices popped
                    s32 n = (s32) (w1 >> 6);

                    while (n-- > 0 && sp > 0) {
                        sp--;
                    }
                    break;
                }
                case 0xDE:      // a list called: a segment's?
                    for (k = 0; k < glint_seg_n; k++) {
                        s32 j;

                        if (glint_drawn[k]) {
                            continue;
                        }
                        for (j = 0; j < glint_seg[k].lists_n; j++) {
                            if (glint_seg[k].lists[j] == w1) {
                                break;
                            }
                        }
                        if (j < glint_seg[k].lists_n) {
                            glint_drawn[k] = 1;
                            if (mode == 0) {
                                glint_segment(k, stack[sp], at, la, breath, list, &opened);
                            } else if (mode == 1) {
                                ember_segment(k, stack[sp], list, &opened);
                            } else {
                                mist_segment(k, stack[sp], at, chain[sp], chain_n[sp]);
                            }
                            break;
                        }
                    }
                    break;
                default:
                    break;
            }
            cmd += 2;
        }
        if (opened) {
            gSPDisplayList((*list)++, night_rain_end);
            command(list, (0x64u << 24) | 0x00002Cu, 0);
        }
        gfx_stage_close(game, 0);
    }
    if (!glint_said_drawn && mode != 2) {
        s32 drawn = 0;

        glint_said_drawn = 1;
        for (k = 0; k < glint_seg_n; k++) {
            drawn += glint_drawn[k];
        }
        recomp_printf("[island skies] the glint: %d of %d parts of the water found among the frame's commands\n",
                      drawn, (s32) glint_seg_n);
    }
}

static void moon_glint(void) {
    glint_scan(0);
}

static void embers(void) {
    ember_drawn = 0;
    glint_scan(1);
}

// The dawn's mist, after a model with parts of the record: the pool of the
// water's corners is kept up, the camera's matrices are read once a frame,
// then the parts are found among the model's commands and their copies set
// (mist_segment), for the draw after the scene (mist_draw).
static void mist_scan(void) {
    u32 ticks = ticks_of(night);
    GObj* cam = *(GObj* volatile*) 0x8004AC48u;

    if (cam == NULL || cam->data.cam == NULL) {
        return;
    }
    if (mist_cam != cam || !mist_clip_ok || !on_the_ride) {   // a photo's camera is read at each of its models: the album may draw several photos with one
        mist_cam = cam;
        mist_clip_read();
    }
    mist_floors_keep();
    mist_corners = 0;
    glint_scan(2);
    // Said once a ride, past the intro's own camera, so the eye's height is
    // the ride's, at the first model whose parts carry any mist.
    if (!mist_said && on_the_ride && ticks >= 600 && mist_corners > 0 && sky_now != NULL && sky_now->data.dobj != NULL) {
        DObj* eye = sky_now->data.dobj;

        mist_said = 1;
        recomp_printf("[island skies] the mist stands at %d of 100 at tick %u; the eye at %d, the water from %d to %d (%u corners); the record %d corners in %d parts%s%s, %d of the corners water; %u corners misted in this model; the fog %d to %d, the camera's matrices %s\n",
                      (s32) (mist_share * 100.0f), ticks, (s32) eye->position.v.y, (s32) flow_y_min, (s32) flow_y_max, flow_y_n,
                      (s32) glint_vtx_n, (s32) glint_seg_n,
                      (glint_vtx_n > GLINT_VTX - 64) ? " (the corners FULL)" : "",
                      (glint_seg_n >= GLINT_SEGS) ? " (the parts FULL)" : "",
                      (s32) mist_pool_n, mist_corners, (s32) mist_fog_min, (s32) mist_fog_max, mist_clip_ok ? "read" : "NOT read");
        // The water's own triangles in the world, once, to see where they
        // meet and where they overlap (the lines on the water, Oct 19 2026).
        {
            s32 k;

            for (k = 0; k < glint_seg_n; k++) {
                const GlintSeg* seg = &glint_seg[k];
                u16 slot_copy[64];
                u32 j, o;
                s32 s;

                for (s = 0; s < 64; s++) {
                    slot_copy[s] = 0xFFFF;
                }
                for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
                    o = glint_op[j];
                    if (o & GLINT_LOAD) {
                        u32 start = (o >> 12) & 0x3FFF, count = (o >> 6) & 0x3F, first = o & 63, v;

                        for (v = 0; v < count && first + v < 64; v++) {
                            slot_copy[first + v] = (u16) (start + v);
                        }
                    } else if (o & GLINT_FLOW) {
                        u32 a = (o >> 16) & 0xFF, b = (o >> 8) & 0xFF, c = o & 0xFF;

                        if (a < 64 && b < 64 && c < 64 && slot_copy[a] != 0xFFFF && slot_copy[b] != 0xFFFF && slot_copy[c] != 0xFFFF) {
                            recomp_printf("[island skies] water %d: %d %d %d / %d %d %d / %d %d %d\n", k,
                                          glint_world_x[slot_copy[a]], glint_world_y[slot_copy[a]], glint_world_z[slot_copy[a]],
                                          glint_world_x[slot_copy[b]], glint_world_y[slot_copy[b]], glint_world_z[slot_copy[b]],
                                          glint_world_x[slot_copy[c]], glint_world_y[slot_copy[c]], glint_world_z[slot_copy[c]]);
                        }
                    }
                }
            }
        }
    }
}

// The ride's mist, drawn as the UI camera begins, after the whole scene with
// its depth and before the counter and the pointer, under the scene camera's
// viewport, border, projection and view, as the Tunnel's weather is; the
// whole mode word set first, as a camera's start sets it, since the scene's
// last objects leave their own modes behind.
static void mist_weather(void) {
    GObj* cam = mist_cam;

    if (night == NULL || !on_the_ride || scoring || roof_blink || cam == NULL || cam->data.cam == NULL
        || mist_share <= 0.0f || mist_parts_frame == 0) {
        return;
    }
    {
        u32 oc = (u32) cam->data.cam;
        Gfx** game = &MAIN_GFX_POS[0];
        u32 matrices = *(volatile u32*) 0x8004B248u;  // ren_D_8004B248: the camera's matrix sub-list begins after it
        const volatile s32* border = (const volatile s32*) 0x803AE540u;    // MainCameraBorderXmin, Ymin, Xmax, Ymax (app_level/player.c)
        s32 x0 = border[0], y0 = border[1], x1 = border[2], y1 = border[3];

        if (x0 < 0) {
            x0 = 0;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (x1 > 320) {
            x1 = 320;
        }
        if (y1 > 240) {
            y1 = 240;
        }
        if (matrices != 0 && *game != NULL && x1 > x0 && y1 > y0) {
            Gfx** list = gfx_stage_open(game);

            gDPPipeSync((*list)++);
            gSPViewport((*list)++, oc + 0x08);
            gDPSetScissor((*list)++, G_SC_NON_INTERLACE, x0, y0, x1, y1);
            gSPDisplayList((*list)++, (Gfx*) (matrices + sizeof(Gfx)));
            gDPPipeSync((*list)++);
            gDPSetOtherMode((*list)++,
                            G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE
                            | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                            G_AC_THRESHOLD | G_ZS_PIXEL | G_RM_AA_ZB_XLU_SURF | G_RM_AA_ZB_XLU_SURF2);
            mist_draw(list);
            gfx_stage_close(game, 0);
        }
    }
}

// A photo's mist, drawn as its camera's object pass ends, under the matrices
// still in force, the whole mode word set first as for the ride.
static void mist_photo(GObj* cam) {
    Gfx** game = &MAIN_GFX_POS[0];
    Gfx** list;

    if (night == NULL || on_the_ride || scoring || cam == NULL || cam != mist_cam || cam->data.cam == NULL
        || mist_share <= 0.0f || mist_parts_frame == 0 || *game == NULL) {
        return;
    }
    list = gfx_stage_open(game);
    gDPPipeSync((*list)++);
    gDPSetOtherMode((*list)++,
                    G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE
                    | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_THRESHOLD | G_ZS_PIXEL | G_RM_AA_ZB_XLU_SURF | G_RM_AA_ZB_XLU_SURF2);
    mist_draw(list);
    gfx_stage_close(game, 0);
}

// Fireflies: four groups of small dots around the camera, near the water,
// each group drifting on slow waves of its own. Each dot flashes by itself
// and seldom: its time is cut into spans of twelve seconds, and in each
// span a draw from the span's number and the dot's own says whether it
// flashes at all (five spans in eight) and when, so no two spans are alike
// and no two dots keep time together; the flash is a soft rise, a moment
// held, and a longer fade, under a second in all. A photo keeps the flashes
// of its instant, as it keeps the twinkle.
#define FIREFLY_SPAN 720        // ticks: twelve seconds
#define FIREFLY_ODDS 5          // of 8: how many spans have a flash
#define FIREFLY_RISE 12         // ticks: the rise
#define FIREFLY_HOLD 6          // held at the top
#define FIREFLY_FADE 36         // and the fade
#define FIREFLY_LIT (FIREFLY_RISE + FIREFLY_HOLD + FIREFLY_FADE)
#define FIREFLY_PEAK 150        // the brightest it gets, of 255

// A draw for a dot and a span: the same numbers in, the same number out.
static u32 firefly_draw(u32 dot, u32 span) {
    u32 x = dot * 0x9E3779B1u ^ span * 0x85EBCA77u;

    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

// How bright a dot is at an instant, 0 when dark.
static s32 firefly_bright(u32 dot, u32 ticks) {
    u32 t = ticks + dot * 97;           // each dot's spans begin at a moment of its own
    u32 span = t / FIREFLY_SPAN;
    u32 in = t - span * FIREFLY_SPAN;
    u32 draw = firefly_draw(dot, span);
    u32 at;

    if ((draw & 7) >= FIREFLY_ODDS) {
        return 0;
    }
    at = (draw >> 3) % (FIREFLY_SPAN - FIREFLY_LIT);
    if (in < at || in >= at + FIREFLY_LIT) {
        return 0;
    }
    in -= at;
    if (in < FIREFLY_RISE) {
        return (s32) (in + 1) * FIREFLY_PEAK / FIREFLY_RISE;
    }
    if (in < FIREFLY_RISE + FIREFLY_HOLD) {
        return FIREFLY_PEAK;
    }
    return (s32) (FIREFLY_LIT - in) * FIREFLY_PEAK / FIREFLY_FADE;
}

static void fireflies(GObj* sky) {
    Gfx** game = &MAIN_GFX_POS[1];
    Gfx** list;
    DObj* eye = sky->data.dobj;
    u32 ticks = ticks_of(night);
    Mtx* set = glow_mtx[frame_slot(&glow_slot_n, FRAME_SLOTS)];
    u32 g, i;

    if (eye == NULL || *game == NULL) {
        return;
    }
    list = gfx_stage_open(game);
    command(list, (0x64u << 24) | 0x00002Cu, 1);
    gSPDisplayList((*list)++, night_glow_begin);
    for (g = 0; g < NIGHT_FIREFLY_GROUPS; g++) {
        f32 dx = (f32) (night_sine[(((ticks * 3) >> 3) + 40 * g) & 0xFF] * 24) / 127.0f;
        f32 dy = (f32) (night_sine[(((ticks * 2) >> 3) + 90 * g) & 0xFF] * 12) / 127.0f;
        f32 dz = (f32) (night_sine[(((ticks * 5) >> 4) + 20 * g) & 0xFF] * 20) / 127.0f;
        Mtx* mtx = &set[1 + g];
        s32 opened = 0;

        for (i = 0; i < NIGHT_FIREFLY_DOTS; i++) {
            u32 k = g * NIGHT_FIREFLY_DOTS + i;
            s32 bright = firefly_bright(k, ticks);

            if (bright <= 0) {
                continue;
            }
            if (!opened) {
                opened = 1;
                place(mtx, eye->position.v.x + dx, eye->position.v.y + dy, eye->position.v.z + dz);
                command(list, (0x64u << 24) | 0x00000Cu, 0x46460000u | g);
                command(list, 0x00002829u, 0);
                gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
            }
            gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (u32) ((f32) bright * clear_share));
            gSPDisplayList((*list)++, night->fireflies[k]);
        }
        if (opened) {
            gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
            command(list, (0x64u << 24) | 0x00000Du, 1);
        }
    }
    gSPDisplayList((*list)++, night_rain_end);
    command(list, (0x64u << 24) | 0x00002Cu, 0);
    gfx_stage_close(game, 0);
}

// Lightning. Time is cut into spans of a few seconds; whether a span has a
// strike, when in the span, where around the sky and in which shape all
// come from the span's number alone, so the same instant always shows the
// same thing -- on the ride, and when a photo of that instant is redrawn.
#define STRIKE_SPAN 420         // ticks: seven seconds
#define STRIKE_ODDS 115         // of 256: how many spans have a strike
#define FLASH_TICKS 18          // three tenths of a second

void setFogColor(u8 r, u8 g, u8 b);
void setFogDistance(u16 min, u16 max);
s32 auPlaySoundWithParams(u32 sound, s32 volume, s32 pan, f32 pitch, s32 reverb);
void auStopSound(u32 handle);
void auSetSoundVolume(s32 handle, u32 volume);

// The weather's sounds are the cartridge's own. The rain is the rush of
// water the Valley plays for its whirlpool, looping. The thunder is two
// booms the cartridge holds: a deep one it never plays, as the strike, and
// the one that sounds under an evolution, slowed, as the body; the body
// again, lower and softer, is the echo. (A long low roll that snores, and
// Electrode's burst, were tried and heard first.)
#define RAIN_SOUND     33
#define RAIN_VOLUME    0x3000
#define THUNDER_STRIKE 102
#define THUNDER_BODY   395

static u32 strike_turn;         // where around the sky it stands, of 256
static u32 strike_shape;        // and which shape it has
static u32 strike_span;         // the span it belongs to, and that span's number mixed, for the thunder
static u32 strike_mix;
static s32 rain_handle = -1;    // the rain's voice while it plays
static u32 rain_vol;            // and the volume it was last set to
static u32 thunder_span;        // the span whose strike was heard last
static u32 thunder_at;          // when the thunder is due, in ticks; 0 when none is
static u32 body_at, echo_at;    // when its body and its echo are due; 0 when none is

static void strike_at(u32 ticks) {
    // A flash flickers: full, half, nearly full again, then dies away. The bolt is
    // there for the first four ticks only.
    static const u8 flash[FLASH_TICKS / 2] = { 255, 120, 220, 110, 70, 46, 30, 18, 8 };
    static const u8 bolt[4] = { 255, 90, 200, 60 };
    u32 span = ticks / STRIKE_SPAN;
    u32 mix = span * 0x9E3779B1u;
    u32 when;

    mix ^= mix >> 15;
    mix *= 0x85EBCA77u;
    mix ^= mix >> 13;
    strike_flash = 0;
    strike_bolt = 0;
    if (span == 0 || (mix & 0xFF) >= STRIKE_ODDS) {
        return;     // no strike in the ride's first seconds, nor in most spans
    }
    when = span * STRIKE_SPAN + ((mix >> 8) % (STRIKE_SPAN - FLASH_TICKS));
    if (ticks >= when && ticks < when + FLASH_TICKS) {
        u32 step = (ticks - when) / 2;      // each value holds for two ticks

        strike_flash = flash[step];
        if (step < 4) {
            strike_bolt = bolt[step];
        }
        // The switch: Gentle keeps the bolt and the thunder but dims the flash
        // that fills the screen to two fifths; Off has no lightning at all,
        // and with no strike the thunder never comes either.
        if (lightning == 2) {
            strike_flash = 0;
            strike_bolt = 0;
            return;
        }
        if (lightning == 1) {
            strike_flash = (strike_flash * 102) / 255;
        }
        if (ticks - when < 2) {           // the strike's first frame (the frames step by two ticks)
            recomp_printf("[island skies] lightning at tick %u: flash %u of 255, bolt %u\n", ticks, strike_flash, strike_bolt);
        }
        strike_turn = (mix >> 16) & 0xFF;
        strike_shape = (mix >> 24) % NIGHT_BOLTS;
        strike_span = span;
        strike_mix = mix;
    }
}

// The sound the game has in each of its voices (sys/audio.c, auPlayingSound:
// a pointer to the table), -1 when the voice is free.
#define VOICE_SOUND (*(s32* volatile*) 0x800968BCu)

// Thunder follows a strike after a moment of the strike's own, a sixth of a
// second to a little over one: the strike, its body a tenth of a second
// after, and its echo half a second on. A strike's own number varies the
// pitch a little, so no two are quite alike. On the ride only: a photo's
// instant is silent.
// The side the bolt stood on, as a pan for its thunder: the game sets the
// scene's light behind and above the camera every frame, so the view runs
// the other way from the light, and the bolt's direction on the view's
// right is how far the sound sits to the right (64 is the middle).
static s32 thunder_side = 64;

static s32 thunder_pan(void) {
    volatile s8* dir = (volatile s8*) (COURSE_LIGHT + 16);
    f32 fx = -(f32) dir[0];
    f32 fz = -(f32) dir[2];
    f32 len = __builtin_sqrtf(fx * fx + fz * fz);
    f32 bx = (f32) night_sine[(strike_turn + 64) & 0xFF];
    f32 bz = -(f32) night_sine[strike_turn & 0xFF];
    f32 side;

    if (len < 8.0f) {
        return 64;
    }
    side = (-fz * bx + fx * bz) / (len * 127.0f);
    return 64 + (s32) (side * 52.0f);
}

static void thunder(u32 ticks) {
    if (strike_flash != 0 && strike_span != thunder_span) {
        thunder_span = strike_span;
        thunder_at = ticks + 10 + ((strike_mix >> 4) & 0x3F);
        body_at = echo_at = 0;
    }
    if (thunder_at != 0 && ticks >= thunder_at) {
        f32 pitch = 0.88f + (f32) ((strike_mix >> 12) & 3) * 0.04f;

        thunder_side = thunder_pan();
        auPlaySoundWithParams(THUNDER_STRIKE, 0x7000, thunder_side, pitch, 60);
        body_at = ticks + 6;
        echo_at = ticks + 26 + ((strike_mix >> 20) & 0xF);
        recomp_printf("[island skies] thunder at tick %u, pan %d\n", ticks, thunder_side);
        thunder_at = 0;
    }
    if (body_at != 0 && ticks >= body_at) {
        auPlaySoundWithParams(THUNDER_BODY, 0x6000, thunder_side, 0.8f, 80);
        body_at = 0;
    }
    if (echo_at != 0 && ticks >= echo_at) {
        auPlaySoundWithParams(THUNDER_BODY, 0x3400, 64, 0.6f, 110);
        echo_at = 0;
    }
}

// The rain's sound: one voice, looping, started as the ride's sky is first
// drawn, started again should the game have taken the voice for something
// of its own, and stopped as the world is taken down.
static f32 rain_voice = 1.0f;    // what the voice is scaled by: under the Tunnel's roof it recedes with the mouth

static void rain_sound(void) {
    u32 vol = (u32) ((f32) RAIN_VOLUME * rain_share * rain_voice);     // as loud as the rain is

    if (rain_handle >= 0 && VOICE_SOUND != NULL && VOICE_SOUND[rain_handle] != RAIN_SOUND) {
        rain_handle = -1;
    }
    if (rain_handle < 0) {
        rain_handle = auPlaySoundWithParams(RAIN_SOUND, vol, 64, 1.1f, 20);
        rain_vol = vol;
        recomp_printf("[island skies] the rain's voice: %d\n", rain_handle);
    } else if (vol != rain_vol) {
        auSetSoundVolume(rain_handle, vol);
        rain_vol = vol;
    }
}

// The rain's voice stopped: the rain has ended, or the world is taken down.
static void rain_quiet(void) {
    if (rain_handle >= 0) {
        if (VOICE_SOUND != NULL && VOICE_SOUND[rain_handle] == RAIN_SOUND) {
            auStopSound((u32) rain_handle);
        }
        rain_handle = -1;
    }
}

// The night's voices: three insects in the dark, each with its own pitch and
// its own side, chirping at its own uneven pace, from one short sound of the
// cartridge's bank, played low under the music. The picks are by ear (the
// playtest folder's "night sounds" holds the candidates); the pace comes
// from the world's clock, so a photo's instant is quiet or not as the ride
// was. Not in rain, snow or a storm, which have their own voices, nor at
// sunset, when the day's sounds still stand.
#define CHIRP_SOUND 309
#define CHIRP_VOICES 3
static u32 chirp_next[CHIRP_VOICES];      // the tick of each insect's next chirp
static u8 chirp_said;

static void night_voices(void) {
    static const u8 chirp_pan[CHIRP_VOICES] = { 26, 64, 102 };
    static const f32 chirp_pitch[CHIRP_VOICES] = { 1.0f, 1.12f, 0.91f };
    static const u16 chirp_vol[CHIRP_VOICES] = { 0x1A00, 0x1400, 0x1700 };
    u32 ticks = ride_ticks_of(night);   // a chirp due past the clock's last tick would never come
    s32 k;

    if (!on_the_ride || !night_sounds_on || sunset || raining || snowing || storming || scoring || night->embers
        || ROOFED(night)) {             /* none by the lava, none under the Tunnel's roof */
        return;
    }
    for (k = 0; k < CHIRP_VOICES; k++) {
        if (ticks >= chirp_next[k] && ticks < chirp_next[k] + 30) {
            u32 mix = (ticks + 977u * (u32) k) * 0x9E3779B1u;
            f32 pitch = chirp_pitch[k] * (0.97f + (f32) ((mix >> 8) & 0xF) * 0.004f);
            s32 pan = (s32) chirp_pan[k] + (s32) ((mix >> 16) & 0xF) - 8;

            auPlaySoundWithParams(CHIRP_SOUND, chirp_vol[k], pan, pitch, 40);
            chirp_next[k] = ticks + 45 + ((mix >> 20) % 150) + 30 * (u32) k;
            if (!chirp_said) {
                chirp_said = 1;
                recomp_printf("[island skies] the first chirp at tick %u\n", ticks);
            }
        } else if (ticks >= chirp_next[k] + 30) {
            chirp_next[k] = ticks + 20 + 40 * (u32) k;      // the clock jumped (a new ride): start again soon
        }
    }
}

// What a flash does to the whole scene. The colors painted on the land
// cannot be relit for an instant, but the fog can: it jumps toward the
// flash's color and draws nearer, a veil of lit rain over everything; the
// scene light jumps with it, so the Pokémon are lit too. With no flash
// this writes the night's own values back.
static void storm_light(void) {
    static const u8 lit[3] = { 132, 148, 204 };
    volatile u8* light = light_now;
    u8 fog[3];
    s32 i;

    for (i = 0; i < 3; i++) {
        u32 ambient = scene_ambient[i];
        u32 diffuse = scene_diffuse[i];

        fog[i] = (u8) (sky_horizon[i] + (((s32) lit[i] - sky_horizon[i]) * (s32) strike_flash) / 255);
        ambient += ((230 - ambient) * strike_flash) / 255;
        diffuse += ((255 - diffuse) * strike_flash) / 255;
        light[i] = light[4 + i] = (u8) ambient;
        light[8 + i] = light[12 + i] = (u8) diffuse;
    }
    setFogColor(fog[0], fog[1], fog[2]);
    setFogDistance((u16) (fog_near - (38 * strike_flash) / 255), fog_far);
    light_store();
}

// A matrix that turns a thing about the upright axis and moves it to a
// place: for the bolt, which is made standing to one side.
static void place_turned(Mtx* mtx, f32 x, f32 y, f32 z, u32 turn) {
    volatile u16* half = (volatile u16*) mtx;
    s32 cosine = night_sine[(turn + 64) & 0xFF] * 516;     // 16.16 fixed point: 65536 / 127
    s32 sine = night_sine[turn & 0xFF] * 516;

    place(mtx, x, y, z);
    half[0] = (u16) (cosine >> 16);
    half[16] = (u16) (cosine & 0xFFFF);
    half[2] = (u16) ((-sine) >> 16);
    half[18] = (u16) ((-sine) & 0xFFFF);
    half[8] = (u16) (sine >> 16);
    half[24] = (u16) (sine & 0xFFFF);
    half[10] = (u16) (cosine >> 16);
    half[26] = (u16) (cosine & 0xFFFF);
}

// A matrix from a place and three axes: the model's x, y and z run along them.
static void place_frame(Mtx* mtx, f32 x, f32 y, f32 z, const f32* ax, const f32* ay, const f32* az) {
    volatile u16* half = (volatile u16*) mtx;
    const f32* axes[3];
    s32 r, c;

    place(mtx, x, y, z);
    axes[0] = ax;
    axes[1] = ay;
    axes[2] = az;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            s32 v = (s32) (axes[r][c] * 65536.0f);

            half[r * 4 + c] = (u16) (v >> 16);
            half[16 + r * 4 + c] = (u16) (v & 0xFFFF);
        }
    }
}

// Sine and cosine off the mod's table, for angles that need no better.
static f32 tsin(f32 rad) {
    return (f32) night_sine[(u32) (rad * (256.0f / 6.2831853f) + 0.5f) & 0xFF] / 127.0f;
}

static f32 tcos(f32 rad) {
    return tsin(rad + 1.5707963f);
}

// The flash over the sky and the bolt, drawn right after the sky's own
// box and so behind all the land. Neither is to be placed between ticks:
// each is named to the renderer as a group it must leave alone.
static void storm_sky_at(Gfx** game, f32 ex, f32 ey, f32 ez) {
    Mtx* pair;
    Gfx** list;

    if (strike_flash == 0 || *game == NULL) {
        return;
    }
    list = gfx_stage_open(game);
    pair = storm_mtx[frame_slot(&storm_slot_n, FRAME_SLOTS)];
    place(&pair[0], ex, ey, ez);
    command(list, (0x64u << 24) | 0x00000Cu, 0);
    command(list, 0x00000001u, 0);
    gSPMatrix((*list)++, &pair[0], G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    gDPSetPrimColor((*list)++, 0, 0, 150, 170, 235, (120 * strike_flash) / 255);
    gSPDisplayList((*list)++, night_flash_dome);
    gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    if (strike_bolt != 0) {
        place_turned(&pair[1], ex, ey, ez, strike_turn);
        gSPMatrix((*list)++, &pair[1], G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
        gSPDisplayList((*list)++, night_bolt_begin);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (110 * strike_bolt) / 255);
        gSPDisplayList((*list)++, night_bolt_glows[strike_shape]);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, strike_bolt);
        gSPDisplayList((*list)++, night_bolt_cores[strike_shape]);
        gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    }
    gSPDisplayList((*list)++, night_storm_end);
    command(list, (0x64u << 24) | 0x00000Du, 1);
    gfx_stage_close(game, 0);       // in a window of its own: the sky's may be shut by now
}

static void storm_sky(void) {
    if (sky_now != NULL && sky_now->data.dobj != NULL) {
        DObj* eye = sky_now->data.dobj;

        storm_sky_at(&MAIN_GFX_POS[0], eye->position.v.x, eye->position.v.y, eye->position.v.z);
    }
}

// The storm's and the shooting star's lists set their own render mode, for a
// drum's sky, drawn before the land. A roofed course draws its sky after the
// land, under the depth test at the far limit (night_sprites_begin_zb, and
// the depth source the caller sets), so that only the painted sky takes the
// flash, the bolt and the streak and every hill hides them: at such a load
// their modes take the depth test, and a drum's load puts them back.
static u32 sky_mode_orig[3];
static u8 sky_mode_kept;

static void sky_lists_zb(s32 on) {
    Gfx* lists[3];
    s32 i, n;

    lists[0] = night_flash_dome;
    lists[1] = night_bolt_begin;
    lists[2] = ((night_streak[0].words.w0 >> 24) == 0xDE) ? (Gfx*) night_streak[0].words.w1 : NULL;    // its sprite states
    for (i = 0; i < 3; i++) {
        for (n = 0; lists[i] != NULL && n < 16 && lists[i][n].words.w0 != 0xDF000000u; n++) {
            if (lists[i][n].words.w0 == 0xE200001Cu) {      // gsDPSetRenderMode
                if (!sky_mode_kept) {
                    sky_mode_orig[i] = lists[i][n].words.w1;
                }
                lists[i][n].words.w1 = on ? (G_RM_AA_ZB_XLU_SURF | G_RM_AA_ZB_XLU_SURF2) : sky_mode_orig[i];
                break;
            }
        }
    }
    sky_mode_kept = 1;
}

// The key light. By day the game aims its one light from over the viewer's
// shoulder, every frame, from the camera (app_level/player.c,
// func_80357FD4; the photo code does the same for a photo's camera). At
// night the moon is the key: the light leans toward it, the viewer's side
// keeping a share so that a Pokémon between the camera and the moon is not
// a silhouette; under cloud it comes from above; while lightning flashes,
// from the bolt; on the Volcano, from the lava below. Directions are in the
// world's own axes, as the game's are.
static void key_turn(s8* dir) {
    f32 gx = dir[0], gy = dir[1], gz = dir[2];      // the game's own, this update's
    f32 kx, ky, kz, w, len;

    if (storming && strike_flash != 0) {
        kx = night_sine[(strike_turn + 64) & 0xFF];
        ky = 60.0f;
        kz = -(f32) night_sine[strike_turn & 0xFF];
        w = 1.0f;
    } else if (raining || snowing) {
        kx = 0.0f;
        ky = 127.0f;
        kz = 0.0f;
        w = 0.7f;
    } else if (night->embers && !sunset) {
        // The lava's light from below, and toward the camera's side, as the
        // cartridge's own key comes from behind the camera: the faces one
        // sees take it (Nov 7 2026).
        f32 flat = __builtin_sqrtf(gx * gx + gz * gz);

        kx = flat > 1.0f ? gx / flat * 102.0f : 0.0f;
        ky = -76.0f;
        kz = flat > 1.0f ? gz / flat * 102.0f : 0.0f;
        w = 0.75f;
    } else {
        kx = relight_dir[0] * 127.0f;
        ky = relight_dir[1] * 127.0f;
        kz = relight_dir[2] * 127.0f;
        w = sunset ? 0.85f : 0.75f;
    }
    w *= nightness * (1.0f - cave_now);     // by day the game's own light stands, and in a roofed course's cave
    len = __builtin_sqrtf(kx * kx + ky * ky + kz * kz);
    if (len < 1.0f) {
        return;
    }
    kx /= len;
    ky /= len;
    kz /= len;
    len = __builtin_sqrtf(gx * gx + gy * gy + gz * gz);
    if (len < 1.0f) {
        gx = 0.0f;
        gy = 1.0f;
        gz = 0.0f;
    } else {
        gx /= len;
        gy /= len;
        gz /= len;
    }
    kx = kx * w + gx * (1.0f - w);
    ky = ky * w + gy * (1.0f - w);
    kz = kz * w + gz * (1.0f - w);
    len = __builtin_sqrtf(kx * kx + ky * ky + kz * kz);
    if (len < 0.01f) {
        return;
    }
    dir[0] = (s8) (kx * 127.0f / len);
    dir[1] = (s8) (ky * 127.0f / len);
    dir[2] = (s8) (kz * 127.0f / len);
}

// The frame's copy of the record: the colors in force, and the direction the game
// gave this update, leaned as key_turn says. Written again whenever the colors, the
// flash or the lean change while the frame is drawn, all before it goes to the
// renderer; the renderer reads only the last.
static void light_store(void) {
    volatile u8* b = light_block;
    volatile s8* raw = (volatile s8*) (COURSE_LIGHT + 16);
    s8 dir[3];
    s32 i;

    if (!light_block_ok()) {
        return;
    }
    for (i = 0; i < 16; i++) {
        b[i] = light_now[i];
    }
    dir[0] = raw[0];
    dir[1] = raw[1];
    dir[2] = raw[2];
    if (night != NULL && moonlight_on) {
        key_turn(dir);
    }
    b[16] = (u8) dir[0];
    b[17] = (u8) dir[1];
    b[18] = (u8) dir[2];
    for (i = 19; i < 24; i++) {
        b[i] = 0;
    }
}

RECOMP_HOOK("func_800A1A50") void night_light_list_begins(Gfx** gfx) {
    prerender_at = gfx;
    rain_slot_n = storm_slot_n = photo_slot_n = glow_slot_n = streak_slot_n = 0;     // a frame begins: its slots from the first
    gfx_frame_begins();
}

// The frame's first list has just called the light list (rdpReset, at the start of
// every frame; the game calls that start again at each change of microcode). Outside
// Day the call goes to a copy of the light list whose two loads read the frame's copy
// of the record, both in the frame context's own set in the mod's memory; the call's
// place takes the extension's enable (the port gives it only at each camera's matrices,
// later in the frame), and the call follows in a window of the extension's addresses:
// [enable] [window open] [the call] [window shut], 24 bytes more than the cartridge's.
RECOMP_HOOK_RETURN("func_800A1A50") void night_light_list_ends(void) {
    Gfx** at = prerender_at;
    volatile u32* list = (volatile u32*) CAMERA_LIGHTS;
    Gfx* g;
    Gfx* call;
    s32 i;

    prerender_at = NULL;
    light_block = NULL;
    if (at == NULL || night == NULL || nightness <= 0.0f || !light_set) {
        return;         // Day, or no course of the mod's: the call and the record as the cartridge has them
    }
    g = *at;
    call = g - 1;
    if (((u32) g & 7) != 0 || call->words.w0 != 0xDE000000u || call->words.w1 != CAMERA_LIGHTS
        || list[6] != 0xDC08060Au || list[7] != 0x800AEBD8u || list[8] != 0xDC08090Au || list[9] != 0x800AEBD0u
        || list[14] != 0xDF000000u) {
        if (!light_block_said) {
            light_block_said = 1;
            recomp_printf("[island skies] the light list is not the cartridge's here: the frames keep the game's record\n");
        }
        return;
    }
    if (!gfx_may(g, 3)) {
        return;         // not by plan or room this frame: it keeps the game's own record, and no part loads it again
    }
    {
        s32 c = frame_ctx();
        Gfx* copy = light_lists[c];
        Gfx* again = light_reloads[c];

        for (i = 0; i < 8; i++) {
            copy[i].words.w0 = list[i * 2];
            copy[i].words.w1 = list[i * 2 + 1];
        }
        copy[3].words.w1 = (u32) &copy[9];          // the light: the copy's diffuse and its direction
        copy[4].words.w1 = (u32) &copy[8];          // the ambient
        again[0].words.w0 = 0xDC08060Au;
        again[0].words.w1 = (u32) &copy[9];
        again[1].words.w0 = 0xDC08090Au;
        again[1].words.w1 = (u32) &copy[8];
        again[2].words.w0 = 0xDF000000u;
        again[2].words.w1 = 0;
        light_block = (volatile u8*) &copy[8];
        light_block_drawn = GTL_DRAWN;
        // [the extension's enable, in the call's place] [the window open] [the call] [the window shut]
        call->words.w0 = EXT_ENABLE_W0;
        call->words.w1 = EXT_ENABLE_W1;
        g[0].words.w0 = EXT_WINDOW_W0;
        g[0].words.w1 = 1;
        g[1].words.w0 = 0xDE000000u;
        g[1].words.w1 = (u32) copy;
        g[2].words.w0 = EXT_WINDOW_W0;
        g[2].words.w1 = 0;
        *at = &g[3];
    }
    light_store();
}

// The Volcano's sky from below: the lava's glow on the clouds' undersides, the
// box's low corners tinted anew every frame with a slow breath and a quicker
// flicker on top of the night's own tint.
static void lava_sky(void) {
    static const u8 pulse[3] = { 58, 30, 12 };
    u32 ticks = ticks_of(night);
    f32 slow = (f32) night_sine[((ticks * 182) >> 8) & 0xFF] / 127.0f;
    f32 fast = (f32) night_sine[((ticks * 900) >> 8) & 0xFF] / 127.0f;
    f32 glow = 0.5f + 0.35f * slow + 0.15f * fast;
    u8 keep[3];
    s32 k, i;

    for (k = 0; k < 3; k++) {
        s32 v = tint_lo[k] + (s32) (glow * (f32) pulse[k]);

        keep[k] = tint_lo[k];
        tint_lo[k] = (u8) (v > 255 ? 255 : v);
    }
    for (i = 0; i < 2; i++) {
        if (night->sky_vtx[i] != 0) {
            sky_corners(night->sky_vtx[i], night->sky_vtx_n[i], i);
        }
    }
    for (k = 0; k < 3; k++) {
        tint_lo[k] = keep[k];
    }
}

// The sky's own drawing places the weather at the sky object, which rides
// with the camera.
static void rain(GObj* sky) {
    DObj* eye = sky->data.dobj;

    if (eye != NULL) {
        rain_at(&MAIN_GFX_POS[1], eye->position.v.x, eye->position.v.y, eye->position.v.z);
    }
}

static void snow(GObj* sky) {
    DObj* eye = sky->data.dobj;

    if (eye != NULL) {
        snow_at(&MAIN_GFX_POS[1], eye->position.v.x, eye->position.v.y, eye->position.v.z);
    }
}

RECOMP_HOOK("drawSkyBox1Cycle") void night_sky_begins(GObj* obj) {
    sky_now = obj;
    // The last drawn frame's mist, said once a ride past the intro's own camera.
    if (night != NULL && !mist_said_frame && on_the_ride && mist_corners_last > 0 && ticks_of(night) >= 900) {
        mist_said_frame = 1;
        recomp_printf("[island skies] the mist in one frame: %d parts of the record's %d carried it, %u corners\n",
                      (s32) mist_parts_last, (s32) glint_seg_n, mist_corners_last);
    }
    // The mist's books were once kept here, and a model drawn before the sky
    // in a frame lost its mist for that frame; they close with the draw now
    // (mist_draw). Said once if the sky does come after a misty model.
    if (night != NULL && !mist_sky_said && mist_parts_frame > 0) {
        mist_sky_said = 1;
        recomp_printf("[island skies] the sky came after %d misty parts in one frame\n", (s32) mist_parts_frame);
    }
    {
        volatile s8* dir = (volatile s8*) (COURSE_LIGHT + 16);
        f32 fx = -(f32) dir[0], fz = -(f32) dir[2];
        f32 len = __builtin_sqrtf(fx * fx + fz * fz);

        if (len > 8.0f) {
            view_fwd[0] = fx / len;
            view_fwd[1] = fz / len;
        }
    }
    if (night != NULL) {
        if (night->twinkle != NULL) {
            twinkle(night);         // at every hour: the fade makes them dark by day
        }
        aurora_frame();             // the curtains' corners for this frame, or none
        extended_addresses(1);
        if (scoring) {
            return;         // no weather in the pass that measures a photo
        }
        if (storming) {
            strike_at(ticks_of(night));
            storm_light();
            if (on_the_ride) {
                thunder(ticks_of(night));
            }
        }
        light_store();              // the frame's light with this frame's strike, if any
        if (rain_share > 0.0f && obj != NULL) {
            rain(obj);
            if (on_the_ride) {
                rain_sound();
            }
        } else if ((snow_share > 0.0f || ash_share > 0.0f) && obj != NULL) {
            snow(obj);
        }
        if (rain_share <= 0.0f) {
            rain_quiet();           // the rain has ended, or there was none
        }
        if (night->embers && !scoring) {
            lava_sky();
        }
        night_voices();
    }
}

// The year's strong meteor showers, each the night that begins on its date
// (the evening of the peak), and the ticks between its streaks here, by its
// strength: the Perseids and the Geminids a streak every four seconds or so,
// the Lyrids and the Leonids every eight.
typedef struct Shower {
    u8 month, day;
    u16 span;
    const char* name;
} Shower;

static const Shower showers[] = {
    { 1, 3, 240, "the Quadrantids" },
    { 4, 22, 480, "the Lyrids" },
    { 5, 5, 300, "the Eta Aquariids" },
    { 8, 12, 240, "the Perseids" },
    { 10, 21, 420, "the Orionids" },
    { 11, 17, 480, "the Leonids" },
    { 12, 13, 200, "the Geminids" },
};

// The shower whose night this is, by the clock's date and minute: the night
// that begins on the peak's date, from noon that day to noon the next. NULL
// when none.
static const Shower* shower_tonight(u32 date, f32 minute) {
    u32 month = (date >> 8) & 0xFF, day = date & 0xFF;
    u32 i;

    for (i = 0; i < sizeof(showers) / sizeof(showers[0]); i++) {
        const Shower* sh = &showers[i];

        if (month == sh->month && ((day == sh->day && minute >= 720.0f) || (day == (u32) sh->day + 1 && minute < 720.0f))) {
            return sh;
        }
    }
    return NULL;
}

// Every clear night has a meteor shower (his word, Oct 23 2026: "keep it to
// every night has the meteor shower"), drawn as the course's world is built
// for the ride and kept for its photos' worlds (as Random's weather is): a
// radiant of its own in the upper sky, 54 to 76 degrees up, from the clock
// as Random's weather is; under the clock, on the showers' own nights, the
// streaks come at that shower's pace and the details page names it.
static void shower_draw(const Night* by) {
    s32 idx = (s32) (by - nights);

    if (idx < 0 || idx >= 8) {
        shower = 0;
        return;
    }
    if (on_the_ride || !shower_drawn[idx]) {
        u32 t = snap64_local_time();
        u32 date = snap64_local_date();
        u32 roll = t * 2246822519u + date * 3266489917u + (u32) idx * 668265263u;

        roll ^= roll >> 15;
        roll *= 0x2C1B3C6Du;
        roll ^= roll >> 12;
        shower_for[idx] = !sunset && !daytime;
        shower_name = NULL;
        shower_span = 240;
        if (cycle) {
            const Shower* sh = shower_tonight(date, clock_minutes());

            if (sh != NULL) {
                shower_name = sh->name;
                shower_span = sh->span;
            }
        }
        shower_az = (f32) ((roll >> 8) & 0xFF) * (6.2831853f / 256.0f);
        shower_el = 0.94f + (f32) ((roll >> 16) & 0x3F) * 0.006f;
        shower_drawn[idx] = 1;
        if (on_the_ride && shower_for[idx]) {
            recomp_printf("[island skies] a meteor shower tonight%s%s: the radiant %d degrees around, %d up; a streak every %u ticks\n",
                          shower_name != NULL ? ", " : "", shower_name != NULL ? shower_name : "",
                          (s32) (shower_az * 57.29578f), (s32) (shower_el * 57.29578f), shower_span);
        }
    }
    shower = stamped ? (!sunset && !daytime) : shower_for[idx];     // a stamped photo's night has its shower
}


// A shooting star: now and then, in a clear night, a streak crosses the upper
// sky in four fifths of a second -- about fourteen degrees along a great
// circle, down and across, away from the moon -- the soft dot stretched
// into a streak (gen_night.py), white at its head and dark at its tail. The
// half-minutes that have one, where it begins and which way it runs come
// from the world's clock, as the storm's strikes do, so a photo's night
// has the star its instant had. It is drawn with a matrix group of its
// own, so the port moves it smoothly between the game's frames. On a clear
// night (shower_draw) the streaks come every few seconds, all from the
// radiant; the lone star below is what a night without a shower would have.
#define STREAK_SPAN  1800       // ticks: thirty seconds
#define STREAK_ODDS  150        // of 256: how many spans have a star
#define STREAK_TICKS 48         // its life
#define STREAK_ARC   0.25f      // how far it travels, in radians
#define STREAK_DIST  1400.0f    // where the sky's sprites stand (gen_night.py DIST)

static void shooting_star_at(Gfx** list, f32 ex, f32 ey, f32 ez) {
    Mtx* mtx;
    u32 ticks, span, mix, when, age;
    f32 d[3], e[3], m[3], c[3], t[3], n[3];
    f32 az, el, ce, phi, th, cs, sn, len, alpha;
    s32 k;

    if (sunset || raining || snowing || storming || scoring || clear_share < 1.0f) {
        return;
    }
    ticks = ticks_of(night);
    if (shower) {
        // A shower's night: a streak most spans of a few seconds, each
        // starting 11 to 40 degrees from the radiant and running straight
        // away from it along the great circle through both, as a real
        // shower's do; the longer and brighter ones vary a little.
        f32 r[3], u[3], rho, psi, cr, sr, bright;

        span = ticks / shower_span;
        if (span == 0) {
            return;
        }
        mix = (span + 0x3C6EF372u) * 0x9E3779B1u;
        mix ^= mix >> 15;
        mix *= 0x85EBCA77u;
        mix ^= mix >> 13;
        if ((mix & 0xFF) >= 208) {
            return;         // a span in five has none
        }
        when = span * shower_span + ((mix >> 8) % (shower_span - STREAK_TICKS));
        if (ticks < when || ticks >= when + STREAK_TICKS) {
            return;
        }
        age = ticks - when;
        az = shower_az;
        el = shower_el;
        ce = tcos(el);
        r[0] = ce * tcos(az);
        r[1] = tsin(el);
        r[2] = ce * tsin(az);
        if (r[0] * relight_dir[0] + r[1] * relight_dir[1] + r[2] * relight_dir[2] > 0.9f) {
            az += 1.5707963f;       // a quarter turn off the moon
            r[0] = ce * tcos(az);
            r[2] = ce * tsin(az);
        }
        len = __builtin_sqrtf(r[0] * r[0] + r[2] * r[2]);
        if (len < 0.05f) {
            return;
        }
        e[0] = -r[2] / len;
        e[1] = 0.0f;
        e[2] = r[0] / len;
        n[0] = r[1] * e[2] - r[2] * e[1];
        n[1] = r[2] * e[0] - r[0] * e[2];
        n[2] = r[0] * e[1] - r[1] * e[0];
        psi = (f32) ((mix >> 16) & 0xFF) * (6.2831853f / 256.0f);
        rho = 0.2f + (f32) ((mix >> 24) & 0x3F) * (0.5f / 63.0f);
        cs = tcos(psi);
        sn = tsin(psi);
        cr = tcos(rho);
        sr = tsin(rho);
        for (k = 0; k < 3; k++) {
            u[k] = e[k] * cs + n[k] * sn;
            d[k] = r[k] * cr + u[k] * sr;          // where it begins
            m[k] = u[k] * cr - r[k] * sr;          // and its way: straight from the radiant
        }
        bright = 0.6f + (f32) ((mix >> 4) & 0xF) / 37.5f;        // 0.6 to 1
        th = STREAK_ARC * (0.7f + (f32) ((mix >> 12) & 0xF) / 30.0f) * ((f32) age / (f32) STREAK_TICKS) - 0.054f;
        alpha = ((age < 6) ? (f32) age / 6.0f : (age < 30) ? 1.0f : (f32) (STREAK_TICKS - age) / 18.0f) * bright;
        if (streak_logged != when) {
            streak_logged = when;
        }
        goto place;
    }
    span = ticks / STREAK_SPAN;
    if (span == 0) {
        return;         // none in the ride's first half-minute
    }
    mix = span * 0x9E3779B1u;
    mix ^= mix >> 15;
    mix *= 0x85EBCA77u;
    mix ^= mix >> 13;
    if ((mix & 0xFF) >= STREAK_ODDS) {
        return;
    }
    when = span * STREAK_SPAN + ((mix >> 8) % (STREAK_SPAN - STREAK_TICKS));
    if (ticks < when || ticks >= when + STREAK_TICKS) {
        return;
    }
    age = ticks - when;
    if (streak_logged != when) {
        streak_logged = when;
        recomp_printf("[island skies] a shooting star at tick %u\n", when);
    }
    // Where it begins: the upper sky, 26 to 70 degrees up, away from the moon.
    az = (f32) ((mix >> 16) & 0xFF) * (6.2831853f / 256.0f);
    el = 0.45f + (f32) ((mix >> 24) & 0x3F) * 0.012f;
    ce = tcos(el);
    d[0] = ce * tcos(az);
    d[1] = tsin(el);
    d[2] = ce * tsin(az);
    if (d[0] * relight_dir[0] + d[1] * relight_dir[1] + d[2] * relight_dir[2] > 0.96f) {
        az += 1.5707963f;       // a quarter turn off the moon
        d[0] = ce * tcos(az);
        d[2] = ce * tsin(az);
    }
    // Which way: along the dome, down and across. e runs level around it, n down it.
    len = __builtin_sqrtf(d[0] * d[0] + d[2] * d[2]);
    if (len < 0.05f) {
        return;
    }
    e[0] = -d[2] / len;
    e[1] = 0.0f;
    e[2] = d[0] / len;
    n[0] = d[1] * e[2] - d[2] * e[1];
    n[1] = d[2] * e[0] - d[0] * e[2];
    n[2] = d[0] * e[1] - d[1] * e[0];
    phi = 0.35f + (f32) ((mix >> 4) & 0xFF) * (2.44f / 255.0f);     // 20 to 160 degrees from level: always downward
    cs = tcos(phi);
    sn = tsin(phi);
    for (k = 0; k < 3; k++) {
        m[k] = e[k] * cs + n[k] * sn;
    }
    // The streak's center along the great circle (the head leads it by half the
    // streak's length) and its heading there; small angles, so a short series.
    th = STREAK_ARC * ((f32) age / (f32) STREAK_TICKS) - 0.054f;
    alpha = (age < 6) ? (f32) age / 6.0f : (age < 30) ? 1.0f : (f32) (STREAK_TICKS - age) / 18.0f;
place:
    cs = 1.0f - th * th * 0.5f + th * th * th * th / 24.0f;
    sn = th - th * th * th / 6.0f;
    for (k = 0; k < 3; k++) {
        c[k] = d[k] * cs + m[k] * sn;
        t[k] = m[k] * cs - d[k] * sn;
    }
    n[0] = c[1] * t[2] - c[2] * t[1];
    n[1] = c[2] * t[0] - c[0] * t[2];
    n[2] = c[0] * t[1] - c[1] * t[0];
    mtx = &streak_mtx[frame_slot(&streak_slot_n, FRAME_SLOTS)];
    place_frame(mtx, ex + c[0] * STREAK_DIST, ey + c[1] * STREAK_DIST, ez + c[2] * STREAK_DIST, t, n, c);
    {
        Gfx** game = list;

        list = gfx_stage_open(game);        // an extra of its own, called in a window of its own
        // Its own matrix group, as the bolt's: pushed, not interpolated (a group
        // that asked the port to interpolate it stopped the sky's frames).
        command(list, (0x64u << 24) | 0x00000Cu, 0);
        command(list, 0x00000001u, 0);
        gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
        gSPDisplayList((*list)++, night_rain_begin);     // the state the stars have inside the sky's list
        gDPSetPrimColor((*list)++, 0, 0, 230, 240, 255, (u8) (210.0f * alpha));
        gSPDisplayList((*list)++, night_streak);
        gSPDisplayList((*list)++, night_rain_end);
        gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
        command(list, (0x64u << 24) | 0x00000Du, 1);
        gfx_stage_close(game, 0);
    }
}

static void shooting_star(void) {
    if (sky_now != NULL && sky_now->data.dobj != NULL) {
        DObj* eye = sky_now->data.dobj;

        shooting_star_at(&MAIN_GFX_POS[0], eye->position.v.x, eye->position.v.y, eye->position.v.z);
    }
}

// An aurora: on some clear nights (aurora_draw) two curtains of light stand
// low in the sky a quarter turn around from where the sun sets, the pole's
// side (the other side south of the equator under Live). Each is a band of
// light, bright at a foot that arcs and folds along the curtain, with a soft
// rose glow under its brightest stretches and broad rays rising from it,
// fading up through green to a violet top. The rays are a small picture
// (aurora_pic: 256 by 4, light only, drawn once from nine broad soft streaks
// of their own widths, heights and strengths over a floor that is strongest
// at the foot), laid along each curtain two and a half times over; two waves
// of light, thirty and fifty degrees long, travel along the curtain and the
// rays brighten and fade as they pass; a faint shimmer; the whole breathes
// over forty seconds.
// HOW IT MOVES. The port's renderer draws the display's frames between the
// game's by gliding what it can pair between two of the game's frames: a
// matrix group it knows, and a picture's tile while the tile scrolls at a
// steady pace (the River's and the Valley's water; render_patch.c). It
// never glides corners, picture coordinates or the corners' colors (the
// sky's group leaves its corners to the game). So the rays slide by the
// TILE: each layer's tile is moved a quarter of a texel every frame of the
// game, and the renderer carries it between frames at the display's rate,
// across the picture's repeat too, while each frame's step goes the same
// way as the last and 0.4 to 2.5 times as far; a frame that turns back is
// drawn where the game put it, and one that stands still, it and the one
// after (rt64_game_frame.cpp). All of it is counted from the world's
// clock, which starts over at 10000 of its units (block.c), every seven
// minutes on the Beach and the Volcano. So that nothing jumps there, the
// light's waves, the breath and the shimmer each turn a whole number of
// times in the clock's round (aurora_turns), and each curtain's tile, which
// no steady slide brings back to its start in a round, goes one way for
// half the round and back for the other half, the two curtains turning a
// quarter round apart (aurora_slide). The curtains' corners stand still: a
// corner moves only by whole units, a twenty-fifth of a degree, and a slow
// drift moves it one unit at a time at the game's rate. The light's waves
// live in the corners' alpha and change it by under a hundredth of its full
// light a frame. The whole is drawn within the sky's own list, after the
// stars and the moon (the sky's spare slot runs aurora_hook: the sky's
// list, then the curtains'), under the drum's own matrix group, which the
// port glides as it does the stars.
// One list and one set of corners, rewritten in place each frame as the
// stars' colors are: what changes in a frame is too small to show if a
// frame reads half of it. (A group of the mod's own drawn after the sky,
// as the shooting star is, moved only at the game's frame rate and
// stuttered as the view turned: JackandBeans, Oct 23 2026. The first cut,
// broad soft swells laid over the sky, read as a green haze: the same
// night. The third drew the rays from the strips alone, two degrees wide,
// with waves a few degrees long driving each strip's height and light;
// they jumped from strip to strip and read as a choppy wave. The fourth
// slid the picture by the corners' coordinates, which the port does not
// glide, and moved at the game's rate: JackandBeans, Oct 24 2026. The
// fifth slid the tile by ticks_of's whole ticks, drifted the corners and
// shimmered twelve in a hundred every second and a half, and was still a
// little choppy: JackandBeans, Nov 1 2026. The seventh drew its rays six
// times finer than the game's own sky pictures (64 by 32, six around the
// sky, about a texel a degree) on strips two degrees wide, 1,152 triangles
// a frame where the Beach's sky drum has 28; JackandBeans asked whether
// that suited an N64 game and this one, the same night. The eighth's thin
// rose fringe ran the whole foot, and its strips, six degrees wide, drew a
// fold twenty-seven degrees long in sharp corners, which the fringe traced
// like an outline: JackandBeans's picture, Nov 4 2026.) They are drawn as
// the N64 drew such things: strips of colored corners under a tiny picture,
// their light ADDED to the sky (the horizon glow's blend, night_glow_begin),
// so the stars show through undimmed. The corners are rewritten every frame
// from the world's clock, so a photo's night has the curtains its instant
// had. They come up and go with the stars (star_fade), and a cloudy sky
// hides them.
#define AURORA_LAYERS 2
#define AURORA_COLS   17
#define AURORA_ROWS   7
#define AURORA_DIST   1400.0f
#define AURORA_ALPHA  255.0f
#define AURORA_PIC_W  256
#define AURORA_PIC_H  4
#define AURORA_RAYS   9
#define AURORA_GFX    (32 + AURORA_LAYERS * (AURORA_COLS - 1) * AURORA_ROWS)
// The glow's adding blend (gen_night.py's RM_GLOW) without its depth test,
// as the sky's drum is drawn: what is drawn brightens what is behind it by
// its own alpha and hides nothing.
#define AURORA_RM(clk) (IM_RD | CVG_DST_FULL | ZMODE_XLU | FORCE_BL | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1))

typedef struct AuroraLayer {
    f32 turn;                   // its middle, radians around from the pole's side
    f32 half;                   // half its width, radians
    f32 foot;                   // the elevation of its foot, radians
    f32 rows[AURORA_ROWS];      // each row's height over the foot, radians, at full height (the first below it)
    f32 strength;               // its alpha against the main curtain's
    s32 pic_col;                // the picture along the curtain: 32nds of a texel a column
    s32 slide;                  // which way its tile slides first, a quarter of a texel a frame: 1 or -1 (aurora_slide)
    s32 late;                   // how far into the clock's round (of 10000) its own starts: it turns there and half a round on
    u32 phase[4];               // where its waves start: the light's, the folds', the height's and the shimmer's, the fixed warp's
} AuroraLayer;

static const AuroraLayer aurora_layers[AURORA_LAYERS] = {
    // The main curtain: 96 degrees wide, its foot eight up, 25 tall at full height; the picture forty degrees long.
    { 0.0f, 0.8378f, 0.1396f, { -0.04f, 0.0f, 0.0262f, 0.0873f, 0.1745f, 0.2967f, 0.4363f }, 1.0f, 1230, 1, 0,
      { 0x0000u, 0x4000u, 0x8000u, 0xC000u } },
    // A fainter curtain behind it, higher, a little around, with folds of its own; its picture slides the other way, turning a quarter round later.
    { 0.3142f, 0.6109f, 0.2443f, { -0.04f, 0.0f, 0.0349f, 0.1047f, 0.2007f, 0.3316f, 0.4712f }, 0.6f, 1098, -1, 2500,
      { 0x2A00u, 0xE000u, 0x5000u, 0x9800u } },
};
static const u8 aurora_row_w[AURORA_ROWS] = { 35, 255, 235, 160, 95, 40, 0 };     // the alpha by row: the glow under the foot at its fullest (aurora_state), a bright foot, fading up
static const u8 aurora_row_color[AURORA_ROWS][3] = {
    { 255, 70, 130 }, { 210, 255, 215 }, { 120, 255, 140 }, { 70, 235, 150 }, { 110, 150, 210 }, { 150, 80, 220 }, { 160, 60, 200 },
};
static const s16 aurora_row_t[AURORA_ROWS] = { 0, 0, 16, 40, 64, 88, 112 };     // each row's line of the picture, 32nds of a texel (the fringe shares the foot's)
static u8 aurora_pic[AURORA_PIC_H][AURORA_PIC_W] __attribute__((aligned(16)));
static Vtx aurora_vtx[AURORA_LAYERS][AURORA_COLS * AURORA_ROWS];
static Gfx aurora_list[AURORA_GFX];
static Gfx* aurora_tile[AURORA_LAYERS];     // each layer's tile command in the list, rewritten each frame
static Gfx aurora_none[] = { gsSPEndDisplayList() };
// The sky's own list, then the curtains, then the window that the spare pair opened shut again (spare_point).
static Gfx aurora_hook[] = { gsSPDisplayList(aurora_none), gsSPDisplayList(aurora_none), {{ EXT_WINDOW_W0, 0 }}, gsSPEndDisplayList() };
static u8 aurora_lists_built;
static f32 aurora_pole[2];       // the pole's side, around: x and z
static u32 aurora_seed;

// Where the curtains stand for this course: a quarter turn around from the
// sunset's sun (which shares the moon's side), the other way south of the
// equator under Live.
static void aurora_place(const Night* by) {
    f32 sx = (f32) by->sun[0], sz = (f32) by->sun[2];
    f32 len = __builtin_sqrtf(sx * sx + sz * sz);
    s32 lat = (setup_of() == 1) ? sun_latitude() : 999;
    f32 side = (lat != 999 && lat < 0) ? -1.0f : 1.0f;

    if (len < 1.0f) {
        sx = 1.0f;
        sz = 0.0f;
        len = 1.0f;
    }
    aurora_pole[0] = -(sz / len) * side;
    aurora_pole[1] = (sx / len) * side;
}

// The next of a fixed run of numbers, 0 to 1: the rays' places and shapes
// are the same every night; the nights differ by their waves and slide.
static f32 aurora_roll(void) {
    aurora_seed = aurora_seed * 1664525u + 1013904223u;
    return (f32) (aurora_seed >> 8) / 16777216.0f;
}

// The picture: light only, 256 texels along the curtain (it repeats) by 4
// up it (the foot at the first line). Nine soft streaks, one to each
// stretch of twenty-eight texels, of their own half-widths (six texels to
// twenty), strengths and heights (the shorter ones fade out partway up, the
// tallest reach the top), over a floor that is strongest at the foot, so
// the lower border is a continuous band of light and the rays stand apart
// above it. Its detail is about that of the game's own sky pictures; it
// keeps 256 texels so that its slide, a quarter of a texel a frame, stays
// slow. A texel holds the square root of its light: the picture multiplies
// both the color and the alpha, and the adding blend multiplies those
// again.
static void aurora_pic_build(void) {
    f32 at[AURORA_RAYS], wide[AURORA_RAYS], strong[AURORA_RAYS], high[AURORA_RAYS];
    s32 k, x, r;

    aurora_seed = 0x5EED1234u;
    for (k = 0; k < AURORA_RAYS; k++) {
        at[k] = ((f32) k + 0.15f + 0.7f * aurora_roll()) * ((f32) AURORA_PIC_W / (f32) AURORA_RAYS);
        wide[k] = 6.4f + 13.6f * aurora_roll();
        strong[k] = 0.35f + 0.65f * aurora_roll();
        high[k] = 0.55f + 0.8f * aurora_roll();
    }
    for (r = 0; r < AURORA_PIC_H; r++) {
        f32 v = (f32) r / (f32) (AURORA_PIC_H - 1);
        f32 floor = 0.9f - 0.8f * v;

        for (x = 0; x < AURORA_PIC_W; x++) {
            f32 rays = 0.0f, light;

            for (k = 0; k < AURORA_RAYS; k++) {
                f32 d = (f32) x - at[k];
                f32 g;

                if (d > (f32) (AURORA_PIC_W / 2)) {
                    d -= (f32) AURORA_PIC_W;
                }
                if (d < -(f32) (AURORA_PIC_W / 2)) {
                    d += (f32) AURORA_PIC_W;
                }
                if (d < 0.0f) {
                    d = -d;
                }
                if (d >= wide[k]) {
                    continue;
                }
                g = (v - high[k] + 0.35f) / 0.35f;      // 0 below where the ray fades, 1 past it
                g = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
                rays += strong[k] * (0.5f + 0.5f * sky_cos(3.14159265f * d / wide[k])) * (1.0f - g * g * (3.0f - 2.0f * g));
            }
            if (rays > 1.0f) {
                rays = 1.0f;
            }
            light = floor + (1.0f - floor) * rays;
            aurora_pic[r][x] = (u8) (255.0f * __builtin_sqrtf(light) + 0.5f);
        }
    }
}

// The list: the strips' states (the picture, loaded as the night's own
// pictures are; the sky's own combiner, picture times shade; the horizon
// glow's adding blend), then each layer's tile (its slide, set each frame)
// and its columns as pairs of rows, then the sky's own states back, as the
// generator's night_sprites_end puts them.
static void aurora_lists_build(void) {
    Gfx* g = aurora_list;
    s32 layer, c, r;

    aurora_pic_build();
    gDPPipeSync(g++);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gDPSetTextureFilter(g++, G_TF_BILERP);
    gDPLoadTextureBlock(g++, aurora_pic, G_IM_FMT_I, G_IM_SIZ_8b, AURORA_PIC_W, AURORA_PIC_H, 0,
                        G_TX_WRAP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, 8, 2, G_TX_NOLOD, G_TX_NOLOD);
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPSetCombineMode(g++, G_CC_MODULATEIA, G_CC_MODULATEIA);
    gDPSetRenderMode(g++, AURORA_RM(1), AURORA_RM(2));
    gSPClearGeometryMode(g++, G_CULL_BACK | G_CULL_FRONT);
    for (layer = 0; layer < AURORA_LAYERS; layer++) {
        gDPPipeSync(g++);
        aurora_tile[layer] = g;
        gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, (AURORA_PIC_W - 1) << 2, (AURORA_PIC_H - 1) << 2);
        for (c = 0; c < AURORA_COLS - 1; c++) {
            gSPVertex(g++, &aurora_vtx[layer][c * AURORA_ROWS], 2 * AURORA_ROWS, 0);
            for (r = 0; r < AURORA_ROWS - 1; r++) {
                gSP2Triangles(g++, r, r + 1, AURORA_ROWS + r + 1, 0, r, AURORA_ROWS + r + 1, AURORA_ROWS + r, 0);
            }
        }
    }
    gDPPipeSync(g++);
    gDPSetRenderMode(g++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
    g->words.w0 = 0xFC121824u;          // the sky's combiner: picture times shade
    g->words.w1 = 0xFF33FFFFu;
    g++;
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gSPSetGeometryMode(g++, G_CULL_BACK);
    gSPEndDisplayList(g++);
    aurora_lists_built = 1;
}

// A wave: the sine of a phase in 65536ths of a turn, smoothly (the night's
// table has 256 steps, which would show as the waves crawl).
static f32 aurora_wave(u32 phase) {
    return sky_sin((f32) (phase & 0xFFFFu) * 9.58738e-5f);
}

// A wave's phase now, 65536ths of a turn, for a wave of `rate` 65536ths of
// a turn a tick: made a whole number of turns in the clock's round (10000
// of its units), the nearest to that pace and at least one, so the wave
// runs on unbroken when the clock starts over.
static u32 aurora_turns(f32 clock, f32 speed, u32 rate) {
    u32 n = (u32) ((f32) rate * 10000.0f / (65536.0f * speed) + 0.5f);     // the round's ticks over the wave's

    if (n < 1) {
        n = 1;
    }
    return (u32) ((f32) n * 6.5536f * clock);       // n turns over the round's 10000
}

// Where a curtain's tile stands, quarter texels within one repeat. No
// steady slide comes back to its start in the clock's round (12500 frames
// on the Beach is no whole number of the repeat's 1024 steps), so the tile
// goes one way for half the round and back for the other half: the
// distance from the nearer end of the curtain's round, which starts `late`
// into the clock's, in whole frames, a quarter of a texel each. It never
// jumps when the clock starts over, and at a turn it stands still for a
// frame or turns straight back. It counts from half a repeat in, so a turn
// never crosses the picture's repeat, where the renderer would take the
// turn for a wrap and draw it a little off.
static u32 aurora_slide(f32 clock, f32 speed, const AuroraLayer* lay) {
    f32 e = clock - (f32) lay->late;
    u32 frames;

    if (e < 0.0f) {
        e += 10000.0f;
    }
    if (e > 5000.0f) {
        e = 10000.0f - e;
    }
    frames = (u32) (e / speed + 0.5f) >> 1;         // whole ticks, two a frame
    return (u32) (AURORA_PIC_W * 2 + lay->slide * (s32) frames) & (AURORA_PIC_W * 4 - 1);
}

// The corners for this frame, from the world's clock: each layer's tile at
// its place on the curtain's back and forth (aurora_slide); each column's
// place around; a fixed slow warp along the curtain, which keeps the
// picture's repeats from matching; its light from two waves along the
// curtain, thirty and fifty degrees long, traveling against each other,
// raised so the bright stretches stand out, and a faint shimmer, five in a
// hundred over two and a half seconds, each a whole number of turns in the
// clock's round, as the breath is (aurora_turns); the foot's arc, highest
// at its middle, and its folds (two waves, two degrees and one deep) and
// the height, all held still; and from them each row's corner on the dome,
// its line of the picture, its color and its alpha. The first row, under
// the foot, takes its color and alpha from the light above it: rose under
// the brightest stretches, elsewhere fading away in the green above the
// foot, so that no line runs along the foot.
static void aurora_state(const Night* by) {
    f32 clock = world_clock();
    f32 speed = clock_speed(by);
    f32 breath = 0.9f + 0.1f * aurora_wave(aurora_turns(clock, speed, 27u) + 0x3000u);     // over forty seconds
    u32 wave1 = aurora_turns(clock, speed, 55u);        // the light's waves: twenty seconds a wave,
    u32 wave2 = aurora_turns(clock, speed, 33u);        // and thirty-three
    u32 shine = aurora_turns(clock, speed, 455u);       // the shimmer: two and a half seconds
    s32 layer, c, r, k;

    for (layer = 0; layer < AURORA_LAYERS; layer++) {
        const AuroraLayer* lay = &aurora_layers[layer];
        f32 scale = AURORA_ALPHA * lay->strength * breath * ((f32) star_fade / 255.0f);
        u32 uls = aurora_slide(clock, speed, lay);      // quarter texels, within one repeat

        gDPSetTileSize(aurora_tile[layer], G_TX_RENDERTILE, uls, 0, uls + ((AURORA_PIC_W - 1) << 2), (AURORA_PIC_H - 1) << 2);
        for (c = 0; c < AURORA_COLS; c++) {
            f32 u = (f32) (2 * c - (AURORA_COLS - 1)) / (f32) (AURORA_COLS - 1);     // -1 at one end, 1 at the other
            f32 a = lay->turn + lay->half * u;
            f32 ca = sky_cos(a), sa = sky_sin(a);
            f32 dx = aurora_pole[0] * ca - aurora_pole[1] * sa;
            f32 dz = aurora_pole[0] * sa + aurora_pole[1] * ca;
            u32 cu = (u32) c;
            // The waves' steps are by the column, six degrees on the main curtain.
            f32 b1 = aurora_wave(lay->phase[0] + cu * 0x3000u - wave1);                // thirty degrees long, twenty seconds a wave
            f32 b2 = aurora_wave(lay->phase[0] + 0x6000u + cu * 0x1BF0u + wave2);      // fifty degrees, thirty-three seconds, the other way
            f32 f1 = aurora_wave(lay->phase[1] + cu * 0x1800u);                        // the folds: sixty degrees long, held still
            f32 f2 = aurora_wave(lay->phase[1] + 0x4000u + cu * 0x2000u);              // and forty-eight, long enough for the strips to bend gently
            f32 h1 = aurora_wave(lay->phase[2] + cu * 0x2001u);                        // the height: forty-eight degrees long, held still
            f32 shimmer = aurora_wave(lay->phase[2] + 0x8000u + cu * 0x0FFFu + shine);       // two and a half seconds
            f32 warp = aurora_wave(lay->phase[3] + cu * 0x1800u);                       // fixed
            f32 mix = 0.5f + 0.5f * (0.6f * b1 + 0.4f * b2);
            f32 bright = 0.5f + 0.5f * mix * __builtin_sqrtf(mix);
            f32 rose = mix * mix;                                            // how rose and how strong the glow under the foot is here
            f32 edge = 1.3f * (0.5f + 0.5f * sky_cos(3.14159265f * u));      // the curtain thins to its ends
            f32 tall = 0.85f + 0.25f * h1;
            f32 foot = lay->foot + 0.03f * (1.0f - u * u) + 0.035f * f1 + 0.02f * f2;     // an arc, highest at its middle, with the folds on it
            f32 alpha;
            s32 s = (s32) ((f32) lay->pic_col * ((f32) c + 0.4f * warp));      // the warp: two and a half degrees either way

            edge = edge > 1.0f ? 1.0f : edge;
            alpha = scale * edge * bright * (1.0f + 0.05f * shimmer);
            for (r = 0; r < AURORA_ROWS; r++) {
                Vtx* p = &aurora_vtx[layer][c * AURORA_ROWS + r];
                f32 el = foot + lay->rows[r] * tall;
                f32 ce = sky_cos(el);
                f32 al = alpha * (f32) aurora_row_w[r] / 255.0f;

                p->v.ob[0] = (s16) (AURORA_DIST * ce * dx);
                p->v.ob[1] = (s16) (AURORA_DIST * sky_sin(el));
                p->v.ob[2] = (s16) (AURORA_DIST * ce * dz);
                p->v.flag = 0;
                p->v.tc[0] = (s16) s;
                p->v.tc[1] = aurora_row_t[r];
                for (k = 0; k < 3; k++) {
                    p->v.cn[k] = aurora_row_color[r][k];
                }
                if (r == 0) {
                    for (k = 0; k < 3; k++) {
                        p->v.cn[k] = (u8) ((f32) aurora_row_color[2][k] + ((f32) aurora_row_color[0][k] - (f32) aurora_row_color[2][k]) * rose);
                    }
                    al *= rose;
                }
                p->v.cn[3] = (u8) (al > 255.0f ? 255.0f : al);
            }
        }
    }
}

// Each frame, before the sky is drawn: the curtains' corners and tiles for
// the frame, and the hook's second command pointed at their list, or at
// nothing when the night has no aurora, the hour or the weather hides it,
// or the game is measuring a photo.
static void aurora_frame(void) {
    Gfx* show = aurora_none;

    if (night != NULL && aurora && !(sunset && !cycle) && !raining && !snowing && !storming && !scoring && star_fade != 0) {
        if (!aurora_lists_built) {
            aurora_lists_build();
        }
        aurora_state(night);
        show = aurora_list;
    }
    gSPDisplayList(&aurora_hook[1], show);
}

// ---- The sky's tail, each frame its own ----
// After the drum the sky goes on in the game's own list, whose spare slot calls the
// hook: the night's stars and moon (or the sunset's or the dawn's list), then the
// curtains. The stars' colors (twinkle) and the curtains' corners and tiles
// (aurora_state) change every frame in lists the mod keeps once, and the renderer
// read them on its own thread while the game wrote the next frame's. So as each
// frame's sky is drawn, its context takes copies of that tail, of the hook, of the
// stars' list with this frame's colors and of the curtains' list with this frame's
// corners, and the frame's drum branches to them (Nov 8 2026). Day draws the
// port's drum and the cartridge's own list, as before.
#define SKY_TAIL  256
#define SKY_STARS 2048
static Gfx sky_tail[FRAME_CTX][SKY_TAIL];
static Gfx sky_hook[FRAME_CTX][3];
static Gfx sky_stars[FRAME_CTX][SKY_STARS];
static Gfx sky_twinkle[FRAME_CTX][NIGHT_GROUPS_MAX + 1][2];
static Gfx aurora_list_f[FRAME_CTX][AURORA_GFX];
static Vtx aurora_vtx_f[FRAME_CTX][AURORA_LAYERS][AURORA_COLS * AURORA_ROWS];
static u8 sky_tail_said, sky_stars_said;

// The night's stars list as this frame has it: its calls of the brightness commands
// pointed at the frame's own copies of them. Any other list is given back as it is.
static Gfx* sky_stars_snap(s32 c, Gfx* list) {
    const Night* by = night;
    u32 tw0, tw1;
    s32 n, i;

    if (by == NULL || list == NULL || list != by->sky || by->twinkle == NULL || by->twinkles > NIGHT_GROUPS_MAX) {
        return list;
    }
    tw0 = (u32) &by->twinkle[0][0];
    tw1 = (u32) &by->twinkle[by->twinkles + 1][0];        // past the bright stars' steady hearts
    for (i = 0; i <= by->twinkles; i++) {
        sky_twinkle[c][i][0] = by->twinkle[i][0];
        sky_twinkle[c][i][1] = by->twinkle[i][1];
    }
    for (n = 0; n < SKY_STARS; n++) {
        u32 w0 = list[n].words.w0, w1 = list[n].words.w1;

        if (w0 == 0xDE000000u && w1 >= tw0 && w1 < tw1 && ((w1 - tw0) & 15) == 0) {
            w1 = (u32) &sky_twinkle[c][(w1 - tw0) >> 4][0];
        }
        sky_stars[c][n].words.w0 = w0;
        sky_stars[c][n].words.w1 = w1;
        if ((w0 >> 24) == 0xDF) {
            return &sky_stars[c][0];
        }
    }
    if (!sky_stars_said) {
        sky_stars_said = 1;
        recomp_printf("[island skies] the stars' list is longer than %d commands: its shared copy is drawn\n", SKY_STARS);
    }
    return list;
}

// The curtains' list as this frame has it, its corner loads pointed at the frame's corners.
static Gfx* aurora_snap(s32 c) {
    u32 v0 = (u32) &aurora_vtx[0][0], v1 = v0 + (u32) sizeof(aurora_vtx);
    s32 n, l, i;

    for (l = 0; l < AURORA_LAYERS; l++) {
        for (i = 0; i < AURORA_COLS * AURORA_ROWS; i++) {
            aurora_vtx_f[c][l][i] = aurora_vtx[l][i];
        }
    }
    for (n = 0; n < AURORA_GFX; n++) {
        u32 w0 = aurora_list[n].words.w0, w1 = aurora_list[n].words.w1;

        if ((w0 >> 24) == 0x01 && w1 >= v0 && w1 < v1) {
            w1 = (u32) &aurora_vtx_f[c][0][0] + (w1 - v0);
        }
        aurora_list_f[c][n].words.w0 = w0;
        aurora_list_f[c][n].words.w1 = w1;
        if ((w0 >> 24) == 0xDF) {
            return &aurora_list_f[c][0];
        }
    }
    return aurora_list;
}

// The hook as this frame has it: the list in force and the curtains, each the frame's copy. It is reached only
// from the frame's copy of the tail, inside the sky's window, so it leaves the window as it is.
static Gfx* sky_hook_for(s32 c) {
    static const Gfx end = gsSPEndDisplayList();

    sky_hook[c][0] = aurora_hook[0];
    sky_hook[c][1] = aurora_hook[1];
    sky_hook[c][2] = end;
    if (night != NULL && aurora_hook[0].words.w1 == (u32) night->sky) {
        sky_hook[c][0].words.w1 = (u32) sky_stars_snap(c, night->sky);
    }
    if (aurora_hook[1].words.w1 == (u32) aurora_list) {
        sky_hook[c][1].words.w1 = (u32) aurora_snap(c);
    }
    return &sky_hook[c][0];
}

// The game's own sky list after the drum, as this frame has it, its spare slot calling
// the frame's hook. Where it cannot be copied whole the shared list is branched to.
static u32 sky_tail_for(s32 c) {
    volatile u32* src = (volatile u32*) fine_rest;
    u32 spare = (night != NULL) ? night->spare : 0;
    s32 n;

    if (fine_rest == 0 || (fine_rest & 7) != 0) {
        return fine_rest;
    }
    for (n = 0; n < SKY_TAIL; n++) {
        u32 at = fine_rest + (u32) n * 8u;
        u32 w0 = src[n * 2], w1 = src[n * 2 + 1];

        if (at == spare && w0 == 0xDE000000u && w1 == (u32) aurora_hook) {
            w1 = (u32) sky_hook_for(c);
        }
        sky_tail[c][n].words.w0 = w0;
        sky_tail[c][n].words.w1 = w1;
        if ((w0 >> 24) == 0xDF) {
            return (u32) &sky_tail[c][0];
        }
    }
    if (!sky_tail_said) {
        sky_tail_said = 1;
        recomp_printf("[island skies] the sky's own list is longer than %d commands: its shared copy is drawn\n", SKY_TAIL);
    }
    return fine_rest;
}

RECOMP_HOOK_RETURN("drawSkyBox1Cycle") void night_sky_ends(void) {
    if (night != NULL) {
        if (sky_repaint_due) {
            // The port has just dissolved each picture once, at this load's
            // first draw and after the mod's paint, each from its own clearest
            // stretch (patches/src/sky_patch.c), and the Valley's two no longer
            // met beside the drum's corners at an hour that is not painted
            // again. The masks have that dissolve already, so the paint is laid
            // over it, before the renderer reads the pictures for this frame.
            sky_repaint_due = 0;
            if (nightness > 0.0f) {
                recomp_printf("[island skies] the sky's pictures painted again: begun\n");
                sky_paint(night->pics[0], sky_mask_pic[0], 0);
                sky_paint(night->pics[1], sky_mask_pic[1], 1);
                recomp_printf("[island skies] the sky's pictures painted again: done\n");
            }
        }
        if (storming && !scoring) {
            storm_sky();            // each in a window of its own (gfx_stage_close)
        }
        if (night->sky != NULL) {
            shooting_star();
        }
        if (ext_open) {
            sky_fine_draw();        // without the window the drum stays the game's own call, whose
        }                           // spare pair runs the hook in a window of its own (spare_point)
        extended_addresses(0);
    }
}

// The game's effects -- splashes, dust, smoke, spores, sparks, fire -- are
// drawn by one function, fx_draw, from each particle's own color, set as a
// primitive color (for some an environment color too) in the frame's command
// lists. Under the night those colors take the light that falls on bodies, as
// the splashes, the dust the Zero-One kicks up and the River's mist do; a
// color that is a fire's -- strongly orange or red, as lava and flames are --
// is a light in itself and keeps its day color, and so does the environment
// color that follows it.
// The light is given at the source, not to the commands: each particle's
// own colors (app_render/effect.h: primColor at 0x48, envColor at 0x50) are
// lit before the frame's cameras draw and put back after they have, so the
// game's own fades go on from the colors it keeps. (Rewriting the drawn
// commands afterward made the effects glitch as the view turned:
// JackandBeans, Oct 6 2026.)
// The hooks sit on omDrawAll, which runs every camera's drawing, and NOT on
// fx_draw. A hook on a game function makes the port's mod loader recompile
// that function afresh from the cartridge, and the port's own fx_draw carries
// additions the fresh copy lacks: the name it gives each particle's
// rectangle, which is how the effects move at the display's rate instead of
// the game's. With a hook on fx_draw, whatever the hook did and whether this
// option was on or off, every effect stepped as the view turned (JackandBeans,
// Oct 6 2026). Nothing runs between omDrawAll's start and its end but the
// drawing, so no particle is born or dies while its colors are the night's.
#define FX_LISTS     ((volatile u32*) 0x800BE1A8u)   // the drawer's sixteen particle lists
#define FX_PARTICLES 512
static u32 fx_lit_at[FX_PARTICLES];       // the particles lit this frame,
static u32 fx_prim[FX_PARTICLES];         // and their colors as they were
static u32 fx_env[FX_PARTICLES];
static u32 fx_lit_n;

static s32 fire_color(u32 word) {
    u32 r = word >> 24;
    u32 g = (word >> 16) & 0xFF;
    u32 b = (word >> 8) & 0xFF;

    return r >= 232 && g <= 176 && b <= 96;
}


RECOMP_HOOK("omDrawAll") void night_cameras_begin(void) {
    s32 list;

    gfx_frame_begins();         // a scene without the light list's call begins its frame here
    fx_lit_n = 0;
    status_tick();
    cycle_tick();
    roof_tick();
    roof_outside = 0;
    roof_volume = 0;
    roof_placed = 0;
    if (night != NULL && snow_cover && !snow_relit && flow_y_n > 0 && !scoring && ticks_of(night) > 120) {
        // The water's height is known now: the record lit again once, so
        // the surf line's sand is bare where it was lit before any water was.
        snow_relit = 1;
        relight_record();
    }
    if (night == NULL || scoring || !fx_lit) {
        return;
    }
    for (list = 0; list < 16; list++) {
        u32 at = FX_LISTS[list];
        s32 guard = FX_PARTICLES;

        while (at != 0 && (at & 3) == 0 && at >= 0x80000000u && guard-- > 0 && fx_lit_n < FX_PARTICLES) {
            volatile u32* prim = (volatile u32*) (at + 0x48);
            volatile u32* env = (volatile u32*) (at + 0x50);

            if (!fire_color(*prim)) {
                fx_lit_at[fx_lit_n] = at;
                fx_prim[fx_lit_n] = *prim;
                fx_env[fx_lit_n] = *env;
                fx_lit_n++;
                *prim = night_word(*prim);
                *env = night_word(*env);
            }
            at = *(volatile u32*) at;       // next
        }
    }
}

RECOMP_HOOK_RETURN("omDrawAll") void night_cameras_end(void) {
    u32 i;

    for (i = 0; i < fx_lit_n; i++) {
        *(volatile u32*) (fx_lit_at[i] + 0x48) = fx_prim[i];
        *(volatile u32*) (fx_lit_at[i] + 0x50) = fx_env[i];
    }
    fx_lit_n = 0;
    light_block = NULL;         // the frame is drawn: it goes to the renderer as it is
    gfx_frame_ends();           // its buffers' fill, the plan for the next
}

// A roofed course's open-air start: the weather drawn into the frame's main
// list as the UI camera begins, after the whole scene with its depth and
// before the counter and the pointer (the see-through list the sky's own
// drawing uses is 3072 bytes on the Tunnel by the level's setup, and rain's
// sheets overran it at Electrode's explosion: the game's own panic, a hang,
// Oct 11 2026). In the scene camera's viewport and the border the game cuts
// its picture to (the intro's black bars), under that camera's projection
// and view by the game's own sub-list for them (ren_D_8004B248 + 1, with the
// perspective normalization). One volume over the first block's open
// stretch, the storm's strikes, thunder and the rain's voice while the
// camera is under the open sky. A blink frame (the shutter's black) is left
// black.
RECOMP_HOOK("mainCameraRender") void night_scene_begins(GObj* obj) {
    s32 k;

    roof_blink = (*(volatile s32*) 0x80382DA0u > 0);       // MainCameraBlinkTimeout (app_level/player.c)
    // The frame's mist starts clean: a part marked in a frame that drew
    // none (a blink's) is not drawn from old copies.
    for (k = 0; k < GLINT_SEGS; k++) {
        mist_ready[k] = 0;
    }
    mist_parts_frame = 0;
    mist_corners_frame = 0;
}

RECOMP_HOOK("renSpriteCameraRender") void night_ui_begins(GObj* ui) {
    mist_weather();
    roof_weather();
}

static void roof_weather(void) {
    GObj* cam = roof_cam;

    if (night == NULL || !ROOFED(night) || !roof_volume || roof_blink || cam == NULL
        || cam->data.cam == NULL || !on_the_ride || scoring) {
        if (night != NULL && ROOFED(night)) {
            rain_quiet();           // past the next block the rain is not heard
        }
        return;
    }
    {
        u32 oc = (u32) cam->data.cam;
        Gfx** game = &MAIN_GFX_POS[0];
        u32 matrices = *(volatile u32*) 0x8004B248u;  // ren_D_8004B248: the camera's matrix sub-list begins after it
        const volatile s32* border = (const volatile s32*) 0x803AE540u;    // MainCameraBorderXmin, Ymin, Xmax, Ymax (app_level/player.c)
        s32 x0 = border[0], y0 = border[1], x1 = border[2], y1 = border[3];

        if (x0 < 0) {
            x0 = 0;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (x1 > 320) {
            x1 = 320;
        }
        if (y1 > 240) {
            y1 = 240;
        }
        if (matrices != 0 && *game != NULL && x1 > x0 && y1 > y0) {
            Gfx** list = gfx_stage_open(game);

            gDPPipeSync((*list)++);
            gSPViewport((*list)++, oc + 0x08);
            gDPSetScissor((*list)++, G_SC_NON_INTERLACE, x0, y0, x1, y1);
            gSPDisplayList((*list)++, (Gfx*) (matrices + sizeof(Gfx)));
            // The scene's last objects leave their own modes behind (the effects
            // the primitive's depth and no texture perspective, app_render/effect.c;
            // models their alpha compare): the whole word is set for the sheets,
            // as a camera's start sets it.
            gDPPipeSync((*list)++);
            gDPSetOtherMode((*list)++,
                            G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE
                            | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                            G_AC_NONE | G_ZS_PIXEL | G_RM_AA_ZB_XLU_SURF | G_RM_AA_ZB_XLU_SURF2);
            roof_sky_draw(list, oc, &roof_sky_mtx[frame_ctx()]);
            // The strike was decided as the frame's world began (roof_strike).
            if (storming && roof_outside) {
                storm_light();
                thunder(ticks_of(night));
            }
            roof_volume_draw(list, 1);
            gfx_stage_close(game, 0);
        }
    }
}

// A course without a drum: its stars, moon and suns stand around the eye as
// the drums' do, drawn after the land at the far limit's depth (roof_sky_depth),
// so they show over the painted sky alone and the rock and the hills hide what
// they should; none under the weather, as the drums' go out then. Their twinkle
// is roof_tick's (or the photo's instant) and the moon's state moment_apply's,
// as everywhere. (Asked for by JackandBeans, Oct 17 2026.) The shooting star,
// and a storm's flash and bolt while the camera is under the open sky, the same
// way, as a drum's sky has them.
static void roof_sky_draw(Gfx** list, u32 oc, Mtx* mtx) {
    const volatile f32* eye = (const volatile f32*) (oc + 0x3C);
    u8 strike = (storming && roof_outside && strike_flash != 0);

    if (roof_sky != NULL && !raining && !snowing) {
        roof_stars(list, oc, mtx);
    }
    if (roof_sky != NULL || strike) {
        command(list, (0x64u << 24) | 0x00002Cu, 1);        // the lists live in the mod's memory
        gDPPipeSync((*list)++);
        gSPSetGeometryMode((*list)++, G_ZBUFFER);
        gDPSetDepthSource((*list)++, G_ZS_PRIM);
        gDPSetPrimDepth((*list)++, 0x7FFF, 0);
        if (roof_sky != NULL) {
            shooting_star_at(list, eye[0], eye[1], eye[2]);      // on a clear night only, as over a drum
        }
        if (strike) {
            storm_sky_at(list, eye[0], eye[1], eye[2]);
        }
        gDPPipeSync((*list)++);
        gDPSetDepthSource((*list)++, G_ZS_PIXEL);
        command(list, (0x64u << 24) | 0x00002Cu, 0);
    }
}

// The volume: night_data's box of sheets, the slope's width and length, ending
// a little past the roof's edge, in the current block's frame; at the far end
// the same box laid from a little under the arch out past the gate, its width
// along the path. On the ride the rain is heard too, receding with the mouth.
static void roof_volume_draw(Gfx** list, s32 heard) {
    f32 cx, cy, cz;

    if (!roof_volume) {
        return;
    }
    if (roof_volume == 1) {
        cx = roof_shift[0];
        cy = NIGHT_TUNNEL_OPEN_Y + roof_shift[1];
        cz = night->roof_z + NIGHT_TUNNEL_EAVE - (f32) NIGHT_VOLUME_HALF_Z + roof_shift[2];
    } else {
        cx = NIGHT_TUNNEL_EXIT_ORIGIN_X + NIGHT_TUNNEL_EXIT_X - NIGHT_TUNNEL_EAVE + (f32) NIGHT_VOLUME_HALF_X + roof_shift[0];
        cy = NIGHT_TUNNEL_OPEN_Y + roof_shift[1];
        cz = NIGHT_TUNNEL_EXIT_ORIGIN_Z + NIGHT_TUNNEL_EXIT_Z + roof_shift[2];
    }
    if (rain_share > 0.0f) {
        rain_sheets_now = night_rain_volume;
        rain_at(list, cx, cy, cz);
        rain_sheets_now = night_rain_sheets;
        if (heard) {
            // The voice recedes with the mouth: full under the open sky,
            // gone this far under the roof.
            rain_voice = 1.0f - roof_past / NIGHT_TUNNEL_VOICE_Z;
            if (rain_voice > 0.0f) {
                rain_sound();
            } else {
                rain_quiet();
            }
            rain_voice = 1.0f;
        }
    } else if (snow_share > 0.0f) {
        snow_sheets_now = night_snow_volume;
        snow_at(list, cx, cy, cz);
        snow_sheets_now = night_snow_sheets;
    }
    if (heard && rain_share <= 0.0f) {
        rain_quiet();
    }
}

// A course without a drum: its stars, moon and suns placed around the camera's
// eye, under the camera's view and projection in force.
static void roof_stars(Gfx** list, u32 oc, Mtx* mtx) {
    const volatile f32* eye = (const volatile f32*) (oc + 0x3C);

    place(mtx, eye[0], eye[1], eye[2]);
    command(list, (0x64u << 24) | 0x00002Cu, 1);        // the lists live in the mod's memory
    command(list, (0x64u << 24) | 0x00000Cu, 0x534B5900u);   // the group: its place followed between ticks
    command(list, 0x00002829u, 0);
    gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    gSPDisplayList((*list)++, sky_stars_snap(frame_ctx(), roof_sky));      // the stars with this frame's colors
    gDPSetDepthSource((*list)++, G_ZS_PIXEL);
    gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    command(list, (0x64u << 24) | 0x00000Du, 1);
    command(list, (0x64u << 24) | 0x00002Cu, 0);
}

// A photo of a roofed course (the album, Professor Oak's review, the
// report's pictures): the photo code's own camera draws its world, with no
// main camera and no UI camera after it, so the ride's way (roof_weather)
// never runs and the photo had no stars. The sky is drawn as that camera's
// object pass ends, after the land as on the ride, in its viewport and under
// its matrices still (every object pushes and pops its own; the decompilation,
// sys/render.c), and at the photo's instant. (JackandBeans, Oct 17 2026.)
// The weather's volume with it, as a drum's photo keeps its rain and snow.
RECOMP_HOOK("renCameraRenderObjects") void night_objects_begin(GObj* cam_obj, s32 mode) {
    photo_sky_cam = NULL;
}

RECOMP_HOOK_RETURN("renCameraRenderObjects") void night_objects_end(void) {
    GObj* cam = photo_sky_cam;
    Gfx** game = &MAIN_GFX_POS[0];
    Gfx** list;

    photo_sky_cam = NULL;
    mist_photo(*(GObj* volatile*) 0x8004AC48u);
    if (cam == NULL || cam != *(GObj* volatile*) 0x8004AC48u || cam->data.cam == NULL || night == NULL
        || !ROOFED(night) || on_the_ride || scoring || *game == NULL) {
        return;
    }
    if (night->twinkle != NULL) {
        twinkle(night);         // the photo's instant (ticks_of)
    }
    list = gfx_stage_open(game);
    gDPPipeSync((*list)++);
    gDPSetOtherMode((*list)++,
                    G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE
                    | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | G_RM_AA_ZB_XLU_SURF | G_RM_AA_ZB_XLU_SURF2);
    roof_sky_draw(list, (u32) cam->data.cam, &photo_sky_mtx[frame_slot(&photo_slot_n, FRAME_SLOTS)]);
    roof_volume_draw(list, 0);
    gfx_stage_close(game, 0);
}

RECOMP_HOOK("score_Render") void night_scoring_begins(GObj* camera) {
    scoring = 1;
}

RECOMP_HOOK_RETURN("score_Render") void night_scoring_ends(void) {
    scoring = 0;
}

// A photo is about to be redrawn: its record holds the world's clock at the
// instant it was taken, and the mod's stamp of the sky it was taken under
// (stamp_of). The game builds a photo's world once and keeps it for the
// photos of the same course; when the world standing was built under
// another sky than this photo's, it is made to build the world afresh, as
// it does for a photo of another course. Dawn's and Dusk's rounds say where
// in the ride the photo stands, and ask for no world of their own.
RECOMP_HOOK("func_8009D65C") void night_photo_begins(PhotoData* photo) {
    if (photo != NULL) {
        const volatile u8* bytes = (const volatile u8*) photo;
        u16 stamp = (u16) (((u32) bytes[2] << 8) | bytes[3]);
        u32 mode = (stamp >> 13) & 7;

        photo_clock = photo->unk_04.f32;
        photo_rounds = 0;
        if (mode == 4 || mode == 5) {
            photo_rounds = (stamp >> 1) & 0x1FF;
            stamp = (u16) (stamp & 0xFC01);
        }
        photo_stamp = stamp;
        if (stamp != world_stamp && photo->levelID >= 0 && *(volatile s32*) PHOTO_LEVEL == photo->levelID) {
            *(volatile s32*) PHOTO_LEVEL = 0x7F;
        }
    }
}

// The shutter. The game fills a photo's record in steps; the last of them
// (func_8009C604, the effects) is given the ride's stamp in the two bytes
// of the record the game leaves empty (PhotoData 0x02), which are saved
// with the photo in the album. So the album and Professor Oak's review
// draw a photo under the sky it was taken under, whatever the rows say
// by then (JackandBeans, Oct 23 2026: with Time at Night every saved photo
// had turned to night). A photo from before has no stamp and follows the
// rows, as every photo did.
RECOMP_HOOK("func_8009C604") void night_photo_taken(PhotoData* photo) {
    if (photo != NULL && on_the_ride) {
        volatile u8* bytes = (volatile u8*) photo;
        u16 stamp = shutter_stamp();

        bytes[2] = (u8) (stamp >> 8);
        bytes[3] = (u8) (stamp & 0xFF);
    }
}

// The options' two faces. Simple: one Time and one Weather row for the
// island. Custom: each course's own rows and the fine switches.
// Simple 0, Live 1 (the player's clock, the weather drawn), Custom 2.
static u32 setup_of(void) {
    return recomp_get_config_u32("setup");
}

static u8 simple_setup(void) {
    return setup_of() != 2;
}

#define TIME_NIGHT 0
#define TIME_SUNSET 1
#define TIME_DAWN 2
#define TIME_DUSK 3
#define TIME_DAY 4
#define TIME_CLOCK 5

// The weather codes, the same on every course (the options' rows list them in
// this order), and the Volcano's own fifth.
#define WX_CLEAR 0
#define WX_RAIN 1
#define WX_STORM 2
#define WX_SNOW 3               // the island's rain in the cold, on every course
#define WX_ASH 4                // the Volcano's own: ash from the eruption, a weather of its own


// The player's clock, read through the port, as minutes of the day with the
// seconds in them, so the cycle moves between the minutes too.
static f32 clock_minutes(void) {
    u32 t = snap64_local_time();
    u32 hour = (t >> 16) & 0xFF;
    u32 minute = (t >> 8) & 0xFF;
    u32 second = t & 0xFF;

    return (f32) (hour * 60 + minute) + (f32) second / 60.0f;
}

// The course's night, by its set-up record; NULL for a course with no sky.
static const Night* night_for(WorldSetup* setup) {
    u32 i;

    for (i = 0; i < sizeof(nights) / sizeof(nights[0]); i++) {
        if ((u32) setup == nights[i].setup) {
            return &nights[i];
        }
    }
    return NULL;
}

// A photo's stamp: the sky a ride's world was built under, in sixteen bits.
//   15-13  the hour: 1 the clock, 2 Night, 3 Sunset, 4 Dawn, 5 Dusk, 6 Day;
//          0 is no stamp
//   12-10  the weather, WX_CLEAR to WX_ASH
//    9-1   under the clock, the ride's first minute of the day in quarters;
//          a photo's own instant adds its clock (mode_minutes); at Dusk and
//          at a Dawn with a mist, how many times the clock had started over
//          at the shutter (shutter_stamp), none on a photo from before
//      0   an aurora stood that night (aurora_draw)
static u16 stamp_of(u32 weather) {
    u32 mode = cycle ? 1 : daytime ? 6 : dusk ? 5 : dawn ? 4 : sunset ? 3 : 2;
    u32 m = (cycle && ride_minutes >= 0.0f) ? ((u32) (ride_minutes / 4.0f) & 0x1FF) : 0;

    return (u16) ((mode << 13) | ((weather & 7) << 10) | (m << 1) | (aurora ? 1 : 0));
}

// The stamp at the shutter: the ride's, with the weather of the instant
// under Live, whose weather can turn during a ride (the half of a turn
// that is nearer), and at Dusk and at a Dawn with a mist, whose skies move
// with the ride, the clock's rounds so far.
static u16 shutter_stamp(void) {
    if (night != NULL && cycle && !stamped && setup_of() == 1 && night->weather != NULL) {
        return stamp_of(wx_share < 0.5f ? wx_from : wx_to);
    }
    if (night != NULL && (dusk || misty)) {
        u32 rounds = clock_rounds();

        return (u16) (world_stamp | ((rounds > 0x1FFu ? 0x1FFu : rounds) << 1));
    }
    return world_stamp;
}

// The hour and the weather a stamp says, in place of the rows' (night_of),
// for a photo's world. Whether the mod has a night to make of it: a Day
// under clear weather is the cartridge's own.
static s32 stamp_apply(u16 stamp, const Night* by) {
    u32 mode = (stamp >> 13) & 7;
    u32 wx = (stamp >> 10) & 7;

    cycle = (mode == 1);
    daytime = (mode == 6);
    dusk = (mode == 5);
    dawn = (mode == 4);
    sunset = (mode == 3 || mode == 4 || mode == 5);
    misty = dawn && (by->setup == NIGHT_RIVER_SETUP || by->setup == NIGHT_VALLEY_SETUP);
    day_weather_set = 0;
    if (cycle) {
        ride_minutes = (f32) (((stamp >> 1) & 0x1FF) * 4);
    }
    stamp_weather = (wx > WX_ASH) ? WX_CLEAR : wx;
    stamp_weather_set = 1;
    return !(daytime && stamp_weather == WX_CLEAR);
}

// The time of day the options ask for a course: in Simple set-up the one
// Time row, in Custom the course's own; a photo's world takes the ride's.
static const Night* night_of(WorldSetup* setup) {
    u32 i;

    for (i = 0; i < sizeof(nights) / sizeof(nights[0]); i++) {
        if ((u32) setup == nights[i].setup) {
            u32 setup = setup_of();
            u32 time = (setup == 1) ? TIME_CLOCK : (setup == 0) ? recomp_get_config_u32("time")
                                   : recomp_get_config_u32(nights[i].option);

            cycle = (time == TIME_CLOCK);
            sunset = (time == TIME_SUNSET || time == TIME_DAWN || time == TIME_DUSK);
            dawn = (time == TIME_DAWN);
            dusk = (time == TIME_DUSK);
            misty = dawn && (nights[i].setup == NIGHT_RIVER_SETUP || nights[i].setup == NIGHT_VALLEY_SETUP);
            daytime = (time == TIME_DAY);
            day_weather_set = 0;
            if (daytime) {
                // Day under clear weather is the cartridge's own, untouched.
                // Under a weather it is the cycle's day hour, held at midday,
                // so the rain, the snow or the ash falls by day as it does
                // under Live (1.0.0 left Day untouched whatever the Weather
                // row said: JackandBeans, Day and Snow on the Valley, Oct 11
                // 2026). The weather drawn here is the load's (Random draws
                // once). Island's own is the cartridge's day on every course:
                // the River's rain is the night's (1.1.0 rained on the River
                // by day, against the 1.0.1 changelog and the README).
                day_weather = (setup == 0 && recomp_get_config_u32("weather") == 0) ? WX_CLEAR
                                                                                     : weather_of(&nights[i]);
                day_weather_set = 1;
                if (day_weather == 0) {
                    return NULL;
                }
            }
            return &nights[i];
        }
    }
    return NULL;
}

// The weather the options ask for a course. In Simple set-up the one Weather
// row: the island's own (rain on the River, clear elsewhere), or one weather
// for the whole island; under Live the island's weather by the clock. In
// Custom the course's own row. The answer is a WX_ code: 0 clear, 1 rain,
// 2 storm, 3 snow, 4 ash (the Volcano's own).
static u32 weather_of(const Night* by) {
    u32 pick;

    if (by->weather == NULL) {
        return 0;
    }
    if (!simple_setup()) {
        return recomp_get_config_u32(by->weather);
    }
    pick = (setup_of() == 1) ? 5 : recomp_get_config_u32("weather");    // 0 the island's own, 1 clear, 2 rain, 3 storm, 4 snow, 5 drawn
    if (pick == 5 && setup_of() == 1) {
        return weather_live(by);        // Live: the island's weather by the clock and the date
    }
    if (pick == 5) {
        // Random: drawn once per ride for the course (a photo's world keeps the ride's):
        // mostly clear; rain a quarter of the time, a storm one in ten, snow one
        // in ten, on every course; on the Volcano, ash one in six, taken from
        // the clear.
        s32 idx = (s32) (by - nights);

        if (on_the_ride || !live_weather_drawn[idx]) {
            u32 t = snap64_local_time();
            u32 roll = (t * 2654435761u + snap64_local_date() * 40503u + (u32) idx * 97u) >> 24;   // 0..255

            if (by->embers && roll < 40) {
                live_weather[idx] = WX_ASH;
            } else if (roll < 140) {
                live_weather[idx] = WX_CLEAR;
            } else if (roll < 204) {
                live_weather[idx] = WX_RAIN;
            } else if (roll < 230) {
                live_weather[idx] = WX_STORM;
            } else {
                live_weather[idx] = WX_SNOW;
            }
            live_weather_drawn[idx] = 1;
        }
        return live_weather[idx];
    }
    if (pick == 0) {
        return (by->option[0] == 'r') ? WX_RAIN : WX_CLEAR;      // the island's own: the River's rain
    }
    // Clear, Rain, Storm or Snow: the same on every course, the Tunnel's at
    // its mouths (his word, Oct 11 2026: the weather is the island's, and
    // snow lies on the Beach as on the Valley).
    return pick - 1;
}

// The lights and the sky's colors in force for the time of day and the weather
// set: the night's, the sunset's or the dawn's.
static u8 palette_cave;          // the palettes being kept are a roofed course's cave's (the clear sky's)

// The scene light's two parts as the hour makes them (Nov 7 2026). By night the key is
// the moon's color, or the lava's on the Volcano, and the fill a third of the way to the
// sky's own color, each as strong as before (half the way made a Pokémon's shaded side
// too deep a blue); at every hour but Day the fill is at least a part
// in 2.8 of the two together, as the cartridge's own day has it (ambient 100 of 280),
// so no face looks lit by day at night and no side goes black.
static f32 light_lum(const u8* c) {
    return 0.2126f * (f32) c[0] + 0.7152f * (f32) c[1] + 0.0722f * (f32) c[2];
}

static void scene_light_shape(const Night* by) {
    static const u8 moon_key[3] = { 200, 212, 236 };
    static const u8 lava_key[3] = { 255, 150, 80 };
    s32 k;

    if (!sunset && !raining && !snowing) {
        const u8* key = by->embers ? lava_key : moon_key;
        u8 sky[3];
        f32 fill_lum = light_lum(scene_ambient), key_lum = light_lum(scene_diffuse), sky_lum, keyc_lum = light_lum(key);

        for (k = 0; k < 3; k++) {
            sky[k] = (u8) (((u32) sky_horizon[k] + sky_top[k]) / 2);
        }
        sky_lum = light_lum(sky);
        for (k = 0; k < 3; k++) {
            f32 fill = (f32) scene_ambient[k];
            f32 v;

            if (sky_lum > 1.0f) {
                fill = 0.65f * fill + 0.35f * (f32) sky[k] * fill_lum / sky_lum;
            }
            v = (f32) key[k] * key_lum / keyc_lum;
            scene_ambient[k] = (u8) (fill > 255.0f ? 255 : (s32) (fill + 0.5f));
            scene_diffuse[k] = (u8) (v > 255.0f ? 255 : (s32) (v + 0.5f));
        }
    }
    for (k = 0; k < 3; k++) {
        s32 both = (s32) scene_ambient[k] + scene_diffuse[k];
        s32 least = (both * 10 + 27) / 28;

        if (scene_ambient[k] < least) {
            scene_ambient[k] = (u8) (least > 255 ? 255 : least);
            scene_diffuse[k] = (u8) (both - least > 255 ? 255 : both - least < 0 ? 0 : both - least);
        }
    }
}

static void choose_palette(const Night* by) {
    s32 i;

    for (i = 0; i < 3; i++) {
        // An overcast sky. By rain the picture goes gray and the tints a
        // little lighter than a clear night's; by snow the same picture sits
        // under much lighter tints: the brightest of the nights.
        static const u8 wet_pic_sky[3] = { 120, 128, 144 };
        static const u8 wet_pic_cloud[3] = { 168, 176, 188 };
        static const u8 wet_tint_lo[3] = { 56, 68, 92 };
        static const u8 wet_tint_hi[3] = { 28, 36, 60 };
        static const u8 snow_tint_lo[3] = { 124, 132, 148 };
        static const u8 snow_tint_hi[3] = { 64, 72, 96 };
        // The land under snow is the brightest thing of the night, and only a
        // little blue: it has a light of its own.
        static const u8 snow_world[3] = { 168, 176, 212 };
        // By night the land facing the moon is a little lighter.
        static const u8 moon_land_diff[3] = { 28, 32, 48 };
        // The sunset. The sky's picture holds blue sky and warm cloud; the
        // corners are violet at the horizon all around and deep violet-blue
        // at the top, and the warmth toward the sun is the glow band's,
        // drawn smooth (the sky is a six-sided drum, and a tint that turned
        // with the azimuth creased at its six edges). Overcast, gray-rose;
        // snow, pink.
        static const u8 sun_pic_sky[3] = { 150, 160, 210 };
        static const u8 sun_pic_cloud[3] = { 255, 215, 190 };
        static const u8 sun_tint_away[3] = { 130, 120, 170 };
        static const u8 sun_tint_hi[3] = { 90, 80, 140 };
        static const u8 sun_wet_pic_sky[3] = { 150, 150, 160 };
        static const u8 sun_wet_pic_cloud[3] = { 200, 195, 200 };
        static const u8 sun_wet_tint_away[3] = { 130, 126, 150 };
        static const u8 sun_wet_tint_hi[3] = { 96, 96, 120 };
        static const u8 sun_snow_tint_away[3] = { 170, 165, 185 };
        static const u8 sun_snow_tint_hi[3] = { 130, 128, 150 };
        // Its lights: a cool ambient on the land and a warm sun across the
        // faces that meet it; the Pokémon under the same two.
        static const u8 sun_world[3] = { 130, 115, 135 };
        static const u8 sun_world_overcast[3] = { 150, 140, 148 };
        static const u8 sun_land_diff[3] = { 210, 150, 90 };
        static const u8 sun_body[3] = { 225, 205, 190 };
        static const u8 sun_flow[3] = { 180, 155, 165 };
        static const u8 sun_ambient[3] = { 105, 100, 135 };
        static const u8 sun_diffuse[3] = { 255, 205, 150 };
        // The dawn: the same sky from the other side, cooler and pinker.
        static const u8 dawn_pic_sky[3] = { 165, 175, 220 };
        static const u8 dawn_pic_cloud[3] = { 255, 225, 215 };
        static const u8 dawn_tint_away[3] = { 120, 135, 190 };
        static const u8 dawn_tint_hi[3] = { 85, 105, 165 };
        static const u8 dawn_world[3] = { 125, 120, 150 };
        static const u8 dawn_land_diff[3] = { 215, 175, 130 };
        static const u8 dawn_body[3] = { 230, 215, 205 };
        static const u8 dawn_flow[3] = { 175, 165, 185 };
        static const u8 dawn_ambient[3] = { 108, 110, 145 };
        static const u8 dawn_diffuse[3] = { 255, 225, 195 };

        if (sunset) {
            u32 overcast = raining || snowing;

            light_world[i] = overcast ? sun_world_overcast[i] : dawn ? dawn_world[i] : sun_world[i];
            light_body[i] = dawn ? dawn_body[i] : sun_body[i];
            light_flow[i] = dawn ? dawn_flow[i] : sun_flow[i];
            land_diff[i] = overcast ? (u8) (sun_land_diff[i] >> 2) : dawn ? dawn_land_diff[i] : sun_land_diff[i];
            pic_sky[i] = overcast ? sun_wet_pic_sky[i] : dawn ? dawn_pic_sky[i] : sun_pic_sky[i];
            pic_cloud[i] = overcast ? sun_wet_pic_cloud[i] : dawn ? dawn_pic_cloud[i] : sun_pic_cloud[i];
            tint_lo[i] = snowing ? sun_snow_tint_away[i] : raining ? sun_wet_tint_away[i] : dawn ? dawn_tint_away[i] : sun_tint_away[i];
            tint_away[i] = tint_lo[i];
            tint_hi[i] = snowing ? sun_snow_tint_hi[i] : raining ? sun_wet_tint_hi[i] : dawn ? dawn_tint_hi[i] : sun_tint_hi[i];
            scene_ambient[i] = dawn ? dawn_ambient[i] : sun_ambient[i];
            scene_diffuse[i] = overcast ? (u8) part(sun_diffuse[i], 160) : dawn ? dawn_diffuse[i] : sun_diffuse[i];
        } else {
            u32 share = snowing ? 330 : raining ? 238 : 256;
            // The land is lit as snow where snow lies on it (snow_cover), not
            // while snow falls on bare ground (a turn to snow during a ride);
            // nor in a roofed course's cave, which the snow never reaches.
            u32 world = (snow_cover && !palette_cave) ? snow_world[i] : part(by->world[i], raining ? 238 : 256);
            u32 body = part(by->body[i], share);
            u32 flow = part(by->flow[i], share > 256 ? 256 : share);    // water is not made brighter by snow

            light_world[i] = (u8) (world > 255 ? 255 : world);
            light_body[i] = (u8) (body > 255 ? 255 : body);
            light_flow[i] = (u8) (flow > 255 ? 255 : flow);
            land_diff[i] = (raining || snowing) ? 0 : moon_land_diff[i];
            pic_sky[i] = (raining || snowing) ? wet_pic_sky[i] : by->pic_sky[i];
            pic_cloud[i] = (raining || snowing) ? wet_pic_cloud[i] : by->pic_cloud[i];
            tint_lo[i] = snowing ? snow_tint_lo[i] : raining ? wet_tint_lo[i] : by->tint_lo[i];
            tint_away[i] = tint_lo[i];
            tint_hi[i] = snowing ? snow_tint_hi[i] : raining ? wet_tint_hi[i] : by->tint_hi[i];
            scene_ambient[i] = (u8) part(NIGHT_AMBIENT, light_body[i]);
            scene_diffuse[i] = (u8) part(NIGHT_DIFFUSE, light_body[i]);
        }
        sky_horizon[i] = (u8) ((pic_sky[i] * (((u32) tint_lo[i] + tint_away[i]) / 2)) / 255);
        sky_top[i] = (u8) ((pic_sky[i] * tint_hi[i]) / 255);
    }
    scene_light_shape(by);
    // Where the light comes from, for the painted land, the sky's corners
    // and the key light: the sun at sunset, the moon by night.
    {
        const s8* d = dawn ? by->dawn_sun : sunset ? by->sun : by->key;
        f32 len = __builtin_sqrtf((f32) d[0] * d[0] + (f32) d[1] * d[1] + (f32) d[2] * d[2]);

        if (len < 1.0f) {
            relight_dir[0] = 0.0f;
            relight_dir[1] = 1.0f;
            relight_dir[2] = 0.0f;
        } else if (sunset) {
            // The disc hangs six degrees up; the light that models the land
            // and the Pokémon comes from twenty-two, as it does in every game
            // that has a sunset, or flat ground would take none of it.
            f32 flat = __builtin_sqrtf((f32) d[0] * d[0] + (f32) d[2] * d[2]);

            if (flat < 1.0f) {
                flat = 1.0f;
            }
            relight_dir[0] = (d[0] / flat) * 0.927f;
            relight_dir[1] = 0.375f;
            relight_dir[2] = (d[2] / flat) * 0.927f;
        } else {
            relight_dir[0] = d[0] / len;
            relight_dir[1] = d[1] / len;
            relight_dir[2] = d[2] / len;
        }
    }
    cloud_top_fade = sunset ? 255 : 0;
}

// The dusk. The live colors are eleven triples; both ends are kept and the
// live ones slid between them as the ride's clock runs.
static u8* const live_palette[11] = { light_world, light_body, light_flow, land_diff, pic_sky, pic_cloud,
                                      tint_lo, tint_away, tint_hi, scene_ambient, scene_diffuse };

static void pal_keep(u8 pal[11][3], f32* dir) {
    s32 n, k;

    for (n = 0; n < 11; n++) {
        for (k = 0; k < 3; k++) {
            pal[n][k] = live_palette[n][k];
        }
    }
    for (k = 0; k < 3; k++) {
        dir[k] = relight_dir[k];
    }
}

// The cycle's three ends -- the sunset's palette, the night's and the dawn's
// -- kept for the weather in force and again for the one the island turns
// to (the same when there is no turn).
static void weather_palettes_keep(const Night* by) {
    u8 was_sunset = sunset, was_dawn = dawn;
    u8 r = raining, s = storming, n = snowing, a = ashing;
    s32 pass;

    for (pass = 0; pass < 3; pass++) {
        weather_flags_set(pass == 0 ? wx_from : pass == 1 ? wx_to : WX_CLEAR);     // the third: a clear sky, for the cave
        palette_cave = (pass == 2);
        sunset = 1;
        dawn = 0;
        choose_palette(by);
        pal_keep(pass == 0 ? pal_a : pass == 1 ? pal2_a : palc_a, dir_a);
        sunset = 0;
        choose_palette(by);
        pal_keep(pass == 0 ? pal_b : pass == 1 ? pal2_b : palc_b, dir_b);
        sunset = 1;
        dawn = 1;
        choose_palette(by);
        pal_keep(pass == 0 ? pal_d : pass == 1 ? pal2_d : palc_d, dir_d);
    }
    palette_cave = 0;
    sunset = was_sunset;
    dawn = was_dawn;
    raining = r;
    storming = s;
    snowing = n;
    ashing = a;
}

// The two weathers' palettes mixed as far as the turn has come, for the
// moment to blend from.
static void wx_mix(void) {
    s32 n, k;

    for (n = 0; n < 11; n++) {
        for (k = 0; k < 3; k++) {
            mix_a[n][k] = (u8) ((f32) pal_a[n][k] + ((f32) pal2_a[n][k] - (f32) pal_a[n][k]) * pal_share);
            mix_b[n][k] = (u8) ((f32) pal_b[n][k] + ((f32) pal2_b[n][k] - (f32) pal_b[n][k]) * pal_share);
            mix_d[n][k] = (u8) ((f32) pal_d[n][k] + ((f32) pal2_d[n][k] - (f32) pal_d[n][k]) * pal_share);
        }
    }
}

// The land, the water and the scenery lit again from the record's day
// colors with the colors in force, as light_vertex lit them the first time.
static void relight_record(void) {
    u32 j;

    for (j = 0; j < glint_op_n; j++) {
        u32 o = glint_op[j];
        u32 start, count, src, v;

        if (!(o & GLINT_LOAD)) {
            continue;
        }
        start = (o >> 12) & 0x3FFF;
        count = (o >> 6) & 0x3F;
        src = glint_load_src[j];
        if (src < night->world_lo || src >= night->world_hi) {
            continue;
        }
        for (v = 0; v < count; v++) {
            u16 f = glint_face[start + v];
            volatile u8* color = (volatile u8*) (src + 16 * v + 12);
            u32 orig = glint_orig_cn[start + v];
            u8 c[3];
            s32 steep, flow, cave, toward, k;
            const u8* light;
            const u8* diff;

            if (!(f & 0x8000)) {
                continue;       // never lit by a triangle: left alone
            }
            steep = (f >> 14) & 1;
            flow = (f >> 13) & 1;
            cave = (f >> 12) & 1;
            toward = f & 0x1FF;
            if (f & 0x0800u) {      // a painted sky's corner: the sky's rule
                f32 up = sky_up(src + 16 * v);

                if (up >= 0.0f) {
                    c[0] = (u8) (orig >> 24);
                    c[1] = (u8) (orig >> 16);
                    c[2] = (u8) (orig >> 8);
                    sky_corner_color(color, c, up);
                    continue;
                }
            }
            light = flow ? light_flow : cave ? cave_palette[0] : light_world;
            diff = cave ? no_light : land_diff;
            c[0] = (u8) (orig >> 24);
            c[1] = (u8) (orig >> 16);
            c[2] = (u8) (orig >> 8);
            if (snow_cover && !flow && glint_snow[start + v] > 0) {
                snow_paint(c, toward, glint_snow[start + v], (f32) glint_world_y[start + v]);
            }
            for (k = 0; k < 3; k++) {
                u32 lit = light[k] + (flow ? 0 : ((diff[k] * (u32) toward) >> 8));

                c[k] = (u8) part(c[k], whole(lit));
            }
            if (steep) {
                c[0] = (u8) part(c[0], steep_shade[0]);
                c[1] = (u8) part(c[1], steep_shade[1]);
                c[2] = (u8) part(c[2], steep_shade[2]);
                if (snow_cover && !cave) {
                    c[0] = (u8) part(c[0], snow_rock[0]);
                    c[1] = (u8) part(c[1], snow_rock[1]);
                    c[2] = (u8) part(c[2], snow_rock[2]);
                }
            }
            color[0] = c[0];
            color[1] = c[1];
            color[2] = c[2];
        }
    }
}

// ---------------------------------------------------------------------------
// The cycle. One function, moment_apply, sets the whole scene for a minute of
// the day: which palettes blend and how far, how far from the cartridge's own
// day the scene stands (nightness), where the sun is and how bright, how far
// the stars and the moon have come, which list the sky's spare slot calls.
// Every mode is a moment: Night is 23:00, Sunset 18:30, Dawn 05:45; Dusk runs
// the minutes from 18:30 to 19:15 over the ride; the player's clock runs them
// all. The hours, the same all year:
//
//   05:00  night           -> 05:45  dawn (pink, the sun rising)
//   05:45  dawn            -> 06:45  day (the cartridge's own)
//   17:30  day             -> 18:30  sunset (orange, the sun low)
//   18:30  sunset          -> 19:15  night (the sun sinks, the stars come)
//
// The day end of every blend is the cartridge itself: its picture texels, its
// corner colors, its fog, light and background, kept as the course loads.
static f32 m_dawn0 = 300.0f, m_dawn1 = 345.0f, m_day0 = 405.0f;      // the fixed hours, or the sun's at a latitude
static f32 m_day1 = 1050.0f, m_sun1 = 1110.0f, m_night0 = 1155.0f;

static f32 ease(f32 w) {
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    return w * w * (3.0f - 2.0f * w);
}

// The dawn's mist on the River and the Valley: a veil in the fog (fog_apply)
// and wisps lying on the water and the shore (mist_segment). It gathers through the last hour
// of the night, is thickest as the sky pinks and lifts as the day comes; a
// ride at the fixed dawn thins it as the ride goes, as the morning would.
// Clear weather only: rain and snow have their own gray. How much of it
// stands at this instant, 0 to 1.
#define MIST_GATHER 60.0f           // minutes before the dawn's first minute that it begins to gather
#define MIST_RIDE_TICKS 10800.0f    // three minutes: a fixed dawn's mist thins over them
#define MIST_RIDE_KEPT 0.4f         // to this much of itself
static f32 mist_now(void) {
    f32 m = cycle_minute, w;

    if (night == NULL || !mist_on || m < 0.0f || !(night->setup == NIGHT_RIVER_SETUP || night->setup == NIGHT_VALLEY_SETUP)) {
        return 0.0f;
    }
    if (m < m_dawn0 - MIST_GATHER || m >= m_day0) {
        return 0.0f;
    }
    if (m < m_dawn1) {
        w = (m - (m_dawn0 - MIST_GATHER)) / (m_dawn1 - (m_dawn0 - MIST_GATHER));
    } else {
        w = 1.0f - (m - m_dawn1) / (m_day0 - m_dawn1);
    }
    w = ease(w);
    if (!cycle) {
        f32 t = (f32) ride_ticks_of(night) / MIST_RIDE_TICKS;

        t = t > 1.0f ? 1.0f : t;
        w *= 1.0f - (1.0f - MIST_RIDE_KEPT) * ease(t);
    }
    return w * clear_share;
}

// A glow band's corners: faded, and deepened toward a red violet as it goes.
static void band_state(Vtx* v, const u8 colors[4][3], f32 fade, f32 late) {
    static const u8 deep[3] = { 205, 75, 115 };
    s32 c, r, k;

    if (v == NULL) {
        return;
    }
    fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
    for (c = 0; c < NIGHT_BAND_COLS; c++) {
        for (r = 0; r < NIGHT_BAND_ROWS; r++) {
            Vtx* p = &v[c * NIGHT_BAND_ROWS + r];
            u32 a = ((u32) (NIGHT_BAND_ALPHA * fade) * night_band_az_w[c] / 255) * night_band_el_w[r] / 255;

            for (k = 0; k < 3; k++) {
                p->v.cn[k] = (u8) (colors[r][k] + ((s32) deep[k] - colors[r][k]) * late);
            }
            p->v.cn[3] = (u8) a;
        }
    }
}

// A sun's haze and disc: lifted by so many units from where the generator
// put them (below the horizon when negative), and faded. The list: its
// band's call, the sprites' states, the dot, then the haze's color and
// corners and the disc's; each checked to be what it is.
static s16 sun_y0[2][8];
static u8 sun_y0_kept[2];

static void sun_state(Gfx* sun, s32 which, f32 lift, f32 alpha) {
    s32 k;

    alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    if (sun != NULL && (sun[3].words.w0 >> 24) == 0xFA && (sun[6].words.w0 >> 24) == 0xFA
        && (sun[4].words.w0 >> 24) == 0x01 && (sun[7].words.w0 >> 24) == 0x01) {
        Vtx* haze = (Vtx*) sun[4].words.w1;
        Vtx* disc = (Vtx*) sun[7].words.w1;

        if (!sun_y0_kept[which]) {
            sun_y0_kept[which] = 1;
            for (k = 0; k < 4; k++) {
                sun_y0[which][k] = haze[k].v.ob[1];
                sun_y0[which][4 + k] = disc[k].v.ob[1];
            }
        }
        for (k = 0; k < 4; k++) {
            haze[k].v.ob[1] = (s16) (sun_y0[which][k] + lift);
            disc[k].v.ob[1] = (s16) (sun_y0[which][4 + k] + lift);
        }
        sun[3].words.w1 = 0xFFFFFF00u | (u32) (112.0f * alpha);
        sun[6].words.w1 = 0xFFFFFF00u | (u32) (255.0f * alpha);
    }
}

// The moon's two primitive colors sit at fixed places before the sky list's
// end: the end, the states put back, the moon's triangles and vertices, its
// color (four back), the moon picture, the glow's triangles and vertices, the
// glow's color (eight back); each checked to be a color.
static void moon_state(f32 moon) {
    moon = moon < 0.0f ? 0.0f : moon > 1.0f ? 1.0f : moon;
    if (!night->embers && night->sky != NULL) {
        Gfx* sky = night->sky;
        s32 n = 0;

        while (n < 4096 && sky[n].words.w0 != 0xDF000000u) {
            n++;
        }
        if (n >= 9 && (sky[n - 4].words.w0 >> 24) == 0xFA && (sky[n - 8].words.w0 >> 24) == 0xFA) {
            sky[n - 4].words.w1 = 0xFFFFFF00u | (u32) (255.0f * moon);
            sky[n - 8].words.w1 = 0xFFFFFF00u | (u32) (112.0f * moon * moon_light);     // the halo fades with the phase
        }
    }
}

// The sky list's spare pair: two pipe syncs back to back before its end, the second the slot the generator
// found (NIGHT_*_SPARE). It calls the list given in a window of its own: the window opens in the first, the
// call is the second, and the hook shuts the window as it ends. So the sky reaches the mod's memory only
// through a window of its own, whatever the frame's plan; with the call alone, a frame whose sky window the
// guard held back ran whatever the renderer found at the masked address (cut 35 crashed so at a night's
// first frame, Nov 8 2026). The writes go in an order that never leaves the call without its window, should
// the renderer read the list between two of them. Without a call the pair is the cartridge's two pipe syncs.
static u8 spare_pair_said;

static void spare_point(Gfx* list) {
    volatile u32* spare = (volatile u32*) night->spare;
    s32 pair_ok;

    roof_sky = list;        // a course without a drum draws it itself (roof_weather)
    if (spare == NULL) {
        return;
    }
    pair_ok = (spare[-2] == 0xE7000000u && spare[-1] == 0) || (spare[-2] == EXT_WINDOW_W0 && spare[-1] == 1);
    if (!pair_ok && !spare_pair_said) {
        spare_pair_said = 1;
        recomp_printf("[island skies] the sky's spare pair is not the cartridge's here: its stars and moon are left out\n");
    }
    if (list == NULL || raining || snowing || !pair_ok) {
        spare[0] = 0xE7000000u;     // the call goes first
        spare[1] = 0;
        if (pair_ok) {
            spare[-2] = 0xE7000000u;
            spare[-1] = 0;
        }
    } else {
        // The pair runs the hook: the sky's list, then the aurora's (aurora_frame), then the window shut.
        gSPDisplayList(&aurora_hook[0], list);
        spare[-2] = EXT_WINDOW_W0;  // the window goes first
        spare[-1] = 1;
        spare[1] = (u32) aurora_hook;
        spare[0] = 0xDE000000u;
    }
}

// How far each of the eleven colors has gone at a moment of the dusk. The
// land and its lights are dark once the sun is down (the sun sinks until six
// tenths); the clouds lose the sun's warmth early; the top of the sky darkens
// before the horizon; the sky's picture and the horizon's tints take the
// whole dusk.
static f32 dusk_share(s32 n, f32 t) {
    f32 e;

    switch (n) {
        case 0: case 1: case 2: case 3: case 9: case 10: e = t / 0.55f; break;
        case 5: e = (t - 0.15f) / 0.4f; break;
        case 8: e = t / 0.8f; break;
        default: e = t; break;
    }
    return e < 0.0f ? 0.0f : e > 1.0f ? 1.0f : e;
}

// The live colors slid between two palettes, on the dusk's curves or straight.
static void palette_blend(const u8 pa[11][3], const f32* da, const u8 pb[11][3], const f32* db,
                          const u8 ca[11][3], const u8 cb[11][3], f32 t, s32 curved) {
    f32 len;
    s32 n, k;

    for (n = 0; n < 11; n++) {
        f32 share = curved ? dusk_share(n, t) : (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);

        for (k = 0; k < 3; k++) {
            live_palette[n][k] = (u8) ((f32) pa[n][k] + ((f32) pb[n][k] - (f32) pa[n][k]) * share);
            cave_palette[n][k] = (u8) ((f32) ca[n][k] + ((f32) cb[n][k] - (f32) ca[n][k]) * share);
        }
    }
    for (k = 0; k < 3; k++) {
        relight_dir[k] = da[k] + (db[k] - da[k]) * t;
        sky_horizon[k] = (u8) ((pic_sky[k] * (((u32) tint_lo[k] + tint_away[k]) / 2)) / 255);
        sky_top[k] = (u8) ((pic_sky[k] * tint_hi[k]) / 255);
    }
    len = __builtin_sqrtf(relight_dir[0] * relight_dir[0] + relight_dir[1] * relight_dir[1] + relight_dir[2] * relight_dir[2]);
    if (len > 0.01f) {
        for (k = 0; k < 3; k++) {
            relight_dir[k] /= len;
        }
    }
}

// The lights the land, the bodies and the water take go to the cartridge's
// own as the day comes: the whole of each color, and no sun across the
// faces. The palettes hold the night's or the sunset's values; this is what
// stands between them and the day.
static void lights_toward_day(void) {
    s32 share = (s32) (nightness * 256.0f);
    s32 n, k;

    for (n = 0; n < 3; n++) {
        for (k = 0; k < 3; k++) {
            live_palette[n][k] = (u8) (255 + (((s32) live_palette[n][k] - 255) * share) / 256);
            cave_palette[n][k] = (u8) (255 + (((s32) cave_palette[n][k] - 255) * share) / 256);
        }
    }
    for (k = 0; k < 3; k++) {
        land_diff[k] = (u8) (((s32) land_diff[k] * share) / 256);
        cave_palette[3][k] = (u8) (((s32) cave_palette[3][k] * share) / 256);
    }
}

// The fog and the background in force: the day's, or under weather the
// overcast's, so the far land and the sky's lid meet the flat sky; in a
// cave both are the hour's own with no weather in them. At dawn on a course
// with a mist, the fog draws in a little and pales by the mist's share.
#define MIST_FOG_DROP 4.0f          // how much nearer the fog begins at full mist (the rain's is 6, the snow's 10)
#define MIST_FOG_PALE 0.25f         // how far from the sky's horizon toward the pale the fog goes at full mist
static void fog_apply(void) {
    WorldSetup* setup = (WorldSetup*) night->setup;
    s32 share = (s32) (nightness * 256.0f);
    // The weather draws the fog nearer (fog_near, by its drop); in the cave the
    // hour's own distance stands.
    s32 near = fog_near + (s32) ((f32) ((s32) night->fog_min - (s32) fog_near) * cave_now);
    s32 fmin = day_fog_min + ((near - day_fog_min) * share) / 256;
    s32 fmax = day_fog_max + (((s32) fog_far - day_fog_max) * share) / 256;
    u8 fog[3], bg[3];
    u8 base[3];
    f32 mist;
    s32 k;

    overcast_base(base);
    for (k = 0; k < 3; k++) {
        s32 dfog = day_fog[k] + (s32) ((f32) ((s32) base[k] - day_fog[k]) * overcast_share);
        s32 dbg = day_bg[k] + (s32) ((f32) ((s32) base[k] - day_bg[k]) * overcast_share);
        s32 wfog = dfog + (((s32) sky_horizon[k] - dfog) * share) / 256;
        s32 chor = (s32) (((u32) cave_palette[4][k] * (((u32) cave_palette[6][k] + cave_palette[7][k]) / 2)) / 255);
        s32 cfog = day_fog[k] + ((chor - day_fog[k]) * share) / 256;
        s32 wbg = dbg + (((s32) sky_top[k] - dbg) * share) / 256;
        s32 ctop = (s32) (((u32) cave_palette[4][k] * (u32) cave_palette[8][k]) / 255);
        s32 cbg = day_bg[k] + ((ctop - day_bg[k]) * share) / 256;

        fog[k] = (u8) (wfog + (s32) ((f32) (cfog - wfog) * cave_now));
        // The background too: it is the sky through the mouths, which the
        // painted sky's faces cover, and the dark of the hall that draws no
        // ceiling, which must stay the hour's own under weather.
        bg[k] = (u8) (wbg + (s32) ((f32) (cbg - wbg) * cave_now));
    }
    mist = mist_now();
    if (mist > 0.0f) {
        // The dawn's mist: the fog draws in a little and goes from the sky's
        // own horizon color part of the way to a pale gray, so the far land
        // fades into a lighter sky and not into white. The first cut pulled
        // the fog most of the way to a near white and drew it in far, and the
        // walls and the logs went white (JackandBeans, Oct 18 2026); the
        // second cut's two fifths was still too much (Oct 19).
        static const u8 pale[3] = { 204, 196, 204 };

        fmin -= (s32) (MIST_FOG_DROP * mist);
        for (k = 0; k < 3; k++) {
            s32 to = sky_horizon[k] + (s32) ((f32) ((s32) pale[k] - sky_horizon[k]) * MIST_FOG_PALE);

            fog[k] = (u8) (fog[k] + (s32) ((f32) (to - fog[k]) * mist));
        }
    }
    for (k = 0; k < 3; k++) {
        mist_rgb[k] = (u8) (fog[k] + (255 - fog[k]) / 6);
        mist_fog_rgb[k] = fog[k];
    }
    mist_fog_min = (u16) fmin;
    mist_fog_max = (u16) fmax;
    setFogColor(fog[0], fog[1], fog[2]);
    setFogDistance((u16) fmin, (u16) fmax);     // the game reads the record's distances at the load alone
    setup->fogDistanceMin = (u16) fmin;
    setup->fogDistanceMax = (u16) fmax;
    setup->fogR = fog[0];
    setup->fogG = fog[1];
    setup->fogB = fog[2];
    setup->backgroundR = bg[0];
    setup->backgroundG = bg[1];
    setup->backgroundB = bg[2];
}

// A roofed course, each frame: how far the camera is in the cave's own
// light, and the scene light and the fog set again as that changes.
// The painted sky's faces are drawn without writing their depth, so the
// stars, drawn after the land at the primitive depth of the far limit
// (night_sprites_begin_zb), pass over the sky and nowhere else, as over a
// drum: every hill hides them, near or far. The sky material's render mode
// is found in each block's list by the sky picture's load that follows it
// (the decompilation: block0 draw list lines 273 and 278; block5 has a second
// painted box, pale clouds at 801575C0, under the same mode 24 commands before
// its sky load, and takes the stars too, being a sky). Found
// by the Night panorama's ridge frame, Oct 17 2026: stars over hills that
// stood farther than the sprites.
static void roof_sky_depth(const Night* by) {
    const u32 opa = G_RM_AA_ZB_OPA_SURF | G_RM_AA_ZB_OPA_SURF2;
    volatile u32* w = (volatile u32*) by->world_lo;
    volatile u32* end = (volatile u32*) by->world_hi;
    s32 loads = 0, modes = 0;

    for (; w + 1 < end; w += 2) {
        s32 k;

        if (w[0] != 0xFD100000u || (w[1] != by->pics[0] && w[1] != by->pics[1])) {
            continue;       // not gsDPSetTextureImage(RGBA, 16b, 1, a sky picture)
        }
        loads++;
        for (k = 2; k <= 64 && (u32) (w - k) >= by->world_lo; k += 2) {
            if (w[-k] == 0xE200001Cu && (w[1 - k] == opa || w[1 - k] == (opa & ~(u32) Z_UPD))) {
                w[1 - k] = opa & ~(u32) Z_UPD;      // gsDPSetRenderMode, without Z_UPD
                modes++;
                break;
            }
        }
    }
    recomp_printf("[island skies] the painted sky: %d picture loads, %d render modes without depth\n", loads, modes);
}

static void roof_tick(void) {
    f32 f;

    if (night == NULL || !ROOFED(night) || !on_the_ride || scoring) {
        return;
    }
    f = roof_past / NIGHT_TUNNEL_CAVE_Z;
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    if (f != cave_now) {
        cave_now = f;
        set_light(night);
        fog_apply();
    }
    if (night->twinkle != NULL) {
        twinkle(night);         // the drum's hook (night_sky_begins) never runs here
    }
}

// The scene takes the colors in force, as far from the day as nightness says:
// the light, the fog and the background, the sky's pictures and corners, the
// recorded land.
static void scene_apply(void) {
    WorldSetup* setup = (WorldSetup*) night->setup;
    s32 share = (s32) (nightness * 256.0f);
    s32 i, k;

    for (k = 0; k < 3; k++) {
        steep_shade[k] = (u16) (256 + (((s32) (k == 0 ? 110 : k == 1 ? 118 : 160) - 256) * share) / 256);
    }
    lights_toward_day();
    set_light(night);
    fog_apply();
    for (i = 0; i < 2; i++) {
        if (night->pics[i] != 0 && !sky_paint_skip) {
            sky_paint(night->pics[i], sky_mask_pic[i], i);
        }
        if (night->sky_vtx[i] != 0) {
            sky_corners(night->sky_vtx[i], night->sky_vtx_n[i], i);
            sky_orig_cn_kept[i] = 1;
        }
    }
    relight_record();
    fine_roof_refresh();
}

// A minute of the day, 0 to 1440: the whole scene set for it.
static void moment_apply(f32 m) {
    f32 stars = 0.0f, moon = 0.0f;

    cycle_minute = m;
    wx_mix();
    if (m >= m_night0 || m < m_dawn0) {
        // Night.
        palette_blend(mix_b, dir_b, mix_b, dir_b, palc_b, palc_b, 0.0f, 0);
        nightness = 1.0f;
        sunset = 0;
        dawn = 0;
        cloud_top_fade = 0;
        stars = 1.0f;
        moon = 1.0f;
        sun_state(night->sun_list, 0, 0.0f, 0.0f);
        sun_state(night->dawn_list, 1, 0.0f, 0.0f);
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(night->sky);
    } else if (m < m_dawn1) {
        // Night to dawn: the stars and the moon go, the sun comes up pink.
        f32 w = (m - m_dawn0) / (m_dawn1 - m_dawn0);

        palette_blend(mix_b, dir_b, mix_d, dir_d, palc_b, palc_d, w, 0);
        nightness = 1.0f;
        sunset = (w > 0.5f);
        dawn = 1;
        cloud_top_fade = (u8) (255.0f * w);
        stars = 1.0f - ease(w);
        moon = 1.0f - ease(w);
        sun_state(night->sun_list, 0, 0.0f, 0.0f);
        sun_state(night->dawn_list, 1, -260.0f * (1.0f - w), ease(w) * clear_share);
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, ease(w), 0.0f);
        spare_point(night->dawn_list);
    } else if (m < m_day0) {
        // Dawn to day: the pink lifts off the cartridge's own morning.
        f32 w = (m - m_dawn1) / (m_day0 - m_dawn1);

        palette_blend(mix_d, dir_d, mix_d, dir_d, palc_d, palc_d, 0.0f, 0);
        nightness = 1.0f - ease(w);
        sunset = 1;
        dawn = 1;
        cloud_top_fade = (u8) (255.0f * (1.0f - w));
        sun_state(night->sun_list, 0, 0.0f, 0.0f);
        sun_state(night->dawn_list, 1, 350.0f * w, (1.0f - ease(w)) * clear_share);
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 1.0f - ease(w), 0.0f);
        spare_point(w < 0.999f ? night->dawn_list : NULL);
    } else if (m < m_day1) {
        // Day: the cartridge, exactly.
        palette_blend(mix_a, dir_a, mix_a, dir_a, palc_a, palc_a, 0.0f, 0);
        nightness = 0.0f;
        sunset = 1;
        dawn = 0;
        cloud_top_fade = 255;
        sun_state(night->sun_list, 0, 350.0f, 0.0f);
        sun_state(night->dawn_list, 1, 350.0f, 0.0f);
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(NULL);
    } else if (m < m_sun1) {
        // Day to sunset: the sky warms, the sun comes down to its evening place.
        f32 w = (m - m_day1) / (m_sun1 - m_day1);

        palette_blend(mix_a, dir_a, mix_a, dir_a, palc_a, palc_a, 0.0f, 0);
        nightness = ease(w);
        sunset = 1;
        dawn = 0;
        cloud_top_fade = 255;
        sun_state(night->sun_list, 0, 350.0f * (1.0f - w), ease(w) * clear_share);
        sun_state(night->dawn_list, 1, 0.0f, 0.0f);
        band_state(night->band, night_band_sun_color, ease(w), 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(night->sun_list);
    } else {
        // Sunset to night: the dusk. The sun sinks and fades, the band
        // deepens and goes, the stars rise and the moon comes.
        f32 t = (m - m_sun1) / (m_night0 - m_sun1);
        f32 sun_alpha = 1.0f - (t - 0.35f) / 0.3f;
        f32 sink = (t < 0.6f) ? t / 0.6f : 1.0f;

        palette_blend(mix_a, dir_a, mix_b, dir_b, palc_a, palc_b, t, 1);
        nightness = 1.0f;
        sunset = (t < 0.6f);
        dawn = 0;
        cloud_top_fade = (u8) (255.0f * (1.0f - t));
        stars = (t - 0.65f) / 0.3f;
        moon = (t - 0.7f) / 0.3f;
        sun_state(night->sun_list, 0, -260.0f * sink, sun_alpha * clear_share);
        sun_state(night->dawn_list, 1, 0.0f, 0.0f);
        band_state(night->band, night_band_sun_color, (t < 0.3f) ? 1.0f : (t > 0.8f) ? 0.0f : (0.8f - t) / 0.5f, t > 0.8f ? 1.0f : t / 0.8f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(night->sun_list);
    }
    stars = stars < 0.0f ? 0.0f : stars > 1.0f ? 1.0f : stars;
    star_fade = (u8) (255.0f * stars * clear_share);
    moon_state(moon * clear_share);
    scene_apply();
}

// ---------------------------------------------------------------------------
// The calendar. Days since the turn of the century from a date, by the usual
// integer formula; the day of the year from it.
static s32 days_from_2000(s32 y, s32 m, s32 d) {
    return 367 * y - (7 * (y + (m + 9) / 12)) / 4 + (275 * m) / 9 + d - 730530;
}

static f32 sky_sin(f32 x) {
    f32 x2, t, sum;
    s32 n;

    while (x > 3.14159265f) {
        x -= 6.2831853f;
    }
    while (x < -3.14159265f) {
        x += 6.2831853f;
    }
    x2 = x * x;
    t = x;
    sum = x;
    for (n = 1; n < 7; n++) {
        t *= -x2 / (f32) ((2 * n) * (2 * n + 1));
        sum += t;
    }
    return sum;
}

static f32 sky_cos(f32 x) {
    return sky_sin(x + 1.57079633f);
}

static f32 m_rise = 360.0f;      // sunrise, for the page's line

// The latitude the Latitude row names, in degrees north, when the Sun row
// says By latitude; 999 for Fixed hours (and in Simple, which has no Sun row).
static s32 sun_latitude(void) {
    f32 lat;

    if (setup_of() == 0 || recomp_get_config_u32("sun") != 1) {
        return 999;
    }
    lat = (f32) recomp_get_config_double("latitude");
    return (s32) (lat < -60.0f ? -60.0f : lat > 60.0f ? 60.0f : lat);
}

// Solar noon as the clock reads it, in minutes of the day: 12:00 moved by
// the longitude's distance from the clock zone's own meridian (four minutes
// a degree) and by the equation of time (the sun's own lead or lag through
// the year, up to a quarter of an hour). The Longitude row at 0 takes the
// zone's meridian, which puts noon within half an hour.
static f32 sun_noon(s32 doy) {
    f32 zone = (f32) snap64_utc_offset() / 4.0f;       // the zone's meridian, degrees east
    f32 lon = (f32) recomp_get_config_double("longitude");
    f32 b = 6.2831853f * (f32) (doy - 81) / 365.0f;
    f32 eot = 9.87f * sky_sin(2.0f * b) - 7.53f * sky_cos(b) - 1.5f * sky_sin(b);

    if (lon == 0.0f) {
        lon = zone;
    }
    lon = lon < -180.0f ? -180.0f : lon > 180.0f ? 180.0f : lon;
    return 720.0f + (zone - lon) * 4.0f - eot;
}

// The hours from noon at which the sun stands at the altitude given (below
// the horizon when negative), by the textbook formula from the latitude and
// the declination; acos by halving, since the mod has none. Held to 0 to 12.
static f32 sun_hours_from_noon(f32 lat, f32 decl, f32 alt) {
    f32 x = (sky_sin(alt) - sky_sin(lat) * sky_sin(decl)) / (sky_cos(lat) * sky_cos(decl));
    f32 lo = 0.0f, hi = 3.14159265f, w;
    s32 i;

    x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
    for (i = 0; i < 24; i++) {          // cos falls from 1 to -1 over 0..pi
        w = (lo + hi) / 2.0f;
        if (sky_cos(w) > x) {
            lo = w;
        } else {
            hi = w;
        }
    }
    return (lo + hi) / 2.0f * (180.0f / 3.14159265f) / 15.0f;
}

// The hours of the cycle: the fixed ones (sunrise 06:00, sunset 18:30, the
// dawn and the dusk of fixed lengths), or the sun's own at the latitude
// chosen, for today, with the dawn and the dusk as long as the civil
// twilight there -- the sun's way from six degrees under to the horizon,
// about half an hour at 40 degrees at the equinox, twice that in a far
// northern June, less at the equator. Solar noon is taken as 12:00, so
// within an hour of the clock's. In a polar season the day or the night is
// held to two hours. Cheap enough to call often; it says so in the log when
// its answer changes.
static void sun_hours_apply(void) {
    static s32 logged_lat = 1000, logged_doy = -1, logged_noon = -1;
    s32 lat_deg = sun_latitude();
    f32 rise = 360.0f, set = 1110.0f, scale = 1.0f;

    if (lat_deg != 999) {
        u32 date = snap64_local_date();
        s32 y = (s32) (date >> 16), mo = (s32) ((date >> 8) & 0xFF), d = (s32) (date & 0xFF);
        s32 doy = days_from_2000(y, mo, d) - days_from_2000(y, 1, 1) + 1;
        f32 lat = (f32) lat_deg * (3.14159265f / 180.0f);
        f32 decl = (23.44f * 3.14159265f / 180.0f) * sky_sin((360.0f / 365.0f) * (f32) (284 + doy) * (3.14159265f / 180.0f));
        f32 w = sun_hours_from_noon(lat, decl, 0.0f);
        f32 tw;
        f32 noon = sun_noon(doy);

        w = w < 1.0f ? 1.0f : w > 11.0f ? 11.0f : w;
        tw = (sun_hours_from_noon(lat, decl, -6.0f * (3.14159265f / 180.0f)) - w) * 60.0f;     // the civil twilight, in minutes
        scale = tw / 31.0f;                 // 31 minutes: 40 degrees at the equinox, the fixed lengths' own
        scale = scale < 0.75f ? 0.75f : scale > 2.5f ? 2.5f : scale;
        rise = noon - w * 60.0f;
        set = noon + w * 60.0f;
        rise = rise < 46.0f ? 46.0f : rise > 700.0f ? 700.0f : rise;        // the dawn and the night stay inside the day
        set = set < 740.0f ? 740.0f : set > 1393.0f ? 1393.0f : set;
        if (60.0f * scale > rise - 1.0f) {              // the dawn's start stays inside the day
            scale = (rise - 1.0f) / 60.0f;
        }
        if (45.0f * scale > 1439.0f - set) {            // and so does the night's
            scale = (1439.0f - set) / 45.0f;
        }
        if (lat_deg != logged_lat || doy != logged_doy || (s32) noon != logged_noon) {
            logged_lat = lat_deg;
            logged_doy = doy;
            logged_noon = (s32) noon;
            recomp_printf("[island skies] at %d degrees the sun rises at %u:%02u and sets at %u:%02u today, noon at %u:%02u; the twilight runs %u minutes\n",
                          lat_deg, (u32) rise / 60, (u32) rise % 60, (u32) set / 60, (u32) set % 60,
                          (u32) noon / 60, (u32) noon % 60, (u32) (tw < 0.0f ? 0.0f : tw));
        }
    }
    m_rise = rise;
    m_dawn0 = rise - 60.0f * scale;
    m_dawn1 = rise - 15.0f * scale;
    m_day0 = rise + 45.0f * scale;
    m_day1 = set - 60.0f * scale;
    m_sun1 = set;
    m_night0 = set + 45.0f * scale;
}

// The moon's phase by the date: its age from the new moon of January 6, 2000,
// 18:14 UTC, over the synodic month; one of eight pictures. South of the
// equator the lit side is the other one.
// The moon's picture for tonight, made from the generator's phase picture:
// the dark side is left mostly see-through, so the halo and the sky show
// through it and only a faint disc remains (the earth's shine), where the
// generator's picture painted it solid at a sixth of the light and it blotted
// the halo out as a black patch (JackandBeans, Oct 24 2026); and the
// terminator is softened over a texel and a half, since at the moon's size
// on a large display the generator's half-texel edge showed as a stair.
// Each texel's lit share is its light against the full moon's at the same
// place (the two share the face, seas, craters and grain alike).
static u16 moon_face[32 * 32] __attribute__((aligned(8)));

static void moon_face_make(u32 k) {
    const u16* pic = night_moon_phases[k];
    const u16* full = night_moon_phases[4];
    static f32 lit[32 * 32];
    s32 x, y;

    for (y = 0; y < 32 * 32; y++) {
        f32 f = (f32) (full[y] >> 8);
        f32 w = f > 0.0f ? (((f32) (pic[y] >> 8) / f) - 0.16f) / 0.84f : 1.0f;

        lit[y] = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    }
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            s32 i = y * 32 + x;
            f32 wsum = 0.0f, isum = 0.0f, n = 0.0f, lo = 1.0f, hi = 0.0f;
            f32 w, light, a;
            s32 dx, dy;

            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    s32 xx = x + dx, yy = y + dy;
                    f32 wt;

                    if (xx < 0 || xx > 31 || yy < 0 || yy > 31) {
                        continue;
                    }
                    wt = (f32) ((2 - (dx < 0 ? -dx : dx)) * (2 - (dy < 0 ? -dy : dy)));     // 4 at the middle, 2 beside, 1 at a corner
                    wsum += wt * lit[yy * 32 + xx];
                    isum += wt * (f32) (pic[yy * 32 + xx] >> 8);
                    n += wt;
                    lo = lit[yy * 32 + xx] < lo ? lit[yy * 32 + xx] : lo;
                    hi = lit[yy * 32 + xx] > hi ? lit[yy * 32 + xx] : hi;
                }
            }
            if (hi - lo > 0.05f) {          // on the terminator: the softened light and share
                w = wsum / n;
                light = isum / n;
            } else {
                w = lit[i];
                light = (f32) (pic[i] >> 8);
            }
            a = (f32) (pic[i] & 0xFF) * (0.25f + 0.75f * w);
            moon_face[i] = (u16) (((u32) (light + 0.5f) << 8) | (u32) (a + 0.5f));
        }
    }
}

static void moon_phase_apply(void) {
    static const char* const names[8] = { "new", "a waxing crescent", "at first quarter", "waxing gibbous",
                                          "full", "waning gibbous", "at last quarter", "a waning crescent" };
    u32 date = snap64_local_date();
    u32 t = snap64_local_time();
    s32 y = (s32) (date >> 16), mo = (s32) ((date >> 8) & 0xFF), d = (s32) (date & 0xFF);
    f32 days = (f32) days_from_2000(y, mo, d) + (f32) ((t >> 16) & 0xFF) / 24.0f - 6.76f;
    f32 age;
    u32 k;
    s32 lat = sun_latitude();

    while (days < 0.0f) {
        days += 29.530588853f;
    }
    age = days - 29.530588853f * (f32) (s32) (days / 29.530588853f);
    k = (u32) (age / 29.530588853f * 8.0f + 0.5f) & 7;
    moon_light = 0.3f + 0.7f * (1.0f - sky_cos(age / 29.530588853f * 6.2831853f)) / 2.0f;
    if (lat != 999 && lat < 0) {
        k = (8 - k) & 7;                // the southern sky: the lit side mirrored
    }
    moon_face_make(k);
    night_use_moon[0].words.w1 = (u32) moon_face;
    recomp_printf("[island skies] the moon is %u days old, %s\n", (u32) age, names[k]);
}

// The minute a mode stands at, or runs through.
static f32 mode_minutes(void) {
    if (cycle) {
        if (on_the_ride || ride_minutes < 0.0f) {
            return clock_minutes();
        }
        // A photo's world: the minute of the photo's own instant, the ride's
        // first minute plus the photo's clock (ticks_of gives the photo's).
        // The stamp has no room here for the clock's rounds, so a photo taken
        // after the clock started over comes out that much earlier.
        {
            f32 m = ride_minutes + (f32) ticks_of(night) / 3600.0f;

            while (m >= 1440.0f) {
                m -= 1440.0f;
            }
            return m;
        }
    }
    if (daytime) {
        return (m_day0 + m_day1) / 2.0f;      // midday, held still; the ride's photos keep it
    }
    if (dusk) {
        // The ride's own clock: sunset holds for forty seconds, then eighty
        // seconds to night. A photo's world takes its instant's moment.
        f32 t = ((f32) ride_ticks_of(night) - 2400.0f) / 4800.0f;

        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        return m_sun1 + (m_night0 - m_sun1) * t;
    }
    if (dawn) {
        return m_dawn1;
    }
    if (sunset) {
        return m_sun1;
    }
    {
        f32 mid = (m_night0 + m_dawn0 + 1440.0f) / 2.0f;     // the middle of the night

        return mid >= 1440.0f ? mid - 1440.0f : mid;
    }
}

// ---------------------------------------------------------------------------
// The island's weather under Live, from the date and the hour and nothing
// else, so the next ride finds the weather the last one left. The day is cut
// into half hours; a half hour keeps the last one's weather one time in two
// and draws afresh the other time: mostly clear, rain about a quarter of the
// time, a storm less often, the odds leaning with the season. So the weather
// changes about every two hours, holds across the rides of a sitting, and
// about one ride in fifteen meets a change: it comes some minutes into its
// half hour, not on the stroke, over a minute and a half, the sky graying
// and the stars going before the rain comes; clearing, the rain thins first
// and the stars return.
#define STATUS_MOD_ID "snap64_island_skies"      // the id in mod.toml, for the port's two lines
static char notice_buf[120];
static void st_begin(char* dst, u32 cap);
static void st_put(const char* s);
#define WX_BLOCK_MINUTES 30.0f
#define WX_BLOCKS_A_DAY 48
#define WX_TURN_MINUTES 1.5f

static u32 wx_hash(s32 block, u32 salt) {
    u32 x = (u32) block * 2654435761u + salt * 0x9E3779B1u;

    x ^= x >> 15;
    x *= 0x85EBCA77u;
    x ^= x >> 13;
    x *= 0xC2B2AE3Du;
    x ^= x >> 16;
    return x & 0xFF;
}

// Months from midwinter, 0 to 6, on the side of the equator the Latitude
// row names (the north with Fixed hours).
static s32 wx_from_midwinter(s32 month) {
    s32 lat = sun_latitude();
    s32 m = (lat != 999 && lat < 0) ? ((month + 5) % 12) + 1 : month;
    s32 d = (m + 11) % 12;      // January 0

    return d > 6 ? 12 - d : d;
}

// A fresh draw, of 256: clear 170, rain 70, a storm 16 in winter; 150, 70
// and 36 between; 140, 64 and 52 in summer.
static u32 wx_fresh(s32 block, s32 month) {
    u32 roll = wx_hash(block, 1);
    s32 d = wx_from_midwinter(month);
    u32 clear = d <= 1 ? 170 : d >= 5 ? 140 : 150;
    u32 storm = d <= 1 ? 16 : d >= 5 ? 52 : 36;

    return roll < clear ? WX_CLEAR : roll >= 256 - storm ? WX_STORM : WX_RAIN;
}

// The island's weather in a half hour: the latest fresh draw at or before it.
static u32 wx_block(s32 block, s32 month) {
    s32 b = block, n;

    for (n = 0; n < 96 && wx_hash(b, 2) >= 128; n++) {     // a half hour draws afresh one time in two
        b--;
    }
    return wx_fresh(b, month);
}

// The minute of its half hour a change comes at: two to twenty minutes in.
static f32 wx_turn_at(s32 block) {
    return 2.0f + (f32) (wx_hash(block, 3) * 18) / 255.0f;
}

// The island's state at a minute of a half hour: what the weather was, what
// it is turning to, how far the turn has come (0 to 1), and the minute of
// the half hour the turn begins (-1 when it has none).
static void wx_state(s32 block, f32 in_block, s32 month, u32* from, u32* to, f32* share, f32* turn) {
    u32 now = wx_block(block, month), was = wx_block(block - 1, month);

    *turn = -1.0f;
    *from = *to = now;
    *share = 1.0f;
    if (now != was) {
        f32 at = wx_turn_at(block);

        *turn = at;
        *from = was;
        if (in_block < at) {
            *share = 0.0f;
        } else if (in_block < at + WX_TURN_MINUTES) {
            *share = (in_block - at) / WX_TURN_MINUTES;
        }
    }
}

// Whether the island's rain falls as snow, on every course: in the deep
// winter from 35 degrees, five months of it from 50, one day in three of the
// deep winter from 20; nearer the equator never. With Fixed hours the island
// has snow in December, January and February, the winter of the north.
static u8 wx_cold(s32 day, s32 month) {
    s32 lat = sun_latitude();
    s32 size = lat < 0 ? -lat : lat;
    s32 d = wx_from_midwinter(month);
    u8 third = (wx_hash(day, 4) < 86);

    if (lat == 999) {
        return d <= 1;
    }
    if (size >= 50) {
        return d <= 2;
    }
    if (size >= 35) {
        return d <= 1;
    }
    if (size >= 20) {
        return d <= 1 && third;
    }
    return 0;
}

// The island's weather as a course has it: one weather for every course, the
// rain snow in the cold. The Volcano adds its own: on three days in eight,
// ash from the eruption fills its clear hours, so a turn to or from it comes
// with the island's own turns and fades as they do.
static u32 wx_course(const Night* by, u32 island, s32 day, s32 month) {
    if (island == WX_CLEAR) {
        return (by->embers && wx_hash(day, 6) < 96) ? WX_ASH : WX_CLEAR;
    }
    if (wx_cold(day, month)) {
        return WX_SNOW;
    }
    return island;
}

// Today, from the port: the day since the turn of the century, and the month.
static void wx_today(s32* day, s32* month) {
    u32 date = snap64_local_date();

    *month = (s32) ((date >> 8) & 0xFF);
    *day = days_from_2000((s32) (date >> 16), *month, (s32) (date & 0xFF));
}

// The island's weather in words, for the page's line and the log.
static const char* wx_island_name(u32 code, u8 cold) {
    return code == WX_CLEAR ? "clear" : cold ? "snow" : code == WX_STORM ? "a storm" : "rain";
}

// The course's weather now under Live: wx_from, wx_to, wx_share and the turn's
// minute of the day set; the one in force returned.
static u32 weather_live(const Night* by) {
    f32 m = mode_minutes();
    s32 day, month, block, at;
    u32 from, to;
    f32 share, turn, in;

    wx_today(&day, &month);
    if (!on_the_ride && ride_minutes >= 0.0f && ride_day >= 0) {
        // A photo's world: the ride's own date, the next day's when the photo's
        // instant came after midnight (a photo checked after midnight took the
        // next day's weather for its instant).
        day = ride_day;
        month = ride_month;
        if (ride_minutes + (f32) ticks_of(by) / 3600.0f >= 1440.0f) {
            day++;
        }
    }
    block = (s32) (m / WX_BLOCK_MINUTES);
    at = day * WX_BLOCKS_A_DAY + block;
    in = m - WX_BLOCK_MINUTES * (f32) block;
    wx_state(at, in, month, &from, &to, &share, &turn);
    // The half hour before midnight is the day before's.
    wx_from = wx_course(by, from, (block == 0) ? day - 1 : day, month);
    wx_to = wx_course(by, to, day, month);
    if (wx_from != wx_to && turn < 0.0f) {
        // Midnight: the island's weather holds, but the course's own day turns
        // (the Volcano's ash days, the snow days of the deep winter), and it
        // turns as the island's weather does, not at once.
        turn = wx_turn_at(at);
        share = (in < turn) ? 0.0f : (in < turn + WX_TURN_MINUTES) ? (in - turn) / WX_TURN_MINUTES : 1.0f;
    }
    if (wx_from == wx_to) {
        share = 1.0f;
    }
    wx_share = share;
    wx_turn_minute = (turn < 0.0f) ? -1.0f : WX_BLOCK_MINUTES * (f32) block + turn;
    return (share < 0.5f) ? wx_from : wx_to;
}

static f32 wx_ramp(f32 x) {
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

// The flags the drawing reads, for one weather of the course.
static void weather_flags_set(u32 code) {
    raining = (code == WX_RAIN || code == WX_STORM);
    storming = (code == WX_STORM);
    snowing = (code == WX_SNOW);
    ashing = (code == WX_ASH);
}

static const char* wx_name(u32 code) {
    return code == WX_CLEAR ? "clear" : code == WX_STORM ? "a storm" : code == WX_SNOW ? "snow" : code == WX_ASH ? "ash" : "rain";
}

// From the turn under way, the shares the drawing takes: the palettes mix over
// the whole turn, eased; the clear sky's stars, moon and glint go in its first
// half and come back in its last; the rain, the snow or the ash start a third
// of the way in and, on the way out, thin over the first two thirds. The
// flags turn over at the middle. The fog comes in with the wet. A turn of the
// flags is said in the log when asked (the ticks of a ride, not a load).
static void weather_shares_apply(s32 say) {
    u8 r = raining, s = storming, n = snowing, a = ashing;
    f32 e = ease(wx_share);
    f32 up = wx_ramp((wx_share - 0.3f) / 0.7f), down = wx_ramp(wx_share / 0.7f);
    f32 wet0 = (wx_from == WX_RAIN || wx_from == WX_STORM);
    f32 wet1 = (wx_to == WX_RAIN || wx_to == WX_STORM);
    f32 snow0 = (wx_from == WX_SNOW), snow1 = (wx_to == WX_SNOW);
    f32 ash0 = (wx_from == WX_ASH), ash1 = (wx_to == WX_ASH);
    f32 clear0 = (wx_from == WX_CLEAR), clear1 = (wx_to == WX_CLEAR);
    f32 drop0 = snow0 ? 10.0f : wet0 ? 6.0f : 0.0f, drop1 = snow1 ? 10.0f : wet1 ? 6.0f : 0.0f;

    pal_share = e;
    overcast_share = (1.0f - clear0) + (clear0 - clear1) * e;
    rain_share = wet0 + (wet1 - wet0) * (wet1 > wet0 ? up : down);
    snow_share = snow0 + (snow1 - snow0) * (snow1 > snow0 ? up : down);
    ash_share = ash0 + (ash1 - ash0) * (ash1 > ash0 ? up : down);
    clear_share = clear0 + (clear1 - clear0) * (clear1 > clear0 ? wx_ramp((wx_share - 0.5f) / 0.5f) : wx_ramp(wx_share / 0.5f));
    weather_flags_set(wx_share < 0.5f ? wx_from : wx_to);
    fog_near = (u16) ((f32) night->fog_min - (drop0 + (drop1 - drop0) * e));
    if (say && (r != raining || s != storming || n != snowing || a != ashing)) {
        u32 code = wx_share < 0.5f ? wx_from : wx_to;

        recomp_printf("[island skies] the weather turns at tick %u: %s\n", ticks_of(night), wx_name(code));
        // Said over the course too, as the turn comes.
        st_begin(notice_buf, sizeof(notice_buf));
        st_put(code == WX_CLEAR ? "The sky clears" : code == WX_ASH ? "Ash falls" : code == WX_STORM ? "A storm comes"
               : code == WX_SNOW ? "Snow comes" : "The rain comes");
        snap64_notice(STATUS_MOD_ID, notice_buf);
    }
}

// The state at the load, for the log and the runs that check it.
static void weather_log(void) {
    if (wx_from == wx_to) {
        recomp_printf("[island skies] the island's weather: %s\n", wx_name(wx_from));
    } else {
        s32 at = (s32) wx_turn_minute;

        recomp_printf("[island skies] the island's weather: %s, turning to %s at %d:%02d, %d of 100 of the way\n",
                      wx_name(wx_from), wx_name(wx_to), at / 60, at % 60, (s32) (wx_share * 100.0f));
    }
}

// Under Live, every ten ticks: the state now; a new pair of weathers has its
// palettes kept; the shares follow. Says whether anything moved, so that the
// scene is painted again.
static s32 weather_tick(void) {
    u32 from = wx_from, to = wx_to;
    f32 share = wx_share;

    if (night->weather == NULL) {
        return 0;
    }
    weather_live(night);
    if (wx_from != from || wx_to != to) {
        weather_palettes_keep(night);
    }
    if (wx_from == from && wx_to == to && wx_share == share) {
        return 0;
    }
    weather_shares_apply(1);
    return 1;
}

// ---------------------------------------------------------------------------
// The mod's line on its details page (snap64_set_status): under Live, or with
// a course on Your clock, the hour as the island has it, the weather, the
// sun's hours, and the next change of weather when one is within six hours;
// the port fills %T with the clock as the page is read. Looked at every two
// seconds, sent when it changes.
static char status_buf[200], status_sent[200];
static u32 status_len, status_ticks;
static char* st_dst = status_buf;     // the buffer the builder writes, and its size
static u32 st_cap = sizeof(status_buf);

static void st_begin(char* dst, u32 cap) {
    st_dst = dst;
    st_cap = cap;
    status_len = 0;
    dst[0] = 0;
}

static void st_put(const char* s) {
    while (*s != 0 && status_len < st_cap - 1) {
        st_dst[status_len++] = *s++;
    }
    st_dst[status_len] = 0;
}

static void st_num(u32 n) {
    char digits[12];
    s32 i = 0;

    do {
        digits[i++] = (char) ('0' + n % 10);
        n /= 10;
    } while (n != 0);
    while (i > 0) {
        char one[2] = { digits[--i], 0 };

        st_put(one);
    }
}

// A minute of the day as a clock reads it: "6:41 PM".
static void st_hm12(f32 minutes) {
    s32 m = (s32) minutes;
    s32 h = (m / 60) % 24, mm = m % 60;

    st_num((u32) (h % 12 == 0 ? 12 : h % 12));
    st_put(":");
    if (mm < 10) {
        st_put("0");
    }
    st_num((u32) mm);
    st_put(h < 12 ? " AM" : " PM");
}

static const char* phase_name(f32 m) {
    if (m >= m_night0 || m < m_dawn0) {
        return "night";
    }
    if (m < m_day0) {
        return "dawn";
    }
    if (m < m_day1) {
        return "day";
    }
    if (m < m_sun1) {
        return "sunset";
    }
    return "dusk";
}

static void status_tick(void) {
    u32 setup, i;
    u8 clocked;

    if (++status_ticks < 60) {
        return;
    }
    status_ticks = 0;
    setup = setup_of();
    clocked = (setup == 1);
    if (setup == 0) {
        clocked = (recomp_get_config_u32("time") == TIME_CLOCK);
    } else if (setup == 2) {
        for (i = 0; i < sizeof(nights) / sizeof(nights[0]); i++) {
            if (recomp_get_config_u32(nights[i].option) == TIME_CLOCK) {
                clocked = 1;
            }
        }
    }
    st_begin(status_buf, sizeof(status_buf));
    if (clocked) {
        f32 m = clock_minutes();

        sun_hours_apply();
        st_put(setup == 1 ? "Live: %T, " : "Your clock: %T, ");
        st_put(phase_name(m));
        if (setup == 1) {
            s32 day, month, block = (s32) (m / WX_BLOCK_MINUTES), b;
            u32 from, to, now;
            f32 share, turn, at = -1.0f;
            u32 next = WX_CLEAR;

            wx_today(&day, &month);
            wx_state(day * WX_BLOCKS_A_DAY + block, m - WX_BLOCK_MINUTES * (f32) block, month, &from, &to, &share, &turn);
            st_put(", ");
            st_put(wx_island_name(share < 0.5f ? from : to, wx_cold(day, month)));
            // The next change: this half hour's own if it is still ahead or
            // under way, else the first within the six hours after.
            if (turn >= 0.0f && share < 1.0f) {
                at = WX_BLOCK_MINUTES * (f32) block + turn;
                next = to;
            } else {
                now = wx_block(day * WX_BLOCKS_A_DAY + block, month);
                for (b = block + 1; b <= block + 12; b++) {
                    u32 w = wx_block(day * WX_BLOCKS_A_DAY + b, month);

                    if (w != now) {
                        at = WX_BLOCK_MINUTES * (f32) b + wx_turn_at(day * WX_BLOCKS_A_DAY + b);
                        next = w;
                        break;
                    }
                }
            }
            st_put(". Sunrise ");
            st_hm12(m_rise);
            st_put(", sunset ");
            st_hm12(m_sun1);
            st_put(".");
            if (at >= 0.0f) {
                st_put(next == WX_CLEAR ? " Clearing at " : wx_cold(day, month) ? " Snow from " : next == WX_STORM ? " A storm from " : " Rain from ");
                st_hm12(at >= 1440.0f ? at - 1440.0f : at);
                st_put(".");
            }
            {
                const Shower* sh = shower_tonight(snap64_local_date(), m);

                if (sh != NULL) {
                    st_put(" A meteor shower tonight, ");
                    st_put(sh->name);
                    st_put(".");
                }
            }
            if (night != NULL && on_the_ride && aurora) {
                st_put(" An aurora tonight.");       // the ride's draw: from the sunset's start to the dawn's end
            }
        } else {
            st_put(". Sunrise ");
            st_hm12(m_rise);
            st_put(", sunset ");
            st_hm12(m_sun1);
            st_put(".");
        }
    }
    for (i = 0; i < sizeof(status_buf); i++) {
        if (status_buf[i] != status_sent[i]) {
            break;
        }
        if (status_buf[i] == 0) {
            return;             // the same line as last sent
        }
    }
    for (i = 0; i < sizeof(status_buf); i++) {
        status_sent[i] = status_buf[i];
        if (status_buf[i] == 0) {
            break;
        }
    }
    snap64_set_status(STATUS_MOD_ID, status_buf);
}

// The weather over the course now, as a WX_ code: under Live the side of
// the turn the course is on, else what the flags say.
static u32 wx_code_now(void) {
    if (setup_of() == 1 && !stamped && night != NULL && night->weather != NULL) {
        return wx_share < 0.5f ? wx_from : wx_to;
    }
    return storming ? WX_STORM : snowing ? WX_SNOW : (raining || ashing) ? WX_RAIN : WX_CLEAR;
}

// The sky in a few words: the weather as a word on the hour's light,
// "clear night", "rainy dawn", "stormy day"; the Volcano's ash has no
// such word, so "night under ash".
static void st_sky(u32 code, const char* phase) {
    if (code == WX_ASH) {
        st_put(phase);
        st_put(" under ash");
        return;
    }
    st_put(code == WX_CLEAR ? "clear " : code == WX_STORM ? "stormy " : code == WX_SNOW ? "snowy " : "rainy ");
    st_put(phase);
}

// The line over the course as a ride starts (snap64_notice): the hour and
// the sky, "7:23 PM, clear night".
static void ride_notice(void) {
    f32 m = clock_minutes();

    st_begin(notice_buf, sizeof(notice_buf));
    st_hm12(m);
    st_put(", ");
    st_sky(wx_code_now(), phase_name(m));
    if (aurora) {
        st_put(", an aurora");
    }
    snap64_notice(STATUS_MOD_ID, notice_buf);
}

// Every ten ticks of a frame's drawing, the moment moves on: the dusk by the
// ride's clock, the cycle by the player's. The day holds still between its
// ends, so nothing is repainted there.
static void cycle_tick(void) {
    u32 t;
    f32 m;
    s32 turning = 0;
    u32 step;

    if (night == NULL || scoring || !(cycle || dusk || misty)) {
        return;
    }
    t = ride_ticks_of(night);   // two photos of one clock may stand a round apart
    // A dusk on the ride moves on with every frame: in steps of ten ticks a lit
    // face moved by as much as two levels at once (its ambient and its diffuse each
    // a level). The sky's pictures are still painted again every ten (scene_apply).
    step = (dusk && on_the_ride) ? 1u : 10u;
    if (!(dusk_last == 0xFFFFFFFFu || t - dusk_last >= step || t < dusk_last)) {
        return;
    }
    dusk_last = t;
    if (!(cycle || dusk)) {
        if (!storming) {
            fog_apply();        // a fixed dawn's mist thins as the ride goes: the fog alone moves
        }
        return;
    }
    m = mode_minutes();
    if (cycle && !stamped) {
        turning = weather_tick();       // the island's weather moves with the clock
    }
    if (!turning && m >= m_day0 && m < m_day1 && cycle_minute >= m_day0 && cycle_minute < m_day1) {
        return;
    }
    sky_paint_skip = (step == 1u && sky_paint_last != 0xFFFFFFFFu && t >= sky_paint_last && t - sky_paint_last < 10u);
    if (!sky_paint_skip) {
        sky_paint_last = t;
    }
    moment_apply(m);
    sky_paint_skip = 0;         // a photo's world, a load: their pictures are painted
}

// Whether tonight has an aurora, drawn as the ride's world is built and kept
// for its photos' worlds. Under Live it is the latitude's: most nights beyond
// 55 degrees north or south, one in two to 45, one in five to 35, one in
// twelve to 25, one in forty nearer the equator, and one in three with Fixed
// hours; drawn from the night's own date, so it holds past midnight, and
// said from the sunset's start to the dawn's end. On a fixed hour one ride
// in three, or as Custom's Aurora row says. The Tunnel has none: its sky is
// a sliver over each end. A stamped photo's night has what its note says
// (stamp_of).
static void aurora_draw(const Night* by) {
    s32 idx = (s32) (by - nights);

    if (idx < 0 || idx >= 8 || ROOFED(by)) {
        aurora = 0;
        return;
    }
    if (on_the_ride || !aurora_drawn[idx]) {
        u32 t = snap64_local_time();
        u32 date = snap64_local_date();
        u32 odds, roll;
        u8 tonight;

        if (setup_of() == 1) {
            s32 lat = sun_latitude();
            s32 far = lat < 0 ? -lat : lat;

            odds = (lat == 999) ? 85 : far >= 55 ? 224 : far >= 45 ? 128 : far >= 35 ? 51 : far >= 25 ? 21 : 6;
            if ((t >> 16) < 12) {
                date -= 1;      // a night past midnight is still last evening's
            }
            roll = date * 3266489917u + (u32) idx * 668265263u + 0x5BD1E995u;
        } else {
            u32 pick = simple_setup() ? 0 : recomp_get_config_u32("aurora");    // 0 some nights, 1 every night, 2 off

            odds = (pick == 1) ? 256 : (pick == 2) ? 0 : 85;
            roll = t * 2246822519u + date * 3266489917u + (u32) idx * 668265263u + 0x5BD1E995u;
        }
        roll ^= roll >> 15;
        roll *= 0x2C1B3C6Du;
        roll ^= roll >> 12;
        if (cycle) {
            f32 m = clock_minutes();

            tonight = (m >= m_day1 || m < m_dawn1);      // from the sunset's start to the dawn's end
        } else {
            tonight = !daytime && (!sunset || dusk);
        }
        aurora_for[idx] = (u8) (tonight && (roll & 0xFF) < odds);
        aurora_drawn[idx] = 1;
        if (on_the_ride && aurora_for[idx]) {
            recomp_printf("[island skies] an aurora tonight\n");
        }
    }
    aurora = stamped ? (u8) (photo_stamp & 1) : aurora_for[idx];
}

// A course's world is about to be built, for the ride or for a photo. For a
// course with a night: make its data night, unless this load of it already
// is.
RECOMP_HOOK("createWorld") void night_create_world(WorldSetup* setup, s32 sky_id, s32 block_min, s32 block_max,
                                                   s32 link, s32 dl_link, void* pokemon_enters) {
    const Night* by;
    s32 i;

    on_the_ride = ((u32) pokemon_enters != PHOTO_WORLD_CALLBACK);
    if (on_the_ride) {
        ride_rounds = 0;            // the ride's time begins (ride_ticks_of)
        ride_clock_last = GlobalTimer;
    }
    by = night_of(setup);
    stamped = 0;
    stamp_weather_set = 0;
    aurora = 0;
    if (!on_the_ride && photo_stamp != 0) {
        // A stamped photo's world: the sky it was taken under (night_photo_begins).
        const Night* sky = night_for(setup);

        if (sky != NULL) {
            by = stamp_apply(photo_stamp, sky) ? sky : NULL;
            stamped = 1;
        }
    }
    night = by;
    sky_repaint_due = (by != NULL && by->setup == NIGHT_VALLEY_SETUP);
    thunder_span = 0xFFFFFFFFu;
    thunder_at = body_at = echo_at = 0;
    chirp_next[0] = 240;
    chirp_next[1] = 330;
    chirp_next[2] = 420;
    chirp_said = 0;
    if (by == NULL) {
        set_light(NULL);
        // The cartridge's own day on a course with a sky, or a course without one.
        world_stamp = on_the_ride ? ((night_for(setup) != NULL) ? stamp_of(WX_CLEAR) : 0) : photo_stamp;
        return;
    }
    if (ROOFED(by)) {
        roof_sky_depth(by);
    }
    sky_lists_zb(ROOFED(by));
    // The lights and the sky in force. A rainy night is overcast and a little
    // darker; nothing of it shows a star or the moon.
    {
        u32 weather = stamp_weather_set ? stamp_weather : day_weather_set ? day_weather : weather_of(by);     // a Day load's is night_of's draw

        day_weather_set = 0;
        stamp_weather_set = 0;
        world_weather = weather;
        if (stamped || !(setup_of() == 1 && by->weather != NULL)) {
            wx_from = wx_to = weather;      // one weather for the ride
            wx_share = 1.0f;
            wx_turn_minute = -1.0f;
        }
        weather_shares_apply(0);
        // Snow lies on the land when it was snowing as the ride loaded, or had
        // just stopped; snow that begins during a ride falls and does not lie.
        snow_cover = (wx_from == WX_SNOW) || (wx_to == WX_SNOW && wx_share >= 0.5f);    // on a roofed course, only in the open air (open_air)
        snow_relit = 0;
        roof_outside = 0;
        roof_volume = 0;
        roof_placed = 0;
        roof_photo_said = 0;
        cave_now = 0.0f;
        // The fine switches are Custom's; Simple takes their defaults.
        fx_lit = simple_setup() || (recomp_get_config_u32("effects") == 1);     // "Night-lit" is the second choice
        glows_on = simple_setup() || (recomp_get_config_u32("water_glow") == 0);
        moonlight_on = simple_setup() || (recomp_get_config_u32("moonlight") == 0);
        night_sounds_on = simple_setup() || (recomp_get_config_u32("night_sounds") == 0);
        lightning = (u8) (simple_setup() ? 1 : recomp_get_config_u32("lightning"));     // Gentle unless Custom says
        mist_on = simple_setup() || (recomp_get_config_u32("dawn_mist") == 0);
        if (!mist_on) {
            misty = 0;
        }
        shower_draw(by);
        aurora_draw(by);
        aurora_place(by);
    }
    weather_palettes_keep(by);      // the cycle's three ends, for both weathers
    sun_hours_apply();
    moon_phase_apply();
    star_fade = 255;
    dusk_last = 0xFFFFFFFFu;
    sky_paint_last = 0xFFFFFFFFu;
    sky_paint_skip = 0;
    cycle_minute = -1.0f;
    fog_far = by->fog_max;          // fog_near: weather_shares_apply
    // The set-up record ends in a byte the game leaves empty; it is marked here
    // so that a load is made night once. A fresh load brings it back empty.
    if (((volatile u8*) setup)[0x1F] != DONE_MARK) {
        volatile u8* light = COURSE_LIGHT;

        ((volatile u8*) setup)[0x1F] = DONE_MARK;
        // The course as the cartridge has it, the day end of every blend.
        day_fog[0] = setup->fogR;
        day_fog[1] = setup->fogG;
        day_fog[2] = setup->fogB;
        day_bg[0] = setup->backgroundR;
        day_bg[1] = setup->backgroundG;
        day_bg[2] = setup->backgroundB;
        day_fog_min = setup->fogDistanceMin;
        day_fog_max = setup->fogDistanceMax;
        for (i = 0; i < 3; i++) {
            day_ambient[i] = light[i];
            day_diffuse[i] = light[8 + i];
        }
        sun_y0_kept[0] = sun_y0_kept[1] = 0;
        sky_orig_cn_kept[0] = sky_orig_cn_kept[1] = 0;
        fine_reset();
        clear_maps();
        actor_reset();
        flow_y_sum = 0.0f;
        flow_y_n = 0;
        mist_said = 0;
        mist_said_frame = 0;
        mist_floor_n = 0;
        mist_pool_n = 0;
        mist_cam = NULL;
        mist_clip_ok = 0;
        mist_sky_said = 0;
        mist_parts_last = 0;
        mist_corners_last = 0;
        water_model = 0;
        glint_reset();
        sky_extent(by);
        for (i = 0; i < 2; i++) {
            if (by->pics[i] != 0) {
                s32 k;

                sky_at_night(by->pics[i], i);
                for (k = 0; k < SKY_TEXELS; k++) {
                    sky_mask_pic[i][k] = sky_mask[k];
                }
            }
        }
        if (by->setup == NIGHT_VALLEY_SETUP) {
            sky_masks_lift();
            sky_masks_meet();
        }
        sky_floor_make(by);
        {
            f32 m = mode_minutes();

            if (on_the_ride && cycle) {
                ride_minutes = m;       // the ride's first minute: its photos count from it
                wx_today(&ride_day, &ride_month);
            }
            moment_apply(m);
            world_stamp = on_the_ride ? stamp_of(world_weather) : photo_stamp;      // what the ride's photos are stamped with
        }
        recomp_printf("[island skies] the %s was loaded: %s%s\n", by->option,
                      cycle ? "the clock's hour" : daytime ? "by day" : dusk ? "the sun is setting" : dawn ? "the sun rises" : sunset ? "the sun sets" : "night falls",
                      storming ? ", with a storm" : raining ? ", with rain" : snowing ? ", with snow" : ashing ? ", with ash" : "");
        if (cycle) {
            u32 t = snap64_local_time();

            recomp_printf("[island skies] your clock says %u:%u%u\n", (t >> 16) & 0xFF, ((t >> 8) & 0xFF) / 10, ((t >> 8) & 0xFF) % 10);
            weather_log();
            if (on_the_ride) {
                ride_notice();
            }
        }
    } else {
        // A photo's world of the same load: the moment of its instant.
        moment_apply(mode_minutes());
    }
}

// The world is taken down: the light the courses share goes back to day.
RECOMP_HOOK("destroyWorld") void night_destroy_world(void) {
    if (night != NULL && glint_seg_n != 0) {
        recomp_printf("[island skies] the record at the end: %d corners, %d triangles of water, %d of land, %d parts%s%s%s\n",
                      (s32) glint_vtx_n, (s32) glint_tris, (s32) glint_land, (s32) glint_seg_n,
                      (glint_vtx_n > GLINT_VTX - 64) ? "; the corners FULL" : "",
                      (glint_op_n > GLINT_OPS - 64) ? "; the ops FULL" : "",
                      (glint_seg_n >= GLINT_SEGS) ? "; the parts FULL" : "");
    }
    if ((gfx_most[0] | gfx_most[1] | gfx_most[2] | gfx_most[3] | gfx_short_n) != 0) {
        recomp_printf("[island skies] the frame buffers: the mod's most in one frame %u, %u, %u and %u bytes (its budgets %u, %u, %u, %u); %u additions left out for room\n",
                      gfx_most[0], gfx_most[1], gfx_most[2], gfx_most[3], gfx_budget[0], gfx_budget[1], gfx_budget[2], gfx_budget[3], gfx_short_n);
    }
    gfx_most[0] = gfx_most[1] = gfx_most[2] = gfx_most[3] = 0;
    gfx_short_n = 0;
    gfx_short_said = 0;
    night = NULL;
    set_light(NULL);
    actor_reset();
    rain_quiet();
    thunder_at = body_at = echo_at = 0;
    chirp_next[0] = 240;
    chirp_next[1] = 330;
    chirp_next[2] = 420;
    chirp_said = 0;
}
