"""inputs_hash(CONFIG): sha1 of what generation reads besides the ROM - the title's config and the recompiler's
Python sources. generate.py writes it to OUTDIR/.inputs.sha1; sidebyside.generated_stale compares it."""
import hashlib, os


def inputs_hash(config):
    h = hashlib.sha1(open(config, 'rb').read() if config and os.path.exists(config) else b'')
    here = os.path.dirname(os.path.abspath(__file__))
    for f in sorted(os.listdir(here)):
        if f.endswith('.py'):
            h.update(f.encode()); h.update(open(os.path.join(here, f), 'rb').read())
    return h.hexdigest()
