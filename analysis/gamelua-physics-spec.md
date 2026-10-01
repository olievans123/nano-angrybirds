# GameLua physics / object side: behavioural spec (Angry Birds 1.0 iOS, armv6)

Source of truth: `decomp-annot/GameLua.c`, `GameApp.c`, `lua.c`, `_global.c`, `framework.c`, `io.c`,
cross-checked against the real ARM/VFP disassembly of the binary for every numeric comparison
(Ghidra's flag-soup float compares are unreliable; all thresholds below were read from `vcmp`/branch
pairs). Helper scripts used and left in `analysis/`:

- `analysis/disfn.py START [END|+SIZE]`: annotated disassembly (VFP decoded, literal pools resolved).
- `analysis/b2consts.py [substr]`: float literals used by each Box2D function in the binary.

Conventions: "pair" = `lang::HashtablePair<String,RenderObjectData>` (0x60 bytes); "data"/"value" =
the `RenderObjectData` inside it (pair+4). All floats are 32-bit. Everything not marked **(inferred)**
was read directly from code. Lua numbers are **32-bit floats** in this build (`lua_pushnumber(lua_State*, float)`,
bytecode header `sizeof(lua_Number)=4`).

---

## 0. Three things to fix in the port before anything else

1. **Rovio changed three `b2Settings.h` constants** (port/Box2D still has the stock values):

   | constant | stock | binary | evidence |
   |---|---|---|---|
   | `b2_linearSlop` | 0.005 | **0.05** | `b2ContactSolver::SolvePositionConstraints` @0012a614 uses 0.05 (lit @12a9c8) and −0.075 = −1.5·slop (lit @12a9d0); `b2Contact::ComputeTOI` @00129400 tolerance 0.05 (lit @1294c8); `b2DistanceJoint` 0.05 |
   | `b2_polygonRadius` (= 2·slop) | 0.01 | **0.1** | `GameLua::createBox` writes `m_radius = 0.1f` (lit @00007bf0); `b2PolygonShape::Clone` @00123138 (lit @123230) |
   | `b2_linearSleepTolerance` | 0.01 | **0.1** | `b2Island::Solve` @00126524: `linTolSqr` literal 0.01 (lit @126e04) |
   | `b2_timeToSleep` | 0.5 | **0.25** | `b2Island::Solve`: `minSleepTime >= 0.25` (lit @126e08) |

   Verified unchanged: `b2_aabbExtension` 0.1, `b2_aabbMultiplier` 2, `b2_maxLinearCorrection` 0.2,
   `b2_contactBaumgarte` 0.2, `b2_velocityThreshold` 1, `b2_maxTranslation` 2, `b2_maxRotation` π/2,
   `b2_angularSlop` 2°, `b2_maxAngularCorrection` 8°, `b2_angularSleepTolerance` 2°,
   `b2_maxTOIContactsPerIsland` 32, `b2_maxPolygonVertices` 8, TOI constants 0.75/0.02/0.5/0.025,
   `k_maxConditionNumber` 100, friction mix = sqrt, restitution mix = max, stack allocator 100 KiB
   (`sizeof(b2World)` = 0x19258).

2. **The Box2D in the binary is not exactly the port/Box2D snapshot.** Differences seen:
   - `b2World::Step(float dt, int velIters, int posIters, bool resetForces)` @00128d20 (4 args).
     `b2TimeStep` has a `bool resetForces` at +0x15 (after `warmStarting` +0x14). It is consumed in the
     integrate loop of `b2Island::Solve` (@00126628): after integrating velocities of a dynamic body,
     `if (step.resetForces) { b->m_force.SetZero(); b->m_torque = 0; }`. The port's snapshot has the later
     `Step(dt,vi,pi)` + `ClearForces()` API. Since the 1.0 scripts never call `applyForce`, forces are
     always zero and the difference is unobservable; emulate with `Step(); if (reset) ClearForces();`.
   - `b2BodyDef` member order in the binary: `userData@0, position@4, angle@0xc, linearVelocity@0x10,
     angularVelocity@0x18, linearDamping@0x1c, angularDamping@0x20, allowSleep@0x24, awake@0x25,
     fixedRotation@0x26, bullet@0x27, active@0x28, type@0x2c, inertiaScale@0x30` (port has `type` first).
     Semantically identical.
   - `b2Island::Solve` sleep loop skips bodies with `m_type != b2_dynamicBody` (port: `== b2_staticBody`).
     Only differs for kinematic bodies, which the game never creates.
   - `b2Body::SetType` and `b2Body::SetTransform` were compared in full with the port source and match;
     `b2Body::b2Body`, `b2Fixture::Create`, `b2Joint`/`b2DistanceJoint` ctors and the flag handling / listener
     calls of `b2Contact::Update` were spot-checked and match.
   - I did not diff the solver / TOI / collision routines line by line (see section 10).

3. **Lua type checks are strict.** Every native-function argument is fetched with
   `LuaState::toString/toNumber/toBoolean`, which call `checkType` (@00075d60) and throw `lua::LuaException`
   ("Excepted type {0} at stack position {1} but found {2}{3}") unless `lua_type()` is *exactly*
   string / number / boolean. No coercion, nil is not accepted for a bool. The shipped scripts satisfy this.

---

## 1. Constructor `GameLua::GameLua(App*, Resources*, LuaState*, gr::Context*)` @00010474

Class: `GameLua : lua::LuaObject, b2ContactListener, b2ContactFilter`, `sizeof == 0x1d8` (`operator new(0x1d8)`
in `GameApp::GameApp` @00004f2c).

### 1.1 Object layout

| off | type | meaning |
|---|---|---|
| +0x00 | vptr | primary vtable: dtor, dtor, `BeginContact` (0xcbec), `EndContact` (0x5f44), `ShouldCollide` (0x6d78), `RayCollide` (0x136ac) |
| +0x04 | int | `lang::Object` refcount |
| +0x08 | `lua::LuaState*` | (`LuaState+8` = `lua_State*`) |
| +0x0c | int | registry ref (`luaL_ref`) of this object's Lua table = **the script environment** |
| +0x10 | vptr | `b2ContactListener` sub-object (BeginContact/EndContact thunks, PreSolve/PostSolve = base no-ops) |
| +0x14 | vptr | `b2ContactFilter` sub-object (ShouldCollide/RayCollide thunks) |
| +0x18 | `gr::Context*` | vtbl+0x64 = width(), +0x68 = height(), +0x84 = orientation() |
| +0x1c | `framework::App*` | (the `GameApp`) |
| +0x20 | `game::Resources*` | |
| +0x24 | ? | not written in the ctor body I read (unknown) |
| +0x28 | `Particles*` | `new Particles(L, this, resources)` (0x4c bytes) |
| +0x2c | `lua::LuaTable` (16 B) | **keyPressed** |
| +0x3c | `lua::LuaTable` | **keyReleased** |
| +0x4c | `lua::LuaTable` | **keyHold** |
| +0x5c | `lua::LuaTable` | **cursor** |
| +0x6c | uint32 | background colour, init 0xffffffff (`setBGColor`/`getBGColor`) |
| +0x70 | int | interpolation slot index (0/1), init 0 |
| +0x74..+0x8b | ? | rendering/theme state (not analysed) |
| +0x8c,+0x90,+0x94 | `lang::String` ×3 | theme sprite/sheet names (rendering) |
| +0x98,+0x9c,+0xa0 | ptr/int | theme arrays (rendering), init 0 |
| +0xa8 | int | audio clip handle "rock_rolling" (−1 after loadLevel) |
| +0xac | int | audio clip handle "wood_rolling" |
| +0xb0 | int | audio clip handle "light_rolling" |
| +0xb4 | `b2World*` | |
| +0xb8 | `lang::Hashtable<String,RenderObjectData>` | objects by name. `+0xc0` bucket count, `+0xc4` bucket array (pairs of 0x60 B), `+0xcc` entry count, `+0xd0` grow threshold, **`+0xd4` default value** (a `RenderObjectData` returned for unknown keys, 0x54 B) |
| +0x130 | `lang::Array<jointData>` | data ptr, `+0x134` size, `+0x138` capacity |
| +0x13c | `lang::Array<userData>` | data ptr, `+0x140` size, `+0x144` cap. Exactly one element is ever added (ctor) |
| +0x148 | `lua::LuaTable` | **objects** |
| +0x158 | `lua::LuaTable` | **clippedText** |
| +0x168 | float | physicsSimulationScale, init 1.0. **Written by `setPhysicsSimulationScale`, never read anywhere** |
| +0x16c | bool | physicsEnabled, **init true** |
| +0x16d | bool | "loading" flag (true only inside `loadLevel`), init false. Suppresses BeginContact |
| +0x16e | bool | editing (`setEditing`), init false. Suppresses BeginContact |
| +0x170, +0x174 | float | topLeft x,y (`setTopLeft`), init 0 (rendering only) |
| +0x178 | float | physics time accumulator, init 0 |
| +0x17c | float | worldScale, init 1.0 |
| +0x180..+0x1c7 | 6 × `lang::Array<8-byte elem>` (12 B each) | trajectory point arrays (rendering). Sizes zeroed by `loadLevel` |
| +0x1c8 | int | 0 (trajectory state) |
| +0x1cc | `MaskedImage*` | `new MaskedImage(ctx)` (0x3c B) |
| +0x1d0 | int | level limit left (`(int)` of setLevelLimits arg 1) |
| +0x1d4 | int | level limit right (`(int)` of setLevelLimits arg 3) |

