#!/usr/bin/env python3
"""ps_gym.py - the original game (GPGX, through libps_env) as a Gymnasium environment, so
off-the-shelf agents (Stable-Baselines3, MIT) can look for new code. Reward, in order:
reaching a new level (the main goal), moving forward in the level, code the original runs for
the first time (the novelty bonus, tracer build), minus a lost life. Every episode's inputs are
kept; the furthest becomes a route (one "P1 P2" hex line per frame, GENRECOMP_MOVIE format)
that check.sh turns into a recording replayed on both the original and the port.

  ps_gym.py train TITLE [--steps N] [--out DIR]      PPO (+ RND curiosity), routes in DIR
  ps_gym.py smoke TITLE                              random actions: speed and reward check
Per title: GAMES (RAM of the level, x position, lives; the boot to the first level)."""
import ctypes, json, os, sys, time
import numpy as np
import gymnasium as gym
from gymnasium import spaces

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
UP, DOWN, LEFT, RIGHT, B, C, A, START = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
ACTIONS = [0, RIGHT, LEFT, RIGHT | C, LEFT | C, C, DOWN, DOWN | C, UP]   # C jumps; down + C: spin dash

# level: RAM word (zone/act), x: player's x (word), lives: byte; mode: the game mode byte and its
# value in a level (the boot presses Start every 90 frames until mode is that and level is first)
GAMES = {
    'shk2':   dict(level=0xFE10, x=0xB008, lives=0xFE12, mode=0xF600, in_level=0x0C, first=0x0000),
    'sh2': dict(level=0xFE10, x=0xB008, lives=0xFE12, mode=0xF600, in_level=0x0C, first=0x0000),
    'sh1': dict(level=0xFE10, x=0xD008, lives=0xFE12, mode=0xF600, in_level=0x0C, first=0x0000),
}


