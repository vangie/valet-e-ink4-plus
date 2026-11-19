#!/usr/bin/env python3
"""Read upstream status, or prepare a full merge in an isolated worktree.
Never touches the current worktree/index, pushes, flashes, or changes gateway pins.
"""
import argparse
import datetime
import json
import pathlib
import re
import subprocess
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
CONFIG = ROOT / 'tools/upstream/baseline.json'

def git(*args, cwd=ROOT, check=True):
    return subprocess.run(['git', *args], cwd=cwd, check=check, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def source_version(commit):
    text = git('show', f'{commit}:CMakeLists.txt').stdout
    match = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    if not match:
        raise ValueError('Upstream PROJECT_VER missing')
    return match[1]

def registry(config):
    request = urllib.request.Request(config['registryApi'], headers={'Accept': 'application/json'})
    with urllib.request.urlopen(request, timeout=20) as response:
        data = json.load(response)
    if data.get('code') != 0:
        raise ValueError('Registry returned unsuccessful response')
    firmware = next(x for x in data['data']['list'] if x['id'] == config['firmwareId'])
    versions = firmware.get('versions') or []
    version = next((x for x in versions if x['id'] == firmware.get('latestVersionId')), None)
    if not version or not version.get('version'):
        raise ValueError('Registry latestVersionId is missing from versions')
    # Only public release metadata, never credentials or user records.
    return {key: version.get(key) for key in ('version', 'createDate', 'remark', 'githubUrl', 'giteeUrl')}

def report(config, skip_registry=False):
    git('fetch', '--no-tags', config['repository'], config['branch'])
    target = git('rev-parse', 'FETCH_HEAD').stdout.strip()
    baseline = config['commit']
    if git('merge-base', '--is-ancestor', baseline, target, check=False).returncode:
        raise ValueError('Upstream diverged from baseline; manual ancestry review required')
    if git('merge-base', '--is-ancestor', baseline, 'HEAD', check=False).returncode:
        raise ValueError('Pinned upstream baseline is not merged in this checkout')
    files = git('diff', '--name-only', baseline, target).stdout.splitlines()
    result = {'checkedAt': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'baseline': baseline, 'baselineVersion': config['sourceVersion'],
              'sourceCommit': target, 'sourceVersion': source_version(target),
              'sourceUpdate': target != baseline, 'changedFiles': files,
              'uiReviewRequired': any(x.startswith(('main/display/', 'main/assets/', 'main/reader/',
                                                    'main/boards/', 'main/application')) for x in files),
              'dirty': bool(git('status', '--porcelain', '--untracked-files=normal').stdout.strip())}
    if not skip_registry:
        try:
            result['release'] = registry(config)
            result['releaseSourceMismatch'] = result['release']['version'] != result['sourceVersion']
        except Exception as error:
            result['registryError'] = f'{type(error).__name__}: {error}'
    return result

def prepare(config, result, worktree):
    if not result['sourceUpdate']:
        return {'prepared': False, 'reason': 'No new public source commit to merge'}
    if result['dirty']:
        raise ValueError('Commit the current Valet work first; no automatic stash or partial snapshot')
    path = pathlib.Path(worktree).expanduser().resolve()
    if path.exists():
        raise ValueError('Choose a new worktree directory')
    branch = f"sync/metalio-{result['sourceVersion']}-{result['sourceCommit'][:8]}"
    git('worktree', 'add', '-b', branch, str(path), 'HEAD')
    merged = git('merge', '--no-ff', '--no-commit', result['sourceCommit'], cwd=path, check=False)
    conflicts = git('diff', '--name-only', '--diff-filter=U', cwd=path).stdout.splitlines()
    if merged.returncode and not conflicts:
        raise ValueError(merged.stderr.strip() or merged.stdout.strip())
    return {'prepared': True, 'branch': branch, 'worktree': str(path), 'conflicts': conflicts,
            'mergePending': True, 'next': 'Resolve, update simulator, validate, advance baseline, then commit the merge'}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'prepare'], nargs='?', default='check')
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--skip-registry', action='store_true')
    parser.add_argument('--worktree')
    args = parser.parse_args()
    config = json.loads(CONFIG.read_text())
    try:
        result = report(config, args.skip_registry)
        if args.action == 'prepare':
            if not args.worktree:
                parser.error('prepare requires --worktree')
            result['merge'] = prepare(config, result, args.worktree)
    except Exception as error:
        parser.exit(1, f'Upstream sync failed: {error}\n')
    body = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')

if __name__ == '__main__':
    main()
