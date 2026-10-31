#!/usr/bin/env python3
"""build_ui.py [--scan DIR] - the build screen: finds the games in a ROM folder by their content
(cartridge header, then the exact dump's SHA-1 from roms.json, whatever the file names), shows
which titles can be built, and builds the chosen ones with live progress.

  The settings are the first rows (ROM folder, processes = titles built at the same time, cores,
  mode): up/down to one, left/right to change it, each with the value suggested for this Mac.
  keys  up/down move   left/right change a setting   space choose a title   a all   s sh shelf
        d dc shelf   n none   r rescan   Enter start (on the ROM folder row: change it)   q quit

Before building, the ROMs are put where play.sh, launch.sh and the builds expect them (../roms
next to the repository): a link to each verified dump found elsewhere, the lock-on images made by
lockon.py from SHK and the other cartridge, SHK1 by its own builder. Build: the C
regenerated where stale, then ninja; self-check: recomp.sh. Logs: games/TITLE/build_all.log.
Zip archives are looked into too (a dump inside one is extracted into ../roms).
--scan DIR: print what the folder holds and exit (no screen). --prepare DIR: also put the ROMs in
place (as Enter does before building), for build_all.sh --roms DIR."""
import curses, hashlib, json, os, re, signal, subprocess, sys, threading, time, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, 'engine', 'tools')
ROMS = os.path.normpath(os.environ.get('PS_ROMS_DIR') or os.path.join(ROOT, '..', 'roms'))   # PS_ROMS_DIR: tests
PY = os.path.join(ROOT, 'engine', '.venv', 'bin', 'python')
CAT = {k: v for k, v in json.load(open(os.path.join(TOOLS, 'roms.json'))).items() if not k.startswith('_')}
PREFS = os.path.expanduser('~/Library/Application Support/PixelSorcery/build_ui.json')
EXTS = ('.bin', '.md', '.gen', '.68k', '.sgd')


# without Genesis Plus GX's sources (the Mac app's tree) only native ports build: no self-check,
# and no title whose runtime still needs the emulator core (SHCD)
NATIVE_ONLY = not os.path.exists(os.path.join(ROOT, 'engine', 'ext', 'Genesis-Plus-GX', 'core', 'system.c'))


def native_port(folder):
    try: return 'genrecomp_native' in open(os.path.join(ROOT, 'games', folder, 'CMakeLists.txt')).read()
    except OSError: return False


def titles():
    """[(folder, code, name)] in shelf order, from titles.zsh (the names play.sh shows)"""
    out = subprocess.run(['zsh', '-c', f'ROOT={ROOT!r}; source {TOOLS}/titles.zsh; ps_tsv'], capture_output=True, text=True).stdout
    return [tuple(l.split('\t')) for l in out.splitlines() if l.count('\t') == 2
            and (not NATIVE_ONLY or native_port(l.split('\t')[0]))]


def shelf(name):
    try: lines = open(os.path.join(ROOT, 'launchers', name + '.txt')).read().splitlines()
    except OSError: return set()
    out = set()
    for l in lines:
        if l.startswith('game '):
            f = [x.strip() for x in l[5:].split('|')]
            out.add(f[7] if len(f) > 7 and f[7] else f[0])
    return out


# ------------------------------------------------------------------ ROM identification
SIZES = {e['size'] for e in CAT.values() if 'size' in e}   # only dumps of these sizes are read
SERIALS = {e['serial'] for e in CAT.values() if 'serial' in e}
NOSERIAL = {e['size'] for e in CAT.values() if 'size' in e and 'serial' not in e}   # the patch chip
def many(v): return v if isinstance(v, list) else [v]       # a disc title: one serial and hash per region
DISCS = {d for e in CAT.values() if 'disc' in e for d in many(e['disc'])}
def cue_track(cue):
    """a .cue's first track file (the data track)"""
    for l in open(cue, errors='replace'):
        m = re.match(r'\s*FILE\s+"(.+)"', l)
        if m: return os.path.join(os.path.dirname(cue), m.group(1))
    raise OSError('no FILE in ' + cue)
