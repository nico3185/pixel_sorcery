#!/usr/bin/env python3
"""share_session.py TITLE [SESSION.polls] [--agree] [--note TEXT] [--out DIR]: pack a play session to share
(0.5, owner: only with explicit, recorded consent). Without --agree it prints the consent text and stops.
With it: a zip of session.{polls,meta,log[,poke]} + share.json, stripped of anything personal (home folder
and user name in paths, the gamepad's name and identifier, the play time in file names), the consent recorded
locally (consents.json beside the zip folder), and a prefilled issue URL printed (TITLE=... URL=... ZIP=...).
SESSION defaults to the title's latest session (games/TITLE/sessions)."""
import datetime, getpass, glob, hashlib, json, os, re, sys, urllib.parse, zipfile

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
CONSENT = os.path.join(HERE, 'consent.txt'); CONSENT_VERSION = 2
ENDPOINT = os.path.join(HERE, 'endpoint.txt')   # the project's session API (worker/), written by worker/deploy.py
REPO = 'nico3185/pixel_sorcery'

def scrub(text):
    """the log, without the home folder, the user name or the gamepad's lines"""
    home = os.path.expanduser('~'); user = getpass.getuser()
    out = []
    for line in text.splitlines():
        if re.search(r'gamepad|guid|input: ', line, re.I): continue
        line = line.replace(home, '~')
        line = re.sub(r'/(Users|home)/[^/\s]+', '/~', line)
        line = re.sub(r'([A-Za-z]:\\\\Users\\\\)[^\\\\\s]+', r'\1~', line)
        if user and len(user) > 2: line = re.sub(re.escape(user), '~', line, flags=re.I)
        out.append(line)
    return '\n'.join(out) + '\n'

def rom_sha1(title):
    cfg = os.path.join(ROOT, 'games', title, 'sidebyside.json')
    try: rom = json.load(open(cfg)).get('rom', '')
    except OSError: return ''
    for base in (os.path.join(ROOT, 'roms'), os.path.join(ROOT, '..', 'roms')):
        p = os.path.join(base, rom)
        if os.path.isfile(p):
            if p.endswith('.cue'): return ''               # a disc: its data track is hashed by the build screen
            return hashlib.sha1(open(p, 'rb').read()).hexdigest()
    return ''

def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    title = a.pop(0); agree = '--agree' in a; note = ''; out = os.path.join(ROOT, 'build', 'share')
    if '--note' in a: note = a[a.index('--note') + 1]
    if '--out' in a: out = a[a.index('--out') + 1]
    rec = os.path.join(out, 'consents.json')              # the player's own record of every consent given
    if '--withdraw' in a or '--status' in a: return manage(title, rec, '--withdraw' in a)
    sess = next((x for x in a if x.endswith('.polls')), None)
    if not sess:
        c = sorted(glob.glob(os.path.join(ROOT, 'games', title, 'sessions', '*.polls')), key=os.path.getmtime)
        c = [x for x in c if os.path.getsize(x) > 0]
        if not c: sys.exit('no session for ' + title)
        sess = c[-1]
    if not agree:
        sys.stdout.write(open(CONSENT).read()); sys.exit(2)
    base = sess[:-len('.polls')]
    files = {'session.polls': open(sess, 'rb').read()}
    if os.path.exists(base + '.meta'):
        files['session.meta'] = ''.join(l for l in open(base + '.meta') if l.split()[:1] and l.split()[0] in ('version', 'title', 'logic')).encode()
    if os.path.exists(base + '.log'): files['session.log'] = scrub(open(base + '.log', errors='replace').read()).encode()
    if os.path.exists(base + '.poke'): files['session.poke'] = open(base + '.poke', 'rb').read()
    when = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    consent_sha = hashlib.sha256(open(CONSENT, 'rb').read().replace(b'\r\n', b'\n')).hexdigest()   # a Windows clone: CRLF
    share = {'title': title, 'rom_sha1': rom_sha1(title), 'consent_version': CONSENT_VERSION, 'consent_sha256': consent_sha,
             'consented': when, 'note': note, 'files': {k: hashlib.sha256(v).hexdigest() for k, v in files.items()}}
    files['share.json'] = json.dumps(share, indent=1).encode()
    os.makedirs(out, exist_ok=True)
    n = len(glob.glob(os.path.join(out, title + '-*.zip'))) + 1
    zp = os.path.join(out, f'{title}-{n}.zip')
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
        for k, v in files.items():
            zi = zipfile.ZipInfo(k, date_time=(2000, 1, 1, 0, 0, 0)); zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, v)                              # no play time in the zip's dates either
    log = json.load(open(rec)) if os.path.exists(rec) else []
    entry = dict(share, zip=os.path.basename(zp), zip_sha256=hashlib.sha256(open(zp, 'rb').read()).hexdigest())
    if '--send' in a:                                     # straight to the project's server: no GitHub account needed
        try:
            r = api('POST', '/sessions', open(zp, 'rb').read())
            entry.update(id=r['id'], key=r['key']); print(f'TITLE={title}\nZIP={zp}\nID={r["id"]}\nSTATUS={r["status"]}')
        except Exception as e: print(f'ERROR={e}')
    log.append(entry); json.dump(log, open(rec, 'w'), indent=1)
    if 'id' in entry: return
    # the issue form (.github/ISSUE_TEMPLATE/session.yml): the game and note filled in; its consent box cannot be
    # ticked from a link, so the player ticks it on the issue - the explicit consent the fetch tool requires
    url = f'https://github.com/{REPO}/issues/new?' + urllib.parse.urlencode(
        {'template': 'session.yml', 'title': f'Session: {title}', 'game': title, 'note': note})
    print(f'TITLE={title}\nZIP={zp}\nURL={url}')

def api(method, path, body=None, key=''):
    import urllib.error, urllib.request
    url = open(ENDPOINT).read().strip() + path
    h = {'content-type': 'application/zip', 'user-agent': 'PixelSorcery'} | ({'authorization': 'Bearer ' + key} if key else {})
    try:
        with urllib.request.urlopen(urllib.request.Request(url, data=body, method=method, headers=h), timeout=60) as r: return json.loads(r.read())
    except urllib.error.HTTPError as e:
        d = json.loads(e.read() or b'{}'); raise RuntimeError(d.get('error', e.code) + (f' (retry in {d["retry_after"]} s)' if 'retry_after' in d else ''))

def manage(title, rec, withdraw):   # the last share of TITLE sent to the server: its state, or withdraw it
    log = json.load(open(rec)) if os.path.exists(rec) else []
    e = next((x for x in reversed(log) if x['title'] == title and x.get('id') and x.get('status') != 'withdrawn'), None)
    if not e: sys.exit('no share of ' + title + ' to ' + ('withdraw' if withdraw else 'look up'))
    r = api('DELETE' if withdraw else 'GET', '/sessions/' + e['id'], key=e['key'])
    if withdraw: e['status'] = 'withdrawn'; json.dump(log, open(rec, 'w'), indent=1)
    print(f'TITLE={title}\nID={e["id"]}\nSTATUS={r["status"]}' + (f'\nRESULT={r["result"]}' if r.get('result') else ''))

if __name__ == '__main__':
    main()
