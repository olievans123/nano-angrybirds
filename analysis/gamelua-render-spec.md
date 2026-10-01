# Angry Birds 1.0 (iOS) — `GameLua` rendering side + `Particles` : behavioural spec

Source: Ghidra output in `decomp-annot/`, checked against a real VFP-aware disassembly of the ARMv6 binary
(`analysis/dis.py`, see §10). All addresses are VAs in `v1.0/Payload/AngryBirds.app/AngryBirds`.

Confidence tags used throughout:

* **[D]** read from the disassembly (float math / argument order verified instruction by instruction)
* **[C]** read from the Ghidra decompilation only (integer / pointer logic, low risk)
* **[V]** verified against the shipped data files or the Lua scripts
* **[I]** inferred (not directly provable from this binary); every such claim is repeated in §9

Companion specs: `analysis/resources-spec.md` (sprite sheets, fonts, `res.*`, texture upload, `game::Anchor`).
Nothing from it is repeated here except where GameLua depends on it.

---------------------------------------------------------------------------------------------------------------------

## 0. Quick reference

### 0.1 Coordinate spaces

| space | unit | notes |
|---|---|---|
| physics | Box2D metres | `RenderObjectData.x/y/angle` (interpolated body transform) |
| world | "world pixels" | `world = physics * 20.0f` — the constant `20f` is hard-coded in the native code (@0000a0d0, @0000a5d0) **[D]**; y grows downwards, ground level is world y = 0 |
| screen | logical pixels, origin top-left, y down | `screen = (world - topLeft) * worldScale`; `ctx->width()/height()` = logical size (480x320 landscape on the phone) |
| NDC | GL clip space | `ndc.x = 2*X/W - 1`, `ndc.y = 1 - 2*Y/H`, z = 0.001 |

`topLeft` (`GameLua+0x170/+0x174`) and `worldScale` (`+0x17c`) are pushed by the Lua camera code every frame:
`screen.left = screen.x - screenWidth*0.5/worldScale`, `screen.top = screen.y - screenHeight*0.5/worldScale`,
`setTopLeft(screen.left, screen.top)` (Lua `defaultCamera`, gamelogic.lua:15483) **[V]**.

### 0.2 GameLua members used by the render code **[C]/[D]**

| off | type | meaning | written by |
|---|---|---|---|
| +0x08 | `lua::LuaState*` | | ctor |
| +0x18 | `gr::Context*` | the EGL context (`ctx`) | ctor |
| +0x1c | `framework::App*` | `app+0x27c`, `app+0x280` receive a copy of worldScale | ctor |
| +0x20 | `game::Resources*` | `res` | ctor |
| +0x28 | `Particles*` | | ctor |
| +0x6c | `uint32` | `bgColor` = `0x00RRGGBB` (frame clear colour) | `setBGColor`, `drawBackground` |
| +0x74,+0x78,+0x7c | float | theme sky colour r,g,b (0..255) | `setTheme` |
| +0x80,+0x84,+0x88 | float | theme ground colour r,g,b (0..255) | `setTheme` |
| +0x8c | `lang::String` | theme `image` (e.g. `"BACKGROUND_1"`) | `setTheme` |
| +0x90 | `lang::String` | sheet A = `"INGAME_%d" % (index+2)` | `setTheme` |
| +0x94 | `lang::String` | sheet B = `"INGAME_2"` (constant) | `setTheme` |
| +0x98 | `lang::Array<String>` | 5 layer sprite names (`ptr,+0x9c size,+0xa0 cap`), see §4.1 | `setTheme` |
| +0xa4 | int | theme `index` | `setTheme` |
| +0xb8 | `lang::Hashtable<String,RenderObjectData>` | the object table (`+0xc0` capacity, `+0xc4` bucket array, `+0xc8` loadFactor 0.75, `+0xcc` count, `+0xd0` threshold, `+0xd4` default value) | createBox/Circle, removeObject, loadLevel |
| +0x158 | `lua::LuaTable` | the script-visible table `clippedText` | `clipText` |
| +0x170,+0x174 | float | `topLeftX`, `topLeftY` (world px) | `setTopLeft` |
| +0x17c | float | `worldScale` (init 1.0) | `setWorldScale` |
| +0x180 | `lang::Array<float2>[6]` | trajectories, 12 bytes each (`ptr,size,cap`) → `+0x180 .. +0x1c7` | `addToTrajectory`, … |
| +0x1c8 | int | trajectory base index (0 or 3, init 0) | `startNewTrajectory` |
| +0x1cc | `MaskedImage*` | two-texture batch for textured static blocks | ctor |
| +0x1d0,+0x1d4 | int | level limit left / right (physics units) | `setLevelLimits` |

`lang::String` is 4 bytes (a handle), so `names[i]` is at `*(this+0x98) + 4*i`.

### 0.3 Lua-visible natives covered here

Registered on the GameLua script object by the ctor (@00010474) **[C]**. The game script runs with the GameLua
object's table as its function environment: `lua::LuaObject`'s ctor creates a table holding `_G` (the real globals
table) and `this`, and `GameLua::compile` @00006930 passes that table to `LuaState::compile(stream, size, name, env)`
@00076a50, which does `lua_setfenv(chunk, env)` **[C]**. There is no `__index` fallback, which is why the script
reaches real globals through `_G.` (`_G.res.drawSprite`, `_G.particles.addParticles`, `_G.math`, `_G.pairs`, …).
Data files are loaded by `GameLua::loadLuaFile(file, tableName)` @000064ec into a fresh table stored **on the GameLua
object** under `tableName` (`blockTable`, `particleTable`, …) (see §8.8).

| Lua name | C++ | § |
|---|---|---|
| `setRenderState(...)` | `GameLua::setRenderState(lua::LuaState*)` @0000a960 | 2 |
| `drawGameNative()` | `GameLua::drawGame()` @00009ad4 | 3 |
| `drawBackgroundNative()` | `GameLua::drawBackground()` @0000a5d8 | 4 |
| `drawForegroundNative()` | `GameLua::drawForegroundNative()` @00006ba0 | 4 |
| `setTheme(name)` | @000092b8 | 4.1 |
| `setBGColor(r,g,b)` | @00005f50 | 4.5 |
| `setTopLeft(x,y)` | @00006000 | 4.5 |
| `setWorldScale(s)` | @0000600c | 4.5 |
| `setLevelLimits(l,t,r,b)` | @00006028 | 4.5 |
| `addToTrajectory(which,x,y)` | @00008ff8 | 5 |
| `startNewTrajectory()` | @0000aa64 | 5 |
| `drawLine2D(x1,y1,x2,y2,width,r,g,b,a)` | @00006044 | 6.1 |
| `drawRect(r,g,b,a,x1,y1,x2,y2,keepState)` | @00007898 | 6.2 |
| `drawTexturedRect(name,x1,y1,x2,y2,flag)` | @000066e8 (not used by the 1.0 scripts) | 6.3 |
| `clipText(group,id,maxWidth)` | @00006da4 | 7 |
| `drawParticlesNative()` | `GameLua::drawParticles()` @000060dc → `Particles::draw` (not used by the 1.0 scripts) | 8 |
| `setSprite(name,sprite)` / `setTexture(name,sheet)` / `setObjectParameter(name,k,v)` | @0000b8cc / @0000b76c / @0000b928 | 3.1 |
| `_G.particles.addParticles(type,amount,x,y,w,h,angle)` | `Particles::addParticles` @001185c0 | 8 |

---------------------------------------------------------------------------------------------------------------------

## 1. `gr::RenderState2D` and the 2D draw calls of `gr::EGL_Context`

### 1.1 Where it lives

`ctx->getRenderState2D()` (vtable slot +0x58, @000a876c) returns `(RenderState2D*)((char*)ctx + 0x10c)` — a pointer to
the **live** state inside the context; GameLua, Particles and MaskedImage poke fields through it directly.
`ctx->setRenderState2D(const RenderState2D&)` (slot +0x90, @000a880c) is `memcpy(ctx+0x10c, &s, 0x54)`.
`sizeof(RenderState2D) == 0x54`. **[C]**

### 1.2 Layout **[C]/[D]**

| off | type | name used in this doc | default | meaning |
|---|---|---|---|---|
| +0x00 | `gr::Rect{int left,top,right,bottom}` | `clip` | `(-32000,-32000,32000,32000)` | scissor rectangle in logical screen pixels (applied at batch flush) |
| +0x10 | float | `m00` | 1 | cos θ |
| +0x14 | float | `m01` | 0 | sin θ |
| +0x18 | float | `m10` | 0 | −sin θ |
| +0x1c | float | `m11` | 1 | cos θ |
| +0x20 | float | `tx` | 0 | translation x (added **before** the scale) |
| +0x24 | float | `ty` | 0 | translation y |
| +0x28 | float | `sx` | 1 | scale x (applied **last**, about the screen origin) |
| +0x2c | float | `sy` | 1 | scale y |
| +0x30 | float | `px` | 0 | pivot x, measured from the quad's top-left corner |
| +0x34 | float | `py` | 0 | pivot y |
| +0x38 | uint8 | `f38` | 1 | batch key only — **no visual effect in this build** |
| +0x3c | int | `f3c` | 1 | batch key only (**[I]** source blend enum) |
| +0x40 | int | `f40` | 5 | batch key only (**[I]** destination blend enum) |
| +0x44 | uint8 | `f44` | 0 | batch key only |
| +0x48 | int | `f48` | 0 | batch key only |
| +0x4c | float | `f4c` | 0 | batch key only |
| +0x50 | float | `alphaFactor` | 1.0 | handed to the shader as `"ALPHA_FACTOR"`; the GLES 1.1 shader (`EGL_Shader_Default::setFloat` @000b49ac) only understands `"LINE_WIDTH"` and **ignores it** |