`struct userData { lang::String name; bool collides; }` (8 B). The ctor adds one element `{"", false}`.

`struct jointData { lang::String name; lang::String end1; lang::String end2; b2Joint* joint; }` (16 B).

`struct RenderObjectData` (value offsets; add 4 for pair offsets used in the decompilation):

| value off | pair off | type | meaning |
|---|---|---|---|
| +0x00 | +0x04 | String | name (same as key) |
| +0x04 | +0x08 | LuaTable | the object's Lua table (same table as `objects.world[name]` at creation time) |
| +0x14 | +0x18 | b2Body* | body |
| +0x18 | +0x1c | String | sprite name |
| +0x1c | +0x20 | game::Sprite* | sprite (NULL if sprite name empty) |
| +0x20 | +0x24 | image ptr | override texture (`setTexture`), init 0 |
| +0x24,+0x28,+0x2c | +0x28.. | float×3 | snapshot slot 0: x, y, angle |
| +0x30,+0x34,+0x38 | +0x34.. | float×3 | snapshot slot 1: x, y, angle |
| +0x3c | +0x40 | float | radius (circle) or **−1.0** (box) |
| +0x40,+0x44,+0x48 | +0x44.. | float×3 | **render (interpolated) x, y, angle** |
| +0x4c | +0x50 | bool | **controllable** (bird flag) |
| +0x4d | +0x51 | bool | isCircle |
| +0x4e | +0x52 | bool | "parameter 1" flag (`setObjectParameter(name,1,v)`; Lua sets it for `levelGoal` objects; read only by drawGame) |
| +0x50 | +0x54 | int | rolling-sound material: 0 none, 1 wood, 2 rock, 3 light (`setMaterial`) |
| | +0x58 | pair* | hash chain next |
| | +0x5c | bool | pair in use |

Hash lookup everywhere: `h = (name.hashCode() & 0x7fffffff) % bucketCount`, walk chain from `&buckets[h]`,
match `used && key.compareTo(name)==0`. Any map keyed by string is equivalent; iteration order never
affects results (only max/OR reductions are computed over it).

### 1.2 Box2D world

```
b2Vec2 gravity(0.0f, 20.0f);              // +y is DOWN. literal 20f @00011660
m_world = new b2World(gravity, /*doSleep*/ true);
m_world->SetContactListener(static_cast<b2ContactListener*>(this));
m_world->SetContactFilter  (static_cast<b2ContactFilter*>(this));
```
No destruction listener, no debug draw. Warm starting / continuous physics: Box2D defaults (both on).

### 1.3 The Lua environment

`LuaObject::LuaObject(L)` @00074acc creates a fresh table T (registry ref at this+0xc) and sets
`T[0] = lightuserdata(this)`, `T["_G"] = <real globals table>`, `T["this"] = T`. No metatable.
`GameLua::compile()` loads `gamelogic.lua` and does **`lua_setfenv(chunk, T)`** before calling it, so every
"global" the scripts read or write is a raw field of T (that is why scripts say `_G.pairs`, `_G.math`).
All `setNumber("x", ..)`/`getNumber`/`call("fn")` on `this` are `lua_rawset`/`lua_rawget` on T.

### 1.4 Registered Lua functions (in registration order)

All but one use `LuaObject::registerClosure(name, functor, LuaRawMethodDispatcher<GameLua, Sig>::dispatch)`:
Lua stack index 1..N maps to C++ argument 1..N in order (no self), strict types (section 0.3), always returns 0
values. `S` = string, `N` = number, `B` = boolean.

| Lua name | C++ method | Lua args |
|---|---|---|
| `requestExit` | `requestExit()` @66d0 | — → `App::quit(0)` |
| `print` | `print(String)` @60f4 | S → `lang::Debug::printf(s)` |
| `compile` | `compile()` @6930 | — |
| `setBGColor` | `setBGColor(f,f,f)` @5f50 | N r, N g, N b (each clamped 0..255, colour = r<<16 \| g<<8 \| b) |
| **`createBox`** | **`createBoxLua`** @129d0 | S name, S sprite, N x, N y, N halfWidth, N halfHeight, N density, N friction, N restitution, B collision, B controllable |
| **`createCircle`** | **`createCircleLua`** @13034 | S name, S sprite, N x, N y, N radius, N density, N friction, N restitution, B controllable |
| **`createJoint`** | `createJointLua` @bff8 | S name, S end1, S end2, N type |
| `destroyJoint` | `destroyJointLua` @b5f4 | S name |
| `applyImpulse` | @b9e0 | S name, N ix, N iy, N px, N py |
| `applyForce` | @beb8 | S name, N fx, N fy, N px, N py |
| `setPosition` | @bb28 | S name, N x, N y |
| `setRotation` | @c89c | S name, N angle |
| `setVelocity` | @c6b4 | S name, N vx, N vy |
| `setAngularVelocity` | @c5e4 | S name, N w |
| `setPhysicsSimulationScale` | @5ff0 | N (stored, unused) |
| `setPhysicsEnabled` | @5ff8 | B |
| `setTopLeft` | @6000 | N x, N y |
| `removeObject` | @12578 | S name |
| `setEditing` | @5f34 | B |
| `setWorldScale` | @600c | N s → `this+0x17c = s; app+0x27c = s; app+0x280 = s` |
| `drawRect` | @7898 | N×8, B (rendering) |
| `drawTexturedRect` | @66e8 | S, N×4, B (rendering) |
| `setRenderState` | `int setRenderState(LuaState*)` @a960 | raw (`registerLuaMethod`) (rendering) |
| `loadLevel` | @db94 | S name (no extension) |
| `saveLevel` | @6368 | S name (no extension) |
| `saveLuaFile` | @610c | S fileName, S tableName, B appData |
| `createDirectory` | @6024 | S — **empty function** |
| `drawGameNative` | `drawGame()` @9ad4 | — |
| `drawBackgroundNative` | `drawBackground()` @a5d8 | — |
| `drawParticlesNative` | `drawParticles()` @60dc | — |
| `setSprite` | @b8cc | S name, S sprite |
| `setMaterial` | @b7c4 | S name, S material |
| `setTexture` | @b76c | S name, S sheetName |
| `setTheme` | @92b8 | S |
| `setSleeping` | @c78c | S name, B sleeping |
| `drawLine2D` | @6044 | N×9 |
| `drawForegroundNative` | @6ba0 | — |
| `setObjectParameter` | @b928 | S name, N id, N value |
| `clipText` | @6da4 | S, S, N |
| `startNewTrajectory` | @aa64 | — |
| `addToTrajectory` | @8ff8 | N, N, N |
| `setLevelLimits` | @6028 | N left, N (ignored), N right, N (ignored) |
| `goToTaskSwitcherLua` | @5f30 | — **empty function** |

