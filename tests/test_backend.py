#!/usr/bin/env python3
"""
Backend tests for Metro Route Finder (standard library only).

Run from the project folder:   gcc -O2 -o metro metro.c && python3 tests/test_backend.py

The test has its OWN simple implementation (brute force: list every route, pick the best one)
and checks that the C server agrees with it for every pair of stations and every priority.
"""
import json, os, shutil, subprocess, sys, tempfile, time, urllib.parse, urllib.request, socket, itertools

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRIORITIES = ["all", "fastest", "cheapest", "transfers", "walking", "shortest"]
passed = failed = 0

def check(cond, msg):
    global passed, failed
    if cond: passed += 1
    else:
        failed += 1; print("  FAIL:", msg)

# ---------------- independent reference implementation ----------------
def read_network(path):
    lines, cur = {}, None            # line -> [(station, minutes, km)]
    for raw in open(path, encoding="utf-8"):
        ln = raw.strip()
        if not ln: cur = None if False else cur; continue
        if ln.startswith("#line "): cur = ln[6:].strip(); lines[cur] = []; continue
        if ln.startswith("#") or cur is None: continue
        parts = [p.strip() for p in ln.split(",")]
        km = None
        try:
            if len(parts) >= 3 and float(parts[-1]) >= 0 and parts[-2].replace(".", "", 1).isdigit(): km = float(parts[-1]); parts = parts[:-1]
        except ValueError: pass
        try: mins = int(parts[-1]); nm = ",".join(parts[:-1])
        except ValueError: mins = 2; nm = ln
        lines[cur].append((nm, mins, km))
    edges = {}                       # (a,b,line) -> (minutes, km)
    adj = {}
    for L, rows in lines.items():
        for i in range(len(rows) - 1):
            a, b = rows[i][0], rows[i + 1][0]; w = rows[i][1] or 2; km = rows[i][2]
            edges[(a, b, L)] = edges[(b, a, L)] = (w, km)
            adj.setdefault(a, []).append((b, L)); adj.setdefault(b, []).append((a, L))
    return lines, edges, adj

def all_simple_routes(adj, s, d, closed=()):
    out = []
    def go(u, path, ls, seen):
        for v, L in adj.get(u, []):
            if v in seen or v in closed: continue
            if v == d: out.append((path + [v], ls + [L]))
            else: go(v, path + [v], ls + [L], seen | {v})
    go(s, [s], [], {s})
    return out