"default" = what the `EGL_Context` ctor, the `EGL_RenderBatcher` ctor (@0010492c) and the literal that
`GameLua::drawRect` / `drawTexturedRect` memcpy over the state all contain.

Fields +0x38…+0x50 are only compared by the batcher to decide whether to flush (§1.5). GameLua never changes
them, so a port can drop them entirely.

### 1.3 The vertex transform (identical in every 2D draw call) **[D]**

For a quad whose untransformed top-left is `(x, y)` and size `(w, h)`:

```
X0 = x + px + tx
Y0 = y + py + ty
for each corner, with (dx, dy) = corner offset from the pivot:
        TL: (-px,   -py)      TR: (w-px,  -py)
        BL: (-px,  h-py)      BR: (w-px, h-py)
    X = X0 + m00*dx + m10*dy          // = X0 + cos*dx - sin*dy
    Y = Y0 + m01*dx + m11*dy          // = Y0 + sin*dx + cos*dy
    ndc.x = X * (2*sx / W) - 1        // W = ctx->width()   (float of an int)
    ndc.y = 1 + Y * (-2*sy / H)       // H = ctx->height()
    ndc.z = 0.001
```

i.e. in screen pixels `screen = S * ( T + P + (x,y) + R(θ) * (corner − P) )`: rotate about the pivot, translate,
then scale everything (including the translation) about the screen origin. A positive θ turns the sprite
**clockwise on screen** (y is down). With θ = 0 the pivot cancels out completely.

* No rounding/snapping to pixels anywhere; all math is 32-bit float. (The *callers* of the int-taking entry points
  truncate, see §6.)
* Culling: the quad is skipped unless its NDC bounding box satisfies
  `maxX >= -1 && maxY >= -1 && minX < 1 && minY < 1`. NaN coordinates fail the test (nothing is drawn).
* Vertex order emitted per quad: `TL, TR, BL, BL, TR, BR` (two triangles, `EGL_RenderBatcher::render` @001049f8).
* `glFrontFace(GL_CW)` (ctx ctor: `ctx[0xaa] = 0x900`), `GL_CULL_FACE` is enabled by every shader `begin()`
  (@000b550c) and `glCullFace` is never called (default `GL_BACK`). The quads above are CW in NDC, so they are
  visible; a **negative `sx` or `sy` (mirroring) would make the quad back-facing and invisible**. The game never
  passes negative scales.

### 1.4 vtable of `gr::EGL_Context` (vtable symbol @001407d8, vptr = +8) **[V: read from the binary, `analysis/vt.py 001407d8`]**

Only the slots reachable from GameLua / Particles / Resources / MaskedImage are listed with semantics.

| slot | addr | method | semantics |
|---|---|---|---|
| +0x08 | 000abe3c | `createShader(const String& name, int)` | clones a named shader template (§1.6) |
| +0x14 | 000a9f08 | `createTexture(w,h,…)` | |
| +0x1c / +0x20 | 000a981c / 000a8d28 | `createImage(String)` / `createImage(w,h,…)` | |
| +0x28 | 000ab860 | `getDynamicPrimitive(type, fmt, nVerts, nIdx)` | scratch vertex buffer used by the batcher |
| +0x2c / +0x30 | 000a874c / 000a8750 | `begin2D()` / `end2D()` | empty |
| **+0x34** | 000ab444 | `drawLine2D(uint argb, int x1, int y1, int x2, int y2, int width)` | §1.7 |
| +0x38,+0x3c,+0x40,+0x48,+0x4c,+0x54 | 000a8754… | `drawLine2D(polyline)`, `drawCircle2D`, `fillTriangle2D`, `fillCircle2D`, `texturizeTriangle2D`, `texturizeCircle2D` | **empty stubs** (4-byte `bx lr`) |
| **+0x44** | 000ab0e0 | `fillRect2D(uint argb, int x, int y, int w, int h)` | §1.8 |
| **+0x50** | 000a8fa8 | `texturizeRect2D(Image*, const float4& uv, int x, int y, int w, int h)` | §1.9 |
| **+0x58** | 000a876c | `getRenderState2D()` | returns `ctx+0x10c` |
| +0x5c | 000a8774 | `viewport()` | `ctx+0x290` |
| **+0x64** | 000a877c | `width()` | logical width (swaps w/h when orientation is 1 or 3) |
| **+0x68** | 000a87a0 | `height()` | logical height |
| +0x80 | 000a87ec | `platform()` | returns 1 (fixed-function GLES 1.1) |
| +0x84 | 000a87f4 | `orientation()` | |
| +0x8c | 000a8804 | `shaderPath()` | |
| +0x90 | 000a880c | `setRenderState2D(const RenderState2D&)` | memcpy 0x54 |
| +0xb4 | 000a8afc | `setOrthographicProjection()` | `setProjection(identity)`; `setProjection` only multiplies in the 0/90/180/270° device-orientation rotation |
| +0xd4 | 000a8ac8 | `present()` | |
| +0xd8 | 000a93c0 | `clear(uint argb, bool color, bool depth, bool stencil)` | flushes the batcher, `glClearColor`, `glClear` |

`gr::EGL_Image` (vtable @001408ec): `+0x14 draw(ctx, float x, float y, int srcX, int srcY, int w, int h)` @000b15c4,
`+0x20 width()`, `+0x24 height()` (logical image size), `+0x30 getTexture()` (the power-of-two GL texture; its
`+0x14 width()` / `+0x18 height()` are the POT size), `+0x34 getShader()` (= `ctx->getSpriteShader()`).

`EGL_Image::draw` is what `game::Sprite::draw` → `SpriteSheet::drawSprite` ends in (resources spec): it applies §1.3
to the quad `(x, y, w, h)` with **float** x, y and UVs `u = srcX/texW … (srcX+w)/texW`, `v = srcY/texH … (srcY+h)/texH`
(no half-texel inset), shader = sprite shader, batch type 2. **[C]**

### 1.5 Batching (`gr::EGL_RenderBatcher`, embedded at `ctx+0x7c`) **[C]**

`render(type, shader, float3 pos[], float2 uv[], float4 col[], Image*, int lineWidth)` appends to the current batch.
Types: 0 = lines (2 verts), 1 = coloured quads, 2 = textured quads. The batch is flushed first if any of these
differ from the batch being built: `type`, `image`, (lines only) `lineWidth`, the clip rect (`Rect::operator==`), or any
of state fields +0x38…+0x50. **The transform is not a batch key** — vertices are already in NDC.

`flush()` (@00104230): builds a dynamic primitive (`GL_LINES` for type 0, `GL_TRIANGLES` otherwise) with position,
optional texcoord, optional colour; `ctx->setClipRect(batch.clip)` (`glEnable(GL_SCISSOR_TEST)`,
`glScissor(left, physH - bottom, right-left, bottom-top)` with w/h clamped to ≥ 0 and device-orientation remap,
@000aa774); `ctx->setOrthographicProjection()`; `shader->setFloat("LINE_WIDTH", lineWidth)` (lines);
`shader->setFloat("ALPHA_FACTOR", …)` (ignored); `shader->setTexture("BASEMAP", image->getTexture())`; then
`begin / beginPass / render / endPass / end`. Model/view/projection are identity (apart from the device rotation).

Batches are also flushed by `ctx->clear()`, by any shader `begin()` (so `MaskedImage::render`, §3.4, is correctly
ordered against batched sprites) and at `present()`. Draw order is therefore strictly call order.

### 1.6 GL state per shader (`EGL_Shader_Default::begin` @000b550c, templates built in `gr::createShaders` @000b6f00) **[C]**

| shader | used for | flags | blend |
|---|---|---|---|
| `"sprite-alpha"` | all textured sprites/quads (`getSpriteShader` @000aac38) | 0xc000 | `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` (premultiplied alpha) |
| `"sprite-plain"` | `fillRect2D`, `drawLine2D` (`getPlainShader` @000aae8c) | 0x9000 | `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` (straight alpha, colour from vertices) |
| `"sprite-straightalpha"` | `MaskedImage` | 0xc000 | `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` |

