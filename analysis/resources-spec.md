# Angry Birds 1.0 (iOS) — "resources" layer behavioural spec

Scope: `game::LuaResources` (Lua table `res`), `game::Resources`, `game::SpriteSheet`, `game::Sprite`,
`game::BitmapFont`, `game::TextGroupSet`, `game::TextGroup`, plus the parts of `gr::EGL_*`, `img::ImageReader`
and `audio::*` they call. Source: Ghidra output in `decomp-annot/` (ARMv6, addresses are VM addresses in the
1.0 binary), the shipped data files, and `lua-src/gamelogic.lua`.

Marking convention used throughout:

* **[R]** read directly from the decompilation / disassembly.
* **[V]** validated by parsing the real data files (scripts listed in section 9).
* **[I]** inferred (reasonable, but not read directly). Every inferred claim carries this tag.

All multi-byte integers in the `.dat` files are **big-endian** (`io::DataInputStream`, `readShort` @0004e6b0,
`readInt` @0004e704). `readShort` returns a **signed** 16-bit value. `readUTF` (@0004e934) is a signed-16
big-endian byte length followed by that many UTF-8 bytes (no terminator); a negative length throws.

Lua numbers in this build are 32-bit floats (bytecode header `1b 4c 75 61 51 00 01 04 04 04 04 00`:
`sizeof(lua_Number)=4`, non-integral) **[V]**. Everything the API returns as a number is an `int` converted to `float`.

---------------------------------------------------------------------------------------------------

## 1. Object model and state

`LuaResources` (ctor @000b9874) is a `lua::LuaObject` with an embedded `game::Resources` at `+0x10`. It registers
plain closures (no `self`; called as `res.fn(args)`) in a table stored as global `res`.

`game::Resources` (ctor @000bcbcc) state **[R]**:

| offset | content | initial |
|---|---|---|
| +0x08 | `gr::Context*` | from ctor |
| +0x1c | `Ptr<audio::AudioOutput>` | null |
| +0x20 | `Ptr<audio::AudioInput>` | null |
| +0x24 | current font (`IFont*`, raw pointer) | null |
| +0x28 | current font name (`String`) | "" |
| +0x2c | base path (`io::PathName`) | empty |
| +0x238 | current locale (`String`) | `"en_EN"` |
| +0x23c | `Hashtable<String, Ptr<TextGroupSet>>` | empty |
| +0x264 | `Hashtable<String, Ptr<SpriteSheet>>` | empty |
| +0x28c | `Hashtable<String, Ptr<IFont>>` | empty |
| +0x2b4 | `Hashtable<String, Ptr<AudioClip>>` | empty |

All name lookups are exact, case-sensitive string compares.

Base path: `GameApp::GameApp` (@00004f2c) calls `setDataPath("data")` then `res.setPath(app.path(""))`, i.e. the base
path is `<bundle>/data/` **[R]**. The scripts never call `res.setPath`.

