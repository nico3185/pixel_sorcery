#!/usr/bin/env python3
"""deploy.py - publish the session API (worker.js) on the project's Cloudflare account, through cloud.py's
configuration (ids in ~/.config/pixelsorcery/cloudflare.env, the token in the keychain; the token needs D1, R2 and
Workers Scripts Edit). Creates the D1 tables, uploads the Worker with its bindings (DB, BUCKET, RL: 3 requests a
minute per address, SALT: a fresh random secret for the senders' daily fingerprints), turns its workers.dev address
on and writes it to ../endpoint.txt (the app sends there)."""
import hashlib, json, os, secrets, sys, uuid
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.dirname(HERE))
import cloud

NAME = 'pixelsorcery-sessions'
TABLES = ['''create table if not exists uploads (id text primary key, key_sha256 text, title text, rom_sha1 text,
             consent_version integer, consent_sha256 text, consented_at text, recorded_at text default current_timestamp,
             zip_sha256 text, files text, size integer, status text, result text, updated_at text default current_timestamp)''',
          'create table if not exists throttle (who text primary key, strikes integer, until integer)']

def main():
    if not cloud.ON: sys.exit('cloud.py is not configured')
    for t in TABLES: assert cloud.sql(t)['success']
    consent = hashlib.sha256(open(os.path.join(os.path.dirname(HERE), 'consent.txt'), 'rb').read()).hexdigest()
    js = open(os.path.join(HERE, 'worker.js')).read().replace('__CONSENT_V2_SHA256__', consent)
    meta = {'main_module': 'worker.js', 'compatibility_date': '2025-09-01', 'bindings': [
        {'type': 'd1', 'name': 'DB', 'id': cloud.D1},
        {'type': 'r2_bucket', 'name': 'BUCKET', 'bucket_name': cloud.BUCKET},
        {'type': 'ratelimit', 'name': 'RL', 'namespace_id': '1001', 'simple': {'limit': 3, 'period': 60}},
        {'type': 'secret_text', 'name': 'SALT', 'text': secrets.token_hex(32)}]}
    b = uuid.uuid4().hex
    body = (f'--{b}\r\nContent-Disposition: form-data; name="metadata"\r\nContent-Type: application/json\r\n\r\n{json.dumps(meta)}\r\n'
            f'--{b}\r\nContent-Disposition: form-data; name="worker.js"; filename="worker.js"\r\nContent-Type: application/javascript+module\r\n\r\n{js}\r\n--{b}--\r\n').encode()
    r = json.loads(cloud._call('PUT', f'/workers/scripts/{NAME}', body, f'multipart/form-data; boundary={b}'))
    print('worker:', r['success'])
    cloud._call('POST', f'/workers/scripts/{NAME}/subdomain', json.dumps({'enabled': True, 'previews_enabled': False}).encode())
    sub = json.loads(cloud._call('GET', '/workers/subdomain'))['result']['subdomain']
    url = f'https://{NAME}.{sub}.workers.dev'
    open(os.path.join(os.path.dirname(HERE), 'endpoint.txt'), 'w').write(url + '\n')
    print('endpoint:', url)

if __name__ == '__main__': main()
