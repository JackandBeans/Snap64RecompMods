"""Proves a mod leaves the game's scoring alone: runs the port's evaluation replay twice, without the
mod and with it, from a scratch data folder of its own (a copy of the ROM, the replay's save and the
settings; neither the player's save nor the port's mods folder is touched), and requires every
[SNAP-SCORE] line of the two runs to match. Each run ends a minute after its last expected score
line, the game's own progress marker, never on a wall clock (a cap is a distant safety only). Both
runs open the game window.

    python tools/check_mod.py <port Release folder> <mod .nrm> [--replay eval.inputs] [--scores 45]
                              [--config key:value,key:value]

The port folder holds Snap64Recomp.exe, pokemonsnap.z64, saves/pokemonsnap.bin (the replays' own save),
snapsettings.json and the replay. --config sets the mod's options for the run as the Mods page would
(the id and version are read from the mod's own mod.toml beside the .nrm's source, or given as
--mod-id and --mod-version). Exit code 0 on a match, 1 on a difference, 2 when a run did not reach
its scores."""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__)).replace('\\', '/')
ROOT = os.path.dirname(HERE)


def mod_identity(nrm, mod_id, mod_version):
    """The mod's id and version, from the arguments or the mod.toml of the mod whose build this is."""
    if mod_id and mod_version:
        return mod_id, mod_version
    name = os.path.basename(os.path.dirname(os.path.abspath(nrm)))      # build/<mod>/x.nrm
    toml = os.path.join(ROOT, 'mods', name, 'mod.toml')
    if os.path.exists(toml):
        text = open(toml, encoding='utf-8').read()
        got_id = re.search(r'^id\s*=\s*"([^"]+)"', text, re.M)
        got_version = re.search(r'^version\s*=\s*"([^"]+)"', text, re.M)
        return (mod_id or (got_id.group(1) if got_id else None),
                mod_version or (got_version.group(1) if got_version else None))
    return mod_id, mod_version


def run_once(args, data, with_mod):
    """One run of the replay; returns (score lines, how it ended)."""
    rel = args.release.replace('\\', '/')
    exe = rel + '/Snap64Recomp.exe'
    os.makedirs(data + '/saves', exist_ok=True)
    os.makedirs(data + '/mods', exist_ok=True)
    if not os.path.exists(data + '/pokemonsnap.z64'):
        shutil.copyfile(rel + '/pokemonsnap.z64', data + '/pokemonsnap.z64')
    shutil.copyfile(rel + '/saves/pokemonsnap.bin', data + '/saves/pokemonsnap.bin')
    shutil.copyfile(rel + '/snapsettings.json', data + '/snapsettings.json')
    for old in glob.glob(data + '/mods/*.nrm'):
        os.remove(old)
    shutil.rmtree(data + '/mod_config', ignore_errors=True)
    for name in ('mods.json', 'snap64.running'):
        if os.path.exists(data + '/' + name):
            os.remove(data + '/' + name)
    if with_mod:
        shutil.copyfile(args.nrm, data + '/mods/' + os.path.basename(args.nrm))
        if args.config:
            mod_id, mod_version = mod_identity(args.nrm, args.mod_id, args.mod_version)
            if not mod_id or not mod_version:
                sys.exit('--config needs the mod id and version: --mod-id and --mod-version')
            os.makedirs(data + '/mod_config')
            json.dump({'mod_id': mod_id, 'mod_version': mod_version, 'recomp_version': args.recomp_version,
                       'storage': dict(kv.split(':', 1) for kv in args.config.split(','))},
                      open(data + '/mod_config/' + mod_id + '.json', 'w'), indent=4)
    cwd = data + '/run'
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(cwd)
    env = dict(os.environ, SNAP_DATA_DIR=data, SNAP_REPLAY=rel + '/' + args.replay, SNAP_MUTE='1',
               SNAP_WINDOW=args.window, SNAP_STATS='1', SNAP_SAFE_START_ANSWER='keep')
    log_path = data + ('/with_mod.log' if with_mod else '/without_mod.log')
    log = open(log_path, 'w')
    p = subprocess.Popen([exe], cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
    start = time.time()
    reached = None
    scores = []
    while p.poll() is None and (time.time() - start) < args.cap:
        time.sleep(1.0)
        log.flush()
        text = open(log_path, encoding='utf-8', errors='replace').read()
        scores = [l for l in text.splitlines() if l.startswith('[SNAP-SCORE] pokemon')]
        if len(scores) >= args.scores and reached is None:
            reached = time.time()
        if reached is not None and (time.time() - reached) > 60:
            break
    ended = 'by itself, exit code %s' % p.returncode if p.poll() is not None else 'closed by the check'
    if p.poll() is None:
        p.kill()
    p.wait(timeout=30)
    log.close()
    if os.path.exists(data + '/snap64.running'):
        os.remove(data + '/snap64.running')
    text = open(log_path, encoding='utf-8', errors='replace').read()
    scores = [l for l in text.splitlines() if l.startswith('[SNAP-SCORE] pokemon')]
    print('%s the mod: %d score lines in %.0f s, ended %s' % ('with' if with_mod else 'without', len(scores),
                                                             time.time() - start, ended))
    return scores, ended


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('release', help="the port's Release folder")
    ap.add_argument('nrm', help='the mod to check')
    ap.add_argument('--replay', default='eval.inputs', help='the replay, in the Release folder (eval.inputs)')
    ap.add_argument('--scores', type=int, default=45, help='how many score lines the replay makes (45)')
    ap.add_argument('--config', default=None, help="the mod's options for the run: key:value,key:value")
    ap.add_argument('--mod-id', default=None)
    ap.add_argument('--mod-version', default=None)
    ap.add_argument('--recomp-version', default='1.1.3')
    ap.add_argument('--window', default='1280x960')
    ap.add_argument('--cap', type=float, default=900.0, help='a distant safety, in seconds (900)')
    ap.add_argument('--data', default=ROOT + '/build/check_mod', help='the scratch data folder')
    args = ap.parse_args()
    data = args.data.replace('\\', '/')
    os.makedirs(data, exist_ok=True)
    without, ended_a = run_once(args, data, False)
    with_mod, ended_b = run_once(args, data, True)
    if len(without) < args.scores or len(with_mod) < args.scores:
        print('FAIL: a run did not reach %d score lines (%d without, %d with); see %s' % (args.scores, len(without), len(with_mod), data))
        return 2
    differ = [(a, b) for a, b in zip(without[:args.scores], with_mod[:args.scores]) if a != b]
    if differ:
        print('FAIL: %d of %d score lines differ with the mod on:' % (len(differ), args.scores))
        for a, b in differ[:10]:
            print('  without: ' + a)
            print('  with:    ' + b)
        return 1
    print('PASS: the %d score lines are the same with the mod on as off' % args.scores)
    return 0


if __name__ == '__main__':
    sys.exit(main())
