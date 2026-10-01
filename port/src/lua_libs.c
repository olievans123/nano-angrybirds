/* lua_libs.c — the standard libraries the game scripts get.
 *
 * base, table and string are Lua's own. math and os are small single-precision versions
 * (the game's lua_Number is a float, and the nano build links no double-precision libm);
 * io, package and debug are left out. */
#include <math.h>
#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "platform.h"

#define PI_F 3.14159265358979323846f

static int m_abs(lua_State *L)   { lua_pushnumber(L, fabsf(luaL_checknumber(L, 1))); return 1; }
static int m_sin(lua_State *L)   { lua_pushnumber(L, sinf(luaL_checknumber(L, 1))); return 1; }
static int m_cos(lua_State *L)   { lua_pushnumber(L, cosf(luaL_checknumber(L, 1))); return 1; }
static int m_tan(lua_State *L)   { lua_pushnumber(L, tanf(luaL_checknumber(L, 1))); return 1; }
static int m_asin(lua_State *L)  { lua_pushnumber(L, asinf(luaL_checknumber(L, 1))); return 1; }
static int m_acos(lua_State *L)  { lua_pushnumber(L, acosf(luaL_checknumber(L, 1))); return 1; }
static int m_atan(lua_State *L)  { lua_pushnumber(L, atanf(luaL_checknumber(L, 1))); return 1; }
static int m_atan2(lua_State *L) { lua_pushnumber(L, atan2f(luaL_checknumber(L, 1), luaL_checknumber(L, 2))); return 1; }
static int m_ceil(lua_State *L)  { lua_pushnumber(L, ceilf(luaL_checknumber(L, 1))); return 1; }
static int m_floor(lua_State *L) { lua_pushnumber(L, floorf(luaL_checknumber(L, 1))); return 1; }
static int m_fmod(lua_State *L)  { lua_pushnumber(L, fmodf(luaL_checknumber(L, 1), luaL_checknumber(L, 2))); return 1; }
static int m_sqrt(lua_State *L)  { lua_pushnumber(L, sqrtf(luaL_checknumber(L, 1))); return 1; }
static int m_pow(lua_State *L)   { lua_pushnumber(L, powf(luaL_checknumber(L, 1), luaL_checknumber(L, 2))); return 1; }
static int m_log(lua_State *L)   { lua_pushnumber(L, logf(luaL_checknumber(L, 1))); return 1; }
static int m_log10(lua_State *L) { lua_pushnumber(L, log10f(luaL_checknumber(L, 1))); return 1; }
static int m_exp(lua_State *L)   { lua_pushnumber(L, expf(luaL_checknumber(L, 1))); return 1; }
static int m_deg(lua_State *L)   { lua_pushnumber(L, luaL_checknumber(L, 1) * (180.0f / PI_F)); return 1; }
static int m_rad(lua_State *L)   { lua_pushnumber(L, luaL_checknumber(L, 1) * (PI_F / 180.0f)); return 1; }

static int m_min(lua_State *L)
{
    int n = lua_gettop(L), i;
    lua_Number v = luaL_checknumber(L, 1);
    for (i = 2; i <= n; i++) {
        lua_Number d = luaL_checknumber(L, i);
        if (d < v) v = d;
    }
    lua_pushnumber(L, v);
    return 1;
}

static int m_max(lua_State *L)
{
    int n = lua_gettop(L), i;
    lua_Number v = luaL_checknumber(L, 1);
    for (i = 2; i <= n; i++) {
        lua_Number d = luaL_checknumber(L, i);
        if (d > v) v = d;
    }
    lua_pushnumber(L, v);
    return 1;
}

/* Same generator on every platform (xorshift32), same interface as lmathlib's. */
static uint32_t sRand = 0x2545F491u;

static uint32_t next_rand(void)
{
    sRand ^= sRand << 13;
    sRand ^= sRand >> 17;
    sRand ^= sRand << 5;
    return sRand;
}

static int m_random(lua_State *L)
{
    /* r in [0, 1) with 24 bits, so it is exact in a float and never rounds up to 1 */
    lua_Number r = (lua_Number)(next_rand() >> 8) * (1.0f / 16777216.0f);
    switch (lua_gettop(L)) {
        case 0:
            lua_pushnumber(L, r);
            break;
        case 1: {
            int u = luaL_checkint(L, 1);
            luaL_argcheck(L, 1 <= u, 1, "interval is empty");
            lua_pushnumber(L, floorf(r * (lua_Number)u) + 1);
            break;
        }
        case 2: {
            int l = luaL_checkint(L, 1), u = luaL_checkint(L, 2);
            luaL_argcheck(L, l <= u, 2, "interval is empty");
            lua_pushnumber(L, floorf(r * (lua_Number)(u - l + 1)) + (lua_Number)l);
            break;
        }
        default:
            return luaL_error(L, "wrong number of arguments");
    }
    return 1;
}

static int m_randomseed(lua_State *L)
{
    sRand = (uint32_t)luaL_checkint(L, 1) * 2654435761u;
    if (!sRand) sRand = 0x2545F491u;
    next_rand();
    return 0;
}

static const luaL_Reg mathlib[] = {
    { "abs", m_abs }, { "acos", m_acos }, { "asin", m_asin }, { "atan", m_atan }, { "atan2", m_atan2 },
    { "ceil", m_ceil }, { "cos", m_cos }, { "deg", m_deg }, { "exp", m_exp }, { "floor", m_floor },
    { "fmod", m_fmod }, { "log", m_log }, { "log10", m_log10 }, { "max", m_max }, { "min", m_min },
    { "pow", m_pow }, { "rad", m_rad }, { "random", m_random }, { "randomseed", m_randomseed },
    { "sin", m_sin }, { "sqrt", m_sqrt }, { "tan", m_tan }, { NULL, NULL }
};

static int os_time(lua_State *L)
{
    /* only used to seed the random generator */
#ifndef AB_NANO
    const char *seed = getenv("AB_REPLAY_SEED");
    if (seed) {
        lua_pushnumber(L, (lua_Number)strtoul(seed, NULL, 10));
        return 1;
    }
#endif
    lua_pushnumber(L, (lua_Number)(uint32_t)((plat_time_us() / 1000u) & 0xFFFFFFu));
    return 1;
}

static int os_clock(lua_State *L)
{
    lua_pushnumber(L, (lua_Number)((double)plat_time_us() / 1e6));
    return 1;
}

static const luaL_Reg oslib[] = { { "time", os_time }, { "clock", os_clock }, { NULL, NULL } };

void ab_open_libs(lua_State *L)
{
    lua_pushcfunction(L, luaopen_base);
    lua_pushstring(L, "");
    lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_table);
    lua_pushstring(L, LUA_TABLIBNAME);
    lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_string);
    lua_pushstring(L, LUA_STRLIBNAME);
    lua_call(L, 1, 0);

    luaL_register(L, LUA_MATHLIBNAME, mathlib);
    lua_pushnumber(L, PI_F);
    lua_setfield(L, -2, "pi");
    lua_pushnumber(L, (lua_Number)HUGE_VALF);
    lua_setfield(L, -2, "huge");
    lua_pop(L, 1);
    luaL_register(L, LUA_OSLIBNAME, oslib);
    lua_pop(L, 1);
}
