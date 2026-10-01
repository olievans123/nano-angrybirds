# Angry Birds 1.0 for the iPod nano 7G

An engine reimplementation that runs the original game's Lua bytecode and converted
assets. The Mac harness is playable; the nano app builds and has been installed on
the existing NanoApps test device. The user confirmed that repacking asset pages to
256×128 fixed the reboot entering level 1. Normal gameplay still has visible lag;
an on-device FPS counter and timing recorder are installed to guide optimization.
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
pages remain available for host comparisons. Runtime ground-mask atlases still
use the original 512×512 size and have not been validated on hardware.

The smoke test clears level 1-1, checks a 31,320 score, verifies save reload, and
repeats with the portrait framebuffer rotation used on the nano. Tests use temporary
save directories and do not overwrite normal saves.

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
- Rendering now records complete frames so texture uploads precede draw calls,
  following the device-driver workaround already used by the Mario Kart port.
- Device installation: 123 files copied and read back successfully. Existing Mario
  64 and Mario Kart app entries preserved; no firmware or loader code changed.
- Subsequent small-page update: 156 data files (including the manifest), the app,
  and a one-launch trace flag copied and read back. The previous executable and
  complete data directory are backed up on the mini PC at
  `~/nano-ab/device-backup-20261001/before-small-pages/`.

On hardware, hold the nano with Home on the right. Home exits the app; relaunching
starts it fresh. Device frame rate, touch behavior and long-session memory use
still need hardware confirmation. The engine analysis notes are under `analysis/`;
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

The profiling build displays FPS in the lower-right corner. This uses the interval
between device frame callbacks, including time spent outside the game callback;
it is not the reciprocal of CPU work time. After a two-second level warmup it
collects 30 seconds in RAM, then writes `perf.txt` once. The report includes mean
FPS, frame-time percentiles, slow-frame counts, game/physics/draw/IO/upload timings.
The HUD adds one small texture and two draw batches. Do not enable `trace.once`
for a performance measurement, because those diagnostic writes add stalls.
