# Angry Birds 1.0 for the iPod nano 7G

An engine reimplementation that runs the original game's Lua bytecode and converted
assets. The Mac harness is playable; the nano app builds and has been installed on
the existing NanoApps test device. The user confirmed that repacking asset pages to
256×128 fixed the reboot entering level 1. A later test rebooted during a pig impact
in level 1-2. A fix for repeated scenery rebuilding and oversized generated textures
is now installed; hardware confirmation is pending. An on-device FPS counter and
timing recorder remain enabled to measure the result.
Audio is currently silent on both platforms.

No game binary or assets are tracked here. The converter expects an existing
Angry Birds 1.0 `AngryBirds.app/data` directory.

## Build and test on macOS

The converter requires Pillow, NumPy and texture2ddecoder (installed in the project's
local `.venv`). OpenGL access is required for the host harness, including headless tests.

```sh
.venv/bin/python tools/convert_assets.py v1.0/Payload/AngryBirds.app/data port/build/data-small-pages --max-page 256 --max-page-height 128
make -C port -j4
python3 tools/smoke_host.py
cd port
./build/abhost -w -d build/data-small-pages
```

The texture scale is 0.5. The installed test profile caps asset pages at 256×128,
matching the page bounds used by the working Mario Kart renderer. Large sprites
are reduced further to fit; small sprites keep their original requested scale.
Display coordinates remain 576×320, rendered onto 432×240 pixels. Larger asset
pages remain available for host comparisons. Runtime ground-mask atlases now use
up to eight 256×128 pages, with only one 64 KiB pixel buffer during construction.
The nano renderer rejects textures exceeding the device page limits.

The smoke test clears level 1-1, checks a 31,320 score, verifies save reload, and
repeats with the portrait framebuffer rotation used on the nano. Tests use temporary
save directories and do not overwrite normal saves. It also continues into level
1-2 (internally `Level57`), throws three birds, verifies pig-impact scoring, and
checks texture memory and cache rebuild counts with ±30% frame-time jitter.

## Build the nano app

Use the existing NanoApps SDK checkout and a Linux ARM bare-metal toolchain with
newlib. This app targets the SDK's firmware 1.1.2 / 39A10023 environment; other
firmware builds have not been validated.

```sh
make -C port/nano NANOAPPS=/path/to/NanoApps -j4
mkdir -p /path/to/NanoApps/apps/angrybirds
cp port/nano/app/* /path/to/NanoApps/apps/angrybirds/
make -C /path/to/NanoApps/apps/angrybirds AB_DIR=/absolute/path/to/port -j4
```

The engine build rejects static constructors; the application build checks Thumb
code for unsupported floating-point instructions and validates relocations.
The profiling app is about 252 KiB on disk and uses a separate allocation arena plus up to
3 MiB of texture pixel data. Driver overhead is additional.

## Prepare an installation

`tools/stage_nano.py` builds a file-only stage from an existing app list. It preserves
other apps' bundle bytes, adds Angry Birds, validates asset file sizes, and writes
checksums. It does not access a device.

```sh
.venv/bin/python tools/stage_nano.py \
  --sdk /path/to/NanoApps \
  --app /path/to/angrybirds.hbapp \
  --data port/build/data-small-pages \
  --base-pack /path/to/current/AllApps-B.pack \
  --out port/build/new-stage
```

The stage contains `/Apps/Executables/Angry Birds.hbapp`, its icon,
`/Apps/Data/AngryBirds/`, and the augmented `/Apps/AllApps-B.pack`.
Copy data and executable first, verify readbacks, and replace the app list last.
Existing high scores and settings are not part of the stage. The current test
installation's previous app list is retained in `port/build/AllApps-B.before.pack`
and on the mini PC under `~/nano-ab/device-backup-20261001/`.

## Verified so far

- Nano compile, link, static-constructor check, Thumb-FP check and `.hbapp` packaging.
- Host level 1-1 completed and save reloaded in both screen orientations.
- With the small-page profile, level 1-1 peaks at 2,072 KiB of texture data.
  The previous larger-page profile also loaded displayed level 12 on the host;
  that does not establish hardware compatibility.
