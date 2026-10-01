/* game.cpp — GameLua / GameApp of Angry Birds 1.0, re-implemented: the script environment,
 * the physics objects, the collision rules, the fixed-step update and persistence.
 *
 * Behaviour follows analysis/gamelua-physics-spec.md (addresses in comments are those of
 * the original functions). Rendering natives are in game_render.cpp. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "numfmt.h"
#include "gfx.h"

extern "C" void ab_open_libs(lua_State *L);

Game G;
#ifdef AB_NANO
uint32_t game_stat_physics_us;
#endif

/* ------------------------------------------------------------------------------------ */
/* Lua helpers                                                                          */
/* ------------------------------------------------------------------------------------ */

void env_push(void) { lua_rawgeti(G.L, LUA_REGISTRYINDEX, G.envRef); }

static void script_error(const char *where)
{
    plat_log("LUA ERROR in %s: %s", where, lua_tostring(G.L, -1) ? lua_tostring(G.L, -1) : "?");
    lua_pop(G.L, 1);
}

/* Call T[fn] with the nargs values on top of the stack. A missing function is not an error
 * (the arguments are dropped). */
int env_call(const char *fn, int nargs, int nresults)
{
    lua_State *L = G.L;
    env_push();
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1 + nargs);
        return 1;
    }
    lua_insert(L, -1 - nargs);
    if (lua_pcall(L, nargs, nresults, 0)) {
        script_error(fn);
        return 2;
    }
    return 0;
}

float env_number(const char *name, float def)
{
    lua_State *L = G.L;
    float v = def;
    env_push();
    lua_getfield(L, -1, name);
    if (lua_isnumber(L, -1)) v = (float)lua_tonumber(L, -1);
    lua_pop(L, 2);
    return v;
}

static void env_set_number(const char *name, float v)
{
    env_push();
    lua_pushnumber(G.L, v);
    lua_setfield(G.L, -2, name);
    lua_pop(G.L, 1);
}

static void env_set_bool(const char *name, bool v)
{
    env_push();
    lua_pushboolean(G.L, v);
    lua_setfield(G.L, -2, name);
    lua_pop(G.L, 1);
}

static bool tbl_isnumber(lua_State *L, int idx, const char *k)
{
    lua_getfield(L, idx, k);
    bool r = lua_isnumber(L, -1) != 0;
    lua_pop(L, 1);
    return r;
}

static float tbl_number(lua_State *L, int idx, const char *k)
{
    lua_getfield(L, idx, k);
    float v = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static void tbl_set_number(lua_State *L, int idx, const char *k, float v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, idx < 0 ? idx - 1 : idx, k);
}

static void tbl_set_bool(lua_State *L, int idx, const char *k, bool v)
{
    lua_pushboolean(L, v);
    lua_setfield(L, idx < 0 ? idx - 1 : idx, k);
}

static void tbl_set_string(lua_State *L, int idx, const char *k, const char *v)
{
    lua_pushstring(L, v);
    lua_setfield(L, idx < 0 ? idx - 1 : idx, k);
}

static const char *arg_s(lua_State *L, int i) { const char *s = lua_tostring(L, i); return s ? s : ""; }
static float arg_n(lua_State *L, int i) { return (float)lua_tonumber(L, i); }
static bool arg_b(lua_State *L, int i) { return lua_toboolean(L, i) != 0; }

/* math::RandomUtil::random(): [0, 1] */
float ab_rnd(void)
{
    static uint32_t s = 0x9E3779B9u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (float)(s >> 8) * (1.0f / 16777215.0f);
}

/* ------------------------------------------------------------------------------------ */
/* The object table (lang::Hashtable<String, RenderObjectData>, GameLua+0xb8)           */
/* ------------------------------------------------------------------------------------ */

static const int kSizes[] = { 37, 67, 131, 257, 521, 1031, 2053, 4099, 8209, 16411 };
static ObjNode *sTable;
static int sCap, sCount, sThreshold;

/* lang::String::hashCode @0004bc74 */
static uint32_t hash_code(const char *s)
{
    if (!*s) return 0;
    uint32_t h = (uint32_t)(int32_t)(signed char)s[0];
    for (s++; *s; s++)
        h = h * 31u + (uint32_t)(int32_t)(signed char)*s;
    return h;
}

static void data_default(ObjData *d)
{
    memset(d, 0, sizeof(*d));
    d->tableRef = LUA_NOREF;
    d->maskSlot = -1;
}

/* getPair @000192d8: the key's entry, or a claimed unused slot of its chain (not yet "used"). */
static ObjNode *get_pair(ObjNode *table, int cap, const char *key)
{
    ObjNode *head = &table[(hash_code(key) & 0x7fffffffu) % (uint32_t)cap], *spare = NULL;
    for (ObjNode *e = head; e; e = e->next) {
        if (e->used) {
            if (!strcmp(e->key, key)) return e;
        } else {
            spare = e;                         /* the last unused node of the chain wins */
        }
    }
    if (!spare) {
        spare = (ObjNode *)calloc(1, sizeof(ObjNode));
        spare->next = head->next;              /* new overflow nodes go right after the head */
        head->next = spare;
    }
    strncpy(spare->key, key, sizeof(spare->key) - 1);
    spare->key[sizeof(spare->key) - 1] = 0;
    data_default(&spare->d);
    return spare;
}

static void table_grow(void)
{
    int ncap = sCap;
    for (unsigned i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++)
        if (sCap < kSizes[i]) { ncap = kSizes[i]; break; }
    if (ncap == sCap) ncap = (sCap + 129403) | 1;
    ObjNode *ntab = (ObjNode *)calloc((size_t)ncap, sizeof(ObjNode));
    for (int b = 0; b < sCap; b++) {
        ObjNode *e = &sTable[b];
        while (e) {
            ObjNode *next = e->next;
            if (e->used) {
                ObjNode *p = get_pair(ntab, ncap, e->key);
                p->d = e->d;
                p->used = true;
                if (p->d.body) p->d.body->SetUserData(p);
            }
            if (e != &sTable[b]) free(e);
            e = next;
        }
    }
    free(sTable);
    sTable = ntab;
    sCap = ncap;
    sThreshold = (int)((float)ncap * 0.75f);
}

/* Insert (or find) the entry for `name`; the caller fills in the data. */
static ObjNode *table_insert(const char *name)
{
    if (sThreshold <= sCount + 1)
        table_grow();
    return get_pair(sTable, sCap, name);
}

