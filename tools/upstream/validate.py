#!/usr/bin/env python3
"""Fail when firmware baseline, simulator identity or release coverage drift apart."""
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]

def validate(root=ROOT):
    config = json.loads((root/'tools/upstream/baseline.json').read_text())
    source = subprocess.check_output(['git','show',config['commit']+':CMakeLists.txt'],cwd=root,text=True)
    version = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)',source)[1]
    assert version == config['sourceVersion'], 'Pinned commit/version mismatch'
    current = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)',(root/'CMakeLists.txt').read_text())[1]
    assert current == config['sourceVersion'], 'Advance upstream baseline together with firmware version'
    js = (root/'sim/upstream.js').read_text()
    for key,value in [('sourceVersion',version),('sourceCommit',config['commit']),('releaseVersion',config['reviewedRelease'])]:
        assert f'{key}: "{value}"' in js, f'Simulator {key} is stale'
    coverage = json.loads((root/'tools/upstream/coverage.json').read_text())
    assert coverage['release'] == config['reviewedRelease'], 'Release coverage is stale'
    assert coverage['defaultDecision'] == 'accept-all-when-source-available'
    assert coverage['items'], 'No release items tracked'
    prototype = re.search(r'prototypeVersion: "([^"]+)"', js)[1]
    assert prototype == config['reviewedRelease'], 'Prototype release identity is stale'
    if not coverage['sourceAvailable']:
        assert 'prototypeBasis: "release-notes"' in js, 'Do not label notes-based layout as source verified'
        assert all(x['firmware'] == 'awaiting-source' for x in coverage['items']), 'Unpublished source cannot be marked merged'
    ids = set()
    for item in coverage['items']:
        assert item['id'] not in ids, 'Duplicate coverage item'
        ids.add(item['id'])
        assert all(item.get(k) for k in ('change','firmware','classic','valet')), 'Missing implementation disposition'
    assert config['policy'] == 'merge-all-add-valet'
    assert 'src="upstream.js"' in (root/'sim/index.html').read_text()
    print(f"Baseline {version}@{config['commit'][:8]}, release {config['reviewedRelease']}, both simulator tabs and {len(ids)} release items consistent")

if __name__ == '__main__':
    validate()
