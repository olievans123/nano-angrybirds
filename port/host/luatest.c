/* Smoke test: load the game's original bytecode into the patched Lua VM. */
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
int main(int argc, char **argv)
{
    lua_State *L = luaL_newstate();
    int i;
    luaL_openlibs(L);
    for (i = 1; i < argc; i++) {
        if (luaL_loadfile(L, argv[i]) || lua_pcall(L, 0, 0, 0)) {
            printf("FAIL %s: %s\n", argv[i], lua_tostring(L, -1));
            return 1;
        }
        printf("ok %s\n", argv[i]);
    }
    lua_getglobal(L, "menuWidth");
    printf("menuWidth=%g\n", (double)lua_tonumber(L, -1));
    lua_getglobal(L, "blockTable");
    printf("blockTable is %s\n", lua_typename(L, lua_type(L, -1)));
    return 0;
}