Flag bits: 0x1000 = `glEnable(GL_COLOR_MATERIAL)` + per-vertex colour (otherwise `glColor4f(1,1,1,1)`);
0x4000 = texture unit 0 enabled and bound to `BASEMAP`; 0x8000 = `glDisable(GL_DEPTH_TEST)`, `glDepthMask(0)`;
0x2000 = lighting (never set here → `glDisable(GL_LIGHTING)`).
Always: `glEnable(GL_NORMALIZE)`, `glEnable(GL_CULL_FACE)`, `glDisable(GL_DITHER)`, `glEnable(GL_BLEND)` unless
(src,dst) == (GL_ONE, GL_ZERO), texture unit 1 disabled unless `BASEMAP1` is set (§3.4). Texture env on unit 0 is
never touched → default `GL_MODULATE` (with white ⇒ texture colour unchanged).

Texture filtering is a property of the texture, not of the state: `GL_TEXTURE_MAG_FILTER = GL_LINEAR`;
`GL_TEXTURE_MIN_FILTER = GL_LINEAR`, or `GL_LINEAR_MIPMAP_NEAREST` for textures that carry mipmaps
(`INGAME_6/7/8.pvr`, the ground textures, have 8 mip levels **[V]**); wrap mode is left at the GL default
`GL_REPEAT` (see resources spec). So every scaled / rotated / sub-pixel-positioned sprite is bilinearly filtered.

### 1.7 `drawLine2D(argb, x1, y1, x2, y2, width)` @000ab444 **[C]**

```
dx = float(x2 - x1); dy = float(y2 - y1)
if dx == 0 && dy == 0: return
P1 = transform of corner TL, P2 = transform of corner BR of the "quad" (x1, y1, w=dx, h=dy)      // §1.3
cull on the 2-point bbox (same test as §1.3)
rgba = unpack(argb)                    // a = bits 24..31, r = 16..23, g = 8..15, b = 0..7
nx = dx/len; ny = dy/len
glWidth = (int)( ny*ny*width*sx + nx*nx*width*sy )         // == (int)(width * s) when sx == sy == s
batcher.render(type 0, plainShader, {P1,P2}, no uv, {rgba,rgba}, no image, glWidth)
```

The line is a real `GL_LINES` primitive drawn with `glLineWidth((float)glWidth)`, not a quad. Blending is straight
alpha. See §9 for how wide GL lines rasterise.

### 1.8 `fillRect2D(argb, x, y, w, h)` @000ab0e0 **[C]**

Quad `(x, y, w, h)` through §1.3, all four vertices coloured `argb`, plain shader, batch type 1.

### 1.9 `texturizeRect2D(image, float4 p, x, y, w, h)` @000a8fa8 **[C]**

Quad `(x, y, w, h)` through §1.3, sprite shader, batch type 2, with **screen-space generated UVs**: for each corner,
using the pre-scale position `(X, Y)` of §1.3 (after pivot/rotation/translation, before `sx, sy`):

```
u = (X + p.z) / ( (image.width()  / texture.width())  * p.x )
v = (Y + p.w) / ( (image.height() / texture.height()) * p.y )
```

Only used by `GameLua::drawTexturedRect` (§6.3).

---------------------------------------------------------------------------------------------------------------------

## 2. `GameLua::setRenderState(lua::LuaState*)` @0000a960 **[C]**

Raw Lua C function (variable argument count `n = lua_gettop`). `st = ctx->getRenderState2D()`.

```
if n >= 2:  st.tx = arg1;  st.ty = arg2
if n >= 4:  st.sx = arg3;  st.sy = arg4            // only when n >= 2 too (nested)
if n >= 5:  a = arg5 (radians)
            c = cosf(a); s = sinf(a)
            st.m00 = c; st.m11 = c; st.m01 = s; st.m10 = -s
if n >= 7:  st.px = arg6;  st.py = arg7
return 0 results
```

* n = 0 or 1: nothing; n = 3: translation only; n = 6: like 5.
* Fields that are not covered by the given argument count keep their previous value. In particular the common
  5-argument call `setRenderState(-screen.left, -screen.top, worldScale, worldScale, 0)` **does not reset the pivot**
  (harmless because the rotation is identity), and nothing here ever touches `clip`.
* Numbers are converted double → float.

Script usage **[V]**: world-space drawing uses `(-screen.left, -screen.top, worldScale, worldScale, angle [, pivotX, pivotY])`
followed by `res.drawSprite(sheet, sprite, worldX, worldY)` (default anchor = pivot, so the sprite's pivot lands on
the world point and the sprite rotates about it — pass the sprite's own pivot as args 6, 7). HUD / menu drawing
uses `(0, 0, 1, 1, 0)`.

---------------------------------------------------------------------------------------------------------------------

## 3. `GameLua::drawGame()` @00009ad4 (Lua `drawGameNative`) **[D]**

### 3.1 `GameLua::RenderObjectData` and the object hashtable

Hashtable entry = 0x60 bytes: key at +0x00, value (`RenderObjectData`, 0x54 bytes) at +0x04, `next` at +0x58, `used`
flag at +0x5c. Offsets below are **entry-relative** (subtract 4 for value-relative; the value-relative offset is what
`setSprite` & co. use after `entry+4`).

| entry off | type | field | set by |
|---|---|---|---|
| +0x00 | String | key (object name) | hashtable |
| +0x04 | String | `name` | createBoxLua / createCircleLua |
| +0x08 | LuaTable (0x10) | `objects.world[name]` | create*Lua |
| +0x18 | `b2Body*` | `body` | create*Lua |
| +0x1c | String | `spriteName` | create*Lua |
| +0x20 | `game::Sprite*` | `sprite` (null if the sprite name is empty → never drawn) | create*Lua: `res.findSheet(s).getSprite(s)`; **`setSprite(name, sprite)`** @0000b8cc does the same lookup and overwrites it |
| +0x24 | `gr::Image*` | `texture` (null by default) | **`setTexture(name, sheetName)`** @0000b76c: `res.getSpriteSheet(sheetName).getImage()` |
| +0x28,+0x2c,+0x30 | float x, y, angle | physics state slot 0 | `GameLua::update` |
| +0x34,+0x38,+0x3c | float x, y, angle | physics state slot 1 | `GameLua::update` |
| +0x40 | float | init −1.0 (damage bookkeeping, not render) | update |
| +0x44,+0x48 | float | **render position x, y in physics units** (interpolated between the two slots) | create*Lua (initial x, y), `GameLua::update` |
| +0x4c | float | **render angle** (radians; init 0) | `GameLua::update`, setRotation |
| +0x50 | bool | `controllable` (last arg of `createBox` / `createCircle`; birds) — also makes the body a bullet | create*Lua |
| +0x51 | bool | init 0 (not render) | |
| +0x52 | bool | `front` flag: **`setObjectParameter(name, 1, v)`** sets it to `((int)v == 1)` | script sets it on level-goal objects (pigs) **[V]** |
| +0x54 | int | material: 0 none, 1 "wood", 2 "rock", 3 "light" (`setMaterial`; not render) | |

`setObjectParameter(name, 2, v)` → `b2Body::SetType(v == 0 ? static : dynamic)`; this changes which pass draws the
object (mass becomes 0 for static). Lookups of unknown names fall back to a shared dummy value at `GameLua+0xd4`
(writes go nowhere visible).

"static" below means: `*(float*)(body + 0x7c) == 0.0f` — the body's mass (the same field is exported to Lua as
`mass`), 0 for static bodies. **[C]**

### 3.2 Iteration order = z-order inside each pass

There is **no z field and no sorting**. Each pass walks the hashtable: bucket 0 … capacity−1; inside a bucket the
inline head entry first, then the overflow chain in link order; unused entries are skipped. The order therefore
depends on the object **names** and on the table history:

* hash: `h = s[0]; for i >= 1: h = h*31 + (signed char)s[i]` (32-bit wrap), `""` → 0 (`lang::String::hashCode`
  @0004bc74); bucket = `(h & 0x7fffffff) % capacity`.
* capacity sequence 37, 67, 131, 257, 521, 1031, 2053, 4099, … (`Hashtable_getLargerInt` @00002cd4); load factor
  0.75; before every insert: `if (threshold <= count + 1) grow()`, `threshold = (int)(capacity * 0.75)`; starts at
  capacity 0 (so the first insert grows to 37, threshold 27; the 27th insert grows to 67; the 50th to 131; …).
* insert into a bucket (`getPair` @000192d8): reuse the key's entry if present; else reuse the **last unused**
  entry of the chain (normally only the inline head can be unused); else allocate a node and link it **directly
  after the head** → chain order is `first-inserted, newest, …, second-inserted`.
* `grow()` (@0001b538) re-inserts old entries in iteration order into the new table.
* `removeObject` → `remove()` (@0001b120): an overflow node is unlinked and freed; an inline head just becomes
  unused.
* `loadLevel` → `clear()` (@00019ea0): marks everything unused but **keeps the capacity and the overflow nodes**
  (so the order in a level can depend on how big earlier levels of the same session were — faithful, if obscure).
* A lookup of a non-existent name (e.g. `setSprite` on a removed object) still claims/allocates an unused chain
  slot without marking it used; this can shift where the next insert into that bucket lands.

