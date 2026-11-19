"""Interop test against actual Passport adapters, without running user gateways.

Requires VALET_WIRE_TEST (compiled C++ executable), PASSPORT_SOURCE, cryptography.
All adapter modifications and credentials are confined to a temporary directory.
"""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('enable_valet', root / 'integrations/valet-gateway/tools/enable_valet.py')
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)
source = Path(os.environ['PASSPORT_SOURCE'])
with tempfile.TemporaryDirectory(prefix='valet-gateway-') as temporary:
    folder = Path(temporary)
    shutil.copytree(source / 'tools/hermes', folder / 'adapter', ignore=shutil.ignore_patterns('__pycache__'))
    file, updated, changed = patcher.prepare(folder / 'adapter')
    assert changed
    file.write_text(updated)
    assert not patcher.prepare(folder / 'adapter')[2]
    package = types.ModuleType('valet_test_adapter')
    package.__path__ = [str(folder / 'adapter')]
    sys.modules[package.__name__] = package
    def load(name):
        spec = importlib.util.spec_from_file_location('valet_test_adapter.' + name, folder / 'adapter' / (name + '.py'))
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module
    auth, protocol = load('passport_auth'), load('passport_protocol')
    store = auth.DeviceStore(folder / 'devices.sqlite', now=lambda: 2000000000)
    code = store.issue('wss://gateway.example')
    bootstrap = json.loads(base64.urlsafe_b64decode(code['setupCode'] + '=' * (-len(code['setupCode']) % 4)))['bootstrapToken']
    public = Ed25519PrivateKey.from_private_bytes(bytes(range(32))).public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    identity = hashlib.sha256(public).hexdigest()
    def generate(token, is_bootstrap):
        config = dict(identity=identity, token=token, bootstrap=is_bootstrap, nonce='real-challenge-nonce', timestamp=2000000000000)
        (folder / 'input.json').write_text(json.dumps(config))
        subprocess.run([os.environ['VALET_WIRE_TEST'], str(folder / 'input.json'), str(folder / 'frame.json')], check=True)
        return json.loads((folder / 'frame.json').read_text())
    frame = generate(bootstrap, True)
    claimed, token = store.authenticate(frame['params'], 'real-challenge-nonce', 2000000000000)
    assert claimed == identity and token != bootstrap
    try:
        store.authenticate(frame['params'], 'real-challenge-nonce', 2000000000000)
        raise AssertionError('One-use bootstrap was reusable')
    except auth.ProtocolError:
        pass
    device = generate(token, False)
    assert store.authenticate(device['params'], 'real-challenge-nonce', 2000000000000) == (identity, token)
    tampered = json.loads(json.dumps(device)); tampered['params']['client']['platform'] = 'esp32c3'
    try:
        store.authenticate(tampered['params'], 'real-challenge-nonce', 2000000000000)
        raise AssertionError('Tampered signed family was accepted')
    except auth.ProtocolError:
        pass
    wav = (folder / 'frame.json.wav').read_bytes()
    params = {'sessionKey': 'agent:passport:passport-' + identity, 'idempotencyKey': 'valet-test-1', 'message': '',
              'attachments': [{'type': 'audio', 'mimeType': 'audio/wav', 'content': base64.b64encode(wav).decode()}]}
    voice = protocol.PassportProtocol(store, now=lambda: 2000000000)
    run, content, fresh = voice.prepare(identity, params)
    assert fresh and content[1] == wav
    assert voice.prepare(identity, params)[2] is False
    store.revoke(identity)
    try:
        store.authenticate(device['params'], 'real-challenge-nonce', 2000000000000)
        raise AssertionError('Revoked device was accepted')
    except auth.ProtocolError:
        pass
    store.close()
    print('PASS real Hermes adapter: C++ Ed25519 proof, one-use pairing, token reconnect/revoke, signed-family tampering, WAV question, idempotency')
    shutil.copytree(source / 'tools/openclaw', folder / 'openclaw', ignore=shutil.ignore_patterns('node_modules','__pycache__'))
    file, updated, changed = patcher.prepare(folder / 'openclaw'); assert changed; file.write_text(updated)
    js = '''import {createPassportConnection} from './openclaw/passport-proxy.mjs';
import {readFileSync} from 'node:fs';
const sent=[], emitted=[];let stopped='';
const c=createPassportConnection({sendUpstream:f=>sent.push(f),sendClient:f=>emitted.push(f),close:r=>stopped=r,now:()=>0,schedule:()=>0,cancel:()=>{}});
c.upstream({type:'event',event:'connect.challenge',payload:{nonce:'test'}});
const frame=JSON.parse(readFileSync('./frame.json'));c.client(frame);
if(sent[0]?.params.client.deviceFamily!=='e-ink4-plus'||stopped)throw Error('family rejected');
c.upstream({type:'res',id:frame.id,ok:true,payload:{policy:{maxPayload:660000}}});
c.client({type:'req',id:'call',method:'talk.session.create',params:{mode:'realtime',transport:'gateway-relay',brain:'agent-consult'}});
if(sent.at(-1).params.provider!=='passport-realtime'||stopped)throw Error('call route failed');
c.stop('done');console.log('PASS real OpenClaw proxy: signed-family acceptance and realtime provider routing');'''
    (folder / 'check.mjs').write_text(js)
    # ws resolves from a local test-only symlink, no installation/modification of the user's adapter.
    (folder / 'openclaw/node_modules').symlink_to(source / 'tools/openclaw/node_modules', target_is_directory=True)
    subprocess.run(['node', str(folder / 'check.mjs')], cwd=folder, check=True)