Not exposed to Lua (native-only): `loadLuaFile`, raw `createBox`/`createCircle`, `getBodyName`, `update`,
`createAssets`, `getBGColor`. Registered but not used by the 1.0 scripts: `applyForce`, `setAngularVelocity`,
`destroyJoint`, `drawTexturedRect`, `drawParticlesNative`, `setRenderState`, `createDirectory`.

`clearSaveFunction`, `countsSaved`, `n900`: **do not exist in the binary** (no such strings) and are never
assigned by the scripts. They are simply `nil` globals (`countsSaved == false` is therefore false).

### 1.5 Globals set by the constructor (after the registrations, in this order)

```
screenWidth  = (float) ctx->width()       // vtbl+0x64
screenHeight = (float) ctx->height()      // vtbl+0x68
keyPressed   = <table @+0x2c>   (empty)
keyReleased  = <table @+0x3c>
keyHold      = <table @+0x4c>
cursor       = <table @+0x5c>   (empty: cursor.x/y are nil until Lua or the first touch sets them)
clippedText  = <table @+0x158>
objects      = <table @+0x148>  (empty: Lua creates objects.world / .joints / .counts itself)
world created (1.2)
try { AppDataInputStream f("highscores.lua"); LuaTable t(L); t.read(&f); highscores = t; }
catch (...) { Debug::printf("\nUnable to load highscores!\n\n"); highscores = {} }
try { AppDataInputStream f("settings.lua");   LuaTable t(L); t.read(&f); settings = t; }
catch (...) { settings = {} }                       // silent
loadLuaFile("starLimits.lua", "starTable")          // bundle data dir; not guarded
m_userData.add({ "", false })
m_particles = new Particles(L, this, resources)
```
`GameApp::GameApp` then (after constructing GameLua): clears its key flag arrays, calls `GameLua::compile()`
(runs gamelogic.lua in env T, then calls Lua `updateValues()`), `GameLua::createAssets()` (calls Lua
`createStartUpAssets()`, then `audioOutput->setClipCountLimit(1,4); (2,6); (3,3)`), then for every key id
0..0x55 sets `keyPressed[name] = keyReleased[name] = keyHold[name] = false` (names in 9.3), then if
`highscores.volume` is a number calls `resources->setMasterVolume(highscores.volume)`.

Globals written later by native code: `touches`, `zoomLevel`, `hasMovingObjects`, `hasAwakeObjects`,
`hasMovingObjectsZeroTolerance` (every frame), `blockTable`, `particleTable`, `loadedObjects` (loadLevel),
`cursor.x`, `cursor.y`, `cursor.wheel`, `keyPressed.LBUTTON` etc. (GameApp), `highscores.volume` (GameApp volume keys).

---

## 2. `GameLua::update(float dt)` @00007eb0

Caller chain per frame: `AppController::update` @00068158 computes
`dt = min((now_us − last_us) * 1e-6f, 0.1f)` (literals @6828c, @68290) → `GameApp::update(dt, ctx, 3)`
@000045e4 (section 9) → `RenderScene` begin → clear with `getBGColor()` → **`GameLua::update(dt)`** → scene end →
present. All drawing happens inside the Lua `update` call at the end of this function.

Constants: `STEP = 0.0333333351f` (0x3d088889, @8398/@83c0), `2*STEP = 0.0666666701f` (0x3d888889, @839c),
`PI = lang::Math::PI = 3.14159274f` (0x40490fdb @13a978).

```cpp
void GameLua::update(float dt)
{
    T.screenWidth  = (float)ctx->width();
    T.screenHeight = (float)ctx->height();

    // pinch zoom feed (app+0x27c = current scale, app+0x280 = last scale handed to/set by Lua)
    if (app->scale != app->prevScale)
        T.zoomLevel = (float)((double)T.zoomLevel + (double)(app->scale - app->prevScale) * 0.5); // 0.5 = double @8d6c
        // NOTE: neither field is modified here; they only become equal again when Lua calls setWorldScale()

    T.touches = (float)app->touches().size();        // a NUMBER, not a table

    if (m_physicsEnabled) {                          // +0x16c
        m_acc += dt;                                 // +0x178, no clamp here
        bool resetForces = false;                    // sticky within this call
        for (;;) {
            if (m_acc < STEP) break;
            if (m_acc < 2*STEP) resetForces = true;
            m_world->Step(STEP, 10, 10, resetForces);     // @8018: dt=1/30, 10 velocity, 10 position iterations
            m_acc -= STEP;
            if (m_acc > 2*STEP) continue;                 // more than two steps still pending: no snapshot
            // snapshot (taken after each of the last two steps of a frame)
            m_slot = (m_slot <= 1u) ? 1 - m_slot : 0;     // +0x70, toggles 0/1
            for (each used pair o in m_objects)
                if (o.body->m_flags & e_awakeFlag) {
                    o.snap[m_slot].x     = o.body->m_xf.position.x;
                    o.snap[m_slot].y     = o.body->m_xf.position.y;
                    o.snap[m_slot].angle = atan2f(o.body->m_xf.R.col1.y, o.body->m_xf.R.col1.x); // (-PI, PI]
                    if (o.controllable && o.body->m_mass != 0.0f)
                        o.body->m_flags |= e_bulletFlag;  // birds become bullets once awake & dynamic
                }
        }
        // interpolation for rendering AND for what Lua sees as x/y
        float a = m_acc / STEP;                           // 0 <= a < 1
        for (each used pair o) if (o.body->m_flags & e_awakeFlag) {
            const Snap& cur  = o.snap[m_slot];
            const Snap& prev = o.snap[m_slot == 0 ? 1 : 0];
            o.renderX = (1.0f - a) * prev.x + a * cur.x;
            o.renderY = (1.0f - a) * prev.y + a * cur.y;
            if (fabsf(cur.angle - prev.angle) < PI) o.renderAngle = (1-a) * prev.angle            + a * cur.angle;
            else if (cur.angle > prev.angle)        o.renderAngle = (1-a) * (prev.angle + 2*PI)   + a * cur.angle;
            else                                    o.renderAngle = (1-a) * (prev.angle + -2*PI)  + a * cur.angle;
        }
    }
    // (if physics is disabled: no stepping, no accumulator change, no interpolation; render values stay as they are)

    bool moving = false, awakeAny = false, zeroTol = false;
    float volWood = 0, volRock = 0, volLight = 0;
    if (m_objects.count >= 1)
      for (each used pair o) {
        b2Body* b = o.body;
        if (!(b->m_flags & e_awakeFlag)) continue;          // sleeping bodies: nothing is written at all
        o.table.x = o.renderX;
        o.table.y = o.renderY;
        if (b->m_mass > 0.0f) {                              // dynamic only
            if (o.renderY > 20.0f)                    o.table.frozen = true;   // literal 20f @8d34 (hard-coded "fell below ground")
            if (o.renderX < (float)m_limitLeft)       o.table.frozen = true;   // +0x1d0
            if (o.renderX > (float)m_limitRight)      o.table.frozen = true;   // +0x1d4
        }
        float speed = sqrtf(b->v.y*b->v.y + b->v.x*b->v.x);
        if (speed > 0.0f && !o.controllable) zeroTol = true;
        if (speed > 3.0f)                    moving = true;                    // literal 3f @8d38
        if (b->m_angularVelocity > 0.1f)     moving = true;                    // literal 0.1f @8d3c; SIGNED, no fabs (original quirk)
        if (o.controllable) {
            o.table.xVel     = b->v.x;                       // raw body velocity, physics units/s
            o.table.yVel     = b->v.y;
            o.table.angle    = o.renderAngle;                // only controllables get "angle" refreshed
            o.table.sleeping = !(b->m_flags & e_awakeFlag);  // i.e. always false here
        } else if (o.isCircle) {
            float v = fabsf(b->m_angularVelocity) * o.radius / 400.0f * b->m_mass;   // 400f @8d40
            if (v > 1.0f) v = 1.0f;
            if      (o.material == 1) { if (volWood  < v) volWood  = v; }
            else if (o.material == 2) { if (volRock  < v) volRock  = v; }
            else if (o.material == 3) { if (volLight < v) volLight = v; }
        }
        awakeAny = true;
      }
    T.hasMovingObjects              = moving;
    T.hasAwakeObjects               = awakeAny;
    T.hasMovingObjectsZeroTolerance = zeroTol;

    if (m_joints.size() > 0) {
        LuaTable joints = m_objectsTable.getTable("joints");           // objects.joints
        for (i = 0; i < m_joints.size(); ++i) {
            b2Vec2 a = m_joints[i].joint->GetAnchorA();                // vtbl slot 0
            b2Vec2 b = m_joints[i].joint->GetAnchorB();                // vtbl slot 1
            LuaTable t = joints.getTable(m_joints[i].name);
            t.x1 = a.x; t.y1 = a.y; t.x2 = b.x; t.y2 = b.y;
        }
    }

    // rolling sounds, in this order: wood (handle +0xac), rock (+0xa8), light (+0xb0)
    for (name, vol, handle) in { ("wood_rolling", volWood, m_hWood), ("rock_rolling", volRock, m_hRock), ("light_rolling", volLight, m_hLight) }:
        if (vol <= 0)                              resources->stopAudio(name);
        else if (!resources->isAudioPlaying(name)) handle = resources->playAudio(name, vol, /*loop*/true, 2);
        else                                       resources->getAudioOutput()->setClipVolume(handle, vol);

    call Lua  update(dt, dt);          // LuaObject::call<float,float>("update", dt, dt), unprotected lua_call
    m_particles->update(dt);
}
```