def head(path):
    """the first bytes of a ROM (its header at $180), without reading or decompressing the rest"""
    if '::' in path:
        z, m = path.split('::', 1)
        with zipfile.ZipFile(z) as zf, zf.open(m) as f: return f.read(0x190)
    with open(path, 'rb') as f: return f.read(0x190)
def read_rom(path):
    """the bytes of a ROM: a file, or ARCHIVE.zip::MEMBER"""
    if '::' in path:
        z, m = path.split('::', 1)
        with zipfile.ZipFile(z) as zf: return zf.read(m)
    return open(path, 'rb').read()

_hash_cache = {}
def identify(path):
    """(catalogue key, 'verified' | 'other dump') or None"""
    try:
        if path.lower().endswith('.cue'):                  # a disc: the data track's header and hash
            track = cue_track(path); st = os.stat(track); key = (track, st.st_size, st.st_mtime)
            if key not in _hash_cache:
                with open(track, 'rb') as f: h = f.read(0x200)
                o = 0x10 if h[:12] == b'\x00' + b'\xff' * 10 + b'\x00' else 0      # raw 2352-byte sectors
                ser = h[o + 0x180:o + 0x18E].decode('latin1')
                if ser not in DISCS: return None
                sha = hashlib.sha1()
                with open(track, 'rb') as f:
                    for b in iter(lambda: f.read(1 << 20), b''): sha.update(b)
                _hash_cache[key] = (sha.hexdigest(), ser)
            sha, ser = _hash_cache[key]
            for k, e in CAT.items():
                if ser in many(e.get('disc', [])): return k, ('verified' if sha in many(e['sha1']) else 'other dump')
            return None
        if '::' in path:
            z, m = path.split('::', 1); st = os.stat(z); key = (path, st.st_mtime)
        else:
            st = os.stat(path)
            if st.st_size not in SIZES: return None
            key = (path, st.st_size, st.st_mtime)
        if key not in _hash_cache:
            size = os.stat(path).st_size if '::' not in path else None
            h = head(path)
            if h[0x180:0x18E].decode('latin1') not in SERIALS and not (size in NOSERIAL or '::' in path and
                    any(i.file_size in NOSERIAL for i in zipfile.ZipFile(path.split('::')[0]).infolist() if i.filename == path.split('::', 1)[1])):
                return None                                    # not one of our games: no need to read it all
            d = read_rom(path)
            _hash_cache[key] = (hashlib.sha1(d).hexdigest(), d[0x180:0x18E].decode('latin1'),
                                d[0x200180:0x20018E].decode('latin1') if len(d) > 0x200190 else '')
        sha, ser, ser2 = _hash_cache[key]
    except (OSError, zipfile.BadZipFile, KeyError):
        return None
    for k, e in CAT.items():
        if e.get('sha1') == sha: return k, 'verified'
    for k, e in CAT.items():                                   # the right game, another dump
        if e.get('serial') and e['serial'] == ser and e.get('serial2', '') == (ser2 if e.get('serial2') else ''):
            if 'serial2' in e or not any(c.get('serial2') == ser2 for c in CAT.values() if ser2):
                return k, 'other dump'
    return None


def scan(folder):
    """{catalogue key: (path, status)} - verified dumps first, the canonical ../roms name preferred"""
    found = {}
    folders = [folder] + ([ROMS] if os.path.abspath(folder) != ROMS and os.path.isdir(ROMS) else [])
    for top in folders:
        for dirpath, dirs, files in os.walk(top, followlinks=True):
            if dirpath.count(os.sep) - top.count(os.sep) >= 2: dirs[:] = []
            cands = []
            for f in sorted(files):
                p = os.path.join(dirpath, f)
                if f.lower().endswith(EXTS + ('.cue',)): cands.append(p)
                elif f.lower().endswith('.zip'):                   # the members of a matching size
                    try:
                        with zipfile.ZipFile(p) as zf:
                            cands += [f'{p}::{i.filename}' for i in zf.infolist()
                                      if i.file_size in SIZES and i.filename.lower().endswith(EXTS)]
                    except (OSError, zipfile.BadZipFile): pass
            for p in cands:
                r = identify(p)
                if not r: continue
                k, st = r
                old = found.get(k)
                better = old is None or (st == 'verified' and old[1] != 'verified') or \
                         (st == old[1] and p == os.path.join(ROMS, CAT[k]['file']))
                if better: found[k] = (p, st)
    return found


