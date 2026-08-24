// Submission endpoint for the shared chart-calibration database.
//
// The plugin ships no write credential — a token baked into a public binary is
// a public password.  Instead it POSTs calibrations here, and this worker (the
// only place the repository token lives) commits them to charts/cal/<ICAO>.json.
//
// Deploy:  wrangler secret put GITHUB_TOKEN   (fine-grained, Contents: write)
//          wrangler deploy
// Then publish the resulting URL in charts/cal/config.json as "submit_url".

const REPO         = "PLAYRUrk/CoPilots";
const BRANCH       = "main";
const DIR          = "charts/cal";
const MAX_BODY     = 16 * 1024;   // bytes
const MAX_ENTRIES  = 20;
const THROTTLE_SEC = 10;          // per client address

const ICAO_RE  = /^[A-Z0-9]{3,4}$/;
const CHART_RE = /^[A-Za-z0-9_-]{1,64}$/;

export default {
  async fetch(request, env, ctx) {
    if (request.method === "OPTIONS") return reply(null, 204);
    if (request.method !== "POST")    return reply({ error: "POST only" }, 405);
    if (!env.GITHUB_TOKEN)            return reply({ error: "endpoint not configured" }, 503);

    const raw = await request.text();
    if (raw.length > MAX_BODY) return reply({ error: "body too large" }, 413);

    let payload;
    try { payload = JSON.parse(raw); }
    catch { return reply({ error: "malformed JSON" }, 400); }
    if (payload?.v !== 1 || !Array.isArray(payload.entries))
      return reply({ error: "unsupported payload" }, 400);
    if (payload.entries.length === 0 || payload.entries.length > MAX_ENTRIES)
      return reply({ error: `1..${MAX_ENTRIES} entries per request` }, 400);

    // One submission per client every few seconds.  Cheap, needs no binding:
    // the edge cache remembers the address for exactly as long as we want.
    const ip = request.headers.get("CF-Connecting-IP") || "unknown";
    const throttleKey = new Request("https://throttle.copilots.invalid/" + encodeURIComponent(ip));
    const cache = caches.default;
    if (await cache.match(throttleKey)) return reply({ error: "slow down" }, 429);
    ctx.waitUntil(cache.put(throttleKey,
      new Response("1", { headers: { "Cache-Control": `max-age=${THROTTLE_SEC}` } })));

    // Group the valid entries by airport; the file layout is one per airport.
    const byIcao = new Map();
    let rejected = 0;
    for (const e of payload.entries) {
      const parsed = validate(e);
      if (!parsed) { rejected++; continue; }
      const [icao, key, entry] = parsed;
      if (!byIcao.has(icao)) byIcao.set(icao, {});
      byIcao.get(icao)[key] = entry;
    }
    if (byIcao.size === 0) return reply({ error: "no usable entries", rejected }, 400);

    const written = {};
    for (const [icao, entries] of byIcao) {
      try {
        written[icao] = await commitAirport(env.GITHUB_TOKEN, icao, entries);
      } catch (err) {
        return reply({ error: String(err), written, rejected }, 502);
      }
    }
    return reply({ ok: true, written, rejected }, 200);
  }
};

// Returns [icao, "<chartId>:<page>", entry] or null when anything is off.
// Rejecting a bad entry must never take the whole request down with it: a
// partially usable batch is still worth storing.
function validate(e) {
  if (!e || typeof e !== "object") return null;
  const icao = String(e.icao || "").toUpperCase();
  if (!ICAO_RE.test(icao)) return null;
  const chartId = String(e.chartId || "");
  if (!CHART_RE.test(chartId)) return null;

  const page = Number(e.page);
  if (!Number.isInteger(page) || page < 1 || page > 64) return null;

  const k = Number(e.k), tx = Number(e.tx), ty = Number(e.ty), angle = Number(e.angle);
  if (![k, tx, ty, angle].every(Number.isFinite)) return null;
  // k is metres of Web Mercator per unit of page height, tx/ty a point in
  // Web Mercator: outside these bounds the numbers cannot describe a chart.
  if (!(k > 0) || k > 1e9) return null;
  if (Math.abs(tx) > 2.1e7 || Math.abs(ty) > 2.1e7) return null;
  if (Math.abs(angle) > 2 * Math.PI + 0.01) return null;

  let mtime = Number(e.mtime);
  if (!Number.isFinite(mtime) || mtime <= 0) mtime = Math.floor(Date.now() / 1000);

  return [icao, `${chartId}:${page}`, {
    tx, ty, k, angle, page,
    icao,
    override: e.override === true,
    mtime: Math.floor(mtime)
  }];
}

async function commitAirport(token, icao, entries) {
  const path = `${DIR}/${icao}.json`;
  // One retry: another submission may land between our read and our write.
  for (let attempt = 0; attempt < 2; attempt++) {
    const current = await ghGet(token, path);
    const merged = { ...(current.data || {}) };
    let changed = 0;
    for (const [key, entry] of Object.entries(entries)) {
      const old = merged[key];
      if (old && Number(old.mtime || 0) > entry.mtime) continue;  // ours is older
      merged[key] = entry;
      changed++;
    }
    if (changed === 0) return 0;

    const sorted = {};
    for (const key of Object.keys(merged).sort()) sorted[key] = merged[key];

    const res = await ghPut(token, path, JSON.stringify(sorted, null, 2) + "\n",
                            current.sha,
                            `charts: calibration for ${icao} (${changed} entr${changed === 1 ? "y" : "ies"})`);
    if (res.ok) return changed;
    if (res.status !== 409 && res.status !== 422) throw new Error(`GitHub PUT ${res.status}`);
  }
  throw new Error("conflict, try again");
}

async function ghGet(token, path) {
  const r = await fetch(`https://api.github.com/repos/${REPO}/contents/${path}?ref=${BRANCH}`,
                        { headers: ghHeaders(token) });
  if (r.status === 404) return { data: null, sha: undefined };
  if (!r.ok) throw new Error(`GitHub GET ${r.status}`);
  const j = await r.json();
  let data = null;
  try { data = JSON.parse(b64decode(j.content.replace(/\n/g, ""))); }
  catch { data = null; }          // unreadable file: replaced, not compounded
  return { data, sha: j.sha };
}

function ghPut(token, path, text, sha, message) {
  return fetch(`https://api.github.com/repos/${REPO}/contents/${path}`, {
    method: "PUT",
    headers: { ...ghHeaders(token), "Content-Type": "application/json" },
    body: JSON.stringify({ message, content: b64encode(text), branch: BRANCH, sha })
  });
}

function ghHeaders(token) {
  return {
    "Authorization": `Bearer ${token}`,
    "Accept": "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
    "User-Agent": "copilots-cal-worker"
  };
}

function b64encode(text) {
  const bytes = new TextEncoder().encode(text);
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin);
}

function b64decode(b64) {
  const bin = atob(b64);
  const bytes = Uint8Array.from(bin, c => c.charCodeAt(0));
  return new TextDecoder().decode(bytes);
}

function reply(obj, status) {
  return new Response(obj === null ? null : JSON.stringify(obj), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type"
    }
  });
}