Notes:
- There is **no deferred removal queue** and no deferred callback queue. `birdCollision`/`blockCollision` are
  called synchronously from inside `b2World::Step` (world locked). `removeObject` destroys immediately; the
  scripts only call it from `updateGame`, i.e. after the step.
- Order per frame: input tables (GameApp) → physics steps (with collision callbacks) → per-object write-back →
  flags → joints → rolling sounds → Lua `update(dt, dt)` (game logic + all drawing) → particles.
- What Lua sees in `obj.x`/`obj.y` is the *interpolated render position*, not the body position; `xVel`/`yVel`
  are exact body velocities. Non-controllable objects never get `angle`, `xVel`, `yVel`, `sleeping` refreshed
  by native code (their Lua `angle` stays at whatever `setRotation` last wrote).
- Static bodies are included in all of this (they are created awake). Box2D puts a static body to sleep together
  with any island that contains it (`b2Island::Solve` calls `SetAwake(false)` on every island body, no type
  check, @126d10) and wakes it when it is pulled into an awake island again, so "ground" contributes to
  `hasAwakeObjects` and gets `x`/`y` written while anything resting on it is awake, or while nothing touches it.
  Its mass is 0, so the `frozen` tests are skipped for it.
- A sleeping object's Lua table and render position are left untouched (stale).
- `hasMovingObjects` thresholds: linear speed > 3.0, or angular velocity > +0.1 rad/s (signed).
  `hasMovingObjectsZeroTolerance`: any awake **non-controllable** body with speed > 0. The script also assigns
  this global itself (gamelogic line ~12813); native overwrites it every frame before Lua `update` runs.
- "frozen" is only ever set to true natively; Lua resets it.
- VFP `vmla` is multiply-then-add with intermediate rounding (not fused).

---

## 3. Object creation

### 3.1 Lua `createBox(name, sprite, x, y, hw, hh, density, friction, restitution, collision, controllable)` → `createBoxLua` @000129d0

```cpp
b2Body* body = createBox(name, x, y, hw, hh, density, friction, restitution, collision);   // 3.3
LuaTable t(L);
t.name = name; t.sprite = sprite; t.type = "box";
t.x = x; t.y = y; t.width = hw; t.height = hh; t.angle = 0;
t.density = density; t.friction = friction; t.restitution = restitution;
t.mass = body->m_mass; t.xVel = 0; t.yVel = 0;
objects.world[name] = t;                       // getTable(objects,"world").setTable(name, t) — replaces any old entry

RenderObjectData d;
d.name = name; d.table = objects.world[name]; d.body = body; d.spriteName = sprite;
d.sprite = sprite.hasData() ? resources->findSheet(sprite)->getSprite(sprite) : NULL;
d.texture = 0;
d.snap[0] = d.snap[1] = {x, y, 0};
d.radius = -1.0f;                               // literal @1302c
d.renderX = x; d.renderY = y; d.renderAngle = 0;
d.controllable = controllable; d.isCircle = false; d.param1 = false; d.material = 0;
m_objects[name] = d;                            // grow if needed; overwrite in place if the key exists
```

### 3.2 Lua `createCircle(name, sprite, x, y, radius, density, friction, restitution, controllable)` → `createCircleLua` @00013034

Same as 3.1 with: `body = createCircle(name, x, y, radius, density, friction, restitution)`; table has
`type = "circle"`, `radius = radius` (no width/height), plus **`sphere = true`**; `d.radius = radius`,
`d.isCircle = true`; and after inserting:

```cpp
if (controllable) body->m_angularDamping = 2.0f;     // direct write to b2Body+0x94 (0x40000000)
```

### 3.3 `b2Body* GameLua::createBox(String name, x, y, hw, hh, density, friction, restitution, bool collision)` @000079f8

```cpp
b2BodyDef bd;                 // every field written explicitly:
bd.userData = NULL;  bd.position.Set(x, y);  bd.angle = 0;
bd.linearVelocity.SetZero();  bd.angularVelocity = 0;
bd.linearDamping = 0.0f;
bd.angularDamping = 1.0f;     // NOT the Box2D default
bd.allowSleep = true;  bd.awake = true;  bd.fixedRotation = false;  bd.bullet = false;  bd.active = true;
bd.type = (density != 0.0f) ? b2_dynamicBody : b2_staticBody;
bd.inertiaScale = 1.0f;
b2Body* body = m_world->CreateBody(&bd);

b2PolygonShape shape;         // m_radius = b2_polygonRadius = 0.1 (see section 0)
shape.SetAsBox(hw, hh);       // the Lua "width"/"height" are HALF extents, passed through unscaled

b2FixtureDef fd;
fd.shape = &shape;  fd.friction = friction;  fd.restitution = restitution;  fd.density = density;
fd.isSensor = false;  fd.filter.categoryBits = 1;  fd.filter.maskBits = 0xFFFF;  fd.filter.groupIndex = 0;
fd.userData = collision ? NULL : &m_userData[0];      // m_userData[0] = {"", collides=false}
body->CreateFixture(&fd);
return body;
```
`name` is unused here.

### 3.4 `b2Body* GameLua::createCircle(String name, x, y, radius, density, friction, restitution)` @00007bfc

Identical body def (type by `density != 0`, angularDamping 1.0, linearDamping 0, not bullet, not fixed rotation).
`b2CircleShape { m_p = (0,0); m_radius = radius }`; fixture: friction/restitution/density as given,
`userData = NULL` (circles always collide), category 1, mask 0xFFFF, group 0, not a sensor.

### 3.5 Rules and consequences

- **Static vs dynamic**: `density == 0` → static, otherwise dynamic. No kinematic bodies.
- **Bullet**: never set at creation. `update()` sets `e_bulletFlag` on every *controllable* body that is awake
  and has `m_mass != 0`, right after the last/second-to-last step of a frame (section 2). It is never cleared.
- **Fixed rotation**: never. **Linear damping**: 0 always. **Angular damping**: 1.0 for everything, 2.0 for
  controllable circles (birds).