`analysis/render_hashorder.py` is an executable model of all of the above (insert / lookup / remove / clear / grow /
iterate). Object creation order comes from the Lua script (`pairs()` over the level table), which is reproduced
automatically when the original bytecode runs on a stock Lua 5.1.

### 3.3 Algorithm

```
drawGame():
    st = ctx.getRenderState2D()
    st.tx = -topLeftX;  st.ty = -topLeftY
    st.sx = st.sy = worldScale
    // rotation and pivot are NOT reset here; every object sets them. clip & co. untouched.

    // ---- pass 1: static objects --------------------------------------------------------------
    for e in objects (hash order):
        if e.sprite != null and e.body.mass == 0.0:
            setObjectXform(st, e)
            if e.texture == null:
                e.sprite.draw(ctx, e.x * 20.0f, e.y * 20.0f, Anchor{VPIVOT, HPIVOT})
            else:
                maskedImage.add(e.x * 20.0f, e.y * 20.0f, e.texture, e.sprite)        // deferred, §3.4
    maskedImage.render()        // all textured static objects, after all untextured static sprites

    // ---- pass 2: dynamic, not "front" ----------------------------------------------------------
    for e in objects (hash order):
        if e.sprite != null and !e.controllable and e.body.mass != 0.0 and !e.front:
            setObjectXform(st, e)
            e.sprite.draw(ctx, e.x * 20.0f, e.y * 20.0f, Anchor{VPIVOT, HPIVOT})

    // ---- trajectories ---------------------------------------------------------------------------
    st.m00 = 1; st.m01 = 0; st.m10 = -0.0f; st.m11 = 1          // identity; pivot left as is (irrelevant)
    for t in 0..5:                                               // all six arrays, in index order
        for i in 0 .. trajectories[t].size-1:
            name = ["SMOKE_TRAIL_1", "SMOKE_TRAIL_2", "SMOKE_TRAIL_3"][i % 3]
            res.drawSprite("INGAME", name, trajectories[t][i].x, trajectories[t][i].y, Anchor{VPIVOT, HPIVOT})

    // ---- pass 3: "front" objects (birds, pigs) -------------------------------------------------
    for e in objects (hash order):
        if e.sprite != null and (e.controllable or e.front):          // NOTE: no mass test
            setObjectXform(st, e)
            e.sprite.draw(ctx, e.x * 20.0f, e.y * 20.0f, Anchor{VPIVOT, HPIVOT})

    // ---- native particles ------------------------------------------------------------------------
    particles.draw(ctx)                                           // §8.5

setObjectXform(st, e):
    c = cosf(e.angle); s = sinf(e.angle)
    st.m00 = c; st.m11 = c; st.m01 = s; st.m10 = -s
    st.px = (float)e.sprite.pivotX;  st.py = (float)e.sprite.pivotY       // int16 → float
```

Resulting placement of an object: the sprite's pivot is at screen
`((x*20 − topLeftX) * worldScale, (y*20 − topLeftY) * worldScale)`, the sprite is scaled by `worldScale` and rotated by
`angle` (clockwise on screen) about its pivot. Positions are **not rounded**.

Details that matter for pixel-exactness:

* A static object that is `controllable` or `front` is drawn **twice** (pass 1 and pass 3) at the same place —
  e.g. the birds waiting on the ground are static (`setObjectParameter(name, 2, 0)`) and controllable **[V]**. With
  premultiplied "over" blending this slightly hardens their anti-aliased edge.
* A dynamic controllable object (the flying bird) is only in pass 3, i.e. above the trajectory dots.
* There is no explicit culling; off-screen quads are dropped by the NDC bbox test of §1.3.
* Trajectory dots: sprite pivots are the sprite centres (9×9, 12×11, 14×13 px **[V]**), so dot `i` is centred on its
  world point and scaled by `worldScale`.
* State left behind: `tx,ty = -topLeft`, `sx,sy = worldScale`; rotation/pivot = those of the last pass-3 object
  (or identity + older pivot if there was none); if any particle was drawn, everything is as left by the last
  particle (§8.5). The script always calls `setRenderState` (≥ 5 args) before drawing anything else.

### 3.4 `MaskedImage` (textured static blocks: tiling ground texture clipped by the block sprite's alpha)

Object at `GameLua+0x1cc`; layout: `+0x08 ctx`, `+0x0c shader` (clone of `"sprite-straightalpha"`), `+0x10 Image* tex`,
`+0x14 Image* mask`, `+0x18 Array<float4> pos`, `+0x24 Array<float4> uv0`, `+0x30 Array<float4> uv1`.

Used for blocks whose definition has `texture = "INGAME_6" | "INGAME_7" | "INGAME_8"` (256×256 opaque PVRTC ground
textures with mipmaps) and a `GROUND_BLOCK_THEMExx_yy` sprite from `INGAME_1` as the mask **[V]**; the script calls
`setTexture(name, obj.texture)` after creating the object.

`MaskedImage::add(float x, float y, Image* tex, Sprite* sprite)` @00130268 **[D]** (called with the render state
already set up by `setObjectXform`):

```
this.tex  = tex                                  // last call wins: one texture pair per render()
this.mask = sprite.sheet.image
qx = x - sprite.pivotX;  qy = y - sprite.pivotY          // anchor {VPIVOT,HPIVOT}
corners / NDC positions exactly as §1.3 for the quad (qx, qy, sprite.w, sprite.h), stored as float4(ndc.x, ndc.y, 0.001, 1)
cull with the §1.3 bbox test (nothing is appended if culled)

// texcoord set 0 — the tiling texture, anchored to the WORLD (independent of camera and zoom):
ku = 1 / ( tex.width()  * (tex.width()  / tex.getTexture().width())  )         // = 1/256 for the ground textures
kv = 1 / ( tex.height() * (tex.height() / tex.getTexture().height()) )
for each corner with rotated offset (ox, oy) = R(θ) * (corner − pivot):         // the same ox, oy as in §1.3
    uv0 = ( ku * (x + ox),  kv * (y + oy) )          // x, y = object position in world pixels

// texcoord set 1 — the mask sprite inside its sheet:
uv1: TL = (sx0/mw, sy0/mh), TR = (sx1/mw, sy0/mh), BL = (sx0/mw, sy1/mh), BR = (sx1/mw, sy1/mh)
     with sx0 = sprite.sheetX, sx1 = sheetX + w, sy0 = sheetY, sy1 = sheetY + h, mw/mh = POT texture size of the sheet

append 6 vertices (TL, TR, BL, BL, TR, BR) to pos / uv0 / uv1
```

`MaskedImage::render()` @00130a48 **[C]**: nothing if no vertices. Otherwise one `GL_TRIANGLES` draw with two
texture units: unit 0 = `BASEMAP` = `tex` (uv0), unit 1 = `BASEMAP1` = `mask` (uv1). The shader `begin()` flushes the
sprite batcher first, then sets on unit 1 `GL_TEXTURE_ENV_MODE = GL_COMBINE`, `GL_COMBINE_RGB = GL_REPLACE`,
`GL_SRC0_RGB = GL_PREVIOUS`, `GL_OPERAND0_RGB = GL_SRC_COLOR`; the alpha combiner stays at GL defaults
(`GL_MODULATE` of `GL_TEXTURE` × `GL_PREVIOUS`). Result per fragment:

```
rgb   = tex.rgb                       // ground texture, GL_REPEAT, LINEAR_MIPMAP_NEAREST / LINEAR
alpha = tex.a * mask.a                // tex.a == 1 for the opaque PVRTC ground textures
blend : glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)          // straight alpha
```

Afterwards the three arrays are emptied and `tex = mask = null`. `render()` does not touch the scissor (the one
set by the last batcher flush stays active — always the default "everything" rect in practice).

---------------------------------------------------------------------------------------------------------------------

## 4. Background, foreground, theme

### 4.1 `GameLua::setTheme(lang::String name)` @000092b8 **[C]**

Reads `blockTable.themes[name]` (tables resolved on the GameLua script object):

| Lua field | stored |
|---|---|
| `.color.r/.g/.b` | `+0x74/+0x78/+0x7c` (float, 0..255) — sky colour |
| `.groundColor.r/.g/.b` | `+0x80/+0x84/+0x88` (float, 0..255) |
| `.image` (string) | `+0x8c` |
| `.index` (number → int) | `+0xa4` |

then derives

```
sheetA (+0x90) = sprintf("INGAME_%d", index + 2)
sheetB (+0x94) = "INGAME_2"
names  (+0x98) = [ image.."_LAYER_1", image.."_LAYER_2", image.."_LAYER_3", image.."_LAYER_4", image.."_LAYER_GRASS" ]
```

No other theme field is read natively; there are **no per-theme parallax factors** — they are constants in the code.

Shipped themes (`blocks.lua`) and the sprites actually used **[V]** (`name: sheet, w×h, pivot`):