- All 59 repacked asset pages and 826 sprite rectangles validated.
- The compiled nano engine completes level 1 in QEMU using its actual allocator,
  math routines, Lua and Box2D, including a run with the minimum 2 MiB arena.
  Graphics are mocked; this cannot verify the GPU or its allocation overhead.
  This run scores 31,270, versus 31,320 in the Mac build, so numerical parity
  remains incomplete.
- The level 1-2 collision replay passes 4,600 ARM frames with frame-time jitter
  and a 2 MiB arena: pig-impact score 17,090, five ground atlas builds, 2,458 KiB
  peak texture data including the FPS HUD. No allocation failures were reported.
- Rendering now records complete frames so texture uploads precede draw calls,
  following the device-driver workaround already used by the Mario Kart port.
- Device installation: 123 files copied and read back successfully. Existing Mario
  64 and Mario Kart app entries preserved; no firmware or loader code changed.
- Subsequent small-page update: 156 data files (including the manifest), the app,
  and a one-launch trace flag copied and read back. The previous executable and
  complete data directory are backed up on the mini PC at
  `~/nano-ab/device-backup-20261001/before-small-pages/`.

On hardware, hold the nano with Home on the right. Home exits the app; relaunching
starts it fresh. Post-fix device frame rate and long-session stability still need
hardware confirmation. The engine analysis notes are under `analysis/`;
they are evidence for the reimplementation, not proof of complete behavioral parity.

The ARM regression wrapper can be run on the Linux build host after building the
nano library:

```sh
cd port
python3 tests/qemu/build.py /path/to/NanoApps
qemu-arm -cpu cortex-a7 build/qemu/abtest
# Model 8 MiB free before the game allocates its minimum 2 MiB arena:
python3 tests/qemu/build.py /path/to/NanoApps 8
qemu-arm -cpu cortex-a7 build/qemu/abtest
```

It also rejects texture or vertex-buffer uploads after the first draw of a frame.

For a hardware diagnostic run, create `/Apps/Data/AngryBirds/trace.once` before
launching. The app consumes the flag and saves at most 96 load/upload/draw checkpoints
to `log.txt`, including OS free memory and the arena's largest free block. Reconnect
after a failure without relaunching the app, so startup does not overwrite the log.
Normal launches do not write these checkpoints.

## Level 1-2 cache fix and device measurements

The pre-fix device report measured 179 callbacks in 30.073 seconds: about 5.9 FPS,
with 152.7 ms average callback work. Physics averaged 0.3 ms; game work averaged
119.8 ms, including 19.5 ms of file IO and 4.6 ms of texture uploads. The report's
tail was zero-filled from byte 512, so its missing fields are not usable evidence.

A host reproduction identified a cache bug: interpolation can change a stationary
object's render coordinates by a floating-point rounding unit. The renderer treated
each change as movement and rebuilt the ground atlas. By frame 2,800, uneven timing
caused 566 atlas builds and 2,902 reads, versus one build and 77 reads with steady
timing. The cache now uses the static body's actual pose. The longer 4,600-frame
host regression, including three shots, needs five builds and 97 reads. Its peak
texture data is 2,456 KiB. These are work counts, not device FPS predictions.

Generated ground textures now follow the small-page limits already used for assets;
the old 512×512 runtime upload is gone. This addresses a known driver risk, but a
real-device level 1-2 replay is still needed to confirm the reported reboot is fixed.

To run the matching ARM regression after building the library:

```sh
cd port
python3 tests/qemu/build.py /path/to/NanoApps 8 build/data-small-pages tests/level2.txt 4600 jitter
qemu-arm -cpu cortex-a7 build/qemu/abtest
```

The profiling build displays FPS in the lower-right corner. This uses the interval
between device frame callbacks, including time spent outside the game callback;
it is not the reciprocal of CPU work time. After a two-second level warmup it
collects 30 seconds in RAM, then writes `perf.txt` once at the start of the next
callback, before new GL work. The compact v2 report fits within 512 bytes and includes mean
FPS, frame-time percentiles, slow-frame counts, game/physics/draw/IO/upload timings.
`fps10` is FPS multiplied by ten; times are microseconds except `p95_ms`, whose
histogram is capped at 200 ms. Physics, IO and upload times are subsets of game
time. `mask_builds` counts scenery rebuilds during the measured window; the
draw, quad and texture fields describe its final frame.
The HUD adds one small texture and two draw batches. Do not enable `trace.once`
for a performance measurement, because those diagnostic writes add stalls.