- **Mass** reported to Lua = `b2Body::GetMass()` right after `CreateFixture` (0 for static).
- **Collision filter** `GameLua::ShouldCollide(fA, fB)` @00006d78 (replaces Box2D's category/mask logic entirely):
  ```cpp
  uint8 a = fA->m_userData ? ((userData*)fA->m_userData)->collides : 1;
  if (!fB->m_userData) return a;
  return (a & ((userData*)fB->m_userData)->collides) != 0;
  ```
  Because the only `userData` instance has `collides == false`, a box created with `collision == false`
  collides with **nothing**; everything else collides with everything. (`b2Body::ShouldCollide`, i.e.
  "at least one dynamic" and joint `collideConnected`, still applies before this.)
  `GameLua::RayCollide` @000136ac returns `false` always.
- **Names**: one hashtable entry per name. **Duplicate name**: a *new* body is created anyway, the Lua table
  `objects.world[name]` and the hashtable entry are overwritten, and the **old body is leaked in the world**
  (it keeps colliding; `getBodyName` no longer finds it → its contacts are ignored by BeginContact; it is not
  destroyed by `removeObject` or `loadLevel`). The 1.0 scripts avoid duplicates via `objects.counts`.
- `objects.world` must already be a table (Lua creates it in `loadLevelInternal` right after `loadLevel`),
  otherwise `getTable` throws.
- Units: everything is in Box2D ("physics") units. Lua's `physicsToWorld = 20` (pixels per physics unit at
  worldScale 1), `physicsScale = 1/20`. Level files store physics units already (`scaleFactor =
  loadedObjects.physicsToWorld / physicsToWorld` = 1). y grows downward, gravity (0, +20), angles in radians.

`GameLua::getBodyName(b2Body*)` @00007db4: linear scan of all used pairs for `data.body == body`; returns
`data.name`, or `""` when not found.

---

## 4. Joints

### 4.1 Lua `createJoint(name, end1, end2, type)` → `createJointLua` @0000bff8

```cpp
LuaTable t(L);  t.name = name;  t.end1 = end1;  t.end2 = end2;  t.type = type;
jointData jd = { name, end1, end2, /*joint: uninitialised*/ };
if (type == 1.0f) {                                  // literal @c4c8; the only supported type
    b2DistanceJointDef def;                          // type = e_distanceJoint (3), userData = NULL
    def.bodyA = body(end1);  def.bodyB = body(end2); // hashtable lookup; NULL if unknown → crash below
    def.collideConnected = false;
    def.localAnchorA.Set(0, 0);  def.localAnchorB.Set(0, 0);
    b2Vec2 p1 = def.bodyA->GetWorldPoint(b2Vec2(0,0));   // = body origin (m_xf.position)
    b2Vec2 p2 = def.bodyB->GetWorldPoint(b2Vec2(0,0));
    def.length       = (p2 - p1).Length();           // sqrtf(dy*dy + dx*dx)
    def.frequencyHz  = 4.0f;                          // literal @c4ec
    def.dampingRatio = 0.5f;
    jd.joint = m_world->CreateJoint(&def);
    t.x1 = p1.x;  t.x2 = p2.x;  t.y1 = p1.y;  t.y2 = p2.y;
}
m_joints.add(jd);
objects.joints[name] = t;
```
No limits, no other joint types. For `type != 1` the array entry holds a garbage pointer and the next
`update()` would crash; levels only use type 1. Every frame `update()` refreshes `x1,y1,x2,y2` from
`GetAnchorA/B()` (section 2). Jointed bodies do not collide with each other (`collideConnected = false`).

### 4.2 Lua `destroyJoint(name)` → `destroyJointLua` @0000b5f4

First array entry whose `name` matches: `objects.joints[name] = nil`; `m_world->DestroyJoint(joint)`; remove
the array entry (order preserved); stop. Unknown name: no-op.

---

## 5. Per-object functions

"found" = name present in the hashtable (lookup as in 1.1). `d` = its data.

| function | semantics |
|---|---|
| `setPosition(name,x,y)` @bb28 | `body->SetTransform(b2Vec2(x,y), body->GetAngle())` (angle = `m_sweep.a`). Then `objects.world[name].x = x; .y = y`; `d.snap[0].xy = d.snap[1].xy = d.render.xy = (x,y)`. Does **not** wake the body, does not touch velocity. `SetTransform` is a silent no-op while the world is locked. Unknown name: original dereferences NULL (crash) / `getTable` throws. |
| `setRotation(name,a)` @c89c | `body->SetTransform(body->GetPosition(), a)`; `objects.world[name].angle = a`; `d.snap[0].angle = d.snap[1].angle = d.renderAngle = a` (raw, not normalised). No wake. Unknown name: crash. |
| `setVelocity(name,vx,vy)` @c6b4 | if found and body != NULL and `m_type != b2_staticBody`: `m_linearVelocity = (vx,vy)`. No wake. Unknown: no-op. |
| `setAngularVelocity(name,w)` @c5e4 | same guard: `m_angularVelocity = w`. No wake. Unknown: no-op. |
| `applyImpulse(name,ix,iy,px,py)` @b9e0 | = inlined `b2Body::ApplyImpulse(impulse, point)`: only if found, body != NULL and `m_type == b2_dynamicBody`: if asleep → `m_flags |= e_awakeFlag; m_sleepTime = 0`; `v += invMass * (ix,iy)`; `w += invI * ((px − c.x) * iy − (py − c.y) * ix)` with `c = m_sweep.c` (world centre of mass). `(px,py)` is a world point in physics units. Unknown / static: no-op. |
| `applyForce(name,fx,fy,px,py)` @beb8 | = inlined `b2Body::ApplyForce`: same guards and wake; `m_force += (fx,fy)`; `m_torque += (px − c.x) * fy − (py − c.y) * fx`. (Unused by 1.0 scripts.) |
| `setSleeping(name,flag)` @c78c | `flag == true`: `SetAwake(false)` = `m_sleepTime = 0; flags &= ~e_awakeFlag; v = 0; w = 0; force = 0; torque = 0`. `flag == false`: `SetAwake(true)` = `m_sleepTime = 0; flags |= e_awakeFlag`. Unknown name: NULL dereference in the original. |
| `setMaterial(name,mat)` @b7c4 | `"wood"` → `d.material = 1`, `"rock"` → 2, `"light"` → 3, anything else → unchanged. Only drives the rolling sounds in `update()`; the *damage* material is read from the Lua table field `material` (section 6). Unknown name: writes the hashtable's default value (harmless). |
| `setSprite(name,sprite)` @b8cc | `d.sprite = resources->findSheet(sprite)->getSprite(sprite)` (rendering). |
| `setTexture(name,sheet)` @b76c | `d.texture = resources->getSpriteSheet(sheet)->getImage()` (rendering). |
| `setObjectParameter(name,id,value)` @b928 | `(int)id == 1`: `d.param1 = ((int)value == 1)` (render flag). `(int)id == 2`: `d.body->SetType(value == 0.0f ? b2_staticBody : b2_dynamicBody)`. Other ids: nothing. `(int)` = truncation. `SetType` (when the type actually changes): `ResetMassData()`, zero velocities if static, `SetAwake(true)`, zero force/torque, flag all contacts for filtering. The scripts use (2,0) to freeze waiting birds and (2,1) to arm the next bird, and (1,1) for `levelGoal` objects. Unknown name with id 2: crash (NULL body). |
| `removeObject(name)` @12578 | If found: `m_world->DestroyBody(d.body)` (also destroys attached Box2D joints and contacts) and erase the hashtable entry. Then **always**: for every `jointData` whose `end1 == name` or `end2 == name`: `objects.joints[jd.name] = nil` and remove the array entry (no `DestroyJoint`, the body destruction already did it). Does **not** touch `objects.world[name]`; the scripts set that to nil themselves right after. |
| `setLevelLimits(l,_,r,_)` @6028 | `m_limitLeft = (int)l; m_limitRight = (int)r` (truncate toward zero). Args 2 and 4 are ignored; the lower limit y > 20 is hard-coded in `update()`. |
| `setPhysicsEnabled(b)` | `+0x16c = b`. Initially true; `initialize()` in Lua sets false. |
| `setEditing(b)` | `+0x16e = b`: BeginContact does nothing while true. |
| `setPhysicsSimulationScale(s)` | stored at +0x168, never read. Lua calls it once with `physicsToWorld` (20). No effect. |
| `setWorldScale(s)` | `+0x17c = s`, and `app->scale = app->prevScale = s` (section 9.4). |

