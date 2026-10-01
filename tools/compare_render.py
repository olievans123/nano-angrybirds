#!/usr/bin/env python3
"""Replay both orientations and compare PNG pixels against a reference host build."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageChops


def main():
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('reference', type=Path)
    ap.add_argument('--candidate', type=Path, default=root / 'port/build/abhost')
    ap.add_argument('--data', type=Path, default=root / 'port/build/data-small-pages')
    args = ap.parse_args()
    compared = 0
    with tempfile.TemporaryDirectory(prefix='ab-render-') as directory:
        temp = Path(directory)
        for level, frames in ((1, 2300), (2, 4600)):
            for rotated in (False, True):
                name = 'level%d-%s' % (level, 'portrait' if rotated else 'landscape')
                runs = []
                for label, exe in (('reference', args.reference), ('candidate', args.candidate)):
                    dest = temp / name / label
                    dest.mkdir(parents=True)
                    shots = dest / 'shots'
                    cmd = [str(exe.resolve()), '-d', str(args.data.resolve()), '-S', str(dest / 'save'),
                           '-o', str(shots), '-n', str(frames), '-s', '30', '-i',
                           str(root / ('port/tests/level%d.txt' % level))]
                    if rotated:
                        cmd.append('-r')
                    if level == 2:
                        cmd.append('-j')
                    result = subprocess.run(cmd, capture_output=True, text=True, check=True,
                                            env=dict(os.environ, AB_REPLAY_SEED='12345'))
                    log = result.stdout + result.stderr
                    assert 'capacity exceeded' not in log and 'load failed' not in log, log
                    runs.append(shots)
                    if label == 'candidate':
                        for line in log.splitlines():
                            if line.startswith(('render work:', 'rotation work:')):
                                print(name + ': ' + line, flush=True)
                names = sorted(p.name for p in runs[0].glob('*.png'))
                assert names and names == sorted(p.name for p in runs[1].glob('*.png'))
                for filename in names:
                    with Image.open(runs[0] / filename) as a, Image.open(runs[1] / filename) as b:
                        assert a.size == b.size, (name, filename, a.size, b.size)
                        assert ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox() is None, (name, filename)
                    compared += 1
                print('%s: %d screenshots identical' % (name, len(names)), flush=True)
    print('%d screenshot comparisons passed' % compared)


if __name__ == '__main__':
    main()
