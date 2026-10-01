/* particles.cpp — class Particles of the original (analysis/gamelua-render-spec.md §8):
 * the native particle system behind the Lua global `particles.addParticles`. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

#define MAX_PARTICLES 125
#define MAX_DEFS      24
#define MAX_FRAMES    12
#define PI_F          3.14159274f

/* ParticleSystemData: particleTable.particles[type], parsed on first use and kept */
struct ParticleDef {
    char name[32];
    const AbSprite *sprites[MAX_FRAMES];
    int nsprites;
    bool animation;               /* animation == "lifeTime": the sprites are frames over the life */
    float minVel, maxVel, minAngleVel, maxAngleVel;
    float minScaleBegin, maxScaleBegin, minScaleEnd, maxScaleEnd;
    float lifeTime, gravityX, gravityY;
};

struct Particle {
    float x, y, xVel, yVel, angle, angleVel, scaleBegin, scaleEnd, scale, time, lifeTime;
    int frame;
    const AbSprite *sprite;
    const ParticleDef *def;
};

static ParticleDef sDefs[MAX_DEFS];
static int sDefCount;
static Particle sParticles[MAX_PARTICLES];
static int sCount;

static float field(lua_State *L, const char *k)
{
    lua_getfield(L, -1, k);
    float v = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static const ParticleDef *find_def(lua_State *L, const char *type)
{
    for (int i = 0; i < sDefCount; i++)
        if (!strcmp(sDefs[i].name, type))
            return &sDefs[i];
    if (sDefCount >= MAX_DEFS)
        return NULL;

    int top = lua_gettop(L);
    ParticleDef *d = NULL;
    env_push();
    lua_getfield(L, -1, "particleTable");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "particles");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, type);
            if (lua_istable(L, -1)) {
                char sheet[32] = "";
                d = &sDefs[sDefCount++];
                memset(d, 0, sizeof(*d));
                strncpy(d->name, type, sizeof(d->name) - 1);
                lua_getfield(L, -1, "sheet");
                if (lua_isstring(L, -1)) strncpy(sheet, lua_tostring(L, -1), sizeof(sheet) - 1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "animation");
                d->animation = lua_isstring(L, -1) && !strcmp(lua_tostring(L, -1), "lifeTime");
                lua_pop(L, 1);
                lua_getfield(L, -1, "sprites");
                if (lua_istable(L, -1)) {
                    int n = (int)lua_objlen(L, -1);
                    for (int i = 1; i <= n && d->nsprites < MAX_FRAMES; i++) {
                        lua_rawgeti(L, -1, i);
                        if (lua_isstring(L, -1))
                            d->sprites[d->nsprites++] = res_find_sprite(sheet, lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
                d->minVel = field(L, "minVel");
                d->maxVel = field(L, "maxVel");
                d->minAngleVel = field(L, "minAngleVel");
                d->maxAngleVel = field(L, "maxAngleVel");
                d->minScaleBegin = field(L, "minScaleBegin");
                d->maxScaleBegin = field(L, "maxScaleBegin");
                d->minScaleEnd = field(L, "minScaleEnd");
                d->maxScaleEnd = field(L, "maxScaleEnd");
                d->lifeTime = field(L, "lifeTime");
                d->gravityX = field(L, "gravityX");
                d->gravityY = field(L, "gravityY");
            }
        }
    }
    lua_settop(L, top);
    return d;
}