---

## 6. Contact handling

- `GameLua::EndContact` @00005f44: empty. `PreSolve`/`PostSolve`: base-class no-ops. `RayCollide`: returns false.
- `ShouldCollide`: section 3.5.
- `BeginContact` is invoked by `b2Contact::Update` @0012955c (listener vtbl+8) when a contact starts touching,
  i.e. **during `b2World::Step`**: from `b2ContactManager::Collide` at the start of the step, and from the TOI
  solver (the latter **inferred** from the Box2D source, not traced in the binary). The Lua callbacks run right
  there, synchronously. No impulse / normal / manifold data is used: damage is computed
  purely from body **velocities and masses at the moment the contact begins** (pre-solve velocities).

### 6.1 `GameLua::BeginContact(b2Contact* c)` @0000cbec (thunk @0000cbe4)

```cpp
if (m_loading /*+0x16d*/ || m_editing /*+0x16e*/) return;
b2Body* bA = c->m_fixtureA->m_body;   b2Body* bB = c->m_fixtureB->m_body;
String nameA = getBodyName(bA), nameB = getBodyName(bB);          // "" if unknown
RenderObjectData& dA = m_objects.contains(nameA) ? m_objects[nameA] : m_default;
RenderObjectData& dB = ... likewise;
LuaTable world = objects.world;
if (!world.isTable(nameA) || !world.isTable(nameB)) return;       // raw get + type == table

if (!dA.controllable && !dB.controllable) {
    // ---------- block vs block ----------
    float dx = bA->m_mass * bA->v.x - bB->m_mass * bB->v.x;
    float dy = bA->m_mass * bA->v.y - bB->m_mass * bB->v.y;
    float F  = sqrtf(dy*dy + dx*dx) / 10.0f;                       // |m_A v_A − m_B v_B| / 10   (10f @d2dc)
    float score = T.scoreTable.blocks.score;                       // read BEFORE the callback

    bool flagA;  float total;
    LuaTable& tA = dA.table;
    if (!tA.isNumber("strength"))            { flagA = true;  total = 0; }
    else {
        float def = tA.isNumber("defence") ? tA.defence : 0.0f;
        if (tA.isNumber("defence") && F < def) { flagA = false; total = 0; }
        else {
            float s = tA.strength, dmg = F - def, ns = s - dmg;
            tA.strength = ns;
            flagA = true;
            total = (ns < 0.0f) ? s : dmg;                         // capped at the strength that was left
        }
    }
    bool flagB;
    LuaTable& tB = dB.table;
    if (!tB.isNumber("strength"))            flagB = true;
    else {
        float def = tB.isNumber("defence") ? tB.defence : 0.0f;
        if (tB.isNumber("defence") && F < def) flagB = false;
        else {
            float s = tB.strength, dmg = F - def, ns = s - dmg;
            tB.strength = ns;
            total += (ns < 0.0f) ? s : dmg;
            flagB = true;
        }
    }
    call Lua  blockCollision(nameA, nameB, F, flagA || flagB);     // (string, string, number, boolean)
    if (total > 0.0f)
        T.scoreTable.blocks.score = score + floorf(total) * 10.0f; // uses the value read before the callback
    return;
}

if (dA.controllable && dB.controllable) {
    // ---------- bird vs bird ----------
    float sA = sqrtf(bA->v.y*bA->v.y + bA->v.x*bA->v.x);
    float sB = sqrtf(bB->v.y*bB->v.y + bB->v.x*bB->v.x);
    float F  = (sA > sB) ? (sA * bA->m_mass) / 10.0f : (sB * bB->m_mass) / 10.0f;
    call Lua  birdCollision(nameA, nameB, F, 0);                   // (string, string, number, number)
    return;
}

// ---------- bird vs block (exactly one controllable) ----------
bool aIsBird = dA.controllable;
String  bird   = aIsBird ? nameA : nameB,   other   = aIsBird ? nameB : nameA;
b2Body* bb     = aIsBird ? bA : bB;
String  birdMat  = (aIsBird ? dA : dB).table.getString("material");   // Lua field, must be a string
String  otherMat = (aIsBird ? dB : dA).table.getString("material");

float F = bb->m_mass * sqrtf(bb->v.y*bb->v.y + bb->v.x*bb->v.x) / 10.0f;   // bird momentum / 10
float cdf = T.colorDamageFactor, ybf = T.yellowBirdFactor, bbf = T.blueBirdFactor;   // read every contact
if (birdMat == "wood"  && otherMat == "wood")  F = F * (cdf * ybf);   // yellow bird vs wood
if (birdMat == "rock"  && otherMat == "rock")  F = F * cdf;           // black bird vs stone
if (birdMat == "light" && otherMat == "light") F = F * (cdf * bbf);   // blue bird vs glass

float damage = 0.0f;
if (world.isTable(other)) {
    LuaTable t = world[other];
    if (t.isNumber("strength")) {
        float def = t.isNumber("defence") ? t.defence : 0.0f;
        damage = F - def;                                   // may be <= 0, passed on as is
        if (damage > 0.0f) {
            float s  = t.strength;
            float ns = s - damage;
            t.strength = ns;
            if (ns < 0.0f) {                                // the block is destroyed by this hit
                float k = ((-ns / bb->m_mass) / F) * 10.0f * 1.75f;     // 10f @daec, 1.75f @daf0
                if (k > 1.0f) k = 1.0f;                                  // 1f @daf4
                b2Vec2 v = bb->m_linearVelocity;
                b2Body* b2 = body(bird);                    // hashtable lookup by name (same body)
                if (b2 && b2->m_type != b2_staticBody) {
                    b2->m_linearVelocity.y = k * v.y;       // direct member write, no wake
                    b2->m_linearVelocity.x = k * v.x;
                }
                c->m_flags &= ~0x20;                        // b2Contact::e_enabledFlag: SetEnabled(false)
                damage = s;                                 // report the strength that was left
            }
        }
    }
}
call Lua  birdCollision(bird, other, F, floorf(damage));
```

Details that matter:
- `LuaTable::isNumber` = `lua_isnumber` on a raw get; `getNumber`/`getString` throw if the raw field is not
  exactly a number / string. `isTable` = raw get + `lua_type == LUA_TTABLE`.
- In the block-block branch the per-object tables are the references stored at creation (`d.table`); in the
  bird branch the block's table is looked up again as `objects.world[other]`. Same tables in practice.
- Disabling the contact lets the bird pass through the destroyed block for the rest of **this** `Step` call only:
  `b2Contact::Update` re-enables every contact at the start of each step (`m_flags |= e_enabledFlag`), and
  BeginContact does not fire again while the shapes keep touching. The block itself is removed later by Lua
  (`updateGame` sees `strength <= 0`), so if several sub-steps run in one frame the later ones collide normally
  with the already "dead" block.
- Lua defaults (`updateValues`, called natively from `compile()` and again by Lua `initialize`):
  `colorDamageFactor = 4`, `blueBirdFactor = 0.725`, `yellowBirdFactor = 0.6`,
  `collisionParticleForceThreshold = 10`, `collisionSoundForceThreshold = 3`.
- Natively done scoring: only `scoreTable.blocks.score += floor(total) * 10` (block-block). Bird hits are scored
  by Lua inside `birdCollision` (`scoreTable[bird].score += damage * 10`, floating score text). Destruction
  bonuses, particles and all collision sounds are Lua (`birdCollision`, `blockCollision`, `updateGame`).
  The only sounds native code plays here are the three rolling loops in `update()`.
