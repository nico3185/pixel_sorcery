// PixelSorcery session API (Cloudflare Worker, 0.6): players share a consented play session without a GitHub account.
//   POST   /sessions          body: the share zip (session.* + share.json). Refused without a published consent
//                             version and text hash, other files, wrong checksums, over 1 MB. -> {id, key}
//   GET    /sessions/ID       Authorization: Bearer KEY -> {status, result}
//   DELETE /sessions/ID       Authorization: Bearer KEY -> withdrawn: the zip deleted, the record marked
// Stores nothing about the sender: no IP address, no account. Rate limit, in order: the RL binding (3 requests a
// minute per address, in Cloudflare's memory only); exponential refusal (one upload per 10 minutes per sender; each
// early or refused attempt doubles the wait, 1 min .. 24 h; the sender is a one-way HMAC of the address under a key
// that changes every day, deleted when its wait ends - a day later after refusals); at most DAILY uploads a day and MAX_BYTES in the bucket.
const CONSENTS = { 2: '__CONSENT_V2_SHA256__' };          // version -> sha256 of engine/tools/share/consent.txt
const ALLOWED = new Set(['session.polls', 'session.meta', 'session.log', 'session.poke', 'share.json']);
const MAX_ZIP = 1 << 20, DAILY = 100, MAX_BYTES = 2 * 2 ** 30, GAP = 600, BASE = 60, CAP = 86400;

const json = (o, s = 200, h = {}) => new Response(JSON.stringify(o), { status: s, headers: { 'content-type': 'application/json', ...h } });
const hex = b => [...new Uint8Array(b)].map(x => x.toString(16).padStart(2, '0')).join('');
const sha256 = async d => hex(await crypto.subtle.digest('SHA-256', typeof d === 'string' ? new TextEncoder().encode(d) : d));
const now = () => Math.floor(Date.now() / 1000);