Path join rule (`io::PathName(base, rel)`, `PathName::parse` @000504c0) **[R]**: if `base` is non-empty and `rel`
starts with `/` or `\`, drop that first character; append `/` to `base` if it does not already end in `/` or `\`;
concatenate; convert every `\` to `/`. Total length limit about 250 bytes (throws `IOException` beyond).

"Resource name" = `PathName(arg).basename`: the part after the last `/` and before the **last** `.`
(`"800X480/INGAME.dat"` -> `"INGAME"`, `"FONT_BASIC.dat"` -> `"FONT_BASIC"`). Sheets, fonts and text group sets are
registered under this name **[R]**.

### Argument conversion and errors

`lua::LuaState::toString/toNumber/toBoolean` (@00076564/@00076590/@000765b8) call `checkType` (@00075d60), which
requires `lua_type(L, idx)` to be **exactly** STRING / NUMBER / BOOLEAN. No Lua coercion: a number where a string is
expected (or `nil`, or a missing argument) throws `lua::LuaException`
(`"Excepted type {0} at stack position {1} but found {2}{3}"`) **[R]**. That is why the scripts write `"" .. score`.

All "throws" below are C++ exceptions (`lang::throwError<lang::Exception>` @000245b8 -> `__cxa_throw`). Where they
are caught was not traced; the binary contains `"Exception: %s"` and `"Unknown exception in mainloop!"`, so they
are presumably caught and logged by the main loop, aborting the rest of that frame's script **[I]**.

---------------------------------------------------------------------------------------------------

## 2. Lua API reference (`res.*`)

Legend: `->` return values pushed to Lua. "nothing" = zero return values.

### 2.1 Loading

**`res.createSpriteSheet(path : string)` -> nothing** — `LuaResources` @000b96b4, `Resources::createSpriteSheet` @000bcea0.
Loads `<base>/<path>` (section 4.1) and registers it under the resource name. Re-creating an existing name
replaces the old sheet. Lua passes `sourceFolder .. "INGAME.dat"` (`sourceFolder` is `""` on iPhone).
Sheets loaded by the 1.0 script: SPLASHES, INGAME, INGAME_1..8, MENU, MENU_1, MENU_2, TUTORIALS, TUTORIALS_1,
LEVELSELECTION, CUTSCENES, CUTSCENES_1.

**`res.releaseSpriteSheet(path : string)` -> nothing** (unused) — @000b95c8 / @000bd234. Removes the sheet registered
under the resource name of `path` (so `"INGAME"` and `"INGAME.dat"` are equivalent).

**`res.createBitmapFont(path : string)` -> nothing** — @000b96a0 / @000bd318. Loads `<base>/<path>` (section 4.2),
registers it in the font table under the resource name. Does **not** make it current. The font's glyph sheet is
private; it is not added to the sprite-sheet table.

**`res.createSystemFont(name : string)` -> nothing** (unused) — @000b968c / @000bb7e8. Stub: does nothing.

**`res.createTextGroupSet(path : string)` -> nothing** — @000b9678 / @000bc8c0. Creates a `TextGroupSet` for
`<base>/<path>`, reads only the locale-code list (`loadLocaleCodes` @0006f46c), registers it under the resource
name (`"TEXTS_BASIC"`). No strings are loaded yet.

**`res.loadLocale(setName : string, locale : string)` -> nothing** — @000b95b4 / @000bc52c. Looks up the text group
set `setName` (resource name, e.g. `"TEXTS_BASIC"`); if not registered: silently nothing. Otherwise
`releaseTextGroup("ALL")` (drops every loaded locale of that set, @00071d08) then `loadTextGroup(locale)` (@00070414):
`locale == "ALL"` loads every locale in the file; a locale not listed in the file throws
`Trying to load TextGroup for language not present in data file. Language: "{0}"`.

**`res.useLocale(locale : string)` -> nothing** — @000b95a0 / @000bb7f0. Just stores the string as the current
locale. No validation, nothing loaded.

**`res.setPath(path : string)` -> nothing** (unused by scripts) — @000b96c8 / @000bb804. Sets the base path.

### 2.2 Sprites

**`res.drawSprite(sheet : string, sprite : string, x : number, y : number [, anchorA : string [, anchorB : string]])` -> nothing**
— `LuaResources::drawSprite` @000b936c.

* Args 1-4 are mandatory and strictly typed. `sheet` is **not** optional, but may be `""`.
* Default anchor is `{vertical = VPIVOT, horizontal = HPIVOT}`: the sprite's pivot lands on (x, y).
* If `lua_gettop >= 5`, arg 5 is parsed with `Anchor::fromString` (@0006a02c); if `gettop >= 6`, arg 6 too.
  Each string sets whichever axis it names, so order does not matter (`"BOTTOM","HCENTER"` ==
  `"HCENTER","BOTTOM"`). Args beyond 6 are ignored. A `nil` in position 5/6 throws (type check).
* Anchor strings **[R]**: vertical `"TOP"`=0, `"VCENTER"`=1, `"BOTTOM"`=2, `"BASELINE"`=3, `"VPIVOT"`=4;
  horizontal `"LEFT"`=0, `"HCENTER"`=1, `"RIGHT"`=2, `"HPIVOT"`=3. Empty string: no change. Anything else throws
  `Invalid anchor: {0}`.
* Sheet lookup: `sheet == ""` -> `Resources::findSheet` (@000bbe88) iterates every registered sheet and uses the
  first one that contains `sprite` (hash-table order; no sprite name occurs in two sheets in the 1.0 data **[V]**,
  431 distinct names). `sheet != ""` -> exact lookup in the sheet table.
* Unknown sheet or unknown sprite: silently draws nothing (no error) **[R]**.
* There are no scale / rotation / flip / colour / alpha arguments. Those come only from the context's
  2-D render state (section 5).
* The engine does no rounding; x and y are used as floats. (The scripts call `math.floor` themselves in most places.)

**`res.getSpriteBounds(sheet : string, sprite : string)` -> `w : number, h : number`** — @000b8e1c
(`Resources::getSpriteWidth` @000bc35c, `getSpriteHeight` @000bc294). Sprite rect size in source pixels (the `w`,`h`
fields of the .dat record). Same sheet lookup as `drawSprite` (`""` = search all). Unknown sheet/sprite -> `0, 0`.
Always 2 return values.

**`res.getSpritePivot(sheet : string, sprite : string)` -> `pivotX : number, pivotY : number`** — @000b8c94
(`getSpritePivotX` @000bc1cc, `getSpritePivotY` @000bc104). The pivot fields of the .dat record, in pixels relative
to the sprite rect's top-left; may be negative or outside the rect **[V]** (e.g. `STORY_END_KING_EYE_NORMAL`
pivot (-807,-172), `MENU_SUNSET` 112x338 pivot (62,417)). Unknown -> `0, 0`.

### 2.3 Text

**`res.useFont(name : string)` -> nothing** — @000b958c / @000bbdd4. If a font is registered under `name`, it becomes
the current font; **if not, nothing happens and the previous font stays current** (no error). The script relies on
this: it calls `setFont("FONT_TITLE")`, which is never created, immediately followed (optionally) by a real font.

**`res.drawString(group : string, text : string, x : number, y : number [, anchorA : string [, anchorB : string]])` -> nothing**
— `LuaResources::drawString` @000b9198, `Resources::drawString` @000bf2e4.

* Default anchor `{vertical = TOP, horizontal = LEFT}`; optional anchors parsed exactly as in `drawSprite`
  (order-independent; the script uses both `("TOP","RIGHT")` and `("HCENTER","VCENTER")`).
* The string actually drawn is `Resources::getString(group, text)` (see `getString`): with `group == ""` or an
  unregistered group it is `text` itself; with a registered group, `text` is treated as a string id and replaced by
  the current locale's translation, or left as-is when it is not an id (this is how `"Score: "` is drawn with
  group `"TEXTS_BASIC"`).
* Uses the current font; throws `No font is set while trying to draw string` when none.
* Single line only. There is no line-break handling: a `\n` (or any character without a glyph) throws
  `Trying to draw character not present in font: {0}`. Multi-line text is split by `GameLua::clipText`
  (@00006da4, global `clipText(group, id, width)`, outside this layer) and drawn line by line.
* Layout: section 6.

**`res.getStringWidth(text : string)` -> `width : number`** — @000b8c6c -> `Resources::getStringWidth(text, 0, 0x7fffffff)`
@000c03b8 -> `BitmapFont::getStringWidth` @0006a634/@0006a4bc. Takes the literal text (no group/id translation;
the script composes `res.getStringWidth(res.getString(group, id))`). Pixels, integer-valued. Empty string -> 0.
Otherwise `sum(glyph.w for chars that have a glyph) + tracking * (nChars - 1)`; characters without a glyph add
no width but still count in `nChars`, and do **not** throw here. Throws `No font is set ...` when no font.

**`res.getFontHeight()` -> number** — @000b8bcc / @000c0720: `maxAscending + maxDescending`.
**`res.getFontMaxAscending()` -> number** — @000b8c4c / @000c0a90.
**`res.getFontMaxDescending()` -> number** — @000b8c2c / @000bfd18.
**`res.getFontLeading()` -> number** — @000b8c0c / @000c0de0: first header short of the font file.
**`res.getFontTracking()` -> number** (unused) — @000b8bec / @000c0068: second header short.
All use the current font and throw `No font is set while trying to get font ...` when none. Values: section 4.2.

**`res.getString(group : string, id : string)` -> `text : string`** — @000b9168 / @000bc6dc.

1. Look up text group set `group`. Not registered -> return `id` unchanged.
2. `TextGroupSet::getTextGroup(currentLocale)` (@00071480): if that locale is not loaded, throws
   `Trying to get TextGroup for language which is not loaded. Language: "{0}"` (locale exists in the file) or
   `... not present in data file ...` (it does not).
3. `TextGroup::get(id)` (@0006f0d0): the translation; if `id` is not a key, the entry `id -> id` is inserted and
   `id` is returned **[R]** (confirmed in the disassembly at 0x6f18c..0x6f1ac: `getPair(key)` then
   `String::operator=(pair.value, key)`).

### 2.4 Audio

**`res.createAudioOutput(channels : number, bitsPerSample : number, sampleRate : number)` -> nothing** — @000b9634 /
@000bb8b4. Each arg truncated to int into `audio::AudioConfiguration {int, int, int}`. Script call:
`(1, 16, 16000)`. `AudioOutputImpl` ctor (@000f72b0) accepts field0 in {1,2}, field1 in {8,16}, field2 in
{8000, 11025, 16000, 22050, 32000, 44100, 48000}, else throws; the field names are **[I]** from those checks and
from the data (every shipped sound is mono 16-bit 16 kHz **[V]**). Replaces any previous output. Creating the
output does not start it: `GameApp::activateAudio(true)` (@00004524) calls `Resources::startAudioOutput`.

**`res.createAudio(file : string, name : string)` -> nothing** — @000b95dc / @000be104. **File first, name second.**
Throws `Trying to create audio but no audio output has been created` if there is no output. Opens
`<base>/<file>` as an `audio::AudioClip` (@00102fc4; format by 3-letter extension: `wav` or `mp3`), stops any
playing instances of an existing clip called `name`, then registers the clip under `name` exactly as given
(no path processing). Script: 139 x `("SFX/<n>.wav", "<n>")` and 9 x `("Music/<n>.mp3", "<n>")`.

**`res.playAudio(name : string [, volume : number = 1.0 [, loop : boolean = false [, track : number = 0]]])` -> nothing**
— `LuaResources::playAudio` @000b8fe0, `Resources::playAudio` @000bef28, `AudioMixer::playClip` @000f12c8.
Defaults are chosen by `lua_gettop` (1 / 2 / 3 / >=4 args); `loop` must be a real boolean; `track` is truncated to
int. Throws `Trying to play audio clip but no audio output has been created` without an output. Unknown `name`:
silently nothing. Otherwise a new playing instance `{clip, volume, track, loop}` is appended **if the mixer has
been started**; every call adds another instance (the same clip can overlap itself). The C++ side returns an
instance id / -1, but **nothing is returned to Lua**. `volume` is not clamped. Script usage: effects on tracks
0-4, music on track 7 (`playAudio("title_theme", 0.8, true, 7)`).

**`res.stopAudio(name : string)` -> nothing** — @000b8fcc / @000beb90 / `AudioMixer::stopClip` @000ef09c. Removes every
playing instance of that clip. Unknown name: nothing. Throws without an output.

**`res.stopAllAudio()` -> nothing** — @000b8fb8 / @000be84c / `AudioMixer::stopClips` @000f0f58. Removes all instances.
Throws without an output.

**`res.isAudioPlaying(name : string)` -> boolean** — @000b8fa4 / @000bc84c / `AudioMixer::isClipPlaying` @000edf68.
`true` iff at least one instance of the clip is in the mixer's active list. No output or unknown name -> `false`.
It must be a real boolean: the script tests `res.isAudioPlaying("title_theme") == false`.

**`res.setTrackVolume(volume : number, track : number)` -> nothing** — @000b8b18 -> `AudioMixer::setTrackVolume`
@000eec98. **Volume first, track second.** `track` truncated to int; `track > 8` (unsigned compare) throws
`Track {0} out of bounds! Range [0-{1}]`. Volume is clamped to [0, 1] (below 0 or NaN -> 0, above 1 -> 1).
There are 8 tracks (0..7), each defaulting to 1.0; index 8 is accepted by the check but writes past the array
(original off-by-one; do not replicate). No null check on the output: calling it before `createAudioOutput`
dereferences null in the original. Script: `setEffectsVolume(v)` sets tracks 0..4, `setMusicVolume(v)` sets track 7.

Mixing (`AudioMixer::mixUnlimited16` @000ef63c) **[R]**: per instance `gain = (int)(instance.volume * trackVolume[track] * 4096)`;
if `gain > 0`, `acc[i] += (sample[i] * gain) >> 12` (signed 16-bit samples, 32-bit accumulator); the sum is saturated
to int16. No resampling or channel conversion (clips are assumed to be in the output format). At end of data a
looping instance rewinds and continues in the same buffer; a non-looping one is removed.

**Registered but unused by the scripts:**
`res.startAudioOutput()` (@000bf9d0; throws if no output; starts mixer and the OpenAL source),
`res.stopAudioOutput()` (@000bb838; no-op if no output),
`res.setMasterVolume(v)` (@000bb854; no-op if no output; stores `v` and applies it as OpenAL `AL_GAIN`),
`res.createAudioInput(a, b, c)` / `res.startAudioInput()` / `res.stopAudioInput()` (microphone; irrelevant).
`getMasterVolume` exists in C++ but is **not** registered in Lua.

### 2.5 Misc

**`res.openURL(url : string)` -> boolean** — @000b8a48 -> `pf::Launcher::openURL`. The script ignores the result.

---------------------------------------------------------------------------------------------------

## 3. Anchors (shared by sprites and text)

`game::Anchor` is `{int vertical; int horizontal;}` (vertical first) **[R]**.

Sprite placement, `SpriteSheet::drawSprite` @0006c3f4 (by name) / @0006c294 (by `Sprite*`) **[R]**. Given draw
position (x, y) and sprite record (w, h, pivotX, pivotY), the top-left corner of the quad is:

| vertical | top = | horizontal | left = |
|---|---|---|---|
| TOP (0), BASELINE (3) | `y` | LEFT (0) | `x` |
| VCENTER (1) | `y - (float)(h >> 1)` | HCENTER (1) | `x - (float)(w >> 1)` |
| BOTTOM (2) | `y - h` | RIGHT (2) | `x - w` |
| VPIVOT (4) | `y - pivotY` | HPIVOT (3) | `x - pivotX` |

`>>` is an arithmetic shift on the signed 16-bit value (so odd sizes round the half down). Then
`image->draw(ctx, left, top, srcX, srcY, w, h)`.

---------------------------------------------------------------------------------------------------

## 4. File formats (all validated against the shipped files, zero trailing bytes in every file)

### 4.1 Sprite sheet `.dat` — `SpriteSheet::SpriteSheet(Context*, String)` @0006ca88 **[R][V]**

```
u16be  nameLen
u8[]   imageFileName          (UTF-8, e.g. "INGAME.pvr", "MENU.png")
s16be  spriteCount
repeat spriteCount:
    u16be nameLen ; u8[] spriteName
    s16be x, y, w, h          (source rect in image pixels, top-left origin)
    s16be pivotX, pivotY      (relative to the rect's top-left; signed)
```

* Exactly **one** image per `.dat`, and no further fields. "Multi-image" atlases are simply separate files
  (`INGAME.dat`, `INGAME_1.dat`, ...), each registered as its own sheet (`"INGAME"`, `"INGAME_1"`, ...).
* The image is loaded from `parent_dir(datPath) + "/" + imageFileName` via `gr::Context::createImage(String)`.
* All 20 sheet files: no rect exceeds its image; 431 sprites; pivots are frequently outside the rect or negative.
* In-memory `game::Sprite` (0x18 bytes): `+0x08 sheet*, +0x0c x, +0x0e y, +0x10 w, +0x12 h, +0x14 pivotX, +0x16 pivotY`
  (all `short`).

| sheet | image | size | pixel format | sprites |
|---|---|---|---|---|
| INGAME | INGAME.pvr | 992x935 | RGBA4444 | 136 |
| INGAME_1 | INGAME_1.pvr | 866x1005 | RGBA4444 | 154 |
| INGAME_2 | INGAME_2.pvr | 1024x1024 | PVRTC 4bpp RGB, no mips | 12 |
| INGAME_3 / _4 / _5 | .pvr | 480x242 / 480x193 / 480x168 | RGBA4444 | 4 each |
| INGAME_6 / _7 / _8 | .pvr | 256x256 | PVRTC 4bpp RGB, 9 mip levels | 1 each (`THEME_GROUND_TEXTURE_n`) |
| MENU | MENU.png | 1011x511 | RGBA8 | 51 |
| MENU_1 | MENU_1.png | 1014x658 | RGBA8 | 8 |
| MENU_2 | MENU_2.png | 511x276 | RGBA8 | 13 |
| LEVELSELECTION | .png | 511x972 | RGBA8 | 25 |
| SPLASHES | .png | 481x831 | RGB8 (no alpha) | 3 |
| CUTSCENES / CUTSCENES_1 | .png | 996x441 / 996x960 | RGBA8 | 5 / 3 |
| TUTORIALS / TUTORIALS_1 | .png | 798x412 / 459x206 | RGBA8 | 4 / 2 |

### 4.2 Bitmap font `.dat` — `BitmapFont::BitmapFont` @0006a784 **[R][V]**

```
u16be  nameLen ; u8[] imageFileName      (e.g. "FONT_BASIC.png"; resolved like a sheet image)
s16be  leading
s16be  tracking
s16be  glyphCount
repeat glyphCount:
    u16be code        (UTF-16 code unit; stored as the hash key, a signed short)
    s16be x, y, w, h  (source rect in the font image)
    s16be base        (baseline offset: distance from the glyph rect's top edge down to the baseline)
```

Each glyph becomes a `Sprite(x, y, w, h, pivotX = 0, pivotY = base)` in a private sheet (named with `sprintf("%u", code)`).
There is no advance, bearing, or kerning data: advance = `w + tracking`.

Derived metrics (computed while loading, both start at 0):
`maxAscending = max(base)`, `maxDescending = max(h - base)`.

| getter | value |
|---|---|
| `getFontLeading` | `leading` header field |
| `getFontTracking` | `tracking` header field |
| `getFontMaxAscending` | `max(base)` |
| `getFontMaxDescending` | `max(h - base)` |
| `getFontHeight` | `maxAscending + maxDescending` |

| font | image | leading | tracking | glyphs | maxAsc | maxDesc | height |
|---|---|---|---|---|---|---|---|
| FONT_BASIC | FONT_BASIC.png 1024x42 | 20 | -3 | 161 | 18 | 10 | 28 |
| FONT_MENU | FONT_MENU.png 1024x112 | 60 | -3 | 161 | 32 | 13 | 45 |
| FONT_SCORE | FONT_SCORE.png 1008x61 | 104 | 1 | 39 | 52 | 11 | 63 |
| FONT_BIG_NUMBERS | FONT_BIG_NUMBERS.png 754x50 | 45 | -2 | 34 | 41 | 9 | 50 |

Character coverage **[V]** (codes are Unicode / Latin-1 values; records are in arbitrary order, no duplicates):

* FONT_BASIC and FONT_MENU (161 codes each): U+0010; all 95 printable ASCII codes U+0020..U+007E; U+00A1 ¡,
  U+00A8 ¨, U+00A9 ©, U+00AA ª, U+00AE ®, U+00AF ¯, U+00B0 °, U+00B4 ´, U+00BA º, U+00BF ¿; and U+00C0..U+00FF except
  U+00C6 Æ, U+00D7 ×, U+00D8 Ø, U+00E6 æ, U+00F0 ð, U+00F7 ÷, U+00F8 ø, U+00FE þ and one more that differs per font:
  FONT_BASIC lacks U+00DE Þ (has U+00D0 Ð), FONT_MENU lacks U+00D0 Ð (has U+00DE Þ). **No U+00A0** and no U+000A
  in either.
* FONT_SCORE (39): space, `! " # $ % ' ( ) * + , - . / 0-9 : ; < = > ? [ \ ] { | } ~` and U+00A0. No letters.
* FONT_BIG_NUMBERS (34): U+0010, space, `! " # $ % & ' ( ) * + , - . / 0-9 : ; < = > ?` and `x`.
* Glyph U+0010 exists in three fonts (tiny rect); its purpose was not determined.
* Space (U+0020) is a real glyph with its own width (BASIC 12, MENU 13, SCORE 20, BIG_NUMBERS 16).

### 4.3 Text group set `.dat` — `loadLocaleCodes` @0006f46c, `loadTextGroup` @00070414 **[R][V]**

```
u8     version?                 (0 in both files; read and ignored)
s32be  localeBlockSize          (bytes of the next block; loadTextGroup skips it)
--- locale block (localeBlockSize bytes) ---
u8     localeCount              (read as signed char)
repeat localeCount: u16be len ; u8[] localeCode        ("en_EN","fr_FR","it_IT","de_DE","es_ES")
--- id table ---
s16be  idCount
repeat idCount: u16be len ; u8[] id                    ("MI_PLAY", "TEXT_CREDITS", ...)
--- offset table ---
s32be  offset[localeCount]      offset[i] is relative to the byte immediately AFTER offset[i] itself
--- per-locale string blocks, in locale order ---
repeat idCount: u16be len ; u8[] utf8Text              (i-th string belongs to i-th id)
```

Reading locale `k` exactly as the engine does: after the id table, `skip(4*k)`, `off = readInt()`, `skip(off)`, then
read `idCount` strings. (So `offset[0] = 4*(localeCount-1)`: 16 in both files.)

* TEXTS_BASIC.dat: 5 locales, 73 ids, offsets [16, 1837, 2457, 3000, 3538], 5282 bytes.
* TEXTS_LANGUAGE_SELECTION.dat: 5 locales, 4 ids (`LANGUAGE_NAME`, `LANGUAGE_CODE`, `LANGUAGE_SELECT`,
  `LANGUAGE_CHOOSE`); never loaded by the 1.0 script.
* Encoding: UTF-8. No embedded NULs / modified-UTF-8 `C0 80`, no characters above U+00FF. Three en_EN strings
  (`TEXT_ABOUT`, `TEXT_ABOUT_CLICKGAMER`, `TEXT_CREDITS`) contain `\n`. Eleven fr_FR strings contain U+00A0, which is
  **not** in FONT_BASIC / FONT_MENU (see section 8).
* Runtime structures: `TextGroupSet` = locale-code array + `Hashtable<locale, Ptr<TextGroup>>` + file path;
  `TextGroup` = `Hashtable<id, text>`. The whole file is re-read on every `loadTextGroup`.

Script usage: `createTextGroupSet("TEXTS_BASIC.dat")`; later `loadLocale("TEXTS_BASIC", settings.currentLanguage)`
followed by `useLocale(settings.currentLanguage)` (default `"en_EN"`), again whenever the language setting changes.

### 4.4 Images — `EGL_Context::createImage(String)` @000a981c, `img::ImageReader`

File type is chosen **only** from the last three characters of the file name, lower-cased
(`io::guessFileFormat` @000f8354): `png` -> libpng 1.2.32 (`readHeader_png` @000e32b8), `pvr` -> `readHeader_pvr`
@000638cc. There is no `.pvr`/`.png` fallback; the name in the `.dat` is used verbatim.

**PVR** (legacy PVR v2, little-endian) **[R][V]**: the reader takes the first u32 as the header size (52) and
reads `headerSize - 4` more bytes:

```
+0  u32 headerSize (52)      +4  u32 height        +8  u32 width       +12 u32 mipMapCount (extra levels)
+16 u32 flags                +20 u32 dataSize      +24 u32 bitsPerPixel
+28 u32 rMask +32 gMask +36 bMask +40 aMask        +44 "PVR!"          +48 u32 numSurfaces
```

`mipLevels = mipMapCount + 1`. Pixel format = `flags & 0xff`:
0x10 -> RGBA4444, 0x11 -> RGBA5551, 0x12 -> RGBA8888, 0x13 -> RGB565, 0x15 -> RGB888, 0x16 -> L8, 0x17 -> A8L8,
0x18 -> PVRTC 2bpp, 0x19 -> PVRTC 4bpp (RGBA variant iff `flags & 0x8000`, else RGB), 0x1a -> BGRA8888, 0x36 -> ETC1;
anything else throws. Data follows the header, level 0 first; level `i` is `max(1, w>>i) x max(1, h>>i)`, PVRTC
level size is `max(w,8) * max(h,8) * 4 / 8` (4bpp).

Shipped files: flags 0x8010 = RGBA4444 with alpha, u16 little-endian with R in bits 15-12, G 11-8, B 7-4, A 3-0
(uploaded as `GL_RGBA` / `GL_UNSIGNED_SHORT_4_4_4_4`); flags 0x219 / 0x319 = PVRTC 4bpp **RGB** (no alpha bit), the
latter with 8 extra mip levels (uploaded with `glCompressedTexImage2D(GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG = 0x8C00)`).
A port without PVRTC hardware needs a software PVRTC-4bpp decoder for INGAME_2/6/7/8. Those four files are
genuinely opaque **[V]**: no block has a translucent colour word and no texel uses the punch-through code
(`res_pvrtc_alpha.py`), so decoding them as RGB with alpha = 1 loses nothing.

**PNG** **[R][V]**: all shipped PNGs are ordinary (not Apple CgBI), non-interlaced, 8-bit. Colour type 6 -> RGBA bytes
uploaded as `GL_RGBA`/`GL_UNSIGNED_BYTE`; colour type 2 (SPLASHES only) -> `GL_RGB`/`GL_UNSIGNED_BYTE`.

**Alpha** **[V]**: every RGBA PNG and every RGBA4444 PVR is stored **already premultiplied** (no pixel in any file has
a colour channel greater than its alpha, across 300k+ semi-transparent pixels; `res_check_premult.py`). The loader
performs no premultiplication, no colour-keying and no vertical flip on this path **[R]** for these formats (source
format equals texture format, so `SurfaceFormat::copyPixels` is a straight copy **[I]**; its internals were not audited).

**Texture creation** (`EGL_Image` ctor @000b19b4, `EGL_Texture` ctor @000b62c0, `EGL_Texture::blt` @000b5d2c) **[R]**:

* The GL texture is allocated at the **next power of two** in each dimension (`glTexImage2D` with no data), and
  the image is uploaded into its top-left corner with `glTexSubImage2D(level, 0, 0, w, h)` (`GL_UNPACK_ALIGNMENT` 1).
  The padding area is never written (undefined contents). Row 0 of the file is v = 0.
  `Image::width()/height()` keep the original size; UVs are divided by the **padded** size.
* `GL_TEXTURE_MAG_FILTER = GL_LINEAR`. `GL_TEXTURE_MIN_FILTER = GL_LINEAR`, except `GL_LINEAR_MIPMAP_NEAREST` when
  the file supplies a complete mip chain (only INGAME_6/7/8). No mipmaps are generated.
* Wrap modes are **not set** for images (GL default `GL_REPEAT`); `GL_CLAMP_TO_EDGE` is set only for
  render-target textures.
* Maximum size check against `GL_MAX_TEXTURE_SIZE` (throws `Texture is too large`).

### 4.5 Audio files **[V]**

139 `SFX/*.wav`: RIFF/WAVE PCM, mono, 16-bit, 16000 Hz, chunks `fmt ` + `data` only (largest 204 KB, total 4.3 MB).
9 `Music/*.mp3`: MPEG-2 Layer III, mono, 16000 Hz (3 s .. 69 s). `readHeader_wav` (@000fda20) rejects non-RIFF/WAVE
and non-PCM files.

---------------------------------------------------------------------------------------------------

## 5. Sprite rendering — `gr::EGL_Image::draw(ctx, x, y, srcX, srcY, w, h)` @000b15c4 **[R]**

`drawSprite` has no transform of its own; it uses the context's current `gr::RenderState2D`
(`ctx->getRenderState2D()`, a 0x54-byte struct at `EGL_Context + 0x10c`), which is exactly what
`GameLua::setRenderState` (@0000a960) writes:

| offset | field | default | written by `setRenderState(tx, ty [, sx, sy [, angle [, px, py]]])` |
|---|---|---|---|
| +0x00 | clip `Rect` | (-32000,-32000,32000,32000) | no |
| +0x10, +0x14, +0x18, +0x1c | `m00, m10, m01, m11` | 1, 0, 0, 1 | `cos a, sin a, -sin a, cos a` when >= 5 args |
| +0x20, +0x24 | `tx, ty` | 0, 0 | args 1, 2 when >= 2 args |
| +0x28, +0x2c | `sx, sy` | 1, 1 | args 3, 4 when >= 4 args |
| +0x30, +0x34 | `px, py` (pivot) | 0, 0 | args 6, 7 when >= 7 args |
| +0x38 .. +0x4c | bool 1, int 1, int 5, bool 0, int 0, float 0 | | no (meaning not determined; not used by this path) |
| +0x50 | alpha factor | 1.0 | no (ignored by the renderer, see below) |

`setRenderState` only overwrites the groups for which arguments were supplied; the others **keep their previous
values** (it is not a reset).

Vertex positions. Let `(L, T)` be the quad's top-left from section 3, `W`/`H` = `ctx->width()/height()`
(orientation-aware, 480x320 in landscape):

```
Cx = L + px + tx                 Cy = T + py + ty
for each corner (cx, cy) in (0,0), (w,0), (0,h), (w,h):      // order: TL, TR, BL, BR
    ox = cx - px ;  oy = cy - py
    X  = Cx + ox*m00 + oy*m01    // = Cx + ox*cos - oy*sin
    Y  = Cy + ox*m10 + oy*m11    // = Cy + ox*sin + oy*cos
    ndc.x = X * (2*sx / W) - 1
    ndc.y = 1 - Y * (2*sy / H)
    ndc.z = 0.001
```

So, in pixels: `screen = (R * (corner - pivot) + topLeft + pivot + translate) * scale`, origin top-left, y down.
Notes: rotation is about the point `(px, py)` measured from the **quad's top-left corner** (after the anchor
adjustment), not from the draw position; scale is applied last, so it also scales `tx, ty` and the draw position;
a positive angle rotates clockwise on screen. All float math, no rounding or pixel snapping.

Visibility test: with the NDC bounding box of the four vertices, the quad is submitted only if
`maxX >= -1 && maxY >= -1 && minX < 1 && minY < 1`.

Texture coordinates (no half-texel inset, no flipping), `TW`/`TH` = padded power-of-two texture size:

```
u0 = srcX / TW      u1 = (srcX + w) / TW      v0 = srcY / TH      v1 = (srcY + h) / TH
TL=(u0,v0)  TR=(u1,v0)  BL=(u0,v1)  BR=(u1,v1)
```

Because filtering is `GL_LINEAR` with no inset, a sprite drawn at a non-integer position or scale != 1 samples
up to half a texel of its atlas neighbours (the atlases leave 1-px gaps in places, e.g. the `*_DUMMY` background
sprites are the same art 1 px larger).

Submission: `EGL_RenderBatcher::render(type = 2, spriteShader, pos[4], uv[4], colors = NULL, image, 1)` @001049f8.
The batcher turns each quad into two triangles `(TL,TR,BL)` and `(BL,TR,BR)` and accumulates them. It flushes
(`flush` @00104230) when the primitive type, the image, the clip rect or the non-transform render-state fields
(+0x38..+0x50) change, or when any shader begins. Draw order is preserved, so batching is invisible; a port may
draw immediately.

Pipeline state at flush (`EGL_Shader_Default::begin` @000b550c, shader template `"sprite-alpha"`, flags 0xC000,
created in `gr::createShaders` @000b6f00) **[R]**:

* Blend enabled, **`glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`** — premultiplied-alpha "over":
  `dst = src + dst * (1 - src.a)`.
* `glColor4f(1, 1, 1, 1)`; no per-vertex colours. There is **no tint and no alpha modulation**: the batcher does call
  `shader->setFloat("ALPHA_FACTOR", rs.alphaFactor)`, but `EGL_Shader_Default::setFloat` (@000b49ac) only recognises
  `"LINE_WIDTH"`, so the value is dropped.
* Texture unit 0 enabled with the sheet texture; texture environment is left at the GL default (`GL_MODULATE`) **[I]**.
* Depth test disabled, depth writes off (painter's order). Lighting off. `GL_DITHER` disabled, `GL_CULL_FACE` enabled.
* Projection: identity (times a fixed device-orientation rotation), world/view identity; vertices are already NDC.
* Scissor from the render state's clip rect (`EGL_Context::setClipRect` @000aa774; not audited — default rect is
  effectively "no clipping").

Opaque sheets (SPLASHES RGB PNG, the PVRTC RGB sheets) go through the same path with alpha = 1.

---------------------------------------------------------------------------------------------------

## 6. String rendering — `BitmapFont::drawString` @0006a454 -> @0006ba74 -> @0006b224 **[R]**

Encoding: `lang::String` is a NUL-terminated UTF-8 byte string; `length()` is `strlen`. For drawing/measuring it is
converted to UTF-16 (`String::getUTF16` @0004b044; invalid UTF-8 bytes are skipped one byte at a time) and each
16-bit code unit is looked up in the glyph table keyed by `short`. No surrogate handling, no combining, no bidi,
no kerning, no tabs, no newlines.

```
drawString(text, x, y, anchor):
    if strlen(text) < 1: return
    u16[] s = utf16(text); n = len(s)
    // vertical: convert y to the baseline
    TOP      : y += maxAscending
    VCENTER  : y += maxAscending - ((maxAscending + maxDescending) >> 1)
    BOTTOM   : y -= maxDescending
    BASELINE, VPIVOT : unchanged (y is the baseline)
    // horizontal
    width = sum(glyph(c).w for c in s if glyph exists) + tracking * (n - 1)      // int
    HCENTER  : x -= (float)(width >> 1)
    RIGHT    : x -= (float)width
    LEFT, HPIVOT : unchanged
    for i, c in s:
        g = glyph(c)
        if g is missing: throw "Trying to draw character not present in font: {code}"
        draw sprite g with anchor {VPIVOT, HPIVOT}:  quad top-left = (x - 0, y - g.base), size g.w x g.h,
                                                     source rect (g.x, g.y, g.w, g.h) of the font image
        if i != n - 1: x += (float)(g.w + tracking)
```

* Advance is `glyph width + tracking`; tracking is not applied after the last glyph. With `tracking = -3`
  (BASIC, MENU) glyph quads overlap by 3 px, which works because of the premultiplied "over" blend.
* Glyphs are drawn through exactly the sprite path of section 5 (same render state, scale, rotation, blending).
* Missing glyphs: measuring ignores them (zero width, tracking still counted); drawing throws — after the glyphs
  before it have already been submitted.
* The alignment offset and every advance are integers converted to float; `x`, `y` themselves are not rounded.
* `getStringWidth(text, start, count)` clamps `count` to the UTF-16 length; the Lua entry point always passes
  `(0, 0x7fffffff)`. `GameLua::clipText` uses the ranged form.
* The script uses `getFontLeading()` as the line height / vertical box size for text (e.g. `checkTextBounds`),
  not `getFontHeight()`.

---------------------------------------------------------------------------------------------------

## 7. Routine index (for cross-checking)

| routine | address |
|---|---|
| `LuaResources::LuaResources` (registration table) | 000b9874 |
| `LuaResources::drawSprite` / `drawString` / `playAudio` | 000b936c / 000b9198 / 000b8fe0 |
| `LuaResources::getSpriteBounds` / `getSpritePivot` | 000b8e1c / 000b8c94 |
| `Resources::createSpriteSheet` / `createBitmapFont` / `createTextGroupSet` / `createAudio` | 000bcea0 / 000bd318 / 000bc8c0 / 000be104 |
| `Resources::findSheet` / `drawSprite(name)` / `drawSprite(sheet,name)` | 000bbe88 / 000bc424 / 000bc47c |
| `Resources::drawString` / `getString` / `loadLocale` / `useFont` | 000bf2e4 / 000bc6dc / 000bc52c / 000bbdd4 |
| `Anchor::fromString` | 0006a02c |
| `SpriteSheet::SpriteSheet(ctx, path)` / `drawSprite(name)` / `drawSprite(Sprite*)` | 0006ca88 / 0006c3f4 / 0006c294 |
| `BitmapFont::BitmapFont` / `drawString(u16*)` / `getStringWidth(u16*)` | 0006a784 / 0006b224 / 0006a4bc |
| `TextGroupSet::loadLocaleCodes` / `loadTextGroup` / `getTextGroup` / `TextGroup::get` | 0006f46c / 00070414 / 00071480 / 0006f0d0 |
| `EGL_Image::draw` / `EGL_Image` ctor / `EGL_Texture` ctor / `EGL_Texture::blt` | 000b15c4 / 000b19b4 / 000b62c0 / 000b5d2c |
| `EGL_Context::createImage(String)` / `getSpriteShader` | 000a981c / 000aac38 |
| `EGL_RenderBatcher::render` / `flush` | 001049f8 / 00104230 |
| `gr::createShaders` / `EGL_Shader_Default::begin` / `setFloat` | 000b6f00 / 000b550c / 000b49ac |
| `ImageReader::readHeader_pvr` / `readHeader_png` / `readSurface` | 000638cc / 000e32b8 / 0005f728 |
| `GameLua::setRenderState` | 0000a960 |
| `AudioMixer::playClip` / `setTrackVolume` / `isClipPlaying` / `mixUnlimited16` | 000f12c8 / 000eec98 / 000edf68 / 000ef63c |
| `LuaState::checkType` | 00075d60 |

vtables (vptr): `BitmapFont` 0x1400a0 (`+0x08 drawString(str,x,y,anchor)`, `+0x0c drawString(str,start,count,...)`,
`+0x10 getStringWidth`, `+0x14 getMaxAscending`, `+0x18 getMaxDescending`, `+0x1c getLeading`, `+0x20 getTracking`);
`EGL_Image` 0x1408f4 (`+0x14 draw`); `EGL_Context` 0x1407e0 (`+0x1c createImage(String)`, `+0x58 getRenderState2D`,
`+0x64 width`, `+0x68 height`).

---------------------------------------------------------------------------------------------------

## 8. Surprises and easy mistakes

1. **Premultiplied alpha.** Blend is `(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` and the files are stored premultiplied.
   Blending them as straight alpha darkens every soft edge; premultiplying again does the same.
2. **`createAudio(file, name)` and `setTrackVolume(volume, track)`** have the "obvious" argument order reversed.
3. **`drawSprite` / `drawString` anchors are two free-order strings**, vertical or horizontal each; defaults differ:
   sprites default to PIVOT/PIVOT, strings to TOP/LEFT.
4. **`drawString`'s first argument is a text-group name, the second is an id-or-literal.** Literal text is drawn
   when the group is `""`/unknown or the text is not an id. With a real group, unknown text is cached as `id -> id`.
5. **`getStringWidth` takes literal text**, not (group, id).
6. **`useFont` with an unknown name is a silent no-op** (the script calls `useFont("FONT_TITLE")`, which never exists).
7. **Unknown sheet/sprite in `drawSprite` is silent**; `getSpriteBounds`/`getSpritePivot` return 0,0.
8. **Strict argument types**: numbers are not accepted for string parameters, `nil` is not accepted for optional
   ones (omit them instead), `loop` must be a boolean.
9. **`isAudioPlaying` must return a boolean `false`** (script compares `== false`), and `playAudio` returns nothing.
10. **Missing glyph = exception when drawing, zero width when measuring.** French strings contain U+00A0, which
    FONT_BASIC/FONT_MENU lack, and three English strings contain `\n`; in the original those throw unless the script
    splits/avoids them. A port should decide deliberately (recommended: skip the glyph) rather than crash.
11. **`setRenderState` is sticky per field group**, and its pivot is relative to the quad's top-left; scale is applied
    after translation (translation and positions are in pre-scale units).
12. **NPOT atlases are padded to power-of-two textures and UVs use the padded size.** If the port samples the
    image directly, use plain pixel rects instead.
13. **No UV inset + bilinear filtering**: neighbour bleed is part of the original look; nearest sampling will look
    slightly different at fractional scales.
14. **The alpha factor in the render state does nothing** in this (GLES 1) build; sprites are never tinted or faded
    by this layer.
15. **Sprite `.dat` pivots are signed and can be far outside the rect**; sizes are signed 16-bit, `h >> 1` rounding.
16. **Font "leading" is unrelated to glyph height** (FONT_MENU: leading 60, height 45; FONT_SCORE: 104 vs 63).
17. **Text offsets in `TEXTS_*.dat` are relative to the end of their own table entry**, not to a common base.
18. **Three sheets are RGB PVRTC** (backgrounds and ground textures); the ground textures carry mipmaps and are the
    only textures using a mipmapped minification filter.
19. `setTrackVolume` accepts track 8 (one past the array) in the original — clamp to 0..7 in a port.
20. Audio output must exist before `createAudio`/`playAudio`/`stopAudio`/`stopAllAudio` (they throw otherwise);
    `isAudioPlaying` and `setMasterVolume` tolerate its absence; `setTrackVolume` does not check at all.

---------------------------------------------------------------------------------------------------

## 9. Validation scripts (in `analysis/`)

| script | purpose |
|---|---|
| `res_parse_dat.py [-v]` | parses every sheet / font / text `.dat`, checks bounds and trailing bytes, prints glyph sets and strings |
| `res_check_premult.py` | decodes every PNG and RGBA4444 PVR, counts pixels with colour > alpha |
| `res_pvrtc_alpha.py` | checks the PVRTC files' colour words for the translucent flag |
| `res_lua_calls.py <fn> [ctx] [max]` | prints `res.<fn>` call sites from `lua-src/gamelogic.lua` |
| `res_lua_argcount.py [-l]` | per-function argument-count / literal-argument statistics (counts are inflated where unluac expands a multi-return call) |
| `res_fn.py <file.c> <addr>...` | prints a decompiled function with declaration noise removed |
| `res_dat.py <addr>...` | resolves literal-pool words (strings, floats, symbols) in the binary |

(`dat.py` is an earlier copy of `res_dat.py`; another agent's `vt.py` imports it, so it was left in place.)

---------------------------------------------------------------------------------------------------

## 10. Not determined / low confidence

1. **Exception handling**: where engine exceptions are caught and what the game does afterwards (only the strings
   `"Exception: %s"` / `"Unknown exception in mainloop!"` were seen). Ghidra does not show SjLj catch blocks.
2. **Global GL state set elsewhere** (`EGL_State` ctor, context setup): texture environment for unit 0, front-face
   winding. `GL_CULL_FACE` is enabled when sprites are drawn; whether a negative `sx`/`sy` in `setRenderState`
   (mirroring) would be culled was not established.
3. **RenderState2D fields +0x38..+0x4c** (defaults `1, 1, 5, 0, 0, 0.0`): names/meaning unknown. They only affect
   batch breaking in the sprite path.
4. **Clip rectangle semantics** (`EGL_Context::setClipRect`, scissor mapping under device rotation) — not audited.
5. **`SurfaceFormat::copyPixels`** internals (it has colour-key-like parameters). For the shipped files the source
   and texture formats are equal, so no conversion should occur; this is inferred, not traced.
6. **Contents of the padding area** of NPOT-padded textures (allocated with no data; undefined in GL).
7. **Sheet search order for `sheet == ""`** when a sprite name exists in several sheets (hash-table iteration
   order). Irrelevant for the 1.0 data: no duplicate names.
8. **Glyph U+0010** in FONT_BASIC / FONT_MENU / FONT_BIG_NUMBERS: purpose unknown.
9. **`AudioConfiguration` field names** (channels, bits, rate) are inferred from the validation ranges and the data.
10. **Mixer details not traced**: the per-instance flag at +0x19 (looks like "paused"), the MP3 decode path
    (`AudioReader::readData_mp3`), buffer sizes / latency (`AudioBufferPump`), and the OpenAL side.
11. **`pf::Launcher::openURL`** return value semantics.
12. **File access layer** (`io::FileInputStream`, `FileInputStream::sm_fileBundle`): how the bundle root is
    resolved was not traced; only the relative layout under `data/` is certain.
13. **`readUTF` and Java "modified UTF-8"**: no special handling was seen and no such bytes occur in the data.
14. **Who flushes the batcher at end of frame** (outside this layer).
15. **`GameLua::clipText`** (line wrapping, how it treats `\n` and U+00A0) was only skimmed: it calls
    `Resources::getString` and the ranged `Resources::getStringWidth`, and fills `clippedText.lines` /
    `clippedText.widestLine`. It belongs to the GameLua layer and is not specified here.