- Lua signatures: `birdCollision(birdName, otherName, force, damage)`; for bird-bird the two names are in
  fixture order and damage is 0. `blockCollision(nameA, nameB, force, damaged)`.
- Contacts involving a body that is not in the hashtable (leaked duplicate, section 3.5) resolve to name `""`
  and are ignored because `objects.world[""]` is not a table.

---

## 7. Files: `loadLevel`, `saveLevel`, `loadLuaFile`, `saveLuaFile`, `compile`

Path helpers:
- `framework::App::path(name)` @000b7b1c = `PathName(PathName(os->getDefaultDataPath(), m_dataPath), name)` =
  `<AngryBirds.app bundle path>/data/<name>` (`setDataPath("data")` in the GameApp ctor).
- `io::AppDataInputStream(name)` / `AppDataOutputStream(name)` = `<NSDocumentDirectory>/<name>` (root path set in
  `applicationDidFinishLaunching` from `NSSearchPathForDirectoriesInDomains(9,1,YES)[0]`). The input stream
  **throws `io::IOException`** ("Failed to open {0}") if `fopen(path,"rb")` fails.
- `io::FileInputStream(path)` is the bundle reader (`FileInputStream::sm_fileBundle` is set by GameApp; see the
  Resources spec for bundle index handling). Not analysed here.

### 7.1 `loadLuaFile(const String& file, const String& tableName)` @000064ec (native only)
`FileInputStream in(app->path(file)); LuaTable t(L); t.read(&in); T[tableName] = t;`

`LuaTable::read(stream)` @00077bec = `L->compile(stream, stream->available(), stream->toString(), /*env*/ this)`:
`lua_load` the stream (source text **or** precompiled 5.1 bytecode; shipped files are bytecode), on success
`lua_setfenv(chunk, <this table>)` and `lua_call(chunk, 0, 0)`. I.e. the file is *executed* and its global
assignments become the fields of the table. Load error → `LuaException("Error while compiling Lua: {0}")`.

### 7.2 Lua `loadLevel(name)` @0000db94
```cpp
m_hRock = m_hWood = m_hLight = -1;        // +0xa8, +0xac, +0xb0
m_acc = 0;                                // +0x178
m_loading = true;                         // +0x16d (BeginContact disabled)
printf("Loading blocks...\n");
loadLuaFile("blocks.lua",    "blockTable");        // reloaded on every level load
loadLuaFile("particles.lua", "particleTable");
printf("Removing physics objects...\n");
for (each used pair o) m_world->DestroyBody(o.body);       // joints die with their bodies
m_joints.resize(0);
m_objects.clear();
m_particles->clearAll();
for (i in 0..5) m_trajectory[i].size = 0;                   // six arrays at +0x180
printf("Loading level from file...\n");
FileInputStream in(app->path(name + ".lua"));
LuaTable t(L);  t.read(&in);
T.loadedObjects = t;
m_loading = false;
```
No native post-processing of the loaded data. It does **not** touch the Lua `objects` table, the world gravity,
`m_slot`, the level limits or physicsEnabled. Bodies leaked by duplicate names (3.5) survive.

Everything else is Lua (`loadLevelInternal`): `objects.world/joints/counts = {}`, iterate `loadedObjects.world`
with `pairs`, call `createObject` (→ `createBox`/`createCircle` with half extents `sprite size * 0.5 *
physicsScale * 0.92`, or `* 1` when density == 0), `setRotation`, `setMaterial`, optional `setTexture`,
`setLevelLimits`, the ground (`createBox("ground","", (left+right)/2, 5, 1000, 5, 0, 0.8, 0, true, false)`),
`createJoint` for `loadedObjects.joints`, theme, camera data, then for each controllable object
`setRotation(name,0)` + `setObjectParameter(name,2,0)` (static) and `setObjectParameter(firstBird,2,1)`.
A level file's top-level keys: `world` (name → object table), `joints`, `counts`, `physicsToWorld`, `theme`,
`castleCameraData`, `birdCameraData`, optionally `startObject`.

### 7.3 Lua `saveLevel(name)` @00006368
`FileOutputStream out(app->path(name + ".lua")); m_objectsTable.write(&out, 0, false);` writes the whole `objects`
table (text, format 8.2) into the bundle data dir. Editor only.

### 7.4 Lua `saveLuaFile(fileName, tableName, appData)` @0000610c
```cpp
if (!appData) { FileOutputStream    out(app->path(fileName)); if (T.isTable(tableName)) T[tableName].write(&out, 0, false); }
else          { AppDataOutputStream out(fileName);            if (T.isTable(tableName)) T[tableName].write(&out, 0, false); }
```
The file is created/truncated even when the table does not exist.

### 7.5 Lua `compile()` @00006930
`FileInputStream in(App::get()->path("gamelogic.lua")); L->compile(&in, in.available(), in.toString(), /*env*/ this);`
then `call("updateValues")`. Also triggered by GameApp when `keyPressed["0"]` is true (never on iOS).

---

## 8. Persistence

### 8.1 When
- Load: once, in the constructor (1.5): `Documents/highscores.lua` → global `highscores`,
  `Documents/settings.lua` → global `settings`. Missing/unreadable file → empty table (highscores also prints
  "\nUnable to load highscores!\n\n").
- Save: only from Lua, via `saveLuaFile("settings.lua", "settings", true)` and
  `saveLuaFile("highscores.lua", "highscores", true)` (level completed, audio toggled, lastOpenLevel bumped, …).
  Native code never saves on its own. GameApp writes `highscores.volume` when the volume keys change the master
  volume (not reachable on iOS) and reads it at start-up (`setMasterVolume`).
- `clearSaveFunction` and `countsSaved` are nil globals (1.4), not native features.

### 8.2 File format: `LuaTable::write(OutputStream*, int indent, bool comma)` @0007b074 / `writeValue` @000792a0

Plain text, Lua source, one byte written at a time. For a table, iterate with `lua_next` (table order):

```
hasNamed = false; first = true
for each (k, v):
    if type(k) == string:
        if k == "_G": skip
        else:
            write indent × ' '
            write k                      (raw, unquoted, unescaped)
            write " = "
            writeValue(v, indent)
            if comma: write ","
            write "\n"
            hasNamed = true; first = false
    else:                                (number / any non-string key: positional)
        if not hasNamed:
            if first: write indent × ' '   else: write ","
            writeValue(v, indent)
        (non-string keys seen AFTER a string key are silently dropped)
        first = false

writeValue(v, indent):
    boolean → "true" / "false"
    number  → sprintf("%g", (double)v)          6 significant digits, e.g. 1234567 → "1.23457e+06"
    string  → '"' + raw bytes + '"'             no escaping at all
    table   → "{\n" + write(v, indent + 4, comma = true) + "\n" + indent × ' ' + "}"
    other   → "nil"
```
Top level is written with `indent = 0, comma = false`. Example (`settings`):

```
currentLanguage = "en_EN"
lastOpenLevel = 3
tutorials = {
    BIRD_RED = {
        sprite = "TUTORIAL_1",

    },

}
```
(note the blank line before every closing brace; an array-like table prints as `{\n    1,2,3\n}`).
Reading is 7.1: execute the file with the new table as environment. Any reader that runs the text as a Lua chunk
with `setfenv` is compatible; any writer that emits valid `key = value` Lua assignments is compatible.

---

## 9. Input plumbing

### 9.1 Touch → App (`MyEAGLView touchesBegan/Moved/Ended/Cancelled` @69a6c/@69854/@69e40/@69c54)
For each `UITouch`: `p = [touch locationInView:view]`, `x = (int)p.x, y = (int)p.y`, then
`gr::Context::transformPointToScreen(x, y)` @00034e70 (W,H = context width/height):
orientation 1: `(x', y') = (y, H − x)`; orientation 3: `(W − y, x)`; orientation 2: `(W − x, H − y)`.
Result: integer pixels in the landscape screen, origin top-left, y down (480×320 on the original device).