/* Like the original's lookups, a miss still claims a slot in the chain. */
static ObjNode *table_lookup(const char *name)
{
    if (sCap < 1) return NULL;
    ObjNode *e = get_pair(sTable, sCap, name);
    return e->used ? e : NULL;
}

static void data_release(ObjData *d)
{
    if (d->tableRef != LUA_NOREF)
        luaL_unref(G.L, LUA_REGISTRYINDEX, d->tableRef);
    render_object_removed(d);
    data_default(d);
}

static void table_remove(const char *name)
{
    if (sCap < 1) return;
    ObjNode *head = &sTable[(hash_code(name) & 0x7fffffffu) % (uint32_t)sCap], *prev = NULL;
    for (ObjNode *e = head; e;) {
        ObjNode *next = e->next;
        if (e->used && !strcmp(e->key, name)) {
            data_release(&e->d);
            e->used = false;
            e->key[0] = 0;
            sCount--;
            if (e != head) {                   /* overflow nodes are unlinked and freed */
                prev->next = next;
                free(e);
                e = next;
                continue;
            }
        }
        prev = e;
        e = next;
    }
}

/* clear() @00019ea0 keeps the capacity and the overflow nodes. */
static void table_clear(void)
{
    for (int b = 0; b < sCap; b++)
        for (ObjNode *e = &sTable[b]; e; e = e->next)
            if (e->used) {
                data_release(&e->d);
                e->used = false;
                e->key[0] = 0;
                sCount--;
            }
}

ObjNode *obj_first(ObjIter *it)
{
    it->bucket = -1;
    it->node = NULL;
    return obj_next(it);
}

ObjNode *obj_next(ObjIter *it)
{
    for (;;) {
        if (it->node) it->node = it->node->next;
        while (!it->node) {
            if (++it->bucket >= sCap) return NULL;
            it->node = &sTable[it->bucket];
            if (it->node->used) return it->node;
            it->node = it->node->next;
        }
        if (it->node->used) return it->node;
    }
}

ObjData *obj_find(const char *name)
{
    ObjNode *e = table_lookup(name);
    return e ? &e->d : NULL;
}

int obj_count(void) { return sCount; }

/* ------------------------------------------------------------------------------------ */
/* Contacts                                                                             */
/* ------------------------------------------------------------------------------------ */

/* Fixture user data of a box created with collision == false (GameLua::userData). */
static struct { char name[4]; bool collides; } sNoCollide = { "", false };

static const char *body_name(b2Body *b)
{
    ObjNode *n = (ObjNode *)b->GetUserData();
    return (n && n->used && n->d.body == b) ? n->key : "";
}

static ObjData *body_data(b2Body *b)
{
    ObjNode *n = (ObjNode *)b->GetUserData();
    return (n && n->used && n->d.body == b) ? &n->d : NULL;
}

/* objects.world on the stack (or nil) */
static void push_world(lua_State *L)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, G.objectsRef);
    lua_getfield(L, -1, "world");
    lua_remove(L, -2);
}

/* strength/defence bookkeeping of one side of a block-block hit. Table at the stack top. */
static bool block_damage(lua_State *L, float F, float *total)
{
    if (!tbl_isnumber(L, -1, "strength"))
        return true;
    bool hasDef = tbl_isnumber(L, -1, "defence");
    float def = hasDef ? tbl_number(L, -1, "defence") : 0.0f;
    if (hasDef && F < def)
        return false;
    float s = tbl_number(L, -1, "strength"), dmg = F - def, ns = s - dmg;
    tbl_set_number(L, -1, "strength", ns);
    *total += (ns < 0.0f) ? s : dmg;           /* capped at the strength that was left */
    return true;
}

class ContactHooks : public b2ContactListener, public b2ContactFilter {
public:
    /* GameLua::ShouldCollide @00006d78 */
    bool ShouldCollide(b2Fixture *fA, b2Fixture *fB)
    {
        bool a = fA->GetUserData() ? sNoCollide.collides : true;
        if (!fB->GetUserData()) return a;
        return a && sNoCollide.collides;
    }
    bool RayCollide(void *, b2Fixture *) { return false; }

    /* GameLua::BeginContact @0000cbec: damage from the bodies' momenta when a contact starts. */
    void BeginContact(b2Contact *c)
    {
        if (G.loading || G.editing) return;
        lua_State *L = G.L;
        b2Body *bA = c->GetFixtureA()->GetBody(), *bB = c->GetFixtureB()->GetBody();
        ObjData *dA = body_data(bA), *dB = body_data(bB);
        if (!dA || !dB) return;                 /* unknown body: its name is "" and world[""] is no table */
        char nameA[48], nameB[48];
        strcpy(nameA, body_name(bA));
        strcpy(nameB, body_name(bB));
        int top = lua_gettop(L);

        push_world(L);
        if (!lua_istable(L, -1)) { lua_settop(L, top); return; }
        lua_getfield(L, -1, nameA);
        lua_getfield(L, -2, nameB);
        if (!lua_istable(L, -1) || !lua_istable(L, -2)) { lua_settop(L, top); return; }
        lua_pop(L, 2);                          /* world stays at top+1 */

        b2Vec2 vA = bA->GetLinearVelocity(), vB = bB->GetLinearVelocity();
        float mA = bA->GetMass(), mB = bB->GetMass();

        if (!dA->controllable && !dB->controllable) {
            /* ---- block vs block ---- */
            float dx = mA * vA.x - mB * vB.x, dy = mA * vA.y - mB * vB.y;
            float F = sqrtf(dy * dy + dx * dx) / 10.0f;
            float score = 0.0f, total = 0.0f;
            env_push();
            lua_getfield(L, -1, "scoreTable");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "blocks");
                if (lua_istable(L, -1)) score = tbl_number(L, -1, "score");
                lua_pop(L, 1);
            }
            lua_pop(L, 2);

            lua_rawgeti(L, LUA_REGISTRYINDEX, dA->tableRef);
            bool flagA = block_damage(L, F, &total);
            lua_pop(L, 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, dB->tableRef);
            bool flagB = block_damage(L, F, &total);
            lua_pop(L, 1);