def rom_plan(found):
    """{catalogue key: (state, detail)}: ready (in ../roms) / link (verified elsewhere) / make
    (lock-on or our builder, from verified parts) / other dump / missing"""
    plan = {}
    def have(k): return k in plan and plan[k][0] in ('ready', 'link', 'make')
    for k, e in CAT.items():                                   # dumps first, made images after
        if k in found and found[k][1] == 'verified':
            p = found[k][0]
            canon = os.path.join(ROMS, e['file'])
            plan[k] = ('ready', e['file']) if os.path.exists(canon) and os.path.realpath(canon) == os.path.realpath(p) \
                else ('link', p)
    for k, e in CAT.items():                                   # ours, no fixed hash: its builder refreshes it
        if e.get('made') and os.path.exists(os.path.join(ROMS, e['file'])): plan[k] = ('ready', e['file'])
    for k, e in CAT.items():
        if k in plan: continue
        parts = e.get('lockon', []) + ([e['patch']] if e.get('patch') else []) + e.get('made', [])
        if parts and all(have(x) for x in parts):
            plan[k] = ('make', ' + '.join(parts))
        elif k in found:
            plan[k] = ('other dump', found[k][0])
        else:
            miss = [x for x in parts if not have(x)]
            plan[k] = ('missing', ('needs ' + ', '.join(miss)) if miss else '')
    return plan


def prepare(plan, log):
    """put every usable ROM in ../roms under its expected name; returns the keys that failed"""
    os.makedirs(ROMS, exist_ok=True)
    bad = []
    for k, (st, d) in plan.items():
        if st == 'link':
            dst = os.path.join(ROMS, CAT[k]['file'])
            if os.path.lexists(dst): os.rename(dst, dst + '.old')
            if CAT[k].get('disc'):                               # a disc: its folder, file by file
                src = os.path.dirname(os.path.abspath(d)); os.makedirs(os.path.dirname(dst), exist_ok=True)
                for f in os.listdir(src):
                    if not os.path.lexists(os.path.join(os.path.dirname(dst), f)):
                        os.symlink(os.path.join(src, f), os.path.join(os.path.dirname(dst), f))
                if not os.path.lexists(dst): os.symlink(os.path.abspath(d), dst)
                log(f'linked {CAT[k]["file"]} -> {src}')
            elif '::' in d:                                      # inside an archive: extracted
                open(dst, 'wb').write(read_rom(d)); log(f'extracted {CAT[k]["file"]} from {d}')
            else:
                os.symlink(d, dst); log(f'linked {CAT[k]["file"]} -> {d}')
    for k, (st, d) in plan.items():                            # lock-on images, then our own
        e = CAT[k]
        if st != 'make' or not e.get('lockon'): continue
        cmd = [PY, os.path.join(TOOLS, 'lockon.py')] + [os.path.join(ROMS, CAT[x]['file']) for x in e['lockon']] + \
              [os.path.join(ROMS, e['file'])] + (['--shk2', os.path.join(ROMS, CAT[e['patch']]['file'])] if e.get('patch') else [])
        r = subprocess.run(cmd, capture_output=True, text=True)
        log(f'made {e["file"]} (lockon.py): {"ok" if r.returncode == 0 else "FAILED " + r.stderr[-200:]}')
        if r.returncode: bad.append(k)
    if plan.get('shk1', ('',))[0] in ('make', 'ready'):
        r = subprocess.run([os.path.join(ROOT, 'games', 'shk1', 'tools', 'ensure_rom.sh'), ROMS], capture_output=True, text=True)
        if r.returncode: bad.append('shk1'); log('SHK1 image FAILED: ' + r.stderr[-200:])
    return bad


