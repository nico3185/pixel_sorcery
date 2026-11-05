"""cloud.py - the shared sessions' records on Cloudflare (owner, 0.5): the consent table mirrored in D1 (SQLite,
the same table as shared/consents.db) and each kept session's zip in R2 (sessions/ISSUE.zip, deleted on withdrawal).
Off unless configured: CF_ACCOUNT_ID, CF_D1_DATABASE (the D1 database id), CF_R2_BUCKET and the API token in
CF_API_TOKEN or the login keychain (security add-generic-password -s pixelsorcery-cloudflare -a cf -w TOKEN), a token
limited to that D1 database and R2 bucket. The local database stays the first record; a Cloudflare error is reported,
never fatal."""
import json, os, subprocess, sys, urllib.request

API = 'https://api.cloudflare.com/client/v4/accounts/'

def _token():
    t = os.environ.get('CF_API_TOKEN', '')
    if not t and sys.platform == 'darwin':
        r = subprocess.run(['security', 'find-generic-password', '-s', 'pixelsorcery-cloudflare', '-w'], capture_output=True, text=True)
        t = r.stdout.strip() if r.returncode == 0 else ''
    return t

ACCOUNT, D1, BUCKET = (os.environ.get(k, '') for k in ('CF_ACCOUNT_ID', 'CF_D1_DATABASE', 'CF_R2_BUCKET'))
TOKEN = _token() if ACCOUNT else ''
ON = bool(ACCOUNT and D1 and BUCKET and TOKEN)

def _call(method, path, data=None, ctype='application/json'):
    req = urllib.request.Request(API + ACCOUNT + path, data=data, method=method,
                                 headers={'Authorization': 'Bearer ' + TOKEN, 'Content-Type': ctype})
    with urllib.request.urlopen(req, timeout=60) as r: return r.read()

def _safe(what, f, *a):
    if not ON: return None
    try: return f(*a)
    except Exception as e: print(f'cloudflare {what}: {e}', file=sys.stderr)

def sql(query, params=()):
    """one statement on D1 (the same SQL as the local SQLite database)"""
    return _safe('d1', lambda: json.loads(_call('POST', f'/d1/database/{D1}/query',
                                                json.dumps({'sql': query, 'params': list(params)}).encode())))

def put(key, data):    return _safe('r2 put', _call, 'PUT', f'/r2/buckets/{BUCKET}/objects/{key}', data, 'application/zip')
def delete(key):       return _safe('r2 delete', _call, 'DELETE', f'/r2/buckets/{BUCKET}/objects/{key}')
