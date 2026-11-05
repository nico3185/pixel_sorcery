#!/usr/bin/env python3
"""sessions_fetch.py [--replay] [--comment]: the shared sessions (0.5), kept only with a recorded, explicit consent.
Reads the public repository's issues labelled "session" (gh), and for each:
  - consent: the issue form's box ticked by its author, and the zip's share.json naming a consent text this
    project published (consent.txt, by version and SHA-256); otherwise refused, nothing downloaded;
  - the zip (the issue's attachment) checked: only session.{polls,meta,log,poke} + share.json, every file's
    SHA-256 as share.json lists it;
  - withdrawn (the issue closed, or its author commenting "withdraw"): the session deleted, the record marked;
  - every decision recorded in shared/consents.db (SQLite: issue, account, consent version and hash, dates, zip
    and files' hashes, status kept / refused / withdrawn), the kept sessions in shared/TITLE/ISSUE/.
--replay: each kept session replayed against the original (engine/tools/check.sh TITLE session.polls);
--comment: the result posted on its issue (gh). shared/ stays out of git: players' data, not project files."""
import hashlib, io, json, os, re, shutil, sqlite3, subprocess, sys, urllib.request, zipfile
import cloud      # the Cloudflare mirror (D1 + R2), off unless configured

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
REPO = 'nico3185/pixel_sorcery'; SHARED = os.path.join(ROOT, 'shared')
CONSENTS = {1: hashlib.sha256(open(os.path.join(HERE, 'consent.txt'), 'rb').read()).hexdigest()}   # version -> text hash
ALLOWED = {'session.polls', 'session.meta', 'session.log', 'session.poke', 'share.json'}

def db():
    os.makedirs(SHARED, exist_ok=True)
    c = sqlite3.connect(os.path.join(SHARED, 'consents.db'))
    c.execute(TABLE); cloud.sql(TABLE)
    return c

TABLE = '''create table if not exists consents (issue integer primary key, account text, title text,
             consent_version integer, consent_sha256 text, consented_at text, recorded_at text default current_timestamp,
             zip_sha256 text, files text, status text, reason text, updated_at text default current_timestamp)'''

def record(c, issue, account, status, reason='', share=None, zip_sha=''):
    s = share or {}
    q = ('''insert into consents (issue, account, title, consent_version, consent_sha256, consented_at, zip_sha256, files, status, reason)
                 values (?,?,?,?,?,?,?,?,?,?) on conflict(issue) do update set status=excluded.status, reason=excluded.reason,
                 updated_at=current_timestamp, zip_sha256=coalesce(nullif(excluded.zip_sha256,''), zip_sha256)''',
              (issue, account, s.get('title', ''), s.get('consent_version'), s.get('consent_sha256', ''), s.get('consented', ''),
               zip_sha, json.dumps(s.get('files', {})), status, reason))
    c.execute(*q); c.commit(); cloud.sql(*q)

def status(c, issue):
    r = c.execute('select status, reason from consents where issue = ?', (issue,)).fetchone()
    return r or ('', '')

def gh(*a): return subprocess.run(['gh', *a], capture_output=True, text=True, check=True).stdout

def main():
    replay, comment = '--replay' in sys.argv, '--comment' in sys.argv
    issues = json.loads(gh('issue', 'list', '-R', REPO, '--label', 'session', '--state', 'all', '--limit', '500',
                           '--json', 'number,author,state,body,comments,title'))
    c = db()
    for it in issues:
        n, who = it['number'], it['author']['login']; folder = os.path.join(SHARED, str(n))
        withdrawn = it['state'] == 'CLOSED' or any(cm['author']['login'] == who and re.search(r'\bwithdraw', cm['body'], re.I)
                                                   for cm in it.get('comments', []))
        if withdrawn:
            shutil.rmtree(folder, ignore_errors=True); cloud.delete(f'sessions/{n}.zip')
            record(c, n, who, 'withdrawn', 'closed or withdraw comment'); continue
        if not re.search(r'- \[[xX]\] I agree to share this session', it['body']):
            record(c, n, who, 'refused', 'consent box not ticked'); continue
        m = re.search(r'https://github\.com/[^\s)]+\.zip', it['body'])
        if not m: record(c, n, who, 'refused', 'no zip attached'); continue
        data = urllib.request.urlopen(m.group(0), timeout=60).read()
        try:
            z = zipfile.ZipFile(io.BytesIO(data)); names = set(z.namelist())
            share = json.loads(z.read('share.json'))
        except Exception as e:
            record(c, n, who, 'refused', f'not a session zip ({e})'); continue
        why = ('other files: ' + ', '.join(sorted(names - ALLOWED)) if names - ALLOWED else
               'consent text unknown' if CONSENTS.get(share.get('consent_version')) != share.get('consent_sha256') else
               'file checksums differ' if any(hashlib.sha256(z.read(k)).hexdigest() != v for k, v in share.get('files', {}).items()) else '')
        if why: record(c, n, who, 'refused', why, share); continue
        os.makedirs(folder, exist_ok=True); z.extractall(folder)
        before = status(c, n)
        if before[0] != 'kept': cloud.put(f'sessions/{n}.zip', data)
        record(c, n, who, 'kept', before[1] if before[0] == 'kept' else '', share, hashlib.sha256(data).hexdigest())
        if replay:
            r = subprocess.run([os.path.join(ROOT, 'engine', 'tools', 'check.sh'), share['title'], os.path.join(folder, 'session.polls')],
                               capture_output=True, text=True)
            res = (r.stdout.strip().splitlines() or ['(no output)'])[-1]
            print(f'#{n} {share["title"]}: {res}')
            if res != before[1]: record(c, n, who, 'kept', res, share)      # the last replay result, in reason
            if comment and res != before[1]: gh('issue', 'comment', str(n), '-R', REPO, '--body', f'Replayed against the original: {res}')
    for row in c.execute('select status, count(*) from consents group by status'): print(*row)

if __name__ == '__main__':
    main()
