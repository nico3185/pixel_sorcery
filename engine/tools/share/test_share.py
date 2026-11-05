#!/usr/bin/env python3
"""test_share.py: share_session.py's scrubbing leaves no home folder, user name or gamepad line in a log."""
import getpass, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import share_session as s
home, user = os.path.expanduser('~'), getpass.getuser()
log = (f"genrecomp: ROM {home}/dev/roms/game.bin\nplatform: input: gamepad \"Pro Controller\" guid 0300abcd\n"
       f"menu: saved /Users/{user}/Library/Application Support/PixelSorcery/game/settings.ini\n"
       f"save C:\\Users\\{user}\\AppData\\Roaming\\PixelSorcery\\x\nplain line\n")
out = s.scrub(log)
bad = [w for w in (home, user, 'gamepad', 'guid', 'Pro Controller') if w and w.lower() in out.lower()]
assert not bad, f'left in the shared log: {bad}\n{out}'
assert 'plain line' in out
print('share scrub OK')