| | theme1 | theme2 | theme3 |
|---|---|---|---|
| index / image | 1 / `BACKGROUND_1` | 2 / `BACKGROUND_2` | 3 / `BACKGROUND_3` |
| color (sky) | 148, 206, 222 | 193, 231, 239 | 239, 214, 115 |
| groundColor | 10, 19, 57 | 10, 19, 15 | 47, 23, 14 |
| `_LAYER_4` (sheet B `INGAME_2`, opaque PVRTC) | 478×318, pivot (239, 317) | 478×318, (239, 317) | 478×318, (239, 317) |
| `_LAYER_2` (sheet A) | `INGAME_3`: 478×204, (239, 204) | `INGAME_4`: 478×152, (239, 152) | `INGAME_5`: 478×131, (239, 131) |
| `_LAYER_1` (sheet B `INGAME_2`, opaque) | 332×247, (166, −5) | 346×197, (173, −4) | 322×298, (161, −2) |
| `_LAYER_GRASS` (sheet A) | 332×33, (166, 28) | 346×36, (173, 32) | 322×32, (161, 30) |
| `_LAYER_3` | does not exist in the data and is never drawn | | |

### 4.2 `GameLua::drawBackground()` @0000a5d8 (Lua `drawBackgroundNative`) **[D]**

```
drawBackground():
    ws = worldScale;  W = (float)ctx.width();  H = ctx.height()
    h1  = res.getSpriteHeight(sheetB, names[0])          // LAYER_1 (ints)
    py1 = res.getSpritePivotY(sheetB, names[0])
    st = ctx.getRenderState2D()
    st.tx = -topLeftX; st.ty = -topLeftY; st.sx = st.sy = ws; rotation = identity        // overwritten just below

    // (a) refresh the clear colour from the theme sky colour (same effect as setBGColor(color.r, color.g, color.b))
    bgColor = clampByte(skyR) << 16 | clampByte(skyG) << 8 | clampByte(skyB)
              // clampByte(v): v < 0 → 0;  v > 255 → 255;  else (int)v (truncation)

    // (b) ground fill: everything below the bottom edge of the foreground ground strip
    yTop = -( (topLeftY - (float)(h1 - py1) * 1.5f) * ws )            // = (1.5*(h1-py1) - topLeftY) * ws, screen px
    this.drawRect(groundR/255, groundG/255, groundB/255, 1.0,   0.0, yTop, W, H + 1.0,   false)
        // 'false' ⇒ the whole RenderState2D is reset to defaults first (§6.2): the rect is in plain screen pixels,
        // and afterwards tx = ty = 0, pivot = 0, scale = 1, rotation = identity.
        // → fillRect2D(0xFF000000 | groundRGB, 0, (int)yTop, (int)W, (int)(H + 1 - yTop))

    // (c) two parallax layers, each drawn at 2x sprite scale
    st.sx = st.sy = ws + ws
    tyAll = topLeftY * -0.5f
    halfW = ((double)W * 0.5) / (double)ws                    // screen width in layer-local units (double)
    for (layer, f, sheet) in [ (names[3] /*LAYER_4*/, 0.125f, sheetB),
                               (names[1] /*LAYER_2*/, 0.5f,   sheetA) ]:
        w  = res.getSpriteWidth(sheet, layer)                 // int
        px = res.getSpritePivotX(sheet, layer)                // int
        limit  = (float)((double)px + halfW)
        wf     = (float)w
        period = (double)(wf + wf)
        m  = fmod((double)(f * topLeftX), period)             // C fmod, sign follows the dividend
        v  = (float)((period + m) * -0.5)
        i  = (int)v                                           // truncation toward zero
        x  = (float)i
        st.ty = tyAll
        st.tx = v - x                                         // fractional part
        while limit > x:
            res.drawSprite(sheet, layer, x, 0.0f, Anchor{VPIVOT, HPIVOT})
            x += wf
```

(The literal code is a countdown `layer = 4 → 3`, then `3 → 2 → 1` which **skips index 2** (`LAYER_3`); factor is
0.125 for the first iteration and `1.0f / 2` for the second; sheet is A when the countdown value is 2 or 3, else B.)

Geometry that results (screen pixels, `tl = topLeft`):

```
layer sprite drawn with top-left at local (x - pivotX, -pivotY), size (w, h), scale 2*ws:
    screenX = ws * ( 2*(x - pivotX) + 2*frac )        →  scroll = f * tl.x  (LAYER_4: 1/8, LAYER_2: 1/2), tile period 2*w world px
    screenY = ws * ( 2*(yLocal) - tl.y )              →  NO vertical parallax; the sprite's pivot row sits on world y = 0
```

e.g. theme 1: `LAYER_4` covers world y ∈ [−634, +2], `LAYER_2` world y ∈ [−408, 0]; both are 956 world px wide per tile.
The sky above them is simply the frame clear colour (`bgColor`, §4.5) — **there is no gradient**.

State left behind: identity rotation, pivot (0,0), `sx = sy = 2*ws`, `ty = -0.5*topLeftY`, `tx` = last layer's fraction,
everything else default.

### 4.3 `GameLua::drawForegroundNative()` @00006ba0 **[D]**

```
drawForegroundNative():
    st = ctx.getRenderState2D()
    s  = worldScale * 1.5f
    st.sx = st.sy = s
    rotation = identity (m00 = 1, m01 = 0, m10 = -0.0f, m11 = 1)        // pivot NOT touched (irrelevant)
    w  = res.getSpriteWidth (sheetB, names[0])          // LAYER_1, int
    px = res.getSpritePivotX(sheetB, names[0])
    W  = ctx.width()
    limit  = (float)W / s + (float)px
    period = (double)((float)w * 1.5f)
    m  = fmod((double)topLeftX, period)
    st.ty = -topLeftY / 1.5f
    v  = (float)( -(period + m) / 1.5 )
    i  = (int)v                                         // truncation
    st.tx = v - (float)i
    if limit > (float)i:
        for (x = i; limit > (float)x; x += w):          // integer stepping
            res.drawSprite(sheetB, names[0] /*LAYER_1*/, (float)x, 0.0f, Anchor{VPIVOT, HPIVOT})
        for (x = i; limit > (float)x; x += w):
            res.drawSprite(sheetA, names[4] /*LAYER_GRASS*/, (float)x, 0.0f, Anchor{VPIVOT, HPIVOT})
```

Both layers use the width / pivotX of `LAYER_1` for stepping (the two sprites have the same width and pivotX in all
three themes). Result: `screenX = ws * (1.5*(x − px + frac))` → the foreground scrolls **1:1 with the world**
(period 1.5·w world px), `screenY = ws * (1.5*yLocal − topLeftY)`: sprites are drawn at **1.5× scale** with their pivot
row on world y = 0. Theme 1: the ground strip (`LAYER_1`, pivotY −5) covers world y ∈ [7.5, 378], the grass
(`LAYER_GRASS`, pivotY 28) world y ∈ [−42, 7.5]; the ground-colour rect of §4.2 starts exactly at 378.

State left behind: identity rotation, `sx = sy = 1.5*ws`, `ty = -topLeftY/1.5`, `tx` = fraction, pivot unchanged.

### 4.4 Frame order as driven by the script (Lua `drawGame`, gamelogic.lua:14487) **[V]**

```
setBGColor(theme.color.r, .g, .b)
if showBG: drawBackgroundNative()
setRenderState(-screen.left, -screen.top, worldScale, worldScale, 0)
res.drawSprite("INGAME", "SLING_SHOT_01_BACK", …);  drawLine2D(… back rubber band …);  [SLING_HOLDER]
drawGameNative()                       // objects, trajectories, native particles
if showBG: drawForegroundNative()      // ground strip + grass are drawn OVER the objects
setRenderState(…, 0, 0, 0);  drawLine2D(… front rubber band …);  SLING_HOLDER / SLING_SHOT_01_FRONT, HUD, menus …
```

