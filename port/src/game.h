/* game.h — shared state of the GameLua re-implementation (game.cpp, game_render.cpp,
 * particles.cpp). Follows analysis/gamelua-physics-spec.md and gamelua-render-spec.md. */
#ifndef AB_GAME_H
#define AB_GAME_H

#include <Box2D/Box2D.h>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#include "assets.h"
#include "render.h"
#include "resources.h"
#include "platform.h"

struct Snap { float x, y, angle; };

/* RenderObjectData in the original: one per named physics object. */
struct ObjData {
    int      tableRef;            /* registry ref of the object's Lua table (objects.world[name]) */
    b2Body  *body;
    const AbSprite *sprite;       /* NULL: never drawn */
    AbSheet *texture;             /* setTexture: sheet whose image fills the (static) block */
    Snap     snap[2];
    float    radius;              /* circle radius, -1 for boxes */
    float    renderX, renderY, renderAngle;   /* physics units, interpolated */
    bool     controllable, isCircle, front;
    int      material;            /* rolling sound: 0 none, 1 wood, 2 rock, 3 light */
    /* port: the block's ground texture composited with its sprite's alpha (game_render.cpp) */
    int      maskSlot;            /* -1: not built */
    float    maskX, maskY, maskAngle;
};

/* lang::Hashtable<String, RenderObjectData>: its bucket/chain order is the draw order
 * inside each pass of drawGame, so the structure is reproduced exactly. */
struct ObjNode {
    char     key[48];
    ObjData  d;
    ObjNode *next;
    bool     used;
};

struct ObjIter { int bucket; ObjNode *node; };
ObjNode *obj_first(ObjIter *it);            /* used entries, in the original's iteration order */
ObjNode *obj_next(ObjIter *it);
ObjData *obj_find(const char *name);        /* NULL if there is no such object */
int      obj_count(void);

struct JointData {
    char     name[48], end1[48], end2[48];
    b2Joint *joint;
};
#define MAX_JOINTS 64

struct Game {
    lua_State *L;
    int envRef;                   /* the script environment table T */
    int objectsRef, clippedTextRef, cursorRef, keyPressedRef, keyReleasedRef, keyHoldRef;
    b2World *world;
    JointData joints[MAX_JOINTS];
    int jointCount;
    uint32_t bgColor;             /* 0xRRGGBB */
    int slot;
    bool physicsEnabled, loading, editing;
    float topLeftX, topLeftY;     /* world px at the screen's top-left */
    float acc;
    float worldScale;
    int limitLeft, limitRight;
    float appScale, appPrevScale; /* pinch zoom feed (GameApp +0x27c / +0x280) */
    bool wantExit;
    bool failed;                  /* a script error stopped the game */
};

extern Game G;

/* Lua helpers on the script environment table. */
void env_push(void);
int  env_call(const char *fn, int nargs, int nresults);   /* args on the stack; 0 on success */
float env_number(const char *name, float def);

/* game_render.cpp */
void render_register(lua_State *L);
void render_level_reset(void);              /* loadLevel: trajectories, block textures */
void render_object_removed(ObjData *d);

/* particles.cpp */
void particles_init(lua_State *L);
void particles_update(float dt);
void particles_draw(void);
void particles_clear(void);

/* shared random source for native code (math::RandomUtil::random: [0, 1]) */
float ab_rnd(void);

#endif
