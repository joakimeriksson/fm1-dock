// counter: how many times each module was added from the page. POST /add {"m":"flat20"}, GET /counts.
// Stores a count per project, module and day: no address, no user agent, no cookie.
const ORIGINS = ["https://joakimeriksson.github.io", "http://localhost:8000", "http://127.0.0.1:8000"];
const NAME = /^[A-Za-z0-9._-]{1,40}$/;

const cors = (req) => {
  const o = req.headers.get("Origin");
  return {
    "Access-Control-Allow-Origin": ORIGINS.includes(o) ? o : ORIGINS[0],
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
    "Vary": "Origin",
  };
};

export default {
  async fetch(req, env) {
    const url = new URL(req.url), h = cors(req);
    if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: h });

    if (url.pathname === "/add" && req.method === "POST") {
      // a page of another origin is refused (a script elsewhere can still POST: the numbers are approximate)
      const o = req.headers.get("Origin");
      if (o && !ORIGINS.includes(o)) return new Response("forbidden", { status: 403, headers: h });
      let b;
      try { b = await req.json(); } catch { return new Response("bad", { status: 400, headers: h }); }
      const project = String(b.p || "fm1-dock"), module = String(b.m || "");
      if (!NAME.test(project) || !NAME.test(module)) return new Response("bad", { status: 400, headers: h });
      const day = new Date().toISOString().slice(0, 10);
      await env.DB.prepare(
        "INSERT INTO adds (project, module, day, n) VALUES (?1, ?2, ?3, 1) " +
        "ON CONFLICT(project, module, day) DO UPDATE SET n = n + 1").bind(project, module, day).run();
      return new Response(null, { status: 204, headers: h });
    }

    if (url.pathname === "/counts" && req.method === "GET") {
      const project = url.searchParams.get("p") || "fm1-dock";
      if (!NAME.test(project)) return new Response("bad", { status: 400, headers: h });
      const days = Math.min(Number(url.searchParams.get("days")) || 0, 3650);
      const since = days ? new Date(Date.now() - days * 864e5).toISOString().slice(0, 10) : "0000-00-00";
      const { results } = await env.DB.prepare(
        "SELECT module, SUM(n) AS n FROM adds WHERE project = ?1 AND day >= ?2 GROUP BY module")
        .bind(project, since).all();
      return Response.json({
        updated: new Date().toISOString(),
        total: results.reduce((s, r) => s + r.n, 0),
        counts: Object.fromEntries(results.map((r) => [r.module, r.n])),
      }, { headers: { ...h, "Cache-Control": "public, max-age=300" } });
    }
    return new Response("not found", { status: 404, headers: h });
  },
};
