#!/usr/bin/env python3
"""pixelsorcery_gui.py - PixelSorcery's window (Windows, macOS, Linux; Tk), the Mac app's flow and look
(PixelSorceryBuild.swift): install, then choose and build, then play.
  Your games       a card per built game (name, CODE, Play); Controller... (CONTROLLER SETUP alone),
                   Build games..., Open the shelf (every built game on the cartridge shelf)
  Build games...   the ROM folder, the titles found by content, tick and build (what build_all.sh
                   does, + optional self-check against the original), Stop; '‹ Your games' back
It opens on Your games once a game is built and returns there after a build.
A client of build_ui.py --engine: state lines S|folder|running|scanning|cpus|native only,
T|folder|code|name|rom state|rom detail|stage|done|total|seconds|result|built, N|note, E;
commands scan FOLDER | start CORES PROCS CHECK TITLE... | play TITLE | pad TITLE | stop | quit.
--selftest FOLDER: the engine's view of FOLDER printed, no window."""
import json, os, queue, subprocess, sys, threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOLS))
PREFS = os.path.join(os.path.expanduser('~'), '.pixelsorcery_gui.json')
READY = ('ready', 'link', 'make')
NOWIN = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
# the Mac app's look: system font, light-grey cards, a prominent blue Play
UI = 'Segoe UI' if os.name == 'nt' else ('Helvetica Neue' if sys.platform == 'darwin' else 'Helvetica')
TITLE, HEAD, BOLD, BODY, CAPTION = (UI, 17, 'bold'), (UI, 12, 'bold'), (UI, 10, 'bold'), (UI, 10), (UI, 9)
BG, CARD, ACCENT, ACCENT_DOWN, GREY = '#f5f5f5', '#e8e8e8', '#0a64d8', '#0850ad', '#777777'


def shell(script, *args):
    """a repository script: through zsh on Windows (MSYS2), directly elsewhere"""
    path = os.path.join(ROOT, *script.split('/'))
    if os.name == 'nt':                                      # zsh's ${0:A} mangles C:/...: /c/...
        path = path.replace(os.sep, '/')
        if path[1:2] == ':': path = '/' + path[0].lower() + path[2:]
        return ['zsh', path] + list(args)
    return [path] + list(args)


def collections():
    """[(file stem, the shelf's name)] for launchers/*.txt, as the Mac app's Collections row"""
    out, d = [], os.path.join(ROOT, 'launchers')
    for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if not f.endswith('.txt'): continue
        name = f[:-4]
        try:
            for line in open(os.path.join(d, f), encoding='utf-8'):
                if line.startswith('name '): name = line[5:].strip(); break
        except OSError: pass
        out.append((f[:-4], name))
    return out