- began: `app->touchBegan({id, x, y})` (appends to `App` touch array at app+0x270, 12-byte entries `{id,x,y}`);
  if the view has no "single touch": make this one the single touch, `app->mouseMove(x,y)`, and if
  `!app->isKeyDown(LBUTTON)`: `app->keyDown(LBUTTON); app->setKeyDown(LBUTTON,true)`.
- moved: `app->touchMoved(..)` (updates the entry with the same id); if it is the single touch: `mouseMove(x,y)`.
- ended/cancelled: `app->touchEnded(..)` (erases the entry); if it is the single touch: clear it,
  `mouseMove(x,y)`, and if `isKeyDown(LBUTTON)`: `app->keyUp(LBUTTON); setKeyDown(LBUTTON,false)`.
- `applicationDidBecomeActive` clears the single touch.

### 9.2 GameApp handlers
- `GameApp::mouseMove(x,y)` @44c4: `cursor.x = (float)x; cursor.y = (float)y` (immediately, not per frame).
- `GameApp::keyDown(k)` @438c: `m_pressed[k] = true` (byte array at app+0x298). `keyUp(k)` @439c:
  `m_released[k] = true` (app+0x2ee). `App::isKeyDown(k)` reads app+0x214+k.
- `GameApp::mouseWheel(d)` @43c8 (desktop only): adjusts app->scale by ±0.05 (0.025 below scale 1), clamps to
  [0.01, 100], sets `prevScale` to the old scale first, writes `cursor.wheel`.

### 9.3 `GameApp::update(dt, ctx, n)` @000045e4, before calling `GameLua::update`
```cpp
touches = app->touches();
static bool zooming; static float initialDistance, initialScale;
if (touches.size() == 2) {
    if (!zooming) { zooming = true;
                    initialDistance = dist((t0.x,t0.y),(t1.x,t1.y));     // ints → float, sqrtf
                    initialScale    = app->scale; }                       // app+0x27c
    app->scale = dist(t0, t1) / initialDistance * initialScale;           // every frame while 2 touches
} else if (zooming) zooming = false;

keyPressed.LBUTTON  = m_pressed[0x39];
keyReleased.LBUTTON = m_released[0x39];
keyHold.LBUTTON     = app->isKeyDown(0x39);
m_pressed[0x39] = m_released[0x39] = false;
```
Only `LBUTTON` is ever refreshed. All 86 key names exist in the three tables with value `false`
(set once in the GameApp ctor): `NONE, A..Z, PAGEDOWN, PAGEUP, HOME, END, INSERT, DELETE, RETURN, SHIFT,
CONTROL, TAB, SPACE, BACKSPACE, PAUSE, ESCAPE, F1..F12, LEFT, RIGHT, UP, DOWN, LBUTTON (id 57 = 0x39), MBUTTON,
RBUTTON, "0".."9", MULTIPLY, ADD, SUBTRACT, DIVIDE, LEFTSOFT, RIGHTSOFT, MIDDLE, ASTERISK, HASH, CLEAR, EDIT,
SEND, VOLUME_UP, VOLUME_DOWN, KEY_GAMING_A, KEY_GAMING_B`.
Then (dead on iOS because those keys stay false): `VOLUME_UP`/`ADD` → master volume +0.1 (clamped 0..1) and
`highscores.volume = v`; `VOLUME_DOWN`/`SUBTRACT` → −0.1; `keyPressed["0"]` → `GameLua::compile()`.
Then accelerometer auto-flip between the two landscape orientations (filtered y > 0.75 while orientation == 1 →
`os->setOrientation(3)`; y <= −0.75 otherwise → `setOrientation(1)`). Then render-scene begin, clear,
`GameLua::update(dt)`.

### 9.4 What Lua sees
- `cursor.x`, `cursor.y`: last position of the *first* finger, landscape pixels, top-left origin (Lua itself
  initialises both to 0 in `initialize`). `cursor.wheel` only on desktop.
- `keyPressed.LBUTTON` true for exactly one frame after the first finger goes down, `keyReleased.LBUTTON` one
  frame after it lifts, `keyHold.LBUTTON` while it is down.
- `touches`: **number** of active touches (not a table; no per-touch coordinates reach Lua).
- Pinch zoom path: two fingers change `app->scale` (9.3) → `GameLua::update` adds
  `(app->scale − app->prevScale) * 0.5` to the Lua global `zoomLevel` every frame in which they differ → the
  camera code in Lua derives `worldScale` and calls `setWorldScale(worldScale)` → which sets
  `GameLua.worldScale` and resets `app->scale = app->prevScale = worldScale` (ending the feed until the pinch
  moves again). `screenWidth`/`screenHeight` are refreshed every frame from the context.

---

## 10. Not determined / caveats

1. **Exact Box2D revision.** The binary's Box2D is close to, but not identical with, port/Box2D (section 0.2).
   I verified the settings constants and a handful of functions; I did **not** diff `b2World::Solve/SolveTOI`,
   `b2ContactSolver`, `b2TimeOfImpact`, `b2CollidePolygons`, `b2DynamicTree` against the port source. Constants
   in them match stock values except those listed in section 0.
2. `b2Body` offsets used above (binary): `m_type` 0, `m_flags` 4 (u16: island 1, awake 2, autoSleep 4, bullet 8,
   fixedRotation 0x10, active 0x20), `m_xf.position` 0xc, `m_xf.R.col1` 0x14, `col2` 0x1c, `m_sweep.localCenter`
   0x24, `c0` 0x2c, `c` 0x34, `a0` 0x3c, `a` 0x40, `m_linearVelocity` 0x48, `m_angularVelocity` 0x50, `m_force`
   0x54, `m_torque` 0x5c, `m_world` 0x60, `m_fixtureList` 0x6c, `m_contactList` 0x78, `m_mass` 0x7c, `m_invMass`
   0x80, `m_I` 0x84, `m_invI` 0x88, `m_linearDamping` 0x90, `m_angularDamping` 0x94, `m_sleepTime` 0x98,
   `m_userData` 0x9c. `b2Fixture`: `m_density` 0x10, `m_next` 0x14, `m_body` 0x18, `m_shape` 0x1c, `m_friction`
   0x20, `m_restitution` 0x24, `m_filter` 0x2c, `m_isSensor` 0x32, `m_userData` 0x34. `b2Contact`: `m_flags` 4,
   `m_fixtureA` 0x30, `m_fixtureB` 0x34. Offset 8 of b2Body (`m_islandIndex`) is inferred.
3. GameLua fields `+0x24`, `+0x74..+0x8b`, `+0xa4` and the exact element type of the six arrays at `+0x180`:
   not identified (rendering side).
4. `game::Resources::playAudio(name, volume, bool, int)`: I pass on `(name, vol, true, 2)` as seen; the meaning
   of the bool (loop, **inferred**) and the int (clip group/limit class, **inferred** from
   `setClipCountLimit(1..3, n)`) belongs to the Resources spec.
5. What happens to a `LuaException` / `IOException` thrown out of a native function called from Lua
   (`lua_call` is unprotected, Lua is compiled as C++): not traced. The shipped scripts never trigger one.
6. `io::FileInputStream` bundle-index lookup and `FileOutputStream` behaviour: not analysed (Resources layer).
7. Whether the hashtable iteration order could matter: I found no order-dependent computation, but
   `lua_next` order decides the key order in saved files (harmless).
8. `data.param1` (+0x4e) and `data.texture` (+0x20) consumers are in drawGame (other spec).
9. Frame pacing (the `NSThread` main loop that triggers `AppController::update`) was not analysed; only the dt
   computation and the 0.1 s clamp.
10. The decompiler lost the `"blocks"` argument of the first `scoreTable` lookup in BeginContact; the
    disassembly (@d400) confirms `scoreTable.blocks.score` for both the read and the write.