            lua_pushstring(L, nameA);
            lua_pushstring(L, nameB);
            lua_pushnumber(L, F);
            lua_pushboolean(L, flagA || flagB);
            env_call("blockCollision", 4, 0);
            if (total > 0.0f) {
                env_push();
                lua_getfield(L, -1, "scoreTable");
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "blocks");
                    if (lua_istable(L, -1))
                        tbl_set_number(L, -1, "score", score + floorf(total) * 10.0f);
                }
            }
            lua_settop(L, top);
            return;
        }

        if (dA->controllable && dB->controllable) {
            /* ---- bird vs bird ---- */
            float sA = sqrtf(vA.y * vA.y + vA.x * vA.x), sB = sqrtf(vB.y * vB.y + vB.x * vB.x);
            float F = (sA > sB) ? (sA * mA) / 10.0f : (sB * mB) / 10.0f;
            lua_pushstring(L, nameA);
            lua_pushstring(L, nameB);
            lua_pushnumber(L, F);
            lua_pushnumber(L, 0);
            env_call("birdCollision", 4, 0);
            lua_settop(L, top);
            return;
        }

        /* ---- bird vs block ---- */
        bool aIsBird = dA->controllable;
        const char *bird = aIsBird ? nameA : nameB, *other = aIsBird ? nameB : nameA;
        b2Body *bb = aIsBird ? bA : bB;
        ObjData *dBird = aIsBird ? dA : dB, *dOther = aIsBird ? dB : dA;
        char birdMat[24] = "", otherMat[24] = "";
        lua_rawgeti(L, LUA_REGISTRYINDEX, dBird->tableRef);
        lua_getfield(L, -1, "material");
        if (lua_isstring(L, -1)) strncpy(birdMat, lua_tostring(L, -1), sizeof(birdMat) - 1);
        lua_pop(L, 2);
        lua_rawgeti(L, LUA_REGISTRYINDEX, dOther->tableRef);
        lua_getfield(L, -1, "material");
        if (lua_isstring(L, -1)) strncpy(otherMat, lua_tostring(L, -1), sizeof(otherMat) - 1);
        lua_pop(L, 2);

        b2Vec2 v = bb->GetLinearVelocity();
        float mass = bb->GetMass();
        float F = mass * sqrtf(v.y * v.y + v.x * v.x) / 10.0f;
        float cdf = env_number("colorDamageFactor", 1.0f), ybf = env_number("yellowBirdFactor", 1.0f),
              bbf = env_number("blueBirdFactor", 1.0f);
        if (!strcmp(birdMat, "wood") && !strcmp(otherMat, "wood")) F = F * (cdf * ybf);
        if (!strcmp(birdMat, "rock") && !strcmp(otherMat, "rock")) F = F * cdf;
        if (!strcmp(birdMat, "light") && !strcmp(otherMat, "light")) F = F * (cdf * bbf);

        float damage = 0.0f;
        lua_getfield(L, top + 1, other);        /* objects.world[other] */
        if (lua_istable(L, -1) && tbl_isnumber(L, -1, "strength")) {
            float def = tbl_isnumber(L, -1, "defence") ? tbl_number(L, -1, "defence") : 0.0f;
            damage = F - def;
            if (damage > 0.0f) {
                float s = tbl_number(L, -1, "strength"), ns = s - damage;
                tbl_set_number(L, -1, "strength", ns);
                if (ns < 0.0f) {
                    /* the block breaks: the bird keeps part of its speed and passes through */
                    float k = ((-ns / mass) / F) * 10.0f * 1.75f;
                    if (k > 1.0f) k = 1.0f;
                    bb->SetLinearVelocity(b2Vec2(k * v.x, k * v.y));
                    c->SetEnabled(false);
                    damage = s;
                }
            }
        }
        lua_pop(L, 1);
        lua_pushstring(L, bird);
        lua_pushstring(L, other);
        lua_pushnumber(L, F);
        lua_pushnumber(L, floorf(damage));
        env_call("birdCollision", 4, 0);
        lua_settop(L, top);
    }
};

static ContactHooks *sHooks;

/* ------------------------------------------------------------------------------------ */
/* Object natives                                                                       */
/* ------------------------------------------------------------------------------------ */

/* GameLua::createBox @000079f8 / createCircle @00007bfc */
static b2Body *make_body(float x, float y, float density)
{
    b2BodyDef bd;
    bd.position.Set(x, y);
    bd.angle = 0.0f;
    bd.linearDamping = 0.0f;
    bd.angularDamping = 1.0f;
    bd.autoSleep = true;
    bd.awake = true;
    bd.fixedRotation = false;
    bd.bullet = false;
    bd.active = true;
    bd.type = (density != 0.0f) ? b2_dynamicBody : b2_staticBody;
    bd.inertiaScale = 1.0f;
    return G.world->CreateBody(&bd);
}

/* Common tail of createBoxLua @000129d0 / createCircleLua @00013034. The object's Lua table
 * is on the stack top; it is stored as objects.world[name] and the table entry is made. */
static void register_object(lua_State *L, const char *name, const char *sprite, b2Body *body, float x, float y,
                            float radius, bool controllable, bool circle)
{
    int t = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, G.objectsRef);
    lua_getfield(L, -1, "world");
    if (!lua_istable(L, -1)) {                 /* the script creates it before any object */
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "world");
    }
    lua_pushvalue(L, t);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);

    ObjNode *n = table_insert(name);
    if (n->used) {
        /* duplicate name: the entry is overwritten and the old body stays in the world, nameless */
        if (n->d.body) n->d.body->SetUserData(NULL);
        data_release(&n->d);
    } else {
        n->used = true;
        sCount++;
    }
    ObjData *d = &n->d;
    lua_pushvalue(L, t);
    d->tableRef = luaL_ref(L, LUA_REGISTRYINDEX);
    d->body = body;
    body->SetUserData(n);
    d->sprite = *sprite ? res_find_sprite("", sprite) : NULL;
    d->texture = NULL;
    d->snap[0].x = d->snap[1].x = d->renderX = x;
    d->snap[0].y = d->snap[1].y = d->renderY = y;
    d->snap[0].angle = d->snap[1].angle = d->renderAngle = 0.0f;
    d->radius = radius;
    d->controllable = controllable;
    d->isCircle = circle;
    d->front = false;
    d->material = 0;
    lua_settop(L, t - 1);
}