The frame itself (`GameApp` render, decomp-annot/GameApp.c:325): `ctx->clear(gameLua->getBGColor(), true, true, …)`
and only then `GameLua::update(dt)` (which runs the script's update + draw). So the clear uses the `bgColor` set
during the **previous** frame.

### 4.5 Small setters **[C]**

* `setBGColor(r, g, b)` @00005f50: `bgColor = clampByte(r) << 16 | clampByte(g) << 8 | clampByte(b)` (floats 0..255,
  truncated, clamped). `getBGColor()` @00005f48 returns it; alpha byte is 0.
* `setTopLeft(x, y)` @00006000: `topLeftX = x; topLeftY = y` (world px of the screen's top-left corner).
* `setWorldScale(s)` @0000600c: `worldScale = s`; also `app[0x27c] = app[0x280] = s` (the App's current/target
  pinch-zoom values).
* `setLevelLimits(a, b, c, d)` @00006028: `limitLeft(+0x1d0) = (int)a; limitRight(+0x1d4) = (int)c`; `b`, `d` are
  ignored. Only `GameLua::update` reads them (objects left of / right of the limits get `frozen = true` in their
  Lua table); **no effect on rendering or culling**. Script call: `setLevelLimits(levelLeftEdge − 0.75·screenWidth·physicsScale, −10000, levelRightEdge + 0.75·screenWidth·physicsScale, 20)`.

---------------------------------------------------------------------------------------------------------------------

## 5. Trajectories

Data: `lang::Array<float2> trajectories[6]` at `GameLua+0x180` and `int base` at `+0x1c8` (init 0). The six arrays
form two groups of three: group A = arrays 0..2, group B = arrays 3..5. A group is one "shot": sub-trail 1 for the
launched bird, sub-trails 2 and 3 for the birds it splits into.

### 5.1 `startNewTrajectory()` @0000aa64 **[C]**

```
base += 3
if base > 3: base = 0
trajectories[base].resize(0); trajectories[base+1].resize(0); trajectories[base+2].resize(0)
```

`base` alternates 0 → 3 → 0 → … (the first call after start-up selects group B). The *other* group is left alone, so
exactly **two shots are kept: the current one and the previous one**.

### 5.2 `addToTrajectory(float which, float x, float y)` @00008ff8 **[D]**

```
idx = (base - 1) + (int)which          // which = 1, 2, 3 from the script → arrays base+0 … base+2
trajectories[idx].add(float2(x, y))    // world pixels; unbounded growth (capacity doubles, min 4)
```

No range check on `which`, no maximum point count, no spacing logic — the script decides when to add a point: it adds
one whenever the bird has moved more than 20 world px from the last recorded point (gamelogic.lua:13300-13390), and
calls `addToTrajectory(1, x, y)` for the flying bird and `addToTrajectory(1 + k, x, y)` for the k-th "other bird".

### 5.3 Drawing (inside `drawGame`, §3.3)

All six arrays are drawn every frame, arrays 0…5 in order, each point as one sprite from sheet `"INGAME"`:
point `i` of an array uses `SMOKE_TRAIL_1` when `i % 3 == 0`, `SMOKE_TRAIL_2` when `i % 3 == 1`, `SMOKE_TRAIL_3` when
`i % 3 == 2` (9×9, 12×11, 14×13 px, centre pivots) — a repeating small/medium/large pattern per array. Identity
rotation, scale `worldScale`. **The previous trajectory is not faded or altered in any way**; it just disappears when
its group is recycled two shots later.

`loadLevel` empties all six arrays (it does not reset `base`).

---------------------------------------------------------------------------------------------------------------------

## 6. Immediate-mode primitives

### 6.1 `drawLine2D(x1, y1, x2, y2, width, r, g, b, a)` @00006044 **[D]**

Lua argument order as listed (all numbers → float). Colour components are **0..255** (not 0..1).

```
argb = (int)a << 24 | (int)r << 16 | (int)g << 8 | (int)b            // each float truncated, no clamping
ctx.drawLine2D(argb, (int)x1, (int)y1, (int)x2, (int)y2, (int)width)   // §1.7; truncation toward zero
```

The current RenderState2D applies (endpoints are in the state's coordinate space; GL line width =
`(int)((int)width * scale)`). Script usage: the slingshot rubber bands,
`drawLine2D(x1, y1, x2, y2, clamp(50 / rubberBandLength, 10, 25), 48, 23, 8, 255)` under
`setRenderState(-screen.left, -screen.top, worldScale, worldScale, 0)`.

### 6.2 `drawRect(r, g, b, a, x1, y1, x2, y2, keepState)` @00007898 **[C]/[D]**

Lua argument order as listed: colour **first**, components **0..1**; then the two corners; then a boolean.

```
if !keepState:
    *ctx.getRenderState2D() = default RenderState2D        // clip (-32000,-32000,32000,32000), identity, tx=ty=0,
                                                           // sx=sy=1, pivot 0, +0x38..+0x50 defaults — and it STAYS reset
argb = (int)(a*255) << 24 | (int)(r*255) << 16 | (int)(g*255) << 8 | (int)(b*255)       // truncation, no clamping
ctx.fillRect2D(argb, (int)x1, (int)y1, (int)(x2 - x1), (int)(y2 - y1))                  // §1.8
```

* `keepState == false` → rectangle in raw screen pixels, and the call has the side effect of resetting the render
  state (script: full-screen dim `drawRect(0,0,0,shade, 0,0,screenWidth,screenHeight, false)`).
* `keepState == true` → the rectangle goes through the current transform (script: editor selection rectangle in world
  space, `drawRect(1,0,0,0.5, x1,y1,x2,y2, true)`).
* Always filled (there is no outline mode); straight-alpha blend. Width/height are computed in float and then
  truncated (`(int)(x2 − x1)`, not `(int)x2 − (int)x1`).

### 6.3 `drawTexturedRect(name, x1, y1, x2, y2, flag)` @000066e8 **[D]** — not called by the 1.0 scripts

```
*ctx.getRenderState2D() = default RenderState2D                 // unconditional reset
sheet = res.findSheet(name); sprite = sheet.getSprite(name); image = sheet.getImage()
if sprite == null: return
p = float4( (float)texW, (float)texH,  topLeftX - sprite.sheetX,  topLeftY - sprite.sheetY )     // texW/H = POT texture size
ctx.texturizeRect2D(image, p, (int)x1, (int)y1, (int)(x2 - x1), (int)(y2 - y1))                  // §1.9
```

→ a screen-space rect whose texels are `u = (screenX + topLeftX − sheetX) / image.width()`,
`v = (screenY + topLeftY − sheetY) / image.height()` (wrapping with `GL_REPEAT` over the *whole sheet image*). `flag` is
ignored. Being unused (the shipped game does ground tiling through `MaskedImage`, §3.4), it can be stubbed.

---------------------------------------------------------------------------------------------------------------------

## 7. `clipText(group, id, maxWidth)` @00006da4 **[C], decision points [D]**

`text = res.getString(group, id)` (e.g. `clipText("TEXTS_BASIC", "TEXT_CREDITS", 300)`); widths are measured with
`res.getStringWidth(text, start, count)` in the **currently selected font** (`setFont` must have been called).
Result goes to the script table `clippedText`: `clippedText.lines = { [1] = "…", [2] = "…", … }` (a fresh table each
call) and `clippedText.widestLine = <number>`.

Work is done on the UTF-16 form of the text (`n` code units in `buf`); all indices are UTF-16 indices. Break characters:
`'\n'` (hard break), `' '` (soft break, swallowed), `'-'` (soft break **after** the hyphen, hyphen kept).

```
clipText(group, id, maxWidth):
    text = res.getString(group, id)
    buf, n = UTF16(text)
    lines = {}                        // new Lua table
    widest = 0                        // int
    lineStart = 0
    pos = 0
    while n > 0:                      // do { … } while (pos < n)
        lineEnd   = n
        wordCount = 0
        // pos == lineStart here
        loop:                                                   // scan one more word starting at pos
            i = pos
            while i < n and buf[i] not in {'\n', ' ', '-'}:  i++
            if i < n and buf[i] == '-':  i++                    // a hyphen belongs to the word before it
            wordEnd = i
            count   = wordEnd - lineStart
            if not ( width(text, lineStart, count) < maxWidth ):        // does NOT fit (>= maxWidth)
                if wordCount == 0:
                    // a single word wider than the box: break it by characters
                    k = lineStart
                    repeat:  k++  until not ( maxWidth > width(text, lineStart, k - lineStart) )
                    k = k - 1                                    // longest prefix that is still < maxWidth
                    count = k - lineStart;  lineEnd = k;  pos = k
                else:
                    count = lineEnd - lineStart                  // up to the end of the previous word
                    // pos stays at the start of the word that did not fit
                break loop
            // it fits
            if i >= n:                                           // end of text
                pos = wordEnd;  lineEnd = wordEnd;  break loop
            if buf[i] == '\n':
                pos = wordEnd;  lineEnd = wordEnd;  break loop   // (the '\n' itself is skipped below)
            pos = i
            while pos < n and buf[pos] == ' ':  pos++            // swallow spaces ('-' and '\n' stop the skip)
            if pos >= n:
                lineEnd = wordEnd;  break loop                    // count = wordEnd - lineStart
            lineEnd = wordEnd;  wordCount++
            // continue scanning from pos; note count/width are always measured from lineStart to the new wordEnd,
            // so inter-word spaces are included in the measurement
        // emit
        widest = max(widest, width(text, lineStart, count))      // count == lineEnd - lineStart on every path
        lines[#lines + 1] = UTF16→String( buf[lineStart .. lineEnd) )
        if pos < n and buf[pos] == '\n':  pos++
        if pos >= n: break
        lineStart = pos
    clippedText.widestLine = (float)widest                       // 0 when the text is empty
    clippedText.lines = lines
```

Consequences / edge cases:

* A line fits only while its width is **strictly less** than `maxWidth`.
* Trailing spaces before a soft break are not part of the emitted line (`lineEnd` = end of the last word) and leading
  spaces of the next line are skipped; but when the word scan stops directly at a `'\n'` that was reached through
  the space-skip (e.g. `"abc \n"`), the measured/emitted line includes those trailing spaces if it still fits.
* Every `'\n'` ends a line; consecutive newlines produce empty-string lines (`"a\n\nb"` → `"a"`, `""`, `"b"`); a
  trailing `'\n'` at the very end of the text does not add an extra empty line.
* Over-long word in the middle of a line: the line is first ended before it (`wordCount > 0` path), then on the next
  line (`wordCount == 0`) it is split by characters; the remainder continues as a normal word.
* If even the first character of a word is `>= maxWidth` wide, `k` equals `lineStart`: an empty line is emitted and
  `pos` does not advance → **infinite loop** in the original (never happens with the shipped texts / box sizes; guard
  it in a port by forcing at least one character).
* Empty text: `lines = {}`, `widestLine = 0`.

---------------------------------------------------------------------------------------------------------------------

## 8. `class Particles` (native particle system)

### 8.1 Object layout **[C]**

`Particles : lua::LuaObject` — `+0x10 GameLua*`, `+0x14 game::Resources*`, `+0x18 lang::Array<ParticleData>`
(`ptr`, `+0x1c size`, `+0x20 cap`), `+0x24 lang::Hashtable<String, ParticleSystemData*>` (cache of parsed
definitions; `+0x2c` capacity, `+0x30` buckets, `+0x38` count, `+0x3c` threshold).

### 8.2 Constructor @00117ffc **[C]**

`Particles(LuaState*, GameLua*, Resources*)`: empty particle array, empty definition cache, registers **one** method on
itself, `addParticles`, and publishes itself as the **real global** `particles` (`lua.globals().setTable("particles", this)`).
It reads **nothing** from Lua at construction time; definitions are parsed lazily on first use (§8.3).

### 8.3 Definitions: `ParticleSystemData` (0x44 bytes), parsed on first `addParticles(type, …)` of each type

Source table: `particleTable.particles[type]` resolved on the GameLua script object (`particles.lua` is loaded into
the table `particleTable`). Parsed once and cached forever (never re-read).

| off | type | Lua field | notes |
|---|---|---|---|
| +0x00 | String | (the type name) | |
| +0x04 | Array<String> | `sprites[1..#sprites]` | array part, in order |
| +0x10 | String | `sheet` | sprite sheet name (always `"INGAME"` in 1.0) |
| +0x14 | bool | `animation` | true iff the field is a string equal to `"lifeTime"`; absent → false |
| +0x18 / +0x1c | float | `minVel` / `maxVel` | world px / s, used for **both** x and y velocity |
| +0x20 / +0x24 | float | `minAngleVel` / `maxAngleVel` | rad / s |
| +0x28 / +0x2c | float | `minScaleBegin` / `maxScaleBegin` | |
| +0x30 / +0x34 | float | `minScaleEnd` / `maxScaleEnd` | |
| +0x38 | float | `lifeTime` | seconds |
| +0x3c / +0x40 | float | `gravityX` / `gravityY` | world px / s² (y down) |

These are **all** fields that exist; there is no damping, no colour, no alpha. Shipped types: `smokeBuff`,
`explosionBuff` (both `animation = "lifeTime"`), `woodenBuff`, `rockBuff`, `lightBuff`, `blueBuff`, `redBuff`,
`greenBuff`, `blackBuff`, `yellowBuff`, `eggShells`, `explosion`, `bombBirdTrail`, `bomberBirdTrail`, `redBirdTrail`,
`blueBirdTrail`, `yellowBirdTrail` (values in `lua-src/particles.lua`).

### 8.4 `Particles::addParticles(String type, float amount, float x, float y, float w, float h, float angle)` @001185c0 **[D]**

Lua: `_G.particles.addParticles(type, amount, x, y, w, h, angle)`; x, y, w, h in world pixels. **`angle` is ignored.**

```
addParticles(type, amount, x, y, w, h, angle):
    n = particles.size
    total = (float)n + amount
    if total > 40.0f:   amount *= 0.5f;  total = (float)n + amount          // soft limit: halve the request
    if total > 125.0f:  amount = (float)(125 - n)                           // hard limit: 125 live particles
    if !(amount > 0): return
    def = cache[type]  or  parse particleTable.particles[type] (§8.3) and cache it
    i = 0
    do:
        p = new ParticleData
        p.type      = type
        p.x         = (float)( (double)x + ((double)rnd() - 0.5) * (double)w )      // uniform in the w × h box centred on (x, y)
        p.y         = (float)( (double)y + ((double)rnd() - 0.5) * (double)h )
        p.xVel      = def.minVel + (def.maxVel - def.minVel) * rnd()
        p.yVel      = def.minVel + (def.maxVel - def.minVel) * rnd()
        p.angle     = 2 * (rnd() * PI)                                                // lang::Math::PI as float
        p.angleVel  = def.minAngleVel   + (def.maxAngleVel   - def.minAngleVel)   * rnd()
        p.scaleBegin= def.minScaleBegin + (def.maxScaleBegin - def.minScaleBegin) * rnd()
        p.scaleEnd  = def.minScaleEnd   + (def.maxScaleEnd   - def.minScaleEnd)   * rnd()
        p.scale     = p.scaleBegin
        p.time      = 0
        p.lifeTime  = def.lifeTime
        p.frame     = 1
        p.lifeTimeAnimation = def.animation
        if def.animation:  p.spriteName = def.sprites[0]                              // first frame
        else:              p.spriteName = def.sprites[ (int)(rnd() * (float)#def.sprites) ]   // random, fixed for life
        p.sprite    = res.getSpriteSheet(def.sheet).getSprite(p.spriteName)
        p.def       = def
        particles.add(p)                                                              // appended at the end
        i++
    while amount > (float)i                                                           // ⇒ ceil(amount) particles
```

`rnd()` = `math::RandomUtil::random()` @0005cd9c = `(float)rand() * 2^-31` (libc `rand()`, range [0, 1]; the calls
happen in exactly the order written above: x, y, xVel, yVel, angle, angleVel, scaleBegin, scaleEnd, [sprite index]).

`ParticleData` (0x44 bytes): `+0x00 String type`, `+0x04 String spriteName`, `+0x08 x`, `+0x0c y`, `+0x10 xVel`,
`+0x14 yVel`, `+0x18 angle`, `+0x1c angleVel`, `+0x20 scaleBegin`, `+0x24 scaleEnd`, `+0x28 scale`, `+0x2c Sprite*`,
`+0x30 time`, `+0x34 lifeTime`, `+0x38 int frame`, `+0x3c ParticleSystemData* def`, `+0x40 bool lifeTimeAnimation`.

Capacity summary: at most 125 live particles; requests that would take the population above 40 are halved first
(so with ≥ 40 live particles every request is halved, with e.g. 35 live and a request of 10 → 5 are added).

### 8.5 `Particles::update(float dt)` @00118298 **[D]**

Called once per frame at the very end of `GameLua::update(dt)` (after the script's `update`), with the raw frame dt.

```
update(dt):
    i = 0
    while i < particles.size:
        p = particles[i]
        p.time += dt
        if p.time > p.lifeTime  or  p.y > 0.0f:        // expired, or below ground level (world y > 0; y tested BEFORE moving)
            particles.remove(i)                         // order-preserving erase; do not advance i
            continue
        p.yVel += dt * p.def.gravityY
        p.xVel += dt * p.def.gravityX
        p.x    += dt * p.xVel                           // semi-implicit Euler (new velocity)
        p.y    += dt * p.yVel
        t       = p.time / p.lifeTime
        p.angle += dt * p.angleVel
        p.scale = p.scaleBegin + (p.scaleEnd - p.scaleBegin) * t
        if p.lifeTimeAnimation:
            nF  = #p.def.sprites
            idx = (int)ceilf((float)nF * t);  idx = max(idx, 1);  idx = min(idx, nF)
            if idx != p.frame:
                p.spriteName = p.def.sprites[idx - 1]
                p.sprite     = res.getSpriteSheet(p.def.sheet).getSprite(p.spriteName)
                p.frame      = idx
        i++
```

No damping, no alpha fade, no collision. Only scale, angle and (for `"lifeTime"` types) the sprite frame change.

### 8.6 `Particles::draw(gr::Context*)` @00117e88 **[D]**

Called at the end of `drawGame` (and by the unused `drawParticlesNative`).

```
draw(ctx):
    ws = gameLua.worldScale
    st = ctx.getRenderState2D()
    for p in particles (array order = spawn order, oldest first):
        st.tx = -gameLua.topLeftX / p.scale
        st.ty = -gameLua.topLeftY / p.scale
        st.sx = st.sy = ws * p.scale
        c = cosf(p.angle); s = sinf(p.angle)
        st.m00 = c; st.m11 = c; st.m01 = s; st.m10 = -s
        st.px = (float)p.sprite.pivotX;  st.py = (float)p.sprite.pivotY
        p.sprite.draw(ctx, p.x / p.scale, p.y / p.scale, Anchor{VPIVOT, HPIVOT})
```

Net effect: the sprite's pivot is at screen `((p.x − topLeftX)·ws, (p.y − topLeftY)·ws)`, the sprite is scaled by
`ws · p.scale` and rotated by `p.angle` about its pivot. Normal sprite blending (premultiplied "over"); later
particles cover earlier ones; particles are above everything drawn by `drawGame` and below the foreground layer.
When `p.scale == 0` (e.g. `explosion` exactly at end of life) the divisions give inf/NaN and the quad is rejected
by the NaN-failing cull test — nothing is drawn.

### 8.7 `Particles::clearAll()` @00118474 **[C]**

`particles.resize(0)`. Called from `GameLua::loadLevel`. The definition cache is kept.

### 8.8 Native vs. Lua

| name | where | what |
|---|---|---|
| `_G.particles.addParticles(type, amount, x, y, w, h, angle)` | **native** (`Particles::addParticles`) | spawns |
| `newParticles(type, amount, x, y, w, h, angle)` | Lua (gamelogic.lua:18002) | `if particleTable.particles[type] == nil then return end; _G.particles.addParticles(…)` |
| `addParticles(objectName, type, amount)` | Lua (:16976) | box = the object's position ×20 and its (width, height) or (radius, radius) ×20, then `newParticles(type, amount, x, y, w, h, obj.angle)` |
| `addParticlesToPosition(x, y, type, amount)` | Lua (:17019) | physics → world, then `newParticles(type, amount, wx, wy, 1, 1, …)` |
| `updateParticles(dt)`, `drawParticles()` | Lua (:18025, :18146) | a complete older Lua-side particle implementation that iterates the **script** variable `particles` (a plain list, always empty in 1.0) — never called; dead code |

The script variable `particles = {}` (script environment) and the native object `_G.particles` are two different
things; the script only ever reaches the native one through `_G.particles`.

---------------------------------------------------------------------------------------------------------------------

## 9. Not determined / inferred

1. **[I] Names/meaning of RenderState2D +0x38…+0x50.** Established: they are batch keys only and `ALPHA_FACTOR` is
   ignored by the GLES 1.1 shader, so they cannot affect pixels in this build. The guess "blend src/dst enums" for
   +0x3c/+0x40 is unverified.
2. **[I] Rasterisation of wide lines** (`drawLine2D`, GL widths ≈ 10–25 px × worldScale for the rubber band). The
   binary just calls `glLineWidth(n)` and draws `GL_LINES` (no smoothing enabled anywhere). How the original GPU
   (PowerVR MBX / SGX) shapes a wide aliased line — the GL ES 1.1 spec says: offset copies along the minor axis, i.e.
   a parallelogram with axis-aligned ends, no caps; implementations may clamp the width to
   `GL_ALIASED_LINE_WIDTH_RANGE` — was not verifiable from the binary. If `(int)(width*scale)` is 0, `glLineWidth(0)`
   is a GL error and the previous width stays in effect.
3. **Script environment** (§0.3) is confirmed from `LuaObject::LuaObject` / `LuaState::compile` (setfenv), but I did
   not trace every loader call (which file goes into which table name); that belongs to the other agent's part.
4. **`getStringWidth(text, start, count)` index unit** is assumed to be UTF-16 code units (clipText computes its
   indices on the UTF-16 buffer and passes the original `lang::String`); see resources spec §drawString.
5. **Body "mass" offset.** `*(float*)(b2Body+0x7c)` is treated as the mass (it is what `createBoxLua` stores in the Lua
   field `mass`); whether it is `m_mass` or `m_invMass` in this Box2D build is irrelevant for rendering (both are 0
   exactly for static bodies) but is the physics agent's call.
6. **How `+0x44/+0x48/+0x4c` are produced** (fixed-step physics with interpolation between two state slots, angle
   unwrapping) is only summarised; the exact code is in `GameLua::update` @00007eb0 (physics agent).
7. **`rand()` sequence.** Particle randomness uses libc `rand()`; the original sequence (seed, interleaving with other
   `rand()` users such as Lua `math.random`) was not traced. Visual match can only be statistical.
8. **Texture min-filter / mipmap rule and wrap mode** are quoted from the texture-creation code as read for this
   spec (`EGL_Texture` ctor @000b62c0: MAG = `GL_LINEAR`, MIN = `GL_LINEAR_MIPMAP_NEAREST` or `GL_LINEAR`, wrap untouched);
   the PVR loading path is covered by the resources spec.
9. **`drawTexturedRect` and `drawParticlesNative`** are documented from code only; no script in 1.0 calls them, so
   their behaviour is untested by data.
10. **Session-dependent z-order** (§3.2: capacity and overflow nodes survive `loadLevel`; failed lookups can claim
    chain slots) is as read from the code; I did not find a case in the shipped levels where overlapping sprites
    make the difference visible, nor did I enumerate one.
11. Clear call: third/fourth arguments of `ctx->clear(bgColor, 1, 1, …)` (depth/stencil) were not pinned down; only
    the colour matters for 2D.

---------------------------------------------------------------------------------------------------------------------

## 10. Address / constant index and helper scripts

| item | address | constants |
|---|---|---|
| `GameLua::setBGColor` | 00005f50 | 255.0f |
| `GameLua::setTopLeft` / `setWorldScale` / `setLevelLimits` | 00006000 / 0000600c / 00006028 | |
| `GameLua::drawLine2D` | 00006044 | |
| `GameLua::drawParticles` | 000060dc | |
| `GameLua::drawTexturedRect` | 000066e8 | −32000 (0xffff8300), 32000 |
| `GameLua::drawForegroundNative` | 00006ba0 | 1.5f @00006d64, double 1.5 @00006d68 |
| `GameLua::clipText` | 00006da4 | break chars `'\n' ' ' '-'`; strings `"widestLine"`, `"lines"` |
| `GameLua::drawRect` | 00007898 | 255.0f @000079f0 |
| `GameLua::addToTrajectory` | 00008ff8 | |
| `GameLua::setTheme` | 000092b8 | `"INGAME_%d"`, `"INGAME_2"`, `"%s_LAYER_%d"`, `"%s_LAYER_GRASS"` |
| `GameLua::drawGame` | 00009ad4 | 20.0f @0000a0d0 / @0000a5d0; `"INGAME"`, `"SMOKE_TRAIL_1/2/3"`; 0x55555556 (i % 3) |
| `GameLua::drawBackground` | 0000a5d8 | 255.0f, 1.0f, 1.5f @0000a93c, int 4 @0000a940, −0.5f @0000a944, double 0.5 @0000a948, 0.125f @0000a950, double −0.5 @0000a954, int 2 @0000a95c |
| `GameLua::setRenderState` | 0000a960 | |
| `GameLua::startNewTrajectory` | 0000aa64 | |
| `GameLua::setTexture` / `setSprite` / `setObjectParameter` | 0000b76c / 0000b8cc / 0000b928 | |
| `Hashtable<String,RenderObjectData>` getPair / clear / remove / grow | 000192d8 / 00019ea0 / 0001b120 / 0001b538 | sizes 37, 67, 131, 257, 521, 1031, …; load factor 0.75 (0x3f400000) |
| `lang::String::hashCode` | 0004bc74 | ×31 |
| `math::RandomUtil::random` | 0005cd9c | 2^-31 (0x30000000) |
| `game::SpriteSheet::drawSprite(ctx, Sprite*, x, y, Anchor)` | 0006c294 | |
| `EGL_Context::texturizeRect2D` / `fillRect2D` / `drawLine2D` / `setClipRect` | 000a8fa8 / 000ab0e0 / 000ab444 / 000aa774 | 0.001f (z), −2.0f, 1.0f, 255.0f |
| `EGL_Image::draw` | 000b15c4 | |
| `EGL_Shader_Default::begin` / `gr::createShaders` | 000b550c / 000b6f00 | GL_ONE 1, GL_SRC_ALPHA 0x302, GL_ONE_MINUS_SRC_ALPHA 0x303 |
| `EGL_RenderBatcher::flush` / ctor / `render` | 00104230 / 0010492c / 001049f8 | |
| `Particles::draw` / ctor / `update` / `clearAll` / `addParticles` | 00117e88 / 00117ffc / 00118298 / 00118474 / 001185c0 | 40.0f, 125.0f, 0.5f, double 0.5, PI |
| `MaskedImage::add` / `render` / ctor | 00130268 / 00130a48 / 00131070 | 0.001f, −2.0f |
| vtables | `gr::EGL_Context` 001407d8, `gr::EGL_Image` 001408ec, `gr::EGL_Shader_Default` 00140a84 | |

Helper scripts written for this spec (all in `analysis/`):

| script | purpose |
|---|---|
| `dis.py <start> <end>` | ARM + VFP disassembly of an address range (via `arm-none-eabi-objdump` on the raw `__TEXT` bytes; `otool` cannot decode ARMv6 VFP), literal-pool loads annotated with float/double/string/symbol, `bl` targets demangled. Caches the full listing in `analysis/.dis2.txt`. |
| `fn.py <file.c> <header-substring>…` | print one decompiled function from `decomp-annot/*.c` |
| `vt.py <vtable-addr> [n]` | dump a vtable with slot offsets relative to the vptr |
| `render_hashorder.py name…` | executable model of the object hashtable (insert / lookup / remove / clear / grow / iteration = z-order) |