def metrics(route, edges, cfg):
    path, ls = route
    minutes = sum(edges[(path[i], path[i + 1], ls[i])][0] for i in range(len(path) - 1))
    xf = [i for i in range(1, len(path) - 1) if ls[i] != ls[i - 1]]
    t = minutes
    if cfg.get("time_complete"):
        t += sum(cfg["xmin"][path[i]] + (cfg["headway"][ls[i]] + 1) // 2 for i in xf)
    walk = sum(cfg["xwalk"][path[i]] for i in xf) if cfg.get("walk_complete") else 0
    dist = sum(int(edges[(path[i], path[i + 1], ls[i])][1] * 1000 + 0.5) for i in range(len(path) - 1)) if cfg.get("dist_complete") else 0
    return dict(minutes=minutes, tm=t, stops=len(path) - 1, xf=len(xf), walk=walk, dist=dist)

def key(m, p, cfg, fare=None):
    """Same comparison order as ORDER[] in metro.c. Missing data counts as 0 for every route."""
    if p in ("all", "fastest"): return (m["tm"], m["xf"], m["dist"], m["stops"], m["walk"])
    if p == "cheapest": return ((fare if fare is not None else 0), m["stops"], m["tm"], m["xf"], m["dist"], m["walk"])
    if p == "transfers": return (m["xf"], m["tm"], m["dist"], m["stops"], m["walk"])
    if p == "walking": return (m["walk"], m["xf"], m["tm"], m["dist"], m["stops"])
    if p == "shortest": return (m["dist"], m["stops"], m["tm"], m["xf"], m["walk"])

# ---------------- server helpers ----------------
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

class Server:
    def __init__(self, datadir=None, files=None):
        self.dir = tempfile.mkdtemp()
        shutil.copytree(os.path.join(ROOT, "data"), os.path.join(self.dir, "data"))
        os.symlink(os.path.join(ROOT, "frontend"), os.path.join(self.dir, "frontend"))
        for name, text in (files or {}).items():
            with open(os.path.join(self.dir, "data", name), "w", encoding="utf-8") as f: f.write(text)
        self.port = free_port()
        env = dict(os.environ, PORT=str(self.port))
        self.proc = subprocess.Popen([os.path.join(ROOT, "metro")], cwd=self.dir, env=env, stdout=subprocess.DEVNULL)
        for _ in range(50):
            try: urllib.request.urlopen(self.url("/api/health"), timeout=1); return
            except Exception: time.sleep(0.1)
        raise RuntimeError("server did not start")
    def url(self, p): return "http://127.0.0.1:%d%s" % (self.port, p)
    def get(self, p):
        with urllib.request.urlopen(self.url(p), timeout=5) as r: return r.read(), r.headers
    def j(self, p): return json.loads(self.get(p)[0])
    def route(self, a, b, **kw):
        q = urllib.parse.urlencode(dict(dict(**{"from": a, "to": b}), **kw))
        return self.j("/api/route?" + q)
    def stop(self): self.proc.terminate(); self.proc.wait(); shutil.rmtree(self.dir, ignore_errors=True)

def sig(path_objs): return tuple(p["station"] for p in path_objs)

# ---------------- tests ----------------
def test_all_pairs_default_data():
    print("1. Every station pair x every priority vs brute force (default data)")
    lines, edges, adj = read_network(os.path.join(ROOT, "data", "stations.txt"))
    stations = sorted(adj)
    srv = Server()
    try:
        net = srv.j("/api/network")
        check(sorted(s["name"] for s in srv.j("/api/stations")) == stations, "/api/stations matches data file")
        file_edges = {(a, b, L) for (a, b, L) in edges if [r[0] for r in lines[L]].index(a) < [r[0] for r in lines[L]].index(b)}
        check({(e["a"], e["b"], e["line"]) for e in net["edges"]} == file_edges, "/api/network edges match data file")
        check({(e["a"], e["b"], e["line"]): e["minutes"] for e in net["edges"]} == {k: edges[k][0] for k in file_edges}, "/api/network minutes match data file")
        cfg = {}
        pairs = 0
        for a in stations:
            for b in stations:
                if a == b: continue
                pairs += 1
                allr = all_simple_routes(adj, a, b)
                ms = [metrics(r, edges, cfg) for r in allr]
                fastest = min(m["minutes"] for m in ms)
                within = {tuple(r[0]) + tuple(r[1]) for r, m in zip(allr, ms) if m["minutes"] <= 2 * fastest + 10}
                d = srv.route(a, b)
                if not d.get("ok"): check(False, "route %s -> %s failed: %s" % (a, b, d)); continue
                got = {}
                for r in d["routes"]:
                    p = [x["station"] for x in r["path"]]; ls = [x["line"] for x in r["path"]][:-1]
                    got[tuple(p) + tuple(ls)] = r
                    # route sanity: real consecutive stations, correct numbers
                    m = metrics((p, ls), edges, cfg)
                    ok = len(set(p)) == len(p) and p[0] == a and p[-1] == b
                    ok = ok and r["minutes"] == m["minutes"] and r["stops"] == m["stops"] and r["interchanges"] == m["xf"]
                    ok = ok and all((p[i], p[i + 1], ls[i]) in edges for i in range(len(p) - 1))
                    ok = ok and [x["change"] for x in r["path"]] == [False] + [ls[i] != ls[i - 1] for i in range(1, len(p) - 1)] + [False]
                    segs = r["segments"]
                    ok = ok and sum(s["stops"] for s in segs) == m["stops"] and sum(s["minutes"] for s in segs) == m["minutes"] and len(segs) == m["xf"] + 1
                    if not ok: check(False, "bad route data %s -> %s: %s" % (a, b, r["id"]))
                    else: passed_inc()
                check(len(got) == len(d["routes"]), "duplicate routes %s -> %s" % (a, b))
                check(within <= set(got), "missing alternative %s -> %s" % (a, b))
                check(d["min_stops"] == min(m["stops"] for m in ms), "BFS min stops %s -> %s" % (a, b))
                for p in PRIORITIES:
                    dp = srv.route(a, b, priority=p)
                    best = min(key(m, p, cfg) for m in ms)
                    rec = dp["routes"][dp["recommended"]]
                    rm = metrics(([x["station"] for x in rec["path"]], [x["line"] for x in rec["path"]][:-1]), edges, cfg)
                    if key(rm, p, cfg) != best: check(False, "%s -> %s priority %s not optimal: %s vs %s" % (a, b, p, key(rm, p, cfg), best))
                    else: passed_inc()
                    # top-level (old format) fields describe the recommended route
                    check(dp["stops"] == rec["stops"] and dp["minutes"] == rec["minutes"] and sig(dp["path"]) == sig(rec["path"]), "compat fields %s -> %s %s" % (a, b, p))
        print("   checked %d station pairs" % pairs)
    finally: srv.stop()

def passed_inc():
    global passed; passed += 1

def test_basics_and_errors():
    print("2. Errors, compatibility, API shape")
    srv = Server()
    try:
        g = srv.route
        check(g("Guindy", "Guindy")["error"].startswith("Start and destination are the same"), "same station")
        check(g("Nowhere", "Guindy")["ok"] is False, "invalid from")
        check(g("Guindy", "Nowhere")["ok"] is False, "invalid to")
        check(g("", "")["ok"] is False, "blank")
        check(g("Guindy", "Egmore", priority="warp")["error"].startswith("Unknown priority"), "unknown priority")
        d = g("guindy", "EGMORE", priority="fastest"); check(d["ok"] and d["from"] == "Guindy", "case-insensitive")
        d = srv.j("/api/route?from=Pachaiyappa%27s%20College&to=Guindy&priority=fastest"); check(d["ok"], "url-encoded apostrophe name")
        old = srv.j("/api/route?from=Guindy&to=Egmore&mode=time"); check(old["ok"] and old["mode"] == "time" and old["priority"] == "fastest", "old mode=time")
        old = srv.j("/api/route?from=Guindy&to=Egmore&mode=stops"); check(old["ok"] and old["mode"] == "stops" and old["priority"] == "shortest", "old mode=stops")
        d = g("Guindy", "Egmore"); check(d["priority"] == "all" and d["recommended"] == d["best"]["fastest"], "default priority is all -> fastest recommended")
        # adjacent stations on one line: simplest trip
        d = g("Guindy", "Little Mount", priority="fastest"); r = d["routes"][d["recommended"]]
        check(r["stops"] == 1 and r["interchanges"] == 0 and r["minutes"] == 2, "adjacent stations: 1 stop, 2 min (Little Mount row says 2)")
        body, hdr = srv.get("/"); check(b"<html" in body[:400].lower(), "index.html served")
        body, _ = srv.get("/api/health"); check(json.loads(body)["status"] == "ok", "health endpoint")
        try: urllib.request.urlopen(srv.url("/nope")); check(False, "404 expected")
        except urllib.error.HTTPError as e: check(e.code == 404, "404 for unknown path")
        # fares/walking/waiting are NOT invented
        d = g("Wimco Nagar Depot", "St. Thomas Mount")
        for r in d["routes"]:
            check(r["fare"] is None and r["km"] is None and r["wait_min"] is None and r["xfer_min"] is None and r["total_est"] is None, "no invented fare/time/km")
            check((r["walk_m"] is None) == (r["interchanges"] > 0), "walk_m unknown only when an interchange is used")
        net = srv.j("/api/network")
        check(net["data"]["fare"]["available"] is False and net["data"]["walking_complete"] is False and net["data"]["waiting_complete"] is False, "data flags default")
        check("sample" in net["data"]["times_source"].lower(), "times labelled as sample")
    finally: srv.stop()

def test_interchange_cases():
    print("3. Interchanges and trade-offs")
    srv = Server()
    try:
        # Different lines, one interchange each way round the loop.
        d = srv.route("Wimco Nagar Depot", "St. Thomas Mount")
        ids = {p: d["best"][p] for p in d["best"]}
        r = d["routes"][ids["fastest"]]
        check(r["interchanges"] == 1 and any(x["change"] for x in r["path"]), "cross-line trip has an interchange")
        changes = [x["station"] for x in r["path"] if x["change"]]
        check(changes[0] in ("Puratchi Thalaivar Dr. M.G. Ramachandran Central", "Arignar Anna Alandur"), "interchange is a real interchange station")
        check(len(d["routes"]) >= 2, "alternative route exists via the other interchange")
        # fastest vs fewest transfers can differ in general; here both must be internally consistent
        ft = d["routes"][ids["transfers"]]; check(ft["interchanges"] <= r["interchanges"], "fewest transfers <= fastest transfers")
        # Same line, loop alternative exists (Central <-> Alandur on Blue vs Green)
        d = srv.route("Puratchi Thalaivar Dr. M.G. Ramachandran Central", "Arignar Anna Alandur")
        pa = {tuple(x["station"] for x in r["path"]) for r in d["routes"]}
        check(len(pa) == 2, "Central -> Alandur has 2 real routes (Blue and Green)")
        # Shortest Path = fewest stops, which can differ from fastest
        for a, b in [("Egmore", "Guindy"), ("Wimco Nagar Depot", "St. Thomas Mount"), ("Koyambedu", "Nandanam")]:
            d = srv.route(a, b); s = d["routes"][d["best"]["shortest"]]
            check(s["stops"] == d["min_stops"], "shortest path stops == BFS minimum (%s -> %s)" % (a, b))
    finally: srv.stop()

def test_fares():
    print("4. Fare rules")
    base = "basis | stops\nsource | TEST FILE (made-up numbers)\n"
    srv = Server(files={"fares.txt": base + "verified | no\nslab | 5 | 10\nslab | 12 | 20\nslab | 40 | 30\n"})
    try:
        d = srv.route("Guindy", "Egmore"); check(all(r["fare"] is None for r in d["routes"]), "unverified rules -> no fares")
        check(srv.j("/api/network")["data"]["fare"]["available"] is False, "unverified flagged unavailable")
    finally: srv.stop()
    srv = Server(files={"fares.txt": base + "verified | yes\nslab | 5 | 10\nslab | 12 | 20\nslab | 40 | 30\n"})
    try:
        check(srv.j("/api/network")["data"]["fare"]["available"] is True, "verified rules available")
        for a, b in [("Guindy", "Egmore"), ("Guindy", "Little Mount"), ("Wimco Nagar Depot", "Chennai International Airport"), ("Egmore", "Vadapalani")]:
            d = srv.route(a, b)
            for r in d["routes"]:
                exp = 10 if r["stops"] <= 5 else 20 if r["stops"] <= 12 else 30 if r["stops"] <= 40 else None
                check(r["fare"] == exp, "fare for %d stops is %s (got %s)" % (r["stops"], exp, r["fare"]))
            # cheapest must be the minimum-fare route
            c = d["routes"][d["best"]["cheapest"]]
            check(c["fare"] == min(r["fare"] for r in d["routes"]), "cheapest = minimum fare (%s -> %s)" % (a, b))
        # fare changes when the route changes
        d1 = srv.route("Guindy", "Little Mount"); d2 = srv.route("Guindy", "Egmore")
        check(d1["routes"][0]["fare"] != d2["routes"][0]["fare"], "fare updates with the trip")
    finally: srv.stop()
    # km-based rules need km data on every segment
    stn = open(os.path.join(ROOT, "data", "stations.txt"), encoding="utf-8").read().replace("Guindy,3", "Guindy,3,1.5").replace("Little Mount,2", "Little Mount,2,1.0")
    srv = Server(files={"stations.txt": stn, "fares.txt": "basis | km\nsource | TEST\nverified | yes\nslab | 2 | 10\nslab | 5 | 20\n"})
    try:
        d = srv.route("Little Mount", "Guindy", priority="fastest"); r = d["routes"][d["recommended"]]
        check(r["km"] == 1.0 and r["fare"] == 10, "km basis: 1.0 km -> fare 10 (got km=%s fare=%s)" % (r["km"], r["fare"]))
        d = srv.route("Guindy", "Egmore"); check(all(r["fare"] is None and r["km"] is None for r in d["routes"]), "km unknown -> fare unavailable, not guessed")
    finally: srv.stop()
    # nonsense slab order is rejected
    srv = Server(files={"fares.txt": base + "verified | yes\nslab | 12 | 20\nslab | 5 | 10\n"})
    try: check(srv.j("/api/network")["data"]["fare"]["available"] is False, "out-of-order slabs rejected")
    finally: srv.stop()

def test_extras_and_disruptions():
    print("5. Walking / waiting data and closed stations")
    ext = ("interchange | Puratchi Thalaivar Dr. M.G. Ramachandran Central | 300 | 6\n"
           "interchange | Arignar Anna Alandur | 100 | 3\nheadway | Blue | 6\nheadway | Green | 8\n")
    srv = Server(files={"extras.txt": ext})
    try:
        lines, edges, adj = read_network(os.path.join(ROOT, "data", "stations.txt"))
        cfg = dict(time_complete=True, walk_complete=True,
                   xmin={"Puratchi Thalaivar Dr. M.G. Ramachandran Central": 6, "Arignar Anna Alandur": 3},
                   xwalk={"Puratchi Thalaivar Dr. M.G. Ramachandran Central": 300, "Arignar Anna Alandur": 100},
                   headway={"Blue": 6, "Green": 8})
        net = srv.j("/api/network"); check(net["data"]["walking_complete"] and net["data"]["waiting_complete"], "flags complete")
        # Wimco -> St. Thomas Mount: two ways to switch line, walking differs (300 m vs 100 m)
        d = srv.route("Wimco Nagar Depot", "St. Thomas Mount", priority="walking")
        r = d["routes"][d["recommended"]]
        check(r["walk_m"] == 100, "Less Walking picks the 100 m interchange (got %s)" % r["walk_m"])
        r2 = [x for x in d["routes"] if x["walk_m"] == 300]
        check(r2, "the 300 m alternative is listed")
        # estimates: in-train + interchange + wait
        for r in d["routes"]:
            first = r["path"][0]["line"]
            wait = (cfg["headway"][first] + 1) // 2
            x = [p for p in r["path"] if p["change"]]
            if x:
                st = x[0]["station"]; nl = [p for p in r["path"] if p["station"] == st][0]["line"]
                wait += (cfg["headway"][nl] + 1) // 2; xm = cfg["xmin"][st]
            else: xm = 0
            check(r["xfer_min"] == xm and r["wait_min"] == wait and r["total_est"] == r["minutes"] + xm + wait, "estimate parts add up (%s)" % r["id"])
        # Fastest now includes interchange/wait time: compare with brute force on a few pairs
        for a, b in [("Wimco Nagar Depot", "St. Thomas Mount"), ("Guindy", "Egmore"), ("Koyambedu", "Nandanam")]:
            allr = all_simple_routes(adj, a, b); ms = [metrics(r, edges, cfg) for r in allr]
            for p in PRIORITIES:
                dd = srv.route(a, b, priority=p); rec = dd["routes"][dd["recommended"]]
                rm = metrics(([x["station"] for x in rec["path"]], [x["line"] for x in rec["path"]][:-1]), edges, cfg)
                check(key(rm, p, cfg) == min(key(m, p, cfg) for m in ms), "priority %s optimal with extras (%s -> %s)" % (p, a, b))
    finally: srv.stop()
    # closed station: routes avoid it; closed endpoint is reported; fully cut network is "no route"
    srv = Server(files={"extras.txt": "closed | Guindy\n"})
    try:
        d = srv.route("Saidapet", "Chennai International Airport")
        check(d["ok"], "reroute around closed Guindy via the loop")
        check(all("Guindy" not in [x["station"] for x in r["path"]] for r in d["routes"]), "no route uses the closed station")
        check(len(d["routes"]) >= 1 and d["routes"][0]["interchanges"] == 2, "detour needs 2 interchanges (Central, Alandur)")
        check("closed" in srv.route("Guindy", "Egmore")["error"], "closed start station reported")
        check("closed" in srv.route("Egmore", "Guindy")["error"], "closed destination reported")
    finally: srv.stop()
    srv = Server(files={"extras.txt": "closed | Little Mount\nclosed | Arignar Anna Alandur\n"})
    try:
        d = srv.route("Saidapet", "Chennai International Airport")
        check(d["ok"] is False and "No route" in d["error"], "unreachable destination -> 'No route'")
    finally: srv.stop()

def test_disconnected_network():
    print("6. Missing/odd data")
    stn = "#line Red\nA,2\nB,3\nC,0\n\n#line Yellow\nX,2\nY,0\n"
    srv = Server(files={"stations.txt": stn})
    try:
        d = srv.route("A", "Y"); check(d["ok"] is False and "No route" in d["error"], "two disconnected lines -> unreachable")
        d = srv.route("A", "C"); check(d["ok"] and d["routes"][0]["minutes"] == 5 and len(d["routes"]) == 1, "single route network")
        check(d["routes"][0]["interchanges"] == 0, "no interchanges")
    finally: srv.stop()
    srv = Server(files={"extras.txt": "", "fares.txt": ""})
    try: check(srv.route("Guindy", "Egmore")["ok"], "empty optional files are fine")
    finally: srv.stop()
    # optional files missing entirely
    srv = Server()
    os.remove(os.path.join(srv.dir, "data", "extras.txt")); os.remove(os.path.join(srv.dir, "data", "fares.txt")); srv.stop()
    d = tempfile.mkdtemp(); shutil.copytree(os.path.join(ROOT, "data"), os.path.join(d, "data")); os.symlink(os.path.join(ROOT, "frontend"), os.path.join(d, "frontend"))
    os.remove(os.path.join(d, "data", "extras.txt")); os.remove(os.path.join(d, "data", "fares.txt"))
    port = free_port(); p = subprocess.Popen([os.path.join(ROOT, "metro")], cwd=d, env=dict(os.environ, PORT=str(port)), stdout=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try: urllib.request.urlopen("http://127.0.0.1:%d/api/health" % port, timeout=1); break
            except Exception: time.sleep(0.1)
        r = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/api/route?from=Guindy&to=Egmore" % port).read())
        check(r["ok"], "works without extras.txt and fares.txt")
    finally: p.terminate(); p.wait(); shutil.rmtree(d, ignore_errors=True)
    # CRLF line endings (Windows-edited data file)
    crlf = open(os.path.join(ROOT, "data", "stations.txt"), encoding="utf-8").read().replace("\n", "\r\n")
    srv = Server(files={"stations.txt": crlf})
    try: check(srv.route("Guindy", "Egmore")["ok"], "CRLF data file")
    finally: srv.stop()

TRADEOFF = "#line Red\nS,10\nT,0\n\n#line Blue\nS,1\nX,0\n\n#line Green\nX,1\nY,0\n\n#line Orange\nY,1\nT,0\n"

def test_tradeoff_network():
    print("7. Synthetic network where Fastest and Fewest Transfers really differ")
    open_ = os.path.join(tempfile.mkdtemp(), "stations.txt"); open(open_, "w").write(TRADEOFF)
    lines, edges, adj = read_network(open_)
    srv = Server(files={"stations.txt": TRADEOFF})
    try:
        d = srv.route("S", "T")
        f, t = d["routes"][d["best"]["fastest"]], d["routes"][d["best"]["transfers"]]
        check(f["minutes"] == 3 and f["interchanges"] == 2, "fastest = 3 min with 2 interchanges")
        check(t["minutes"] == 10 and t["interchanges"] == 0, "fewest transfers = 10 min direct")
        check(d["best"]["fastest"] != d["best"]["transfers"], "different routes")
        check(d["routes"][d["best"]["shortest"]]["stops"] == 1, "shortest path = 1 stop")
        for a in "STXY":
            for b in "STXY":
                if a == b: continue
                ms = [metrics(r, edges, {}) for r in all_simple_routes(adj, a, b)]
                for p in PRIORITIES:
                    dp = srv.route(a, b, priority=p); rec = dp["routes"][dp["recommended"]]
                    rm = metrics(([x["station"] for x in rec["path"]], [x["line"] for x in rec["path"]][:-1]), edges, {})
                    check(key(rm, p, {}) == min(key(m, p, {}) for m in ms), "tradeoff net %s->%s %s optimal" % (a, b, p))
    finally: srv.stop()

# S->T has three ways: Red 2 stops/4 min/10 km, Blue 1 stop/6 min/9 km, Green 2 stops/2 min/5 km
KM_NET = ("#line Red\nS,2,5.0\nM,2,5.0\nT,0\n\n#line Blue\nS,6,9.0\nT,0\n\n#line Green\nS,1,2.5\nX,1,2.5\nT,0\n")

def test_distance():
    print("8. Real distance (km): shortest = least km when every km is known")
    f = os.path.join(tempfile.mkdtemp(), "stations.txt"); open(f, "w").write(KM_NET)
    lines, edges, adj = read_network(f)
    srv = Server(files={"stations.txt": KM_NET})
    try:
        check(srv.j("/api/network")["data"]["distance_complete"] is True, "distance_complete flag")
        d = srv.route("S", "T"); R = d["routes"]
        sh = R[d["best"]["shortest"]]; fa = R[d["best"]["fastest"]]
        check(sh["km"] == 5.0 and sh["stops"] == 2, "Shortest Path = least km (5.0 km), not fewest stops (got %s km, %s stops)" % (sh["km"], sh["stops"]))
        check(fa["minutes"] == 2 and fa["km"] == 5.0, "Fastest = Green 2 min")
        check(sorted(r["km"] for r in R) == [5.0, 9.0, 10.0], "every route shows its real km: %s" % sorted(r["km"] for r in R))
        fs = R[d["best"]["transfers"]]; check(fs["interchanges"] == 0, "fewest interchanges: 0")
        cfg = dict(dist_complete=True)
        for a in "SMTX":
            for b in "SMTX":
                if a == b: continue
                try: allr = all_simple_routes(adj, a, b)
                except KeyError: continue
                if not allr: continue
                ms = [metrics(r, edges, cfg) for r in allr]
                for p in PRIORITIES:
                    dp = srv.route(a, b, priority=p)
                    if not dp.get("ok"): continue
                    rec = dp["routes"][dp["recommended"]]
                    rm = metrics(([x["station"] for x in rec["path"]], [x["line"] for x in rec["path"]][:-1]), edges, cfg)
                    check(key(rm, p, cfg) == min(key(m, p, cfg) for m in ms), "km net %s->%s %s optimal" % (a, b, p))
    finally: srv.stop()
    # without km on even one segment: nothing is guessed, Shortest falls back to fewest stops
    partial = KM_NET.replace("S,6,9.0", "S,6")
    srv = Server(files={"stations.txt": partial})
    try:
        check(srv.j("/api/network")["data"]["distance_complete"] is False, "partial km -> not complete")
        d = srv.route("S", "T"); sh = d["routes"][d["best"]["shortest"]]
        check(sh["stops"] == 1 and sh["km"] is None, "no complete km: Shortest = fewest stops, km null")
        check(any(r["km"] is not None for r in d["routes"]) and any(r["km"] is None for r in d["routes"]), "only routes whose every km is known show a distance")
    finally: srv.stop()
    srv = Server()
    try:
        d = srv.route("Guindy", "Egmore")
        check(all(r["km"] is None for r in d["routes"]), "default data: km is null (never invented)")
        check(srv.j("/api/network")["data"]["distance_complete"] is False, "default data: distance_complete false")
    finally: srv.stop()

def test_identical_options_and_speed():
    print("9. Identical Fastest / Fewest Interchanges / Shortest, and response time")
    srv = Server()
    try:
        lines, edges, adj = read_network(os.path.join(ROOT, "data", "stations.txt"))
        stations = sorted(adj)
        same = differ = 0; wall = []; us = []; examples = []
        for a in stations:
            for b in stations:
                if a == b: continue
                t0 = time.perf_counter(); d = srv.route(a, b); wall.append((time.perf_counter() - t0) * 1000); us.append(d["elapsed_us"])
                bst = d["best"]; trio = {bst["fastest"], bst["transfers"], bst["shortest"]}
                if len(trio) == 1: same += 1; examples.append((a, b))
                else:
                    differ += 1
                    # if they differ, they must truly differ on the metric they optimise
                    R = d["routes"]
                    f, t, sh = R[bst["fastest"]], R[bst["transfers"]], R[bst["shortest"]]
                    ok = (bst["fastest"] == bst["transfers"] or t["interchanges"] < f["interchanges"] or t["minutes"] >= f["minutes"])
                    ok = ok and (bst["fastest"] == bst["shortest"] or sh["stops"] <= f["stops"])
                    check(ok, "differing options are consistent %s -> %s" % (a, b))
        print("   %d pairs: all three options identical for %d, different for %d" % (same + differ, same, differ))
        check(same > 0, "identical-options case occurs")
        for a, b in [("Guindy", "Little Mount"), ("Koyambedu", "CMBT"), ("Wimco Nagar Depot", "Chennai International Airport"), ("Egmore", "Kilpauk")]:
            d = srv.route(a, b); bst = d["best"]
            check(bst["fastest"] == bst["transfers"] == bst["shortest"], "all three identical: %s -> %s" % (a, b))
        wall.sort(); us.sort()
        p50, p99, mx = wall[len(wall) // 2], wall[int(len(wall) * .99)], wall[-1]
        print("   HTTP round trip (ms): median %.2f, p99 %.2f, max %.2f | search time (us): median %d, max %d" % (p50, p99, mx, us[len(us) // 2], us[-1]))
        check(p99 < 50, "p99 response under 50 ms (%.1f)" % p99)
        check(us[-1] < 20000, "worst search under 20 ms (%d us)" % us[-1])
    finally: srv.stop()

if __name__ == "__main__":
    if not os.path.exists(os.path.join(ROOT, "metro")): sys.exit("Build first: gcc -O2 -o metro metro.c")
    t0 = time.time()
    for t in (test_basics_and_errors, test_interchange_cases, test_fares, test_extras_and_disruptions, test_disconnected_network, test_tradeoff_network, test_distance, test_identical_options_and_speed, test_all_pairs_default_data):
        t()
    print("\n%d checks passed, %d failed (%.1fs)" % (passed, failed, time.time() - t0))
    sys.exit(1 if failed else 0)