class App:
    def __init__(self, root):
        self.root, self.rows, self.q, self.running = root, {}, queue.Queue(), False
        self.page, self.cards, self.tiles = None, (), []
        try: self.pref = json.load(open(PREFS))
        except (OSError, ValueError): self.pref = {}
        self.sel = set(self.pref.get('selected', []))
        root.title('PixelSorcery'); root.geometry('980x740'); root.minsize(900, 680); root.configure(bg=BG)
        st = ttk.Style()
        for w in ('TFrame', 'TLabel', 'TCheckbutton', 'TRadiobutton', 'TLabelframe', 'TLabelframe.Label'): st.configure(w, background=BG)
        st.configure('Bold.TCheckbutton', font=BOLD)
        st.configure('TLabel', font=BODY); st.configure('Treeview', rowheight=24, font=BODY)
        st.configure('Treeview.Heading', font=BOLD)
        self.body = ttk.Frame(root, padding=16); self.body.pack(fill='both', expand=True)
        self.status = tk.StringVar(value='Starting the engine…')
        ttk.Label(root, textvariable=self.status, padding=(16, 0, 16, 12), foreground=GREY, font=CAPTION).pack(fill='x')
        self.make_games_page(); self.make_build_page()
        py = sys.executable.replace('pythonw', 'python') if os.name == 'nt' else sys.executable
        self.eng = subprocess.Popen([py, os.path.join(TOOLS, 'build_ui.py'), '--engine'], cwd=ROOT, text=True,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    bufsize=1, creationflags=NOWIN)
        threading.Thread(target=self.reader, daemon=True).start()
        if self.folder.get(): self.send('scan ' + self.folder.get())
        root.protocol('WM_DELETE_WINDOW', self.quit); root.after(250, self.poll)

    # ---------------------------------------------------------------- the two screens
    def make_games_page(self):
        p = self.games = ttk.Frame(self.body)
        head = ttk.Frame(p); head.pack(fill='x', pady=(0, 14))
        ttk.Label(head, text='Your games', font=TITLE).pack(side='left')
        ttk.Button(head, text='Build games…', command=lambda: self.go('build')).pack(side='right')
        ttk.Button(head, text='Controller…', command=lambda: self.cards and self.pad(self.cards[0])).pack(side='right', padx=6)
        coll = ttk.Frame(p); coll.pack(fill='x', pady=(0, 12))           # Collections: one shelf per launchers/NAME.txt
        ttk.Label(coll, text='Collections', font=BOLD).pack(side='left', padx=(0, 8))
        for stem, label in collections():
            ttk.Button(coll, text=label, command=lambda n=stem: self.shelf(n)).pack(side='left', padx=(0, 6))
        self.grid = tk.Frame(p, bg=BG); self.grid.pack(fill='both', expand=True)
        self.grid.bind('<Configure>', lambda e: self.layout_cards())

    def make_build_page(self):
        p = self.build = ttk.Frame(self.body)
        self.back = ttk.Button(p, text='‹ Your games', command=lambda: self.go('games'))
        self.top = top = ttk.Frame(p); top.pack(fill='x')
        ttk.Label(top, text='ROM folder', font=BOLD, width=11).pack(side='left')
        self.folder = tk.StringVar(value=self.pref.get('folder', ''))
        ttk.Entry(top, textvariable=self.folder, font=BODY).pack(side='left', fill='x', expand=True, padx=6)
        ttk.Button(top, text='Choose…', command=self.choose).pack(side='left')
        ttk.Button(top, text='Rescan', command=self.rescan).pack(side='left', padx=(6, 0))
        # settings, as the Mac app: processes (titles at the same time), cores, Build / Build + self-check
        self.cpus, self.native_only = os.cpu_count() or 4, False
        self.procs = tk.IntVar(value=self.pref.get('processes', 2)); self.cores = tk.IntVar(value=self.pref.get('cores', 0) or self.cpus)
        self.check = tk.BooleanVar(value=self.pref.get('check', False))
        sets = self.sets = ttk.Frame(p); sets.pack(fill='x', pady=(10, 0))
        ttk.Label(sets, text='Processes').pack(side='left')
        self.s_procs = ttk.Spinbox(sets, from_=1, to=self.cpus, width=3, textvariable=self.procs, command=self.hint)
        self.s_procs.pack(side='left', padx=(4, 24))
        ttk.Label(sets, text='Cores').pack(side='left')
        self.s_cores = ttk.Spinbox(sets, from_=1, to=self.cpus, width=3, textvariable=self.cores, command=self.hint)
        self.s_cores.pack(side='left', padx=4)
        self.l_of = ttk.Label(sets, text=f'of {self.cpus}'); self.l_of.pack(side='left', padx=(0, 24))
        self.modes = ttk.Frame(sets); self.modes.pack(side='left')
        for text, v in (('Build', False), ('Build + self-check', True)):
            ttk.Radiobutton(self.modes, text=text, value=v, variable=self.check, command=self.mode).pack(side='left', padx=(0, 10))
        self.l_hint = ttk.Label(p, font=CAPTION, foreground=GREY); self.l_hint.pack(anchor='w', pady=(2, 8))
        self.all = tk.BooleanVar()
        self.c_all = ttk.Checkbutton(p, variable=self.all, command=self.select_all, style='Bold.TCheckbutton')
        self.c_all.pack(anchor='w')
        cols = ('pick', 'code', 'game', 'rom', 'progress', 'time')
        self.tree = ttk.Treeview(p, columns=cols, show='headings', selectmode='browse', height=9)
        for c, w, t in (('pick', 34, ''), ('code', 70, 'Code'), ('game', 250, 'Game'), ('rom', 190, 'ROM'),
                        ('progress', 220, 'Progress'), ('time', 60, 'Time')):
            self.tree.heading(c, text=t); self.tree.column(c, width=w, anchor='w', stretch=c in ('game', 'progress'))
        for tag, colour in (('warn', '#b35900'), ('bad', GREY)): self.tree.tag_configure(tag, foreground=colour)
        self.tree.pack(fill='both', expand=True, pady=(4, 6))
        self.tree.bind('<Button-1>', self.click); self.tree.bind('<<TreeviewSelect>>', lambda e: self.picked())
        # the selected title: Play / Build & play, Rebuild, and its log
        self.logbox = ttk.LabelFrame(p, text='Log (select a title)', padding=6); self.logbox.pack(fill='x')
        row = ttk.Frame(self.logbox); row.pack(fill='x', pady=(0, 4))
        self.b_play = ttk.Button(row, text='Play', command=lambda: self.chosen_row() and self.play(self.chosen_row()))
        self.b_rebuild = ttk.Button(row, text='Rebuild', command=self.rebuild)
        self.log = tk.Text(self.logbox, height=6, font=('Consolas', 9) if os.name == 'nt' else ('Menlo', 10),
                           relief='flat', wrap='none', state='disabled', bg='white')
        self.log.pack(fill='x')
        bar = ttk.Frame(p); bar.pack(fill='x', pady=(8, 0))
        self.b_build = tk.Button(bar, text='Start', font=BOLD, bg=ACCENT, fg='white', activebackground=ACCENT_DOWN,
                                 activeforeground='white', relief='flat', padx=18, pady=4, cursor='hand2', command=self.start_stop)
        self.b_build.pack(side='right')
        self.root.bind('<Return>', lambda e: self.page == 'build' and not self.running and self.start())
        self.root.bind('<Escape>', lambda e: self.running and self.send('stop'))
        self.hint()

    def go(self, page):
        if page == self.page: return
        for f in (self.games, self.build): f.pack_forget()
        (self.games if page == 'games' else self.build).pack(fill='both', expand=True)
        self.page = page; self.update_back()

    def update_back(self):   # '‹ Your games' on the build screen once a game is built, not while building
        if self.cards and not self.running: self.back.pack(anchor='w', pady=(0, 10), before=self.top)
        else: self.back.pack_forget()

    def show_cards(self):
        built = [(t, r) for t, r in self.rows.items() if r['built']]
        key = tuple(t for t, _ in built)
        if key == self.cards and self.grid.winfo_children(): return
        self.cards = key
        for w in self.grid.winfo_children(): w.destroy()
        self.tiles = []
        if not built:
            tk.Label(self.grid, text='No game built yet.\nOpen "Build games…", point it at the folder with your game '
                     'dumps, tick the games and build them.', justify='center', bg=BG, fg=GREY, font=BODY, pady=40).pack()
            return
        for t, r in built:
            c = tk.Frame(self.grid, bg=CARD, padx=12, pady=12)
            tk.Label(c, text=r['name'], font=HEAD, bg=CARD, anchor='w', justify='left', wraplength=190).pack(fill='x')
            tk.Label(c, text=r['code'].upper(), font=CAPTION, fg=GREY, bg=CARD, anchor='w').pack(fill='x', pady=(2, 12))
            tk.Button(c, text='Play', font=BOLD, bg=ACCENT, fg='white', activebackground=ACCENT_DOWN, activeforeground='white',
                      relief='flat', padx=20, pady=5, cursor='hand2', command=lambda t=t: self.play(t)).pack(anchor='w')
            tk.Label(c, text='Share last session…', font=CAPTION, fg=ACCENT, bg=CARD, cursor='hand2').pack(anchor='w', pady=(8, 0))
            c.winfo_children()[-1].bind('<Button-1>', lambda e, t=t: self.share(t))
            self.tiles.append(c)
        self.update_back(); self.layout_cards()

    def layout_cards(self):   # the Mac app's LazyVGrid(.adaptive(minimum: 210)): as many columns as fit
        if not self.tiles: return
        n = max(1, (self.grid.winfo_width() + 12) // 222)
        for i, c in enumerate(self.tiles): c.grid(row=i // n, column=i % n, padx=(0, 12), pady=(0, 12), sticky='nsew')
        for col in range(10): self.grid.columnconfigure(col, weight=1 if col < n else 0, uniform='c' if col < n else '')

    # ---------------------------------------------------------------- engine I/O
    def send(self, cmd):
        try: self.eng.stdin.write(cmd + '\n'); self.eng.stdin.flush()
        except OSError: self.status.set('The engine stopped: restart PixelSorcery')

    def reader(self):
        frame = []
        for line in self.eng.stdout:
            line = line.rstrip('\n')
            if line == 'E': self.q.put(frame); frame = []
            else: frame.append(line)

    def poll(self):
        last = None
        try:
            while True: last = self.q.get_nowait()
        except queue.Empty: pass
        if last is not None: self.show(last)
        self.root.after(250, self.poll)

    def show(self, frame):
        notes, was, scanning = [], self.running, False
        for line in frame:
            f = line.split('|')
            if f[0] == 'S':
                self.running, scanning = f[2] == '1', f[3] == '1'
                if not self.folder.get(): self.folder.set(f[1])
                if f[4].isdigit() and int(f[4]) != self.cpus:
                    self.cpus = int(f[4]); self.s_procs.configure(to=self.cpus); self.s_cores.configure(to=self.cpus)
                    self.l_of.configure(text=f'of {self.cpus}')
                self.native_only = f[5] == '1'
                if self.native_only: self.check.set(False); self.modes.pack_forget()   # no emulator core: no self-check
            elif f[0] == 'T' and len(f) >= 12:
                t, code, name, rst, rdet, stage, n, m, sec, res, built = f[1:12]
                self.rows[t] = dict(code=code, name=name, rom=rst, detail=rdet, built=built != '0')
                rom, tag = {'ready': ('● ROM ok', 'ok'), 'link': ('● found: ' + rdet, 'ok'), 'make': ('● made from ' + rdet, 'ok'),
                            'other dump': ('● other dump (not verified)', 'warn')}.get(rst, ('○ no ROM', 'bad'))
                if stage == 'done' or (res and not stage): prog = ('✓ ' if res in ('OK', 'SELF-CHECK OK') else '✗ ') + res
                elif stage == 'waiting': prog = 'waiting'
                elif stage and m not in ('', '0'): prog = f'{stage} {int(n) * 100 // max(1, int(m))}%'
                else: prog = stage
                s = int(sec) if sec.isdigit() else 0
                vals = ('☑' if t in self.sel else '☐' if rst in READY else '', code, name, rom, prog,
                        f'{s // 60}:{s % 60:02d}' if s else '')
                if self.tree.exists(t): self.tree.item(t, values=vals, tags=(tag,))
                else: self.tree.insert('', 'end', iid=t, values=vals, tags=(tag,))
            elif f[0] == 'N': notes.append(f[1])
        avail = {t for t, r in self.rows.items() if r['rom'] in READY}
        if avail and (not self.pref.get('selected_init') or not self.sel <= avail):   # after a scan: everything found
            self.sel = set(avail); self.pref['selected_init'] = True; self.save(); self.repick()
        ready, built = len(avail), sum(1 for r in self.rows.values() if r['built'])
        head = 'Building…' if self.running else ('scanning…' if scanning else
            f'{built} game{"" if built == 1 else "s"} built, {ready} found in your folder')
        self.status.set(head + ('   —   ' + notes[-1] if notes else ''))
        self.b_build.configure(text='Stop' if self.running else 'Start')
        for w in (self.s_procs, self.s_cores): w.state(['disabled'] if self.running else ['!disabled'])
        self.c_all.configure(text=f'All found in the ROM folder ({ready} of {len(self.rows)} titles, {len(self.sel)} chosen)')
        self.all.set(bool(avail) and avail <= self.sel)
        self.hint(); self.picked(); self.show_cards(); self.update_back()
        if self.page is None and self.rows: self.go('games' if built else 'build')
        elif was and not self.running and built: self.go('games')        # a build ended: your games

    # ---------------------------------------------------------------- actions
    def click(self, e):
        t = self.tree.identify_row(e.y)
        if t and self.tree.identify_column(e.x) == '#1' and self.rows.get(t, {}).get('rom') in READY:
            self.sel ^= {t}; self.save(); self.repick()

    def select_all(self):
        self.sel = {t for t, r in self.rows.items() if r['rom'] in READY} if self.all.get() else set()
        self.save(); self.repick()

    def repick(self):   # the tick column after the choice changed
        for t, r in self.rows.items():
            if self.tree.exists(t): self.tree.set(t, 'pick', '☑' if t in self.sel else '☐' if r['rom'] in READY else '')

    def num(self, var, default):
        try: return max(1, min(self.cpus, int(var.get())))
        except (tk.TclError, ValueError): return default

    def hint(self):   # the Mac app's caption: cores per process and the suggested processes
        cores, procs = self.num(self.cores, self.cpus), self.num(self.procs, 2)
        per = max(1, cores // max(1, min(procs, max(1, len(self.sel)))))
        what = 'self-check (mostly one core per title)' if self.check.get() else 'a build (one generating while one compiles)'
        self.l_hint.configure(text=f'{per} cores per process · suggested {self.suggested()} processes for {what}')
        self.save()

    def suggested(self): return max(1, self.num(self.cores, self.cpus) // 2) if self.check.get() else 2

    def mode(self): self.procs.set(self.suggested()); self.hint()

    def chosen_row(self):
        s = self.tree.selection(); return s[0] if s else None

    def picked(self):   # the selected title: its Play / Build & play, Rebuild and log (the last 40 lines)
        t = self.chosen_row(); r = self.rows.get(t)
        self.logbox.configure(text=r['name'] if r else 'Log (select a title)')
        for b in (self.b_play, self.b_rebuild): b.pack_forget()
        text = ''
        if r:
            self.b_play.configure(text='Play' if r['built'] else 'Build & play',
                                  state='normal' if r['rom'] in READY and not self.running else 'disabled')
            self.b_play.pack(side='left')
            if r['built']: self.b_rebuild.configure(state='disabled' if self.running else 'normal'); self.b_rebuild.pack(side='left', padx=6)
            try:
                with open(os.path.join(ROOT, 'games', t, 'build_all.log'), 'rb') as f:
                    f.seek(0, 2); f.seek(max(0, f.tell() - 6000))
                    text = '\n'.join(f.read().decode('utf-8', 'replace').splitlines()[-40:])
            except OSError: text = '(no log yet)'
        if text != self.log.get('1.0', 'end-1c'):
            self.log.configure(state='normal'); self.log.delete('1.0', 'end'); self.log.insert('1.0', text)
            self.log.see('end'); self.log.configure(state='disabled')

    def rebuild(self):
        t = self.chosen_row()
        if t: self.send(f'start {self.num(self.cores, self.cpus)} 1 {int(self.check.get())} {t}')

    def start_stop(self): self.send('stop') if self.running else self.start()

    def choose(self):
        d = filedialog.askdirectory(title='The folder with your game dumps', initialdir=self.folder.get() or ROOT)
        if d: self.folder.set(d); self.save(); self.rescan()

    def rescan(self):
        if self.folder.get(): self.send('scan ' + self.folder.get())

    def start(self):
        sel = [t for t in self.rows if t in self.sel and self.rows[t]['rom'] in READY]
        if not sel: messagebox.showinfo('PixelSorcery', 'Tick the games to build first.'); return
        self.save()
        self.send(f'start {self.num(self.cores, self.cpus)} {self.num(self.procs, 2)} {int(self.check.get())} ' + ' '.join(sel))

    def play(self, t):
        self.send('play ' + t); self.status.set(f'Starting {self.rows[t]["name"]}…')

    def pad(self, t):
        self.send('pad ' + t); self.status.set(f'{self.rows[t]["name"]}: controller setup (close it when done)')

    def shelf(self, name):   # the engine builds the shelf the first time, then opens it
        self.send('shelf ' + name); self.status.set(f'Opening the {name} shelf…')

    def share(self, t):   # explicit, recorded consent first: only "I agree" sends the command
        w = tk.Toplevel(self.root); w.title('Share your last session'); w.configure(bg=BG); w.transient(self.root); w.grab_set()
        try: text = open(os.path.join(TOOLS, 'share', 'consent.txt'), encoding='utf-8').read()
        except OSError: text = '(consent.txt is missing: nothing can be shared)'
        ttk.Label(w, text=f'Share your last {self.rows[t]["name"]} session', font=HEAD, padding=(16, 16, 16, 6)).pack(anchor='w')
        box = tk.Text(w, wrap='word', width=78, height=20, font=BODY, relief='flat', padx=10, pady=10)
        box.insert('1.0', text); box.configure(state='disabled'); box.pack(fill='both', expand=True, padx=16)
        bar = ttk.Frame(w, padding=16); bar.pack(fill='x')
        def agree(): w.destroy(); self.send('share ' + t); self.status.set('Packing the session (your browser opens the form)…')
        tk.Button(bar, text='I agree - pack the session', font=BOLD, bg=ACCENT, fg='white', activebackground=ACCENT_DOWN,
                  activeforeground='white', relief='flat', padx=14, pady=4, command=agree,
                  state='normal' if 'missing' not in text[:40] else 'disabled').pack(side='right')
        ttk.Button(bar, text='Cancel', command=w.destroy).pack(side='right', padx=8)

    def save(self):
        self.pref.update(folder=self.folder.get(), selected=sorted(self.sel), check=self.check.get(),
                         processes=self.num(self.procs, 2), cores=self.num(self.cores, self.cpus))
        try: json.dump(self.pref, open(PREFS, 'w'))
        except OSError: pass

    def quit(self):
        self.save(); self.send('quit'); self.root.after(300, self.root.destroy)


if __name__ == '__main__':
    root = tk.Tk()
    try: ttk.Style().theme_use('vista' if os.name == 'nt' else ('aqua' if sys.platform == 'darwin' else 'clam'))
    except tk.TclError: pass
    if '--selftest' in sys.argv:
        root.withdraw(); app = App(root); app.folder.set(sys.argv[-1]); app.rescan()
        def done():
            for t, r in app.rows.items(): print(f"{t:24} {r['rom']:8} built={int(r['built'])}  {r['name']}")
            print('page:', app.page, '| cards:', ' '.join(app.cards)); print(app.status.get())
            app.send('quit'); root.destroy()
        root.after(12000, done); root.mainloop(); sys.exit(0)
    App(root); root.mainloop()