class Lib:
    def __init__(self, title):
        self.dll = ctypes.CDLL(os.path.join(ROOT, 'games', title, 'build-trace', 'genrecomp_build', 'libps_env.dylib'))
        d = self.dll
        d.ps_env_init.argtypes = [ctypes.c_char_p]; d.ps_env_step.argtypes = [ctypes.c_uint, ctypes.c_int]
        d.ps_env_code_seen.restype = ctypes.c_long
        rom = json.load(open(os.path.join(ROOT, 'games', title, 'sidebyside.json')))['rom']
        if not d.ps_env_init(os.path.join(ROOT, '..', 'roms', rom).encode()): sys.exit(f'{title}: cannot load {rom}')
        self.n = d.ps_env_state_size()
        self.ram_buf = (ctypes.c_ubyte * 0x10000)(); self.scr = (ctypes.c_uint * (320 * 240))()

    def step(self, pad, render=False): self.dll.ps_env_step(pad, int(render))
    def ram(self): self.dll.ps_env_ram(self.ram_buf); return np.frombuffer(self.ram_buf, dtype=np.uint8)
    def save(self): b = (ctypes.c_ubyte * self.n)(); self.dll.ps_env_save(b); return bytes(b)
    def load(self, st): self.dll.ps_env_load((ctypes.c_ubyte * self.n).from_buffer_copy(st))
    def code(self): return self.dll.ps_env_code_seen()
    def screen84(self):
        self.dll.ps_env_screen(self.scr)
        a = np.frombuffer(self.scr, dtype=np.uint32).reshape(240, 320)
        g = ((a >> 16 & 255) * 77 + (a >> 8 & 255) * 150 + (a & 255) * 29) >> 8          # grey
        return g[8:232:224 // 84, 0:320:320 // 84][:84, :84].astype(np.uint8)


class PsEnv(gym.Env):
    metadata = {'render_modes': []}

    def __init__(self, title, frameskip=4, max_steps=4500, novelty=0.5, out=None):
        self.cfg = GAMES[title]; self.lib = Lib(title); self.fs = frameskip; self.max_steps = max_steps
        self.novelty = novelty; self.out = out
        self.action_space = spaces.Discrete(len(ACTIONS))
        self.observation_space = spaces.Box(0, 255, (84, 84, 1), np.uint8)
        self.boot = []                          # power-on to the first act: Start every 90 frames
        for f in range(6000):
            pad = START if f % 90 < 4 and f > 120 else 0
            self.lib.step(pad); self.boot.append(pad)
            r = self.lib.ram()
            if r[self.cfg['mode']] == self.cfg['in_level'] and self._w(r, self.cfg['level']) == self.cfg['first'] and f % 90 > 60:
                break
        else:
            sys.exit('ps_gym: the boot never reached the first act')
        for _ in range(120): self.lib.step(0); self.boot.append(0)   # the act's title card
        self.boot_frames = len(self.boot)
        self.start = self.lib.save()
        self.best = (-1, -1); self.levels = set()

    def _w(self, r, a): return int(r[a & 0xFFFF]) << 8 | int(r[(a + 1) & 0xFFFF])

    def reset(self, seed=None, options=None):
        super().reset(seed=seed)
        self.lib.load(self.start)
        r = self.lib.ram()
        self.level0 = self.level = self._w(r, self.cfg['level']); self.xmax = self._w(r, self.cfg['x'])
        self.lives = int(r[self.cfg['lives']]); self.code0 = self.lib.code(); self.t = 0; self.inputs = []
        self.lib.step(0, True)
        return self.lib.screen84()[..., None], {}

    def step(self, action):
        pad = ACTIONS[int(action)]
        for i in range(self.fs):
            self.lib.step(pad, i == self.fs - 1); self.inputs.append(pad)
        r = self.lib.ram(); self.t += 1
        level, x, lives = self._w(r, self.cfg['level']), self._w(r, self.cfg['x']), int(r[self.cfg['lives']])
        code = self.lib.code()
        reward = (code - self.code0) * self.novelty; self.code0 = code          # new code
        done = False
        if level != self.level:                                                # the main goal
            if level not in self.levels: reward += 1000.0; self.levels.add(level)
            self.level, self.xmax = level, x
        elif x > self.xmax:
            reward += (x - self.xmax) * 0.1; self.xmax = x                    # forward
        if lives < self.lives: reward -= 100.0; done = True
        trunc = self.t >= self.max_steps
        if done or trunc: self._keep()
        return self.lib.screen84()[..., None], reward, done, trunc, {'level': level, 'x': x, 'code': code}

    def _keep(self):
        """the furthest episode so far (levels reached, then x) becomes the route"""
        mark = (len(self.levels), self.xmax)
        if self.out and mark > self.best:
            self.best = mark
            os.makedirs(self.out, exist_ok=True)
            with open(os.path.join(self.out, 'route.txt'), 'w') as f:
                f.write(f'# ps_gym route: levels {len(self.levels)}, x {self.xmax}, after a {self.boot_frames}-frame boot\n')
                f.writelines(f'{p:04X} 0000\n' for p in self.boot)
                f.writelines(f'{p:04X} 0000\n' for p in self.inputs)


def rnd_reward(env, steps):
    """RND curiosity (Burda et al. 2018): a fixed random network and a predictor learning it;
    the prediction error on a screen is its novelty. Returns a wrapper adding it to the reward."""
    import torch, torch.nn as nn
    def net(): return nn.Sequential(nn.Flatten(), nn.Linear(84 * 84, 256), nn.ReLU(), nn.Linear(256, 64))
    target, pred = net(), net()
    for p in target.parameters(): p.requires_grad_(False)
    opt = torch.optim.Adam(pred.parameters(), lr=1e-4)

    class RND(gym.Wrapper):
        def step(self, a):
            o, rw, d, t, info = self.env.step(a)
            x = torch.tensor(o, dtype=torch.float32).reshape(1, -1) / 255.0
            loss = ((pred(x) - target(x)) ** 2).mean()
            opt.zero_grad(); loss.backward(); opt.step()
            return o, rw + float(loss) * 10.0, d, t, info
    return RND(env)


def main():
    if len(sys.argv) < 3 or sys.argv[2] not in GAMES: sys.exit(__doc__)
    cmd, title = sys.argv[1], sys.argv[2]
    a = sys.argv[3:]
    steps = int(a[a.index('--steps') + 1]) if '--steps' in a else 200_000
    out = a[a.index('--out') + 1] if '--out' in a else os.path.join(ROOT, 'games', title, 'explore', 'gym')
    if cmd == 'smoke':
        env = PsEnv(title, out=out); env.reset(); t0 = time.time(); tot = 0.0; info = {}
        for i in range(2000):
            _, rw, d, tr, info = env.step(env.action_space.sample()); tot += rw
            if d or tr: env.reset()
        dt = time.time() - t0
        print(f'{title}: 2000 steps ({2000 * env.fs} frames) in {dt:.1f} s = {2000 * env.fs / dt:.0f} frames/s; '
              f'reward {tot:.0f}; code seen {info.get("code")}; level {info.get("level", 0):04X} x {info.get("x")}')
        return
    from stable_baselines3 import PPO
    from stable_baselines3.common.vec_env import DummyVecEnv, VecFrameStack
    env = VecFrameStack(DummyVecEnv([lambda: rnd_reward(PsEnv(title, out=out), steps)]), 4)
    model = PPO('CnnPolicy', env, n_steps=512, batch_size=128, learning_rate=2.5e-4, ent_coef=0.02, verbose=1)
    model.learn(total_timesteps=steps)
    model.save(os.path.join(out, 'ppo'))
    print(f'routes in {out}')


if __name__ == '__main__':
    main()