/* createBox(name, sprite, x, y, halfW, halfH, density, friction, restitution, collision, controllable) */
static int l_createBox(lua_State *L)
{
    const char *name = arg_s(L, 1), *sprite = arg_s(L, 2);
    float x = arg_n(L, 3), y = arg_n(L, 4), hw = arg_n(L, 5), hh = arg_n(L, 6);
    float density = arg_n(L, 7), friction = arg_n(L, 8), restitution = arg_n(L, 9);
    bool collision = arg_b(L, 10), controllable = arg_b(L, 11);

    b2Body *body = make_body(x, y, density);
    b2PolygonShape shape;
    shape.SetAsBox(hw, hh);
    b2FixtureDef fd;
    fd.shape = &shape;
    fd.friction = friction;
    fd.restitution = restitution;
    fd.density = density;
    fd.userData = collision ? NULL : (void *)&sNoCollide;
    body->CreateFixture(&fd);

    lua_newtable(L);
    tbl_set_string(L, -1, "name", name);
    tbl_set_string(L, -1, "sprite", sprite);
    tbl_set_string(L, -1, "type", "box");
    tbl_set_number(L, -1, "x", x);
    tbl_set_number(L, -1, "y", y);
    tbl_set_number(L, -1, "width", hw);
    tbl_set_number(L, -1, "height", hh);
    tbl_set_number(L, -1, "angle", 0.0f);
    tbl_set_number(L, -1, "density", density);
    tbl_set_number(L, -1, "friction", friction);
    tbl_set_number(L, -1, "restitution", restitution);
    tbl_set_number(L, -1, "mass", body->GetMass());
    tbl_set_number(L, -1, "xVel", 0.0f);
    tbl_set_number(L, -1, "yVel", 0.0f);
    register_object(L, name, sprite, body, x, y, -1.0f, controllable, false);
    return 0;
}

/* createCircle(name, sprite, x, y, radius, density, friction, restitution, controllable) */
static int l_createCircle(lua_State *L)
{
    const char *name = arg_s(L, 1), *sprite = arg_s(L, 2);
    float x = arg_n(L, 3), y = arg_n(L, 4), radius = arg_n(L, 5);
    float density = arg_n(L, 6), friction = arg_n(L, 7), restitution = arg_n(L, 8);
    bool controllable = arg_b(L, 9);

    b2Body *body = make_body(x, y, density);
    b2CircleShape shape;
    shape.m_p.Set(0.0f, 0.0f);
    shape.m_radius = radius;
    b2FixtureDef fd;
    fd.shape = &shape;
    fd.friction = friction;
    fd.restitution = restitution;
    fd.density = density;
    body->CreateFixture(&fd);

    lua_newtable(L);
    tbl_set_string(L, -1, "name", name);
    tbl_set_string(L, -1, "sprite", sprite);
    tbl_set_string(L, -1, "type", "circle");
    tbl_set_number(L, -1, "x", x);
    tbl_set_number(L, -1, "y", y);
    tbl_set_number(L, -1, "radius", radius);
    tbl_set_number(L, -1, "angle", 0.0f);
    tbl_set_number(L, -1, "density", density);
    tbl_set_number(L, -1, "friction", friction);
    tbl_set_number(L, -1, "restitution", restitution);
    tbl_set_number(L, -1, "mass", body->GetMass());
    tbl_set_number(L, -1, "xVel", 0.0f);
    tbl_set_number(L, -1, "yVel", 0.0f);
    tbl_set_bool(L, -1, "sphere", true);
    register_object(L, name, sprite, body, x, y, radius, controllable, true);
    if (controllable)
        body->SetAngularDamping(2.0f);
    return 0;
}

/* objects.joints on the stack (created if the script hasn't) */
static void push_joints(lua_State *L)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, G.objectsRef);
    lua_getfield(L, -1, "joints");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "joints");
    }
    lua_remove(L, -2);
}