async function sender(req, env) {   // the day's one-way fingerprint of the address (never stored as such)
  const day = new Date().toISOString().slice(0, 10);
  const k = await crypto.subtle.importKey('raw', new TextEncoder().encode(env.SALT + day), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return hex(await crypto.subtle.sign('HMAC', k, new TextEncoder().encode(req.headers.get('cf-connecting-ip') || '')));
}

async function refuse(env, who, row, why, status = 429) {   // exponential refusal: the wait doubles
  const strikes = (row?.strikes || 0) + 1, wait = Math.min(BASE * 2 ** (strikes - 1), CAP);
  await env.DB.prepare('insert into throttle (who, strikes, until) values (?1, ?2, ?3) on conflict(who) do update set strikes = ?2, until = ?3')
    .bind(who, strikes, Math.max(row?.until || 0, now()) + wait).run();   // only ever adds to the wait
  return json({ error: why, retry_after: wait }, status, { 'retry-after': String(wait) });
}

function unzip(buf) {   // the central directory: name -> {method, csize, offset}
  const v = new DataView(buf), out = {};
  let e = buf.byteLength - 22;
  while (e >= 0 && v.getUint32(e, true) !== 0x06054b50) e--;
  if (e < 0) throw new Error('not a zip');
  let p = v.getUint32(e + 16, true), n = v.getUint16(e + 10, true);
  for (let i = 0; i < n; i++) {
    if (v.getUint32(p, true) !== 0x02014b50) throw new Error('bad directory');
    const nl = v.getUint16(p + 28, true), xl = v.getUint16(p + 30, true), cl = v.getUint16(p + 32, true);
    const name = new TextDecoder().decode(new Uint8Array(buf, p + 46, nl));
    out[name] = { method: v.getUint16(p + 10, true), csize: v.getUint32(p + 20, true), offset: v.getUint32(p + 42, true) };
    p += 46 + nl + xl + cl;
  }
  return out;
}

async function read(buf, f) {
  const v = new DataView(buf), o = f.offset, start = o + 30 + v.getUint16(o + 26, true) + v.getUint16(o + 28, true);
  const raw = new Uint8Array(buf, start, f.csize);
  if (f.method === 0) return raw.slice().buffer;
  if (f.method !== 8) throw new Error('compression');
  return await new Response(new Blob([raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer();
}

async function upload(req, env) {
  const who = await sender(req, env);
  await env.DB.prepare('delete from throttle where (strikes = 0 and until < ?1) or until < ?2').bind(now(), now() - CAP).run();
  const row = await env.DB.prepare('select strikes, until from throttle where who = ?1').bind(who).first();
  if (row && row.until > now()) return refuse(env, who, row, 'too soon');
  const day = await env.DB.prepare("select count(*) n from uploads where recorded_at >= date('now')").first();
  if (day.n >= DAILY) return json({ error: 'the day is full, try tomorrow' }, 503, { 'retry-after': '3600' });
  const total = await env.DB.prepare("select coalesce(sum(size), 0) n from uploads where status != 'withdrawn'").first();
  if (total.n >= MAX_BYTES) return json({ error: 'storage full' }, 503);
  if (Number(req.headers.get('content-length') || MAX_ZIP + 1) > MAX_ZIP) return refuse(env, who, row, 'over 1 MB', 413);
  const buf = await req.arrayBuffer();
  if (buf.byteLength > MAX_ZIP) return refuse(env, who, row, 'over 1 MB', 413);
  let share, files;
  try {
    files = unzip(buf);
    const extra = Object.keys(files).filter(n => !ALLOWED.has(n));
    if (extra.length || !files['share.json'] || !files['session.polls']) throw new Error('files: ' + (extra.join(', ') || 'missing'));
    share = JSON.parse(new TextDecoder().decode(await read(buf, files['share.json'])));
    if (!CONSENTS[share.consent_version] || CONSENTS[share.consent_version] !== share.consent_sha256) throw new Error('consent text unknown');
    for (const [k, h] of Object.entries(share.files || {}))
      if (!files[k] || await sha256(await read(buf, files[k])) !== h) throw new Error('checksum: ' + k);
    if (!/^[a-z0-9_-]{1,40}$/.test(share.title || '')) throw new Error('title');
  } catch (e) { return refuse(env, who, row, 'refused: ' + e.message, 400); }
  const id = crypto.randomUUID(), key = hex(crypto.getRandomValues(new Uint8Array(32)));
  await env.BUCKET.put(`api/${id}.zip`, buf);
  await env.DB.prepare(`insert into uploads (id, key_sha256, title, rom_sha1, consent_version, consent_sha256, consented_at, zip_sha256, files, size, status)
                        values (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 'received')`)
    .bind(id, await sha256(key), share.title, share.rom_sha1 || '', share.consent_version, share.consent_sha256, share.consented || '',
          await sha256(buf), JSON.stringify(share.files || {}), buf.byteLength).run();
  await env.DB.prepare('insert into throttle (who, strikes, until) values (?1, 0, ?2) on conflict(who) do update set strikes = 0, until = ?2')
    .bind(who, now() + GAP).run();
  return json({ id, key, status: 'received' }, 201);
}

async function owned(req, env, id) {
  const key = (req.headers.get('authorization') || '').replace(/^Bearer /, '');
  const r = await env.DB.prepare('select * from uploads where id = ?1').bind(id).first();
  return r && key && r.key_sha256 === await sha256(key) ? r : null;
}

export default {
  async fetch(req, env) {
    const u = new URL(req.url), m = u.pathname.match(/^\/sessions(?:\/([0-9a-f-]{36}))?$/);
    if (!m) return json({ service: 'PixelSorcery sessions', consent_versions: Object.keys(CONSENTS) }, u.pathname === '/' ? 200 : 404);
    const { success } = await env.RL.limit({ key: req.headers.get('cf-connecting-ip') || '' });
    if (!success) return json({ error: 'slow down' }, 429, { 'retry-after': '60' });
    if (!m[1] && req.method === 'POST') return upload(req, env);
    if (!m[1]) return json({ error: 'method' }, 405);
    const r = await owned(req, env, m[1]);
    if (!r) return json({ error: 'not found' }, 404);
    if (req.method === 'GET') return json({ id: r.id, title: r.title, status: r.status, result: r.result || '' });
    if (req.method === 'DELETE') {
      await env.BUCKET.delete(`api/${r.id}.zip`);
      await env.DB.prepare("update uploads set status = 'withdrawn', result = '', updated_at = current_timestamp where id = ?1").bind(r.id).run();
      return json({ id: r.id, status: 'withdrawn' });
    }
    return json({ error: 'method' }, 405);
  }
};
