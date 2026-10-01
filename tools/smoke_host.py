#!/usr/bin/env python3
"""Play level 1-1 in both orientations; check completion, texture budget and save reload."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    ap.add_argument('--data', type=Path, default=root / 'port/build/data-small-pages')
    ap.add_argument('--exe', type=Path, default=root / 'port/build/abhost')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='ab-smoke-') as directory:
        temp = Path(directory)
        for rotated in (False, True):
            name = 'portrait' if rotated else 'landscape'
            save, shots = temp / (name + '-save'), temp / (name + '-shots')
            base = [str(args.exe.resolve()), '-d', str(args.data.resolve()), '-S', str(save)]
            cmd = base + ['-n', '2300', '-i', str(root / 'port/tests/level1.txt'),
                          '-o', str(shots), '-D', 'score,levelCompleted', '-De', '500']
            if rotated:
                cmd.append('-r')
            result = subprocess.run(cmd, capture_output=True, text=True, check=True)
            log = result.stdout + result.stderr
            assert 'levelCompleted=true' in log, log
            assert 'budget' not in log and 'load failed' not in log, log
            peak = re.search(r'peaks: .*?, (\d+) KB of texture', log)
            assert peak and int(peak[1]) <= 3072, log
            highscore = (save / 'highscores.lua').read_text()
            assert 'completed = true' in highscore and 'score = 31320' in highscore, highscore
            assert (shots / 't2100.png').is_file()
            reload_result = subprocess.run(base + ['-n', '2', '-D', 'highscores.Level1.score'],
                                           capture_output=True, text=True, check=True)
            assert 'highscores.Level1.score=31320' in reload_result.stderr, reload_result.stderr
            print('%s: level cleared, score 31320, save reloaded; texture peak %s KiB' % (name, peak[1]))


if __name__ == '__main__':
    main()