/* createJoint(name, end1, end2, type) @0000bff8: type 1 = distance joint between the body origins */
static int l_createJoint(lua_State *L)
{
    const char *name = arg_s(L, 1), *end1 = arg_s(L, 2), *end2 = arg_s(L, 3);
    float type = arg_n(L, 4);
    ObjData *a = obj_find(end1), *b = obj_find(end2);
    if (type != 1.0f || !a || !b || G.jointCount >= MAX_JOINTS)
        return 0;                               /* the original crashes on these */
    b2DistanceJointDef def;
    def.bodyA = a->body;
    def.bodyB = b->body;
    def.collideConnected = false;
    def.localAnchorA.Set(0.0f, 0.0f);
    def.localAnchorB.Set(0.0f, 0.0f);
    b2Vec2 p1 = a->body->GetWorldPoint(b2Vec2(0.0f, 0.0f)), p2 = b->body->GetWorldPoint(b2Vec2(0.0f, 0.0f));
    def.length = (p2 - p1).Length();
    def.frequencyHz = 4.0f;
    def.dampingRatio = 0.5f;

    JointData *jd = &G.joints[G.jointCount++];
    memset(jd, 0, sizeof(*jd));
    strncpy(jd->name, name, sizeof(jd->name) - 1);
    strncpy(jd->end1, end1, sizeof(jd->end1) - 1);
    strncpy(jd->end2, end2, sizeof(jd->end2) - 1);
    jd->joint = G.world->CreateJoint(&def);

    push_joints(L);
    lua_newtable(L);
    tbl_set_string(L, -1, "name", name);
    tbl_set_string(L, -1, "end1", end1);
    tbl_set_string(L, -1, "end2", end2);
    tbl_set_number(L, -1, "type", type);
    tbl_set_number(L, -1, "x1", p1.x);
    tbl_set_number(L, -1, "x2", p2.x);
    tbl_set_number(L, -1, "y1", p1.y);
    tbl_set_number(L, -1, "y2", p2.y);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

static void joint_forget(lua_State *L, int i)
{
    push_joints(L);
    lua_pushnil(L);
    lua_setfield(L, -2, G.joints[i].name);
    lua_pop(L, 1);
    memmove(&G.joints[i], &G.joints[i + 1], sizeof(JointData) * (size_t)(G.jointCount - i - 1));
    G.jointCount--;
}

static int l_destroyJoint(lua_State *L)
{
    const char *name = arg_s(L, 1);
    for (int i = 0; i < G.jointCount; i++)
        if (!strcmp(G.joints[i].name, name)) {
            G.world->DestroyJoint(G.joints[i].joint);
            joint_forget(L, i);
            break;
        }
    return 0;
}

/* removeObject(name) @00012578 */
static int l_removeObject(lua_State *L)
{
    char name[48];
    strncpy(name, arg_s(L, 1), sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    ObjData *d = obj_find(name);
    if (d) {
        G.world->DestroyBody(d->body);          /* takes its Box2D joints and contacts with it */
        table_remove(name);
    }
    for (int i = 0; i < G.jointCount;)
        if (!strcmp(G.joints[i].end1, name) || !strcmp(G.joints[i].end2, name))
            joint_forget(L, i);
        else
            i++;
    return 0;
}

/* objects.world[name].<field> = v */
static void world_field(lua_State *L, const char *name, const char *f1, float v1, const char *f2, float v2)
{
    push_world(L);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, name);
        if (lua_istable(L, -1)) {
            tbl_set_number(L, -1, f1, v1);
            if (f2) tbl_set_number(L, -1, f2, v2);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int l_setPosition(lua_State *L)
{
    const char *name = arg_s(L, 1);
    float x = arg_n(L, 2), y = arg_n(L, 3);
    ObjData *d = obj_find(name);
    if (!d) return 0;
    d->body->SetTransform(b2Vec2(x, y), d->body->GetAngle());
    world_field(L, name, "x", x, "y", y);
    d->snap[0].x = d->snap[1].x = d->renderX = x;
    d->snap[0].y = d->snap[1].y = d->renderY = y;
    return 0;
}

static int l_setRotation(lua_State *L)
{
    const char *name = arg_s(L, 1);
    float a = arg_n(L, 2);
    ObjData *d = obj_find(name);
    if (!d) return 0;
    d->body->SetTransform(d->body->GetPosition(), a);
    world_field(L, name, "angle", a, NULL, 0.0f);
    d->snap[0].angle = d->snap[1].angle = d->renderAngle = a;
    return 0;
}

static int l_setVelocity(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    if (d) d->body->SetLinearVelocity(b2Vec2(arg_n(L, 2), arg_n(L, 3)));     /* no-op on static bodies, no wake */
    return 0;
}

static int l_setAngularVelocity(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    if (d) d->body->SetAngularVelocity(arg_n(L, 2));
    return 0;
}

static int l_applyImpulse(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    if (d) d->body->ApplyImpulse(b2Vec2(arg_n(L, 2), arg_n(L, 3)), b2Vec2(arg_n(L, 4), arg_n(L, 5)));
    return 0;
}

static int l_applyForce(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    if (d) d->body->ApplyForce(b2Vec2(arg_n(L, 2), arg_n(L, 3)), b2Vec2(arg_n(L, 4), arg_n(L, 5)));
    return 0;
}

static int l_setSleeping(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    if (d) d->body->SetAwake(!arg_b(L, 2));
    return 0;
}

static int l_setMaterial(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    const char *m = arg_s(L, 2);
    if (!d) return 0;
    if (!strcmp(m, "wood")) d->material = 1;
    else if (!strcmp(m, "rock")) d->material = 2;
    else if (!strcmp(m, "light")) d->material = 3;
    return 0;
}

static int l_setSprite(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    if (d) d->sprite = res_find_sprite("", arg_s(L, 2));
    return 0;
}

static int l_setTexture(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    if (d) {
        d->texture = assets_find_sheet(arg_s(L, 2));
        render_object_removed(d);               /* rebuild its composite */
    }
    return 0;
}

/* setObjectParameter(name, id, value): 1 = "front" draw flag, 2 = static (0) / dynamic body */
static int l_setObjectParameter(lua_State *L)
{
    ObjData *d = obj_find(arg_s(L, 1));
    int id = (int)arg_n(L, 2);
    float value = arg_n(L, 3);
    if (!d) return 0;
    if (id == 1) d->front = ((int)value == 1);
    else if (id == 2) d->body->SetType(value == 0.0f ? b2_staticBody : b2_dynamicBody);
    return 0;
}

static int l_setLevelLimits(lua_State *L)
{
    G.limitLeft = (int)arg_n(L, 1);
    G.limitRight = (int)arg_n(L, 3);
    return 0;
}

static int l_setPhysicsEnabled(lua_State *L) { G.physicsEnabled = arg_b(L, 1); return 0; }
static int l_setEditing(lua_State *L) { G.editing = arg_b(L, 1); return 0; }
static int l_setTopLeft(lua_State *L) { G.topLeftX = arg_n(L, 1); G.topLeftY = arg_n(L, 2); return 0; }
static int l_setWorldScale(lua_State *L) { G.worldScale = G.appScale = G.appPrevScale = arg_n(L, 1); return 0; }
static int l_requestExit(lua_State *L) { (void)L; G.wantExit = true; return 0; }
static int l_print(lua_State *L) { plat_log("%s", arg_s(L, 1)); return 0; }
static int l_nop(lua_State *L) { (void)L; return 0; }

static uint32_t clamp_byte(float v) { return v < 0.0f ? 0u : v > 255.0f ? 255u : (uint32_t)(int)v; }

static int l_setBGColor(lua_State *L)
{
    G.bgColor = clamp_byte(arg_n(L, 1)) << 16 | clamp_byte(arg_n(L, 2)) << 8 | clamp_byte(arg_n(L, 3));
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Files                                                                                */
/* ------------------------------------------------------------------------------------ */

/* LuaTable::read @00077bec: run the file (source or bytecode) with a new table as its
 * environment, so its global assignments become the table's fields. Leaves the table on
 * the stack; false if the file is missing or broken (the table is then empty). */
static bool read_table(lua_State *L, const char *file, int save)
{
    uint32_t size = 0;
    char *data = (char *)plat_read_file(file, &size, save);
    lua_newtable(L);
    if (!data)
        return false;
    bool ok = false;
    if (luaL_loadbuffer(L, data, size, file) == 0) {
        lua_pushvalue(L, -2);
        lua_setfenv(L, -2);
        if (lua_pcall(L, 0, 0, 0) == 0) ok = true;
        else script_error(file);
    } else {
        script_error(file);
    }
    free(data);
    return ok;
}

/* GameLua::loadLuaFile @000064ec */
static bool load_lua_file(const char *file, const char *tableName)
{
    lua_State *L = G.L;
    bool ok = read_table(L, file, 0);
    env_push();
    lua_insert(L, -2);
    lua_setfield(L, -2, tableName);
    lua_pop(L, 1);
    if (!ok) plat_log("could not load %s", file);
    return ok;
}

/* LuaTable::write @0007b074: the table as "key = value" Lua source. */
struct Buf { char *p; size_t n, cap; };

static void buf_put(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 256;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static void buf_indent(Buf *b, int n) { while (n-- > 0) buf_put(b, " ", 1); }

static void write_table(lua_State *L, Buf *b, int indent, bool comma, int depth);

static void write_value(lua_State *L, Buf *b, int indent, int depth)
{
    switch (lua_type(L, -1)) {
        case LUA_TBOOLEAN: buf_str(b, lua_toboolean(L, -1) ? "true" : "false"); break;
        case LUA_TNUMBER: {
            char tmp[32];
            ab_fmt_float(tmp, (float)lua_tonumber(L, -1));
            buf_str(b, tmp);
            break;
        }
        case LUA_TSTRING: buf_str(b, "\""); buf_str(b, lua_tostring(L, -1)); buf_str(b, "\""); break;
        case LUA_TTABLE:
            if (depth > 12) { buf_str(b, "nil"); break; }
            buf_str(b, "{\n");
            write_table(L, b, indent + 4, true, depth + 1);
            buf_str(b, "\n");
            buf_indent(b, indent);
            buf_str(b, "}");
            break;
        default: buf_str(b, "nil"); break;
    }
}

/* table at the stack top */
static void write_table(lua_State *L, Buf *b, int indent, bool comma, int depth)
{
    bool hasNamed = false, first = true;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            if (strcmp(k, "_G")) {
                buf_indent(b, indent);
                buf_str(b, k);
                buf_str(b, " = ");
                write_value(L, b, indent, depth);
                if (comma) buf_str(b, ",");
                buf_str(b, "\n");
                hasNamed = true;
                first = false;
            }
        } else {
            if (!hasNamed) {
                if (first) buf_indent(b, indent); else buf_str(b, ",");
                write_value(L, b, indent, depth);
            }
            first = false;
        }
        lua_pop(L, 1);
    }
}

/* saveLuaFile(fileName, tableName, appData) @0000610c */
static int l_saveLuaFile(lua_State *L)
{
    const char *file = arg_s(L, 1), *tableName = arg_s(L, 2);
    Buf b = { NULL, 0, 0 };
    env_push();
    lua_getfield(L, -1, tableName);
    if (lua_istable(L, -1))
        write_table(L, &b, 0, false, 0);
    lua_pop(L, 2);
    if (*file == '/') file++;
    plat_write_file(file, b.p ? b.p : "", (uint32_t)b.n);
    free(b.p);
    return 0;
}

/* saveLevel(name): the level editor's save */
static int l_saveLevel(lua_State *L)
{
    char file[64];
    Buf b = { NULL, 0, 0 };
    snprintf(file, sizeof file, "%s.lua", arg_s(L, 1));
    lua_rawgeti(L, LUA_REGISTRYINDEX, G.objectsRef);
    write_table(L, &b, 0, false, 0);
    lua_pop(L, 1);
    plat_write_file(file, b.p ? b.p : "", (uint32_t)b.n);
    free(b.p);
    return 0;
}

/* loadLevel(name) @0000db94 */
static int l_loadLevel(lua_State *L)
{
    char file[64];
    snprintf(file, sizeof file, "%s.lua", arg_s(L, 1));
    G.acc = 0.0f;
    G.loading = true;
    port_checkpoint("level-begin", file, 0);
    load_lua_file("blocks.lua", "blockTable");
    port_checkpoint("level-blocks", file, 0);
    load_lua_file("particles.lua", "particleTable");
    port_checkpoint("level-particles", file, 0);
    ObjIter it;
    for (ObjNode *n = obj_first(&it); n; n = obj_next(&it))
        G.world->DestroyBody(n->d.body);
    G.jointCount = 0;
    table_clear();
    particles_clear();
    render_level_reset();
    load_lua_file(file, "loadedObjects");
    G.loading = false;
    port_checkpoint("level-ready", file, 0);
    return 0;
}

/* compile() @00006930: (re)load the game script into the environment, then updateValues() */
static bool compile(void)
{
    lua_State *L = G.L;
    uint32_t size = 0;
    char *data = (char *)plat_read_file("gamelogic.lua", &size, 0);
    if (!data) {
        plat_log("gamelogic.lua is missing");
        return false;
    }
    bool ok = false;
    if (luaL_loadbuffer(L, data, size, "gamelogic.lua") == 0) {
        env_push();
        lua_setfenv(L, -2);
        if (lua_pcall(L, 0, 0, 0) == 0) ok = true;
        else script_error("gamelogic.lua");
    } else {
        script_error("gamelogic.lua");
    }
    free(data);
    if (ok)
        env_call("updateValues", 0, 0);
    return ok;
}

static int l_compile(lua_State *L) { (void)L; compile(); return 0; }

/* ------------------------------------------------------------------------------------ */
/* The frame                                                                            */
/* ------------------------------------------------------------------------------------ */

#define STEP 0.0333333351f
#define PI_F 3.14159274f

static void rolling_sound(const char *name, float vol)
{
    if (vol <= 0.0f) res_audio_stop(name);
    else if (!res_audio_is_playing(name)) res_audio_play(name, vol, 1, 2);
    else res_audio_set_volume(name, vol);
}

/* GameLua::update @00007eb0 */
static void game_update(float dt)
{
    lua_State *L = G.L;
    ObjIter it;

    env_set_number("screenWidth", gScreenW);
    env_set_number("screenHeight", gScreenH);

    if (G.physicsEnabled) {
#ifdef AB_NANO
        uint64_t physics_start = plat_time_us();
#endif
        G.acc += dt;
        bool resetForces = false;
        for (;;) {
            if (G.acc < STEP) break;
            if (G.acc < 2.0f * STEP) resetForces = true;
            G.world->Step(STEP, 10, 10, resetForces);
            G.acc -= STEP;
            if (G.acc > 2.0f * STEP) continue;       /* more than two steps pending: no snapshot */
            G.slot = (G.slot == 0) ? 1 : 0;
            for (ObjNode *n = obj_first(&it); n; n = obj_next(&it)) {
                ObjData *o = &n->d;
                if (!o->body->IsAwake()) continue;
                const b2Transform &xf = o->body->GetTransform();
                o->snap[G.slot].x = xf.position.x;
                o->snap[G.slot].y = xf.position.y;
                o->snap[G.slot].angle = atan2f(xf.R.col1.y, xf.R.col1.x);
                if (o->controllable && o->body->GetMass() != 0.0f)
                    o->body->SetBullet(true);        /* birds are bullets once awake and dynamic */
            }
        }
        /* what is drawn, and what the script sees as x/y, is interpolated between the last two steps */
        float a = G.acc / STEP;
        for (ObjNode *n = obj_first(&it); n; n = obj_next(&it)) {
            ObjData *o = &n->d;
            if (!o->body->IsAwake()) continue;
            const Snap &cur = o->snap[G.slot], &prev = o->snap[G.slot == 0 ? 1 : 0];
            o->renderX = (1.0f - a) * prev.x + a * cur.x;
            o->renderY = (1.0f - a) * prev.y + a * cur.y;
            if (fabsf(cur.angle - prev.angle) < PI_F) o->renderAngle = (1.0f - a) * prev.angle + a * cur.angle;
            else if (cur.angle > prev.angle) o->renderAngle = (1.0f - a) * (prev.angle + 2.0f * PI_F) + a * cur.angle;
            else o->renderAngle = (1.0f - a) * (prev.angle - 2.0f * PI_F) + a * cur.angle;
        }
#ifdef AB_NANO
        /* Includes native snapshots and interpolation; excludes Lua. */
        game_stat_physics_us = (uint32_t)(plat_time_us() - physics_start);
#endif
    }
    bool moving = false, awakeAny = false, zeroTol = false;
    float volWood = 0.0f, volRock = 0.0f, volLight = 0.0f;
    for (ObjNode *n = obj_first(&it); n; n = obj_next(&it)) {
        ObjData *o = &n->d;
        b2Body *b = o->body;
        if (!b->IsAwake()) continue;                 /* sleeping bodies: nothing is written */
        lua_rawgeti(L, LUA_REGISTRYINDEX, o->tableRef);
        tbl_set_number(L, -1, "x", o->renderX);
        tbl_set_number(L, -1, "y", o->renderY);
        if (b->GetMass() > 0.0f) {
            if (o->renderY > 20.0f) tbl_set_bool(L, -1, "frozen", true);          /* fell below the ground */
            if (o->renderX < (float)G.limitLeft) tbl_set_bool(L, -1, "frozen", true);
            if (o->renderX > (float)G.limitRight) tbl_set_bool(L, -1, "frozen", true);
        }
        b2Vec2 v = b->GetLinearVelocity();
        float speed = sqrtf(v.y * v.y + v.x * v.x), w = b->GetAngularVelocity();
        if (speed > 0.0f && !o->controllable) zeroTol = true;
        if (speed > 3.0f) moving = true;
        if (w > 0.1f) moving = true;                 /* signed, as in the original */
        if (o->controllable) {
            tbl_set_number(L, -1, "xVel", v.x);
            tbl_set_number(L, -1, "yVel", v.y);
            tbl_set_number(L, -1, "angle", o->renderAngle);
            tbl_set_bool(L, -1, "sleeping", !b->IsAwake());
        } else if (o->isCircle) {
            float vol = fabsf(w) * o->radius / 400.0f * b->GetMass();
            if (vol > 1.0f) vol = 1.0f;
            if (o->material == 1) { if (volWood < vol) volWood = vol; }
            else if (o->material == 2) { if (volRock < vol) volRock = vol; }
            else if (o->material == 3) { if (volLight < vol) volLight = vol; }
        }
        lua_pop(L, 1);
        awakeAny = true;
    }
    env_set_bool("hasMovingObjects", moving);
    env_set_bool("hasAwakeObjects", awakeAny);
    env_set_bool("hasMovingObjectsZeroTolerance", zeroTol);

    if (G.jointCount > 0) {
        push_joints(L);
        for (int i = 0; i < G.jointCount; i++) {
            b2Vec2 a = G.joints[i].joint->GetAnchorA(), b = G.joints[i].joint->GetAnchorB();
            lua_getfield(L, -1, G.joints[i].name);
            if (lua_istable(L, -1)) {
                tbl_set_number(L, -1, "x1", a.x);
                tbl_set_number(L, -1, "y1", a.y);
                tbl_set_number(L, -1, "x2", b.x);
                tbl_set_number(L, -1, "y2", b.y);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    rolling_sound("wood_rolling", volWood);
    rolling_sound("rock_rolling", volRock);
    rolling_sound("light_rolling", volLight);

    lua_pushnumber(L, dt);
    lua_pushnumber(L, dt);
    if (env_call("update", 2, 0) == 2)
        G.failed = true;
    particles_update(dt);
}

/* GameApp::update @000045e4: touches -> cursor, LBUTTON and the pinch-zoom feed */
static void game_input(const PlatTouches *t)
{
    static bool wasDown, zooming;
    static float initialDistance, initialScale;
    lua_State *L = G.L;
    bool down = t->count >= 1;

    if (down) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, G.cursorRef);
        tbl_set_number(L, -1, "x", (float)(int)t->pt[0].x);
        tbl_set_number(L, -1, "y", (float)(int)t->pt[0].y);
        lua_pop(L, 1);
    }
    if (t->count == 2) {
        float dx = t->pt[0].x - t->pt[1].x, dy = t->pt[0].y - t->pt[1].y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (!zooming) {
            zooming = true;
            initialDistance = dist > 1.0f ? dist : 1.0f;
            initialScale = G.appScale;
        }
        G.appScale = dist / initialDistance * initialScale;
    } else {
        zooming = false;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, G.keyPressedRef);
    tbl_set_bool(L, -1, "LBUTTON", down && !wasDown);
    lua_pop(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, G.keyReleasedRef);
    tbl_set_bool(L, -1, "LBUTTON", !down && wasDown);
    lua_pop(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, G.keyHoldRef);
    tbl_set_bool(L, -1, "LBUTTON", down);
    lua_pop(L, 1);
    wasDown = down;

    if (G.appScale != G.appPrevScale)
        env_set_number("zoomLevel", env_number("zoomLevel", 0.0f) + (G.appScale - G.appPrevScale) * 0.5f);
    env_set_number("touches", (float)t->count);
}

/* ------------------------------------------------------------------------------------ */
/* Start-up                                                                             */
/* ------------------------------------------------------------------------------------ */

static const luaL_Reg kNatives[] = {
    { "requestExit", l_requestExit }, { "print", l_print }, { "compile", l_compile },
    { "setBGColor", l_setBGColor },
    { "createBox", l_createBox }, { "createCircle", l_createCircle },
    { "createJoint", l_createJoint }, { "destroyJoint", l_destroyJoint },
    { "applyImpulse", l_applyImpulse }, { "applyForce", l_applyForce },
    { "setPosition", l_setPosition }, { "setRotation", l_setRotation },
    { "setVelocity", l_setVelocity }, { "setAngularVelocity", l_setAngularVelocity },
    { "setPhysicsSimulationScale", l_nop },      /* stored but never read in the original */
    { "setPhysicsEnabled", l_setPhysicsEnabled }, { "setTopLeft", l_setTopLeft },
    { "removeObject", l_removeObject }, { "setEditing", l_setEditing }, { "setWorldScale", l_setWorldScale },
    { "loadLevel", l_loadLevel }, { "saveLevel", l_saveLevel }, { "saveLuaFile", l_saveLuaFile },
    { "createDirectory", l_nop },
    { "setSprite", l_setSprite }, { "setMaterial", l_setMaterial }, { "setTexture", l_setTexture },
    { "setSleeping", l_setSleeping }, { "setObjectParameter", l_setObjectParameter },
    { "setLevelLimits", l_setLevelLimits }, { "goToTaskSwitcherLua", l_nop },
    { NULL, NULL }
};

static const char *const kKeyNames[] = {
    "NONE", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U",
    "V", "W", "X", "Y", "Z", "PAGEDOWN", "PAGEUP", "HOME", "END", "INSERT", "DELETE", "RETURN", "SHIFT", "CONTROL",
    "TAB", "SPACE", "BACKSPACE", "PAUSE", "ESCAPE", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
    "F11", "F12", "LEFT", "RIGHT", "UP", "DOWN", "LBUTTON", "MBUTTON", "RBUTTON", "0", "1", "2", "3", "4", "5", "6",
    "7", "8", "9", "MULTIPLY", "ADD", "SUBTRACT", "DIVIDE", "LEFTSOFT", "RIGHTSOFT", "MIDDLE", "ASTERISK", "HASH",
    "CLEAR", "EDIT", "SEND", "VOLUME_UP", "VOLUME_DOWN", "KEY_GAMING_A", "KEY_GAMING_B"
};

/* A new table stored as T[name]; returns its registry ref. */
static int env_new_table(const char *name)
{
    lua_State *L = G.L;
    env_push();
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    return ref;
}

int game_init(float logical_w, float logical_h)
{
    memset(&G, 0, sizeof(G));
    gScreenW = logical_w;
    gScreenH = logical_h;
    G.bgColor = 0xFFFFFFu;
    G.physicsEnabled = true;
    G.worldScale = G.appScale = G.appPrevScale = 1.0f;

    lua_State *L = G.L = luaL_newstate();
    if (!L) return 1;
    ab_open_libs(L);
    res_register(L);

    /* LuaObject::LuaObject @00074acc: the script environment */
    lua_newtable(L);
    lua_pushlightuserdata(L, &G);
    lua_rawseti(L, -2, 0);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setfield(L, -2, "_G");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "this");
    luaL_register(L, NULL, kNatives);
    render_register(L);
    G.envRef = luaL_ref(L, LUA_REGISTRYINDEX);

    env_set_number("screenWidth", gScreenW);
    env_set_number("screenHeight", gScreenH);
    G.keyPressedRef = env_new_table("keyPressed");
    G.keyReleasedRef = env_new_table("keyReleased");
    G.keyHoldRef = env_new_table("keyHold");
    G.cursorRef = env_new_table("cursor");
    G.clippedTextRef = env_new_table("clippedText");
    G.objectsRef = env_new_table("objects");

    G.world = new b2World(b2Vec2(0.0f, 20.0f), true);    /* +y is down */
    sHooks = new ContactHooks();
    G.world->SetContactListener(sHooks);
    G.world->SetContactFilter(sHooks);

    /* saved state: a missing file gives an empty table */
    read_table(L, "highscores.lua", 1);
    env_push(); lua_insert(L, -2); lua_setfield(L, -2, "highscores"); lua_pop(L, 1);
    read_table(L, "settings.lua", 1);
    env_push(); lua_insert(L, -2); lua_setfield(L, -2, "settings"); lua_pop(L, 1);
    load_lua_file("starLimits.lua", "starTable");
    particles_init(L);

    /* GameApp::GameApp @00004f2c */
    if (!compile())
        return 2;
    env_call("createStartUpAssets", 0, 0);
    const int refs[3] = { G.keyPressedRef, G.keyReleasedRef, G.keyHoldRef };
    for (int r = 0; r < 3; r++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, refs[r]);
        for (unsigned k = 0; k < sizeof(kKeyNames) / sizeof(kKeyNames[0]); k++)
            tbl_set_bool(L, -1, kKeyNames[k], false);
        lua_pop(L, 1);
    }
    return 0;
}

void game_frame(float dt, const PlatTouches *touches)
{
#ifdef AB_NANO
    game_stat_physics_us = 0;
#endif
    if (G.failed) return;
    assets_begin_frame();
    res_audio_tick(dt);
    game_input(touches);
    game_update(dt);
    lua_settop(G.L, 0);
}

/* Kilobytes of Lua heap in use. */
int game_lua_kb(void) { return G.L ? lua_gc(G.L, LUA_GCCOUNT, 0) : 0; }

/* Test hook: print script globals ("a,b.c,d") as one log line. */
void game_debug_dump(const char *names)
{
    lua_State *L = G.L;
    char line[512];
    size_t n = 0;
    while (*names && n < sizeof(line) - 64) {
        char path[64];
        size_t k = 0;
        while (*names && *names != ',' && k < sizeof(path) - 1) path[k++] = *names++;
        path[k] = 0;
        if (*names == ',') names++;
        env_push();
        char *seg = path;
        while (seg) {
            char *dot = strchr(seg, '.');
            if (dot) *dot = 0;
            if (lua_istable(L, -1)) lua_getfield(L, -1, seg); else lua_pushnil(L);
            lua_remove(L, -2);
            if (dot) *dot = '.';
            seg = dot ? dot + 1 : NULL;
        }
        const char *v = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? "true" : "false") :
                        lua_isstring(L, -1) ? lua_tostring(L, -1) : lua_typename(L, lua_type(L, -1));
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%s=%s ", path, v);
        lua_pop(L, 1);
    }
    plat_log("%s", line);
}

void game_gl_lost(void)
{
    assets_gl_lost();
    render_level_reset();
}

uint32_t game_bg_color(void) { return G.bgColor; }
int game_wants_exit(void) { return G.wantExit; }