# ------------------------------------------------------------------ build jobs
class Job:
    def __init__(self, folder):
        self.t, self.stage, self.n, self.m, self.t0, self.t1, self.result, self.proc = folder, 'waiting', 0, 0, 0.0, 0.0, '', None
        self.log = os.path.join(ROOT, 'games', folder, 'build_all.log')

    def run(self, rom, cores, check, stop):
        self.t0 = time.time()
        g = os.path.join(ROOT, 'games', self.t)
        env = dict(os.environ, PS_JOBS=str(cores), PS_BUILD_PROGRESS='1')
        with open(self.log, 'w') as lf:
            def go(cmd, cwd=None):
                if stop.is_set(): return 1
                self.proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                             text=True, bufsize=1, start_new_session=True)
                for line in self.proc.stdout:
                    lf.write(line); lf.flush(); self.feed(line.rstrip('\n'))
                return self.proc.wait()
            disc_gen = os.path.join(g, 'tools', 'generate.sh')    # a disc title: its own generator
            if check and os.path.exists(disc_gen):
                self.result = 'no self-check (disc)'
            elif check:
                self.stage = 'analysing, generating C'
                rc = go([os.path.join(TOOLS, 'recomp.sh'), rom, g])
                ok = 'SELF-CHECK OK' in open(self.log).read()
                self.result = 'SELF-CHECK OK' if ok else ('stopped' if stop.is_set() else 'FAILED')
            else:
                stale = not os.path.exists(disc_gen) and subprocess.run([PY, '-c', f"import sys; sys.argv=['x']; sys.path.insert(0, {TOOLS!r}); import sidebyside; "
                                        f"sys.exit(0 if sidebyside.generated_stale({g!r}) else 1)"], capture_output=True).returncode == 0
                rc = 0
                if os.path.exists(disc_gen):
                    self.stage = 'generating C'
                    rc = go([disc_gen, rom])
                elif stale or not os.listdir(os.path.join(g, 'src', 'recomp')) if os.path.isdir(os.path.join(g, 'src', 'recomp')) else True:
                    self.stage = 'generating C'
                    rc = go([PY, os.path.join(TOOLS, 'recompiler', 'generate.py'), rom, '-o', os.path.join(g, 'src', 'recomp'),
                             '-c', os.path.join(g, 'recomp.json')])
                if rc == 0:
                    self.stage = 'configuring'
                    rc = go(['cmake', '-S', g, '-B', os.path.join(g, 'build'), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'])
                if rc == 0:
                    self.stage = 'compiling'
                    rc = go(['ninja', '-C', os.path.join(g, 'build'), '-j', str(cores)])
                self.result = 'OK' if rc == 0 else ('stopped' if stop.is_set() else 'FAILED')
        self.stage, self.t1 = 'done', time.time()

    def feed(self, line):
        m = re.match(r'\[(\d+)/(\d+)\]', line)
        if m: self.n, self.m = int(m[1]), int(m[2]); self.stage = 'compiling'; return
        for pat, st in (('Generated ', 'Z80 driver'), ('z80recomp:', 'compiling'), ('self-check over', 'audio checks'),
                        ('audio: native synth', 'replaying against the original'), ('native port:', 'finishing')):
            if line.startswith(pat): self.stage = st; self.n = self.m = 0
        if line.startswith('[') is False and 'cmake' in self.stage: pass

    def kill(self):
        if self.proc and self.proc.poll() is None:
            try: os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError: pass


# ------------------------------------------------------------------ engine for the build window
def built(t):
    """the time of TITLE's native build (sidebyside.json's "exe", else build/TITLE), 0 when none"""
    g = os.path.join(ROOT, 'games', t)
    try: exe = json.load(open(os.path.join(g, 'sidebyside.json'))).get('exe', 'build/' + t)
    except (OSError, ValueError): exe = 'build/' + t
    try: return int(os.path.getmtime(os.path.join(g, exe)))
    except OSError: return 0


def engine():
    """--engine: the build window (ps_launcher --build) draws, this does the work. Commands on
    stdin, one a line: scan FOLDER | start CORES PROCS CHECK TITLE... | play TITLE | stop | quit.
    play: ./play.sh TITLE in its own session (the last build; play.sh builds first if it is missing
    or stale), its output in games/TITLE/play.log. State on stdout four times a second, a line
    each, ended by E:
      S|folder|running|scanning|cpus|native only     T|folder|code|name|rom state|rom detail|stage|done|total|seconds|result|built
      (built: the native build's time, 0 when there is none)
      N|note (the last few)"""
    import queue
    T = titles(); state = {'folder': ROMS, 'plan': {}, 'scanning': False}
    jobs, notes, stop, cmds = {}, [], threading.Event(), queue.Queue()
    def note(m): notes.append(m); del notes[:-4]
    def rescan(folder):
        state['scanning'] = True; state['folder'] = folder
        try: state['plan'] = rom_plan(scan(folder))
        finally: state['scanning'] = False
    def reader():
        for line in sys.stdin: cmds.put(line.strip())
        cmds.put('quit')
    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=rescan, args=(state['folder'],), daemon=True).start()
    def start(cores, par, check, sel):
        plan = state['plan']
        sel = [t for t in sel if plan.get(t, ('missing',))[0] in ('ready', 'link', 'make')]
        if not sel: note('nothing chosen'); return
        bad = prepare(plan, note); sel = [t for t in sel if t not in bad]
        state['plan'] = rom_plan(scan(state['folder']))      # links and made images are now in ../roms
        pp = max(1, min(par, len(sel))); each = max(1, cores // pp); sem = threading.Semaphore(pp)
        stop.clear(); jobs.clear()
        for t in sel: jobs[t] = Job(t)
        def worker(t):
            with sem:
                if stop.is_set(): jobs[t].stage, jobs[t].result = 'done', 'stopped'; return
                jobs[t].run(os.path.join(ROMS, CAT.get(t, {}).get('file', '')), each, check, stop)
        for t in sel: threading.Thread(target=worker, args=(t,), daemon=True).start()
        note(f'{len(sel)} titles, {pp} at once, {each} cores each, {"build + self-check" if check else "build"}')
    while True:
        try:
            c = cmds.get(timeout=0.25).split(' ')
            if c[0] == 'quit':
                stop.set(); [j.kill() for j in jobs.values()]; return
            if c[0] == 'scan' and len(c) > 1 and not any(j.stage != 'done' for j in jobs.values()):
                f = os.path.expanduser(' '.join(c[1:]))
                if os.path.isdir(f): threading.Thread(target=rescan, args=(os.path.abspath(f),), daemon=True).start()
                else: note(f'no folder {f}')
            elif c[0] == 'start' and len(c) >= 4 and not any(j.stage != 'done' for j in jobs.values()):
                threading.Thread(target=start, args=(int(c[1]), int(c[2]), c[3] == '1' and not NATIVE_ONLY, c[4:]), daemon=True).start()
            elif c[0] == 'play' and len(c) == 2 and c[1] in [t for t, _, _ in T]:
                log = open(os.path.join(ROOT, 'games', c[1], 'play.log'), 'w')
                subprocess.Popen([os.path.join(ROOT, 'play.sh'), c[1]], cwd=ROOT, stdin=subprocess.DEVNULL,
                                 stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                note(f'playing {c[1]}')
            elif c[0] == 'stop':
                stop.set(); [j.kill() for j in jobs.values()]; note('stopped')
        except queue.Empty:
            pass
        running = any(j.stage != 'done' for j in jobs.values())
        out = [f'S|{state["folder"]}|{int(running)}|{int(state["scanning"])}|{os.cpu_count() or 4}|{int(NATIVE_ONLY)}']
        for t, code, name in T:
            st, d = state['plan'].get(t, ('missing', ''))
            j = jobs.get(t)
            el = int(((j.t1 or time.time()) - j.t0) if j and j.t0 else 0)
            out.append('|'.join(['T', t, code, name, st, os.path.basename(str(d).split('::')[-1]),
                                 (j.stage if j else ''), str(j.n if j else 0), str(j.m if j else 0), str(el), (j.result if j else ''),
                                 str(built(t))]))
        out += [f'N|{n}' for n in notes] + ['E']
        try: sys.stdout.write('\n'.join(x.replace('\n', ' ') for x in out) + '\n'); sys.stdout.flush()
        except BrokenPipeError: stop.set(); [j.kill() for j in jobs.values()]; return


# ------------------------------------------------------------------ the screen
def main(scr):
    curses.curs_set(0); scr.nodelay(True); curses.use_default_colors()
    for i, c in enumerate((curses.COLOR_GREEN, curses.COLOR_YELLOW, curses.COLOR_RED, curses.COLOR_CYAN, curses.COLOR_BLUE), 1):
        curses.init_pair(i, c, -1)
    GREEN, YELLOW, RED, CYAN, BLUE = (curses.color_pair(i) for i in range(1, 6))
    ncpu = os.cpu_count() or 4
    try: pref = json.load(open(PREFS))
    except (OSError, ValueError): pref = {}
    folder = pref.get('folder', ROMS); cores = min(pref.get('cores', ncpu), ncpu); par = pref.get('par', 2); check = pref.get('check', False)
    T = titles(); sh, dc = shelf('sh'), shelf('dc')
    found, plan = {}, {}
    def rescan():
        nonlocal found, plan
        scr.erase(); scr.addstr(0, 0, f'scanning {folder} ...'); scr.refresh()
        found = scan(folder); plan = rom_plan(found)
    rescan()
    def usable(t): return plan.get(t, ('missing',))[0] in ('ready', 'link', 'make')
    chosen = {t for t, _, _ in T if usable(t)}
    NS = 5                                                     # 4 settings rows, ALL, then the titles
    cur, jobs, notes, running, stop = NS - 1, {}, [], False, threading.Event()
    def suggest():   # processes: build = 2 (one generating while one compiles); self-check = mostly one
        return 2 if not check else max(1, cores // 2)          # core per title: half the cores, 2 each
    def note(s): notes.append(s); del notes[:-3]

    def prompt(label, value):
        curses.echo(); curses.curs_set(1); scr.nodelay(False)
        h, w = scr.getmaxyx(); scr.move(h - 1, 0); scr.clrtoeol(); scr.addstr(h - 1, 0, f'{label} [{value}]: ')
        s = scr.getstr(h - 1, len(label) + len(value) + 5, 300).decode().strip()
        curses.noecho(); curses.curs_set(0); scr.nodelay(True)
        return os.path.expanduser(s) if s else value

    def start():
        nonlocal running
        sel = [t for t, _, _ in T if t in chosen and usable(t)]
        if not sel: note('nothing chosen'); return
        bad = prepare(plan, note)
        sel = [t for t in sel if t not in bad]
        each = max(1, cores // max(1, min(par, len(sel))))
        sem = threading.Semaphore(max(1, min(par, len(sel))))
        stop.clear(); running = True
        for t in sel: jobs[t] = Job(t)
        def worker(t):
            with sem:
                if stop.is_set(): jobs[t].stage, jobs[t].result = 'done', 'stopped'; return
                e = CAT.get(t, {})
                jobs[t].run(os.path.join(ROMS, e.get('file', '')), each, check, stop)
        for t in sel: threading.Thread(target=worker, args=(t,), daemon=True).start()
        note(f'{len(sel)} titles, {min(par, len(sel))} at once, {each} cores each, {"build + self-check" if check else "build"}')

    while True:
        h, w = scr.getmaxyx(); scr.erase()
        running = any(j.stage != 'done' for j in jobs.values())
        def put(y, x, s, a=0):
            if 0 <= y < h and x < w: scr.addstr(y, x, s[:max(0, w - x - 1)], a)
        put(0, 0, 'PixelSorcery - build the native ports', curses.A_BOLD)
        nsel = len([t for t, _, _ in T if t in chosen and usable(t)])
        pp = max(1, min(par, nsel or 1)); each = max(1, cores // pp)
        sug = suggest()
        rows = [('ROM folder', folder, 'Enter: change'),
                ('Processes', f'< {par} >', f'titles built at the same time (suggested {sug} for {"self-check" if check else "build"})'),
                ('Cores', f'< {cores} >', f'of {ncpu} on this machine -> {each} per process'),
                ('Mode', f'< {"BUILD + SELF-CHECK" if check else "BUILD"} >',
                 'self-check: replays each port against the original (slower)' if check else 'regenerate the C where stale, compile')]
        for i, (lab, val, hint) in enumerate(rows):
            a = curses.A_REVERSE if cur == i else 0
            put(1 + i, 1, f'{lab:11}', a | curses.A_BOLD); put(1 + i, 13, f'{val} ', (CYAN if i == 0 else BLUE) | a)
            put(1 + i, 13 + max(24, len(val) + 2), hint, 0)
        put(5, 0, '-' * (w - 1))
        avail = [t for t, _, _ in T if usable(t)]
        alls = all(t in chosen for t in avail) and avail
        put(6, 0, f' {"[x]" if alls else "[ ]"} ALL found in the ROM folder ({len(avail)} of {len(T)} titles, {nsel} chosen) ',
            curses.A_BOLD | (curses.A_REVERSE if cur == NS - 1 else 0))
        for i, (t, code, name) in enumerate(T):
            y = 7 + i
            st, d = plan.get(t, ('missing', ''))
            mark = '[x]' if t in chosen else '[ ]' if usable(t) else ' - '
            a = curses.A_REVERSE if i + NS == cur else 0
            put(y, 0, f' {mark} {code:7} {name[:30]:30} ', a)
            rom = {'ready': ('ROM ok', GREEN), 'link': ('ROM found', GREEN), 'make': ('ROM made from ' + ' + '.join(
                dict((x[0], x[1]) for x in T).get(p, p) for p in d.split(' + ')), GREEN),
                   'other dump': ('other dump (not verified)', YELLOW), 'missing': ('no ROM ' + d.replace('needs', '- needs'), RED)}[st]
            put(y, 45, f'{rom[0][:26]:26}', rom[1])
            j = jobs.get(t)
            if j:
                el = (j.t1 or time.time()) - j.t0 if j.t0 else 0
                if j.stage == 'done':
                    col = GREEN if j.result in ('OK', 'SELF-CHECK OK') else RED
                    put(y, 72, f'{j.result:24} {int(el) // 60:2d}:{int(el) % 60:02d}', col)
                elif j.stage == 'waiting':
                    put(y, 72, 'waiting', 0)
                else:
                    if j.m:
                        bw = 16; f = j.n * bw // j.m
                        s = f'{"#" * f}{"." * (bw - f)} {j.n * 100 // j.m:3d}%'
                    else:
                        s = j.stage[:21] + ' ' + '|/-\\'[int(time.time() * 4) % 4]
                    put(y, 72, f'{s:24} {int(el) // 60:2d}:{int(el) % 60:02d}', YELLOW)
        y = 8 + len(T)
        put(y, 0, '-' * (w - 1)); y += 1
        ti = max(0, cur - NS); t = T[ti][0]; st, d = plan.get(t, ('missing', ''))
        put(y, 0, {'ready': f'{T[ti][2]}: {os.path.join(ROMS, CAT.get(t, {}).get("file", ""))}', 'link': f'{T[ti][2]}: found {d}',
                   'make': f'{T[ti][2]}: will be made from {d}', 'other dump': f'{T[ti][2]}: {d} is this game but not the dump the port was verified with',
                   'missing': f'{T[ti][2]}: no dump of it in {folder} {d}'}[st], CYAN); y += 1
        j = jobs.get(t)
        if j and os.path.exists(j.log):
            try: tail = open(j.log, errors='replace').read().splitlines()[-max(1, h - y - 5):]
            except OSError: tail = []
            for l in tail: put(y, 2, l); y += 1
        for n in notes: put(h - 3 + notes.index(n) - len(notes) + 2, 0, n, BLUE)
        put(h - 1, 0, 'up/down move  left/right change setting  space choose  a all  s sh shelf  d dc shelf  n none  r rescan  Enter START  q quit')
        scr.refresh()
        k = scr.getch()
        if k == -1: time.sleep(0.15); continue
        if k == 27:                                            # an arrow not decoded by curses: ESC [ A or ESC O A
            a, b = scr.getch(), scr.getch()
            if a in (ord('['), ord('O')):
                k = {ord('A'): curses.KEY_UP, ord('B'): curses.KEY_DOWN, ord('C'): curses.KEY_RIGHT, ord('D'): curses.KEY_LEFT}.get(b, -1)
        if k == ord('q'):                                      # (not Esc: arrow keys start with it)
            if running:
                stop.set(); [j.kill() for j in jobs.values()]
                note('stopping...'); time.sleep(0.5)
            break
        if k in (curses.KEY_UP, ord('k')): cur = (cur - 1) % (NS + len(T))
        elif k in (curses.KEY_DOWN, ord('j')): cur = (cur + 1) % (NS + len(T))
        elif running: continue                                 # settings fixed while building
        elif k in (curses.KEY_LEFT, curses.KEY_RIGHT, ord('h'), ord('l')) and 1 <= cur < NS:
            d = 1 if k in (curses.KEY_RIGHT, ord('l')) else -1
            if cur == 1: par = min(ncpu, max(1, par + d))
            elif cur == 2: cores = min(ncpu, max(1, cores + d))
            else: check = not check; par = suggest()
        elif k in (10, 13, curses.KEY_ENTER, ord('f')) and (cur == 0 or k == ord('f')):
            nf = prompt('ROM folder', folder)
            if os.path.isdir(nf): folder = os.path.abspath(nf); rescan(); chosen = {t for t, _, _ in T if usable(t)}
            else: note(f'no folder {nf}')
        elif k == ord(' ') and cur == NS - 1:                  # ALL: everything found, or nothing
            avail = {t for t, _, _ in T if usable(t)}
            chosen = set() if avail <= chosen else avail
        elif k == ord(' ') and cur >= NS:
            t = T[cur - NS][0]
            if usable(t): chosen ^= {t}
        elif k == ord('a'): chosen = {t for t, _, _ in T if usable(t)}
        elif k == ord('n'): chosen = set()
        elif k == ord('s'): chosen = {t for t, _, _ in T if usable(t) and t in sh}
        elif k == ord('d'): chosen = {t for t, _, _ in T if usable(t) and t in dc}
        elif k in (ord('+'), ord('=')): cores = min(ncpu, cores + 1)
        elif k == ord('-'): cores = max(1, cores - 1)
        elif k == ord('m'): check = not check; par = suggest()
        elif k == ord('r'): rescan(); chosen = {t for t, _, _ in T if usable(t)}
        elif k in (10, 13, curses.KEY_ENTER): jobs.clear(); start()
        try:
            os.makedirs(os.path.dirname(PREFS), exist_ok=True)
            json.dump({'folder': folder, 'cores': cores, 'par': par, 'check': check}, open(PREFS, 'w'))
        except OSError: pass


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] in ('--scan', '--prepare'):
        found = scan(os.path.abspath(os.path.expanduser(sys.argv[2]))); plan = rom_plan(found)
        for t, code, name in titles():
            st, d = plan.get(t, ('missing', ''))
            print(f'  {code:7} {name[:32]:32} {st:11} {d}')
        if sys.argv[1] == '--prepare':
            bad = prepare(plan, lambda m: print('  ' + m))
            sys.exit(1 if bad else 0)
        sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] == '--engine':
        engine(); sys.exit(0)
    if not sys.stdout.isatty():
        sys.exit('build_ui.py needs a terminal (engine/tools/build_all.sh works without one)')
    curses.wrapper(main)