/* particles.addParticles(type, amount, x, y, w, h, angle) @001185c0; x, y, w, h in world pixels */
static int l_addParticles(lua_State *L)
{
    const char *type = lua_tostring(L, 1);
    float amount = (float)lua_tonumber(L, 2), x = (float)lua_tonumber(L, 3), y = (float)lua_tonumber(L, 4);
    float w = (float)lua_tonumber(L, 5), h = (float)lua_tonumber(L, 6);
    if (!type)
        return 0;
    float total = (float)sCount + amount;
    if (total > 40.0f) { amount *= 0.5f; total = (float)sCount + amount; }      /* soft limit */
    if (total > 125.0f) amount = (float)(MAX_PARTICLES - sCount);               /* hard limit */
    if (!(amount > 0.0f))
        return 0;
    const ParticleDef *def = find_def(L, type);
    if (!def || def->nsprites == 0)
        return 0;
    int i = 0;
    do {
        if (sCount >= MAX_PARTICLES) break;
        Particle *p = &sParticles[sCount];
        p->x = x + (ab_rnd() - 0.5f) * w;
        p->y = y + (ab_rnd() - 0.5f) * h;
        p->xVel = def->minVel + (def->maxVel - def->minVel) * ab_rnd();
        p->yVel = def->minVel + (def->maxVel - def->minVel) * ab_rnd();
        p->angle = 2.0f * (ab_rnd() * PI_F);
        p->angleVel = def->minAngleVel + (def->maxAngleVel - def->minAngleVel) * ab_rnd();
        p->scaleBegin = def->minScaleBegin + (def->maxScaleBegin - def->minScaleBegin) * ab_rnd();
        p->scaleEnd = def->minScaleEnd + (def->maxScaleEnd - def->minScaleEnd) * ab_rnd();
        p->scale = p->scaleBegin;
        p->time = 0.0f;
        p->lifeTime = def->lifeTime;
        p->frame = 1;
        if (def->animation) {
            p->sprite = def->sprites[0];
        } else {
            int k = (int)(ab_rnd() * (float)def->nsprites);
            if (k >= def->nsprites) k = def->nsprites - 1;
            p->sprite = def->sprites[k];
        }
        p->def = def;
        sCount++;
        i++;
    } while (amount > (float)i);
    return 0;
}

/* Particles::update @00118298 */
void particles_update(float dt)
{
    int i = 0;
    while (i < sCount) {
        Particle *p = &sParticles[i];
        p->time += dt;
        if (p->time > p->lifeTime || p->y > 0.0f) {       /* expired, or below the ground */
            memmove(p, p + 1, sizeof(Particle) * (size_t)(sCount - i - 1));
            sCount--;
            continue;
        }
        p->yVel += dt * p->def->gravityY;
        p->xVel += dt * p->def->gravityX;
        p->x += dt * p->xVel;
        p->y += dt * p->yVel;
        float t = p->time / p->lifeTime;
        p->angle += dt * p->angleVel;
        p->scale = p->scaleBegin + (p->scaleEnd - p->scaleBegin) * t;
        if (p->def->animation) {
            int nF = p->def->nsprites, idx = (int)ceilf((float)nF * t);
            if (idx < 1) idx = 1;
            if (idx > nF) idx = nF;
            if (idx != p->frame) {
                p->sprite = p->def->sprites[idx - 1];
                p->frame = idx;
            }
        }
        i++;
    }
}

/* Particles::draw @00117e88 */
void particles_draw(void)
{
    static const Anchor pivot = { V_PIVOT, H_PIVOT };
    for (int i = 0; i < sCount; i++) {
        const Particle *p = &sParticles[i];
        if (!p->sprite || !(p->scale > 0.0f))
            continue;                                      /* scale 0: the original's quad is NaN and culled */
        float c = cosf(p->angle), s = sinf(p->angle);
        gRS.tx = -G.topLeftX / p->scale;
        gRS.ty = -G.topLeftY / p->scale;
        gRS.sx = gRS.sy = G.worldScale * p->scale;
        gRS.m00 = c; gRS.m11 = c; gRS.m10 = s; gRS.m01 = -s;
        gRS.px = (float)p->sprite->pivot_x;
        gRS.py = (float)p->sprite->pivot_y;
        render_sprite(p->sprite, p->x / p->scale, p->y / p->scale, pivot);
    }
}

void particles_clear(void) { sCount = 0; }

void particles_init(lua_State *L)
{
    static const luaL_Reg lib[] = { { "addParticles", l_addParticles }, { NULL, NULL } };
    sCount = 0;
    sDefCount = 0;
    luaL_register(L, "particles", lib);        /* a real global, reached as _G.particles */
    lua_pop(L, 1);
}
