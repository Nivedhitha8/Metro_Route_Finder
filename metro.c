/*
 * metro.c - Metro Route Finder (upgraded: Smart Route Engine)
 *
 * DSA concepts used
 *   Graph (adjacency list)   station = vertex, track between two stations = undirected edge
 *   BFS                      fewest stops, and a quick "is the destination reachable?" check
 *   Dijkstra + min-heap      best route for a chosen priority (time, transfers, stops, walking)
 *   DFS (backtracking)       lists the other real routes (alternatives) between two stations
 *
 * How a priority works
 *   Every route has 4 numbers: time, stops, transfers (interchanges), walking.
 *   A priority is just an ORDER of those numbers. Dijkstra minimises the first number,
 *   and uses the next ones to break ties.   Example: Fastest = time, then transfers, then stops.
 *   To change a line change at a station Dijkstra must know which line you are on, so its
 *   vertices are (station, line) pairs instead of plain stations.
 *
 * Distance: if every row of data/stations.txt has a km value, "Shortest Path" means least km.
 * Otherwise it means fewest stops (and the website says so). Distances are never estimated.
 *
 * Run as a website:  ./metro           (open http://localhost:8080)
 * Run in the console: ./metro --cli
 * Build: gcc -O2 -o metro metro.c       (Windows/MinGW: add -lws2_32)
 *
 * Data files (all optional except stations.txt):
 *   data/stations.txt  lines, stations, minutes (and optionally km) between stations
 *   data/extras.txt    interchange walking/transfer time, train headways, closed stations
 *   data/fares.txt     fare rules. Fares are shown ONLY when the file says "verified | yes"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#ifdef _WIN32
  #include <winsock2.h>
  typedef int socklen_t;
  #define CLOSE closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <signal.h>
  #include <sys/time.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define CLOSE close
#endif

#define MAXN 256            /* max stations */
#define MAXL 8              /* max lines */
#define MAXP 256            /* max stations on one route */
#define MAXR 10             /* max routes sent to the website */
#define POOL 64             /* max routes collected while searching */
#define INF  (1LL << 60)

/* ---------- Graph: adjacency list ---------- */
typedef struct Edge { int to, w, line; double km; struct Edge *next; } Edge;   /* km < 0 means "unknown" */
static char  name[MAXN][64];
static Edge *adj[MAXN];
static unsigned lmask[MAXN];                 /* which lines serve a station (one bit per line) */
static char  lname[MAXL][24];
static int   lst[MAXL][MAXN], lcnt[MAXL];    /* stations of each line, in order */
static int   n = 0, nl = 0;
static char  times_source[120] = "Not stated";

/* Optional extra data (-1 = unknown). Nothing is guessed: unknown stays unknown. */
static int  xwalk[MAXN], xmin[MAXN];         /* interchange walking metres / minutes, per station */
static int  headway[MAXL];                   /* minutes between trains, per line */
static char closed_st[MAXN];                 /* 1 = station closed (disruption hook) */
static int  walk_complete, time_complete;    /* is the optional data known for EVERY interchange? */
static int  dist_complete;                   /* is the km known for EVERY track segment? */

/* Fare rules */
static char   fare_basis = 's';              /* 's' = stops travelled, 'k' = km travelled */
static int    nslab, slab_fare[16];
static double slab_max[16];
static int    fare_ok;                       /* 1 only when rules are present AND marked verified */
static char   fare_msg[200] = "No fare rules loaded (data/fares.txt not found).";
static char   fare_source[160] = "";

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}
static int find(const char *s) { for (int i = 0; i < n; i++) if (ieq(name[i], s)) return i; return -1; }
static int station(const char *s) {
    int i = find(s);
    if (i >= 0) return i;
    if (n >= MAXN) { fprintf(stderr, "Too many stations\n"); exit(1); }
    strncpy(name[n], s, 63); return n++;
}
static void add_edge(int a, int b, int w, int line, double km) {
    Edge *e = malloc(sizeof *e); e->to = b; e->w = w; e->line = line; e->km = km; e->next = adj[a]; adj[a] = e;
    e = malloc(sizeof *e);       e->to = a; e->w = w; e->line = line; e->km = km; e->next = adj[b]; adj[b] = e;
}
static void trim(char *s) {
    int l = (int)strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) s[--l] = 0;
}
static char *ltrim(char *s) { while (*s && isspace((unsigned char)*s)) s++; return s; }
static int is_number(const char *s) {
    char *end; s = ltrim((char *)s); if (!*s) return 0;
    strtod(s, &end); return *ltrim(end) == 0;
}
static int count_bits(unsigned x) { int c = 0; for (; x; x >>= 1) c += x & 1; return c; }

/* A station row is "Name,minutes" or "Name,minutes,km". Read the numbers from the right. */
static void parse_row(char *ln, int *w, double *km) {
    *w = 2; *km = -1;
    char *c = strrchr(ln, ',');
    if (!c || !is_number(c + 1)) return;
    double last = atof(c + 1); *c = 0;
    char *p = strrchr(ln, ',');
    if (p && is_number(p + 1)) { *km = last; *w = atoi(p + 1); *p = 0; }
    else *w = (int)last;
    trim(ln);
}
static int load(const char *file) {
    FILE *f = fopen(file, "r"); if (!f) return 0;
    char ln[256]; int prev = -1, pw = 2, line = -1; double pkm = -1;
    while (fgets(ln, sizeof ln, f)) {
        trim(ln);
        if (!*ln) { prev = -1; continue; }
        if (!strncmp(ln, "#line ", 6)) {
            if (nl >= MAXL) break;
            line = nl++; strncpy(lname[line], ln + 6, 23); prev = -1; continue;
        }
        if (!strncmp(ln, "#times_source ", 14)) { strncpy(times_source, ln + 14, sizeof times_source - 1); continue; }
        if (ln[0] == '#' || line < 0) continue;
        int w; double km; parse_row(ln, &w, &km);
        int id = station(ln); lmask[id] |= 1u << line;
        if (prev == id) continue;                           /* same station listed twice in a row */
        if (lcnt[line] < MAXN) lst[line][lcnt[line]++] = id;
        if (prev >= 0) add_edge(prev, id, pw > 0 ? pw : 2, line, pkm);
        prev = id; pw = w; pkm = km;
    }
    fclose(f); return n > 0;
}

/* ---------- Optional files: extras.txt and fares.txt (lines look like:  key | value | value) ---------- */
static int split_bar(char *s, char *f[], int max) {
    int k = 0; f[k++] = s;
    for (char *p = s; *p; p++) if (*p == '|' && k < max) { *p = 0; f[k++] = p + 1; }
    for (int i = 0; i < k; i++) { f[i] = ltrim(f[i]); trim(f[i]); }
    return k;
}
static int num_or_unknown(const char *s) { return (*s && strcmp(s, "?") && is_number(s)) ? atoi(s) : -1; }
static int find_line(const char *s) { for (int l = 0; l < nl; l++) if (ieq(lname[l], s)) return l; return -1; }

static void load_extras(const char *file) {
    FILE *f = fopen(file, "r"); if (!f) return;
    char ln[256], *fld[4];
    while (fgets(ln, sizeof ln, f)) {
        trim(ln); if (!*ln || ln[0] == '#') continue;
        int k = split_bar(ln, fld, 4);
        if (!strcmp(fld[0], "interchange") && k >= 4) {          /* interchange | Station | walk metres | minutes */
            int s = find(fld[1]); if (s >= 0) { xwalk[s] = num_or_unknown(fld[2]); xmin[s] = num_or_unknown(fld[3]); }
        } else if (!strcmp(fld[0], "headway") && k >= 3) {       /* headway | Line | minutes between trains */
            int l = find_line(fld[1]); if (l >= 0) headway[l] = num_or_unknown(fld[2]);
        } else if (!strcmp(fld[0], "closed") && k >= 2) {        /* closed | Station */
            int s = find(fld[1]); if (s >= 0) closed_st[s] = 1;
        }
    }
    fclose(f);
}
static void load_fares(const char *file) {
    FILE *f = fopen(file, "r"); if (!f) return;
    char ln[256], *fld[4]; int verified = 0;
    while (fgets(ln, sizeof ln, f)) {
        trim(ln); if (!*ln || ln[0] == '#') continue;
        int k = split_bar(ln, fld, 4);
        if (!strcmp(fld[0], "basis") && k >= 2) fare_basis = ieq(fld[1], "km") ? 'k' : 's';
        else if (!strcmp(fld[0], "source") && k >= 2) strncpy(fare_source, fld[1], sizeof fare_source - 1);
        else if (!strcmp(fld[0], "verified") && k >= 2) verified = ieq(fld[1], "yes");
        else if (!strcmp(fld[0], "slab") && k >= 3 && nslab < 16 && is_number(fld[1]) && is_number(fld[2])) {
            slab_max[nslab] = atof(fld[1]); slab_fare[nslab] = atoi(fld[2]); nslab++;
        }
    }
    fclose(f);
    for (int i = 1; i < nslab; i++)                          /* slabs must go up, or "cheapest" logic breaks */
        if (slab_max[i] <= slab_max[i - 1] || slab_fare[i] < slab_fare[i - 1]) { nslab = 0; break; }
    if (nslab == 0)      snprintf(fare_msg, sizeof fare_msg, "No verified fare rules loaded. Add them to data/fares.txt (see the instructions inside).");
    else if (!verified)  snprintf(fare_msg, sizeof fare_msg, "Fare rules in data/fares.txt are not marked verified, so fares are hidden.");
    else { fare_ok = 1;  snprintf(fare_msg, sizeof fare_msg, "Fare rules loaded from data/fares.txt."); }
}
static void finish_load(void) {
    walk_complete = time_complete = dist_complete = 1;
    for (int l = 0; l < nl; l++) for (int i = 0; i + 1 < lcnt[l]; i++)           /* every segment needs a km value */
        for (Edge *e = adj[lst[l][i]]; e; e = e->next)
            if (e->to == lst[l][i + 1] && e->line == l && e->km < 0) dist_complete = 0;
    for (int s = 0; s < n; s++) if (count_bits(lmask[s]) >= 2) {      /* an interchange station */
        if (xwalk[s] < 0) walk_complete = 0;
        if (xmin[s] < 0)  time_complete = 0;
    }
    for (int l = 0; l < nl; l++) if (headway[l] < 0) time_complete = 0;
}

/* ---------- Metrics, priorities and the binary min-heap ---------- */
/* A route (or a partial route) is described by NM numbers. A priority is an ORDER in which they are compared. */
enum { M_TIME, M_STOPS, M_XFER, M_WALK, M_DIST, M_FARE, NM };
enum { P_ALL, P_FASTEST, P_CHEAPEST, P_TRANSFERS, P_WALKING, P_SHORTEST, NP };
static const char *pkey[NP]   = { "all", "fastest", "cheapest", "transfers", "walking", "shortest" };
static const char *plabel[NP] = { "All Routes", "Fastest", "Cheapest", "Fewest Interchanges", "Less Walking", "Shortest Path" };
/* Metrics whose data is missing are 0 for every route, so they never decide anything and the next one takes over.
 * time = in-train minutes (+ interchange/wait minutes when that data is complete), dist = metres (all km known).
 * Shortest Path = least distance when every km is known, otherwise fewest stops. Never a guess. */
static const int ORDER[NP][NM] = {
    { M_TIME,  M_XFER,  M_DIST,  M_STOPS, M_WALK,  M_FARE },   /* all (recommendation = fastest) */
    { M_TIME,  M_XFER,  M_DIST,  M_STOPS, M_WALK,  M_FARE },   /* fastest */
    { M_FARE,  M_STOPS, M_TIME,  M_XFER,  M_DIST,  M_WALK },   /* cheapest (fare only if verified rules are loaded) */
    { M_XFER,  M_TIME,  M_DIST,  M_STOPS, M_WALK,  M_FARE },   /* fewest interchanges */
    { M_WALK,  M_XFER,  M_TIME,  M_DIST,  M_STOPS, M_FARE },   /* less walking */
    { M_DIST,  M_STOPS, M_TIME,  M_XFER,  M_WALK,  M_FARE },   /* shortest path */
};
static int parse_priority(const char *s) { for (int p = 0; p < NP; p++) if (!strcmp(s, pkey[p])) return p; return -1; }
static int metric_less(const long long *a, const long long *b, int p) {      /* is a better than b for priority p? */
    for (int i = 0; i < NM; i++) { int k = ORDER[p][i]; if (a[k] != b[k]) return a[k] < b[k]; }
    return 0;
}

typedef struct { int v; long long m[NM]; } HN;                 /* heap item: a vertex and its cost so far */
#define HCAP (1 << 16)
static HN hp[HCAP]; static int hn, hp_prio;
static void hpush(int v, const long long *m) {
    if (hn >= HCAP - 1) return;
    int i = hn++; hp[i].v = v; memcpy(hp[i].m, m, sizeof hp[i].m);
    while (i > 0 && metric_less(hp[i].m, hp[(i - 1) / 2].m, hp_prio)) { HN t = hp[i]; hp[i] = hp[(i - 1) / 2]; hp[(i - 1) / 2] = t; i = (i - 1) / 2; }
}
static HN hpop(void) {
    HN top = hp[0]; hp[0] = hp[--hn]; int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < hn && metric_less(hp[l].m, hp[m].m, hp_prio)) m = l;
        if (r < hn && metric_less(hp[r].m, hp[m].m, hp_prio)) m = r;
        if (m == i) break;
        HN t = hp[i]; hp[i] = hp[m]; hp[m] = t; i = m;
    }
    return top;
}

/* ---------- Routes and their numbers ---------- */
typedef struct {
    int n, path[MAXP], line[MAXP];  /* stations in order; line[i] = line used to leave station i (last = arriving line) */
    int stops, minutes, transfers;  /* minutes = in-train time from the data file */
    int tmetric;                    /* minutes + interchange/wait time, only when that data is complete */
    int walk_m, xfer_min, wait_min, total_est, fare;   /* -1 = not available */
    long long dist_m;               /* metres, -1 = not available (some km missing) */
    double km;                      /* dist_m / 1000, or -1 */
} Route;

static Edge *edge_on(int a, int b, int line) {
    for (Edge *e = adj[a]; e; e = e->next) if (e->to == b && e->line == line) return e;
    return NULL;
}
static long long edge_m(const Edge *e) { return e->km < 0 ? -1 : (long long)(e->km * 1000.0 + 0.5); }
static int wait_of(int line) { return (headway[line] + 1) / 2; }     /* average wait = half the headway, rounded up */

static int fare_of(const Route *r) {
    if (!fare_ok) return -1;
    double v = fare_basis == 'k' ? r->km : r->stops;
    if (v < 0) return -1;
    for (int i = 0; i < nslab; i++) if (v <= slab_max[i]) return slab_fare[i];
    return -1;                                                      /* longer than the last slab: unknown */
}
static void measure(Route *r) {
    int xt = 0, xt_known = 1, wk = 0, wk_known = 1, wt = 0, wt_known = 1, wait_tr = 0, kmk = 1;
    long long dm = 0;
    r->stops = r->n - 1; r->minutes = 0; r->transfers = 0;
    for (int i = 0; i < r->n - 1; i++) {
        Edge *e = edge_on(r->path[i], r->path[i + 1], r->line[i]);
        r->minutes += e->w;
        if (e->km < 0) kmk = 0; else dm += edge_m(e);
    }
    if (headway[r->line[0]] < 0) wt_known = 0; else wt += wait_of(r->line[0]);      /* waiting for the first train */
    for (int i = 1; i < r->n - 1; i++) if (r->line[i] != r->line[i - 1]) {         /* a change of line */
        int st = r->path[i];
        r->transfers++;
        if (xwalk[st] < 0) wk_known = 0; else wk += xwalk[st];
        if (xmin[st] < 0)  xt_known = 0; else xt += xmin[st];
        if (headway[r->line[i]] < 0) wt_known = 0; else { wt += wait_of(r->line[i]); wait_tr += wait_of(r->line[i]); }
    }
    r->walk_m   = wk_known ? wk : -1;
    r->xfer_min = xt_known ? xt : -1;
    r->wait_min = wt_known ? wt : -1;
    r->total_est = (xt_known && wt_known) ? r->minutes + xt + wt : -1;
    r->tmetric  = r->minutes + (time_complete ? xt + wait_tr : 0);
    r->dist_m = kmk ? dm : -1;
    r->km = kmk ? dm / 1000.0 : -1;
    r->fare = fare_of(r);
}

/* The numbers a finished route is compared by (same meaning as above). */
static void route_metrics(const Route *r, long long m[NM]) {
    m[M_TIME] = r->tmetric; m[M_STOPS] = r->stops; m[M_XFER] = r->transfers;
    m[M_WALK] = walk_complete ? r->walk_m : 0;
    m[M_DIST] = dist_complete ? r->dist_m : 0;
    m[M_FARE] = fare_ok ? (r->fare >= 0 ? r->fare : 1000000000LL) : 0;   /* fare outside the rules: never "cheapest" */
}
static int route_better(const Route *a, const Route *b, int p) {
    long long x[NM], y[NM]; route_metrics(a, x); route_metrics(b, y); return metric_less(x, y, p);
}

/* ---------- Algorithms ---------- */
/* BFS: fewest stops, ignoring lines. Returns the number of stops, or -1 if unreachable. */
static int bfs_hops(int s, int d) {
    int q[MAXN], h = 0, t = 0, dist[MAXN];
    for (int i = 0; i < n; i++) dist[i] = -1;
    q[t++] = s; dist[s] = 0;
    while (h < t) {
        int u = q[h++];
        for (Edge *e = adj[u]; e; e = e->next)
            if (dist[e->to] < 0 && !closed_st[e->to]) { dist[e->to] = dist[u] + 1; q[t++] = e->to; }
    }
    return dist[d];
}

/* Dijkstra over (station, line) pairs. Moving one stop along the line you are on costs that edge;
 * changing line at an interchange costs one transfer (+ walking/wait time if that data is complete). */
static int dijkstra_route(int s, int d, int p, Route *out) {
    static long long dist[MAXN * MAXL][NM]; static char seen[MAXN * MAXL]; static int prv[MAXN * MAXL];
    for (int i = 0; i < n * MAXL; i++) { seen[i] = 0; prv[i] = -1; }
    hn = 0; hp_prio = p;
    long long zero[NM] = { 0 };
    for (int l = 0; l < nl; l++) if (lmask[s] >> l & 1) { int v = s * MAXL + l; seen[v] = 1; memcpy(dist[v], zero, sizeof zero); hpush(v, zero); }
    int best = -1;
    while (hn) {
        HN c = hpop();
        if (memcmp(c.m, dist[c.v], sizeof c.m)) continue;       /* stale heap entry */
        int u = c.v / MAXL, l = c.v % MAXL;
        if (u == d) { best = c.v; break; }                      /* first time we pop d = best way to d */
        for (Edge *e = adj[u]; e; e = e->next) {                /* ride one stop on the current line */
            if (e->line != l || closed_st[e->to]) continue;
            int v = e->to * MAXL + l; long long nm[NM];
            memcpy(nm, c.m, sizeof nm);
            nm[M_TIME] += e->w; nm[M_STOPS] += 1; nm[M_DIST] += dist_complete ? edge_m(e) : 0;
            if (!seen[v] || metric_less(nm, dist[v], p)) { seen[v] = 1; memcpy(dist[v], nm, sizeof nm); prv[v] = c.v; hpush(v, nm); }
        }
        for (int l2 = 0; l2 < nl; l2++) {                       /* change to another line at this station */
            if (l2 == l || !(lmask[u] >> l2 & 1)) continue;
            int v = u * MAXL + l2; long long nm[NM];
            memcpy(nm, c.m, sizeof nm);
            nm[M_TIME] += time_complete ? xmin[u] + wait_of(l2) : 0; nm[M_XFER] += 1; nm[M_WALK] += walk_complete ? xwalk[u] : 0;
            if (!seen[v] || metric_less(nm, dist[v], p)) { seen[v] = 1; memcpy(dist[v], nm, sizeof nm); prv[v] = c.v; hpush(v, nm); }
        }
    }
    if (best < 0) return 0;
    int stk[MAXN * MAXL], k = 0;
    for (int v = best; v != -1; v = prv[v]) stk[k++] = v;
    out->n = 1; out->path[0] = s;
    for (int i = k - 1; i > 0; i--) {
        int u = stk[i] / MAXL, v = stk[i - 1] / MAXL;
        if (u == v) continue;                                   /* just a line change, no movement */
        if (out->n >= MAXP) return 0;
        out->line[out->n - 1] = stk[i] % MAXL; out->path[out->n++] = v;
    }
    if (out->n < 2) return 0;
    out->line[out->n - 1] = out->line[out->n - 2];
    for (int i = 0; i < out->n; i++) for (int j = i + 1; j < out->n; j++) if (out->path[i] == out->path[j]) return 0;   /* must be simple */
    measure(out);
    return 1;
}

/* DFS: collect every simple route (no station twice) whose in-train time is within `limit` minutes. */
static Route pool[POOL]; static int pool_n;
static int vis[MAXN], stk_st[MAXP], stk_ln[MAXP]; static long steps;
static int same_route(const Route *a, const Route *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) if (a->path[i] != b->path[i] || a->line[i] != b->line[i]) return 0;
    return 1;
}
static void pool_add(const Route *r) {
    for (int i = 0; i < pool_n; i++) if (same_route(&pool[i], r)) return;
    if (pool_n < POOL) pool[pool_n++] = *r;
}
static void dfs(int u, int d, int t, int limit, int depth) {
    if (++steps > 300000) return;                                /* safety net for big networks */
    for (Edge *e = adj[u]; e; e = e->next) {
        int v = e->to;
        if (vis[v] || closed_st[v] || t + e->w > limit || depth + 2 >= MAXP) continue;
        stk_st[depth + 1] = v; stk_ln[depth] = e->line;
        if (v == d) {
            Route r; r.n = depth + 2;
            for (int i = 0; i < r.n; i++) { r.path[i] = stk_st[i]; r.line[i] = stk_ln[i < r.n - 1 ? i : r.n - 2]; }
            measure(&r); pool_add(&r);
        } else { vis[v] = 1; dfs(v, d, t + e->w, limit, depth + 1); vis[v] = 0; }
    }
}

/* ---------- The planner: ties everything together ---------- */
typedef struct { int nr, best[NP], min_hops; Route r[MAXR]; } Plan;
static Plan plan;

static int cmp_label(const void *a, const void *b) {          /* order routes A, B, C...: quickest first */
    const Route *x = a, *y = b;
    if (x->tmetric != y->tmetric) return x->tmetric - y->tmetric;
    if (x->transfers != y->transfers) return x->transfers - y->transfers;
    if (x->stops != y->stops) return x->stops - y->stops;
    for (int i = 0; i < x->n && i < y->n; i++) if (x->path[i] != y->path[i]) return x->path[i] - y->path[i];
    return 0;
}
static int best_index(int p) {
    int bi = 0;
    for (int i = 1; i < plan.nr; i++) if (route_better(&plan.r[i], &plan.r[bi], p)) bi = i;
    return bi;
}
/* returns 0 = ok, 1 = a station is closed, 2 = no route */
static int make_plan(int s, int d) {
    Route opt[NP]; int is_opt[POOL];
    if (closed_st[s] || closed_st[d]) return 1;
    plan.min_hops = bfs_hops(s, d);
    if (plan.min_hops < 0) return 2;
    for (int p = P_FASTEST; p < NP; p++) if (!dijkstra_route(s, d, p, &opt[p])) return 2;
    pool_n = 0; steps = 0;
    for (int i = 0; i < n; i++) vis[i] = 0;
    vis[s] = 1; stk_st[0] = s;
    dfs(s, d, 0, opt[P_FASTEST].minutes * 2 + 10, 0);          /* alternatives slower than 2x the fastest + 10 min are hidden */
    for (int p = P_FASTEST; p < NP; p++) pool_add(&opt[p]);
    /* keep the priority winners first, then the quickest others, up to MAXR; then sort for labelling */
    for (int i = 0; i < pool_n; i++) is_opt[i] = 0;
    plan.nr = pool_n; memcpy(plan.r, pool, sizeof(Route) * (pool_n < MAXR ? pool_n : MAXR));
    if (pool_n > MAXR) {
        Route tmp[POOL]; int k = 0;
        qsort(pool, pool_n, sizeof(Route), cmp_label);
        for (int p = P_FASTEST; p < NP; p++) for (int i = 0; i < pool_n; i++) if (same_route(&pool[i], &opt[p])) is_opt[i] = 1;
        for (int i = 0; i < pool_n && k < MAXR; i++) if (is_opt[i]) tmp[k++] = pool[i];
        for (int i = 0; i < pool_n && k < MAXR; i++) if (!is_opt[i]) tmp[k++] = pool[i];
        memcpy(plan.r, tmp, sizeof(Route) * k); plan.nr = k;
    }
    qsort(plan.r, plan.nr, sizeof(Route), cmp_label);
    for (int p = P_FASTEST; p < NP; p++) plan.best[p] = best_index(p);
    plan.best[P_ALL] = plan.best[P_FASTEST];
    return 0;
}

/* ---------- Output helpers ---------- */
static char out[1 << 20];
static int  off;
static void add(const char *fmt, ...) {
    if (off >= (int)sizeof out - 1) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(out + off, sizeof out - off, fmt, ap);
    va_end(ap);
    if (w > 0) off += w < (int)sizeof out - off ? w : (int)sizeof out - off - 1;
}
static void add_str(const char *s) {                           /* JSON string with escaping */
    add("\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') add("\\%c", c);
        else if (c < 0x20) add("\\u%04x", c);
        else add("%c", c);
    }
    add("\"");
}
static void add_int_or_null(int v)       { if (v < 0) add("null"); else add("%d", v); }
static void add_dbl_or_null(double v)    { if (v < 0) add("null"); else add("%.3f", v); }
static int line_pos(int line, int st)    { for (int i = 0; i < lcnt[line]; i++) if (lst[line][i] == st) return i; return -1; }

/* ---------- Console mode ---------- */
static void trim_nl(char *s) { trim(s); }
static void print_route(const Route *r, int id) {
    printf("  Route %c: %d min (in train, sample data) | %d stops | %d interchange(s)", 'A' + id, r->minutes, r->stops, r->transfers);
    if (r->fare >= 0) printf(" | fare Rs %d", r->fare); else printf(" | fare n/a");
    printf("\n    ");
    for (int i = 0; i < r->n; i++) {
        printf("%s%s", i ? " > " : "", name[r->path[i]]);
        if (i > 0 && i < r->n - 1 && r->line[i] != r->line[i - 1]) printf(" [change to %s]", lname[r->line[i]]);
    }
    printf("\n");
}
static void cli(void) {
    char a[128], b[128], m[32];
    printf("Metro Route Finder (%d stations). Type 'list' to see them, Ctrl+C to quit.\n", n);
    for (;;) {
        printf("\nFrom: "); if (!fgets(a, sizeof a, stdin)) return; trim_nl(a);
        if (ieq(a, "list")) { for (int i = 0; i < n; i++) printf("  %s\n", name[i]); continue; }
        printf("To: ");   if (!fgets(b, sizeof b, stdin)) return; trim_nl(b);
        printf("Priority (all/fastest/cheapest/transfers/walking/shortest) [all]: "); if (!fgets(m, sizeof m, stdin)) return; trim_nl(m);
        int s = find(a), d = find(b), p = *m ? parse_priority(m) : P_ALL;
        if (s < 0 || d < 0) { printf("Unknown station.\n"); continue; }
        if (s == d) { printf("Start and destination are the same station.\n"); continue; }
        if (p < 0) { printf("Unknown priority.\n"); continue; }
        int rc = make_plan(s, d);
        if (rc) { printf(rc == 1 ? "A selected station is closed.\n" : "No route found.\n"); continue; }
        printf("Recommended for '%s':\n", plabel[p]); print_route(&plan.r[plan.best[p]], plan.best[p]);
        printf("All routes found:\n");
        for (int i = 0; i < plan.nr; i++) print_route(&plan.r[i], i);
    }
}

/* ---------- Tiny HTTP server (website + JSON API) ---------- */
static void urldecode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}
static void qparam(const char *q, const char *key, char *dst, int cap) {
    dst[0] = 0; size_t kl = strlen(key);
    for (const char *p = q; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1; int i = 0; while (*p && *p != '&' && i < cap - 1) dst[i++] = *p++;
            dst[i] = 0; urldecode(dst); return;
        }
        p = strchr(p, '&'); if (p) p++;
    }
}
static void json_stations(void) {                              /* unchanged format: older pages keep working */
    add("[");
    for (int i = 0; i < n; i++) {
        add("%s{\"name\":", i ? "," : ""); add_str(name[i]); add(",\"lines\":[");
        int first = 1; for (int l = 0; l < nl; l++) if (lmask[i] >> l & 1) { add("%s", first ? "" : ","); add_str(lname[l]); first = 0; }
        add("]}");
    }
    add("]");
}
static void json_network(void) {                               /* the real network: lines, connections, data quality */
    add("{\"lines\":[");
    for (int l = 0; l < nl; l++) {
        add("%s{\"name\":", l ? "," : ""); add_str(lname[l]); add(",\"headway\":"); add_int_or_null(headway[l]); add(",\"stations\":[");
        for (int i = 0; i < lcnt[l]; i++) { if (i) add(","); add_str(name[lst[l][i]]); }
        add("]}");
    }
    add("],\"edges\":[");
    int first = 1;
    for (int l = 0; l < nl; l++) for (int i = 0; i + 1 < lcnt[l]; i++) {
        Edge *e = edge_on(lst[l][i], lst[l][i + 1], l); if (!e) continue;
        add("%s{\"a\":", first ? "" : ","); add_str(name[lst[l][i]]); add(",\"b\":"); add_str(name[lst[l][i + 1]]);
        add(",\"line\":"); add_str(lname[l]); add(",\"minutes\":%d,\"km\":", e->w); add_dbl_or_null(e->km); add("}"); first = 0;
    }
    add("],\"interchanges\":[");
    first = 1;
    for (int s = 0; s < n; s++) if (count_bits(lmask[s]) >= 2) {
        add("%s{\"station\":", first ? "" : ","); add_str(name[s]);
        add(",\"walk_m\":"); add_int_or_null(xwalk[s]); add(",\"minutes\":"); add_int_or_null(xmin[s]); add("}"); first = 0;
    }
    add("],\"data\":{\"times_source\":"); add_str(times_source);
    add(",\"fare\":{\"available\":%s,\"basis\":\"%s\",\"message\":", fare_ok ? "true" : "false", fare_basis == 'k' ? "km" : "stops"); add_str(fare_msg);
    add(",\"source\":"); add_str(fare_source);
    add("},\"walking_complete\":%s,\"waiting_complete\":%s,\"distance_complete\":%s}}", walk_complete ? "true" : "false", time_complete ? "true" : "false", dist_complete ? "true" : "false");
}
static void json_error(const char *msg) { add("{\"ok\":false,\"error\":"); add_str(msg); add("}"); }

static void json_route_obj(const Route *r, int id) {
    char idc = (char)('A' + id);
    add("{\"id\":\"%c\",\"stops\":%d,\"minutes\":%d,\"interchanges\":%d,\"walk_m\":", idc, r->stops, r->minutes, r->transfers);
    add_int_or_null(r->walk_m); add(",\"km\":"); add_dbl_or_null(r->km); add(",\"fare\":"); add_int_or_null(r->fare);
    add(",\"xfer_min\":"); add_int_or_null(r->xfer_min); add(",\"wait_min\":"); add_int_or_null(r->wait_min);
    add(",\"total_est\":"); add_int_or_null(r->total_est);
    add(",\"best_for\":[");
    int first = 1;
    for (int p = P_FASTEST; p < NP; p++) if (plan.best[p] == id) { add("%s\"%s\"", first ? "" : ",", pkey[p]); first = 0; }
    add("],\"path\":[");
    for (int i = 0; i < r->n; i++) {
        int ln = r->line[i], change = i > 0 && i < r->n - 1 && ln != r->line[i - 1];
        add("%s{\"station\":", i ? "," : ""); add_str(name[r->path[i]]); add(",\"line\":"); add_str(lname[ln]);
        add(",\"change\":%s}", change ? "true" : "false");
    }
    add("],\"segments\":[");
    first = 1;
    for (int a = 0; a < r->n - 1; ) {                           /* one segment = a run of stops on the same line */
        int L = r->line[a], b = a, mins = 0;
        while (b < r->n - 1 && r->line[b] == L) { mins += edge_on(r->path[b], r->path[b + 1], L)->w; b++; }
        int pa = line_pos(L, r->path[a]), pb = line_pos(L, r->path[b]);
        add("%s{\"line\":", first ? "" : ","); add_str(lname[L]);
        add(",\"from\":"); add_str(name[r->path[a]]); add(",\"to\":"); add_str(name[r->path[b]]);
        add(",\"toward\":"); add_str(name[lst[L][pb > pa ? lcnt[L] - 1 : 0]]);
        add(",\"stops\":%d,\"minutes\":%d}", b - a, mins);
        first = 0; a = b;
    }
    add("]}");
}
static void json_route(const char *from, const char *to, const char *priority, const char *mode) {
    int s = find(from), d = find(to);
    if (s < 0 || d < 0) { json_error("Choose both stations from the list."); return; }
    if (s == d)         { json_error("Start and destination are the same station."); return; }
    int p = P_ALL;
    if (*priority) p = parse_priority(priority);               /* new API */
    else if (!strcmp(mode, "stops")) p = P_SHORTEST;           /* old API: mode=stops / mode=time */
    else if (!strcmp(mode, "time"))  p = P_FASTEST;
    if (p < 0) { json_error("Unknown priority. Use all, fastest, cheapest, transfers, walking or shortest."); return; }
    clock_t t0 = clock();
    int rc = make_plan(s, d);
    long us = (long)((clock() - t0) * 1000000.0 / CLOCKS_PER_SEC);      /* time spent searching, in microseconds */
    if (rc == 1) { json_error("A selected station is currently closed."); return; }
    if (rc == 2) { json_error("No route between these stations."); return; }
    int rec = plan.best[p]; const Route *r = &plan.r[rec];
    /* Fields up to "path" keep the old format (recommended route), so older pages still work. */
    add("{\"ok\":true,\"mode\":\"%s\",\"priority\":\"%s\",\"priority_label\":", p == P_SHORTEST ? "stops" : "time", pkey[p]); add_str(plabel[p]);
    add(",\"from\":"); add_str(name[s]); add(",\"to\":"); add_str(name[d]);
    add(",\"stops\":%d,\"minutes\":%d,\"interchanges\":%d,\"path\":[", r->stops, r->minutes, r->transfers);
    for (int i = 0; i < r->n; i++) {
        int ln = r->line[i], change = i > 0 && i < r->n - 1 && ln != r->line[i - 1];
        add("%s{\"station\":", i ? "," : ""); add_str(name[r->path[i]]); add(",\"line\":"); add_str(lname[ln]);
        add(",\"change\":%s}", change ? "true" : "false");
    }
    add("],\"recommended\":%d,\"min_stops\":%d,\"elapsed_us\":%ld,\"best\":{", rec, plan.min_hops, us);
    for (int q = P_ALL; q < NP; q++) add("%s\"%s\":%d", q > P_ALL ? "," : "", pkey[q], plan.best[q]);
    add("},\"routes\":[");
    for (int i = 0; i < plan.nr; i++) { if (i) add(","); json_route_obj(&plan.r[i], i); }
    add("]}");
}

static int send_all(SOCKET c, const char *buf, int len) {
    while (len > 0) { int k = send(c, buf, len, 0); if (k <= 0) return 0; buf += k; len -= k; }
    return 1;
}
static void reply(SOCKET c, int code, const char *type, const char *body, int len) {
    char h[256];
    int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
                      code, code == 200 ? "OK" : "Not Found", type, len);
    if (send_all(c, h, hl) && len) send_all(c, body, len);
}
static void serve(int port) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#else
    signal(SIGPIPE, SIG_IGN);              /* a client that disconnects must not kill the server */
#endif
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0); int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((unsigned short)port);
    if (srv == INVALID_SOCKET || bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 16) < 0) { fprintf(stderr, "Cannot listen on port %d\n", port); exit(1); }
    printf("Metro Route Finder: %d stations, %d lines. Listening on 0.0.0.0:%d\n", n, nl, port); fflush(stdout);
    for (;;) {
        SOCKET c = accept(srv, NULL, NULL); if (c == INVALID_SOCKET) continue;
#ifdef _WIN32
        DWORD tmo = 3000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
#else
        struct timeval tmo = { 3, 0 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
#endif                                     /* idle connections (health probes) must not block the server */
        char req[8192]; int got = recv(c, req, sizeof req - 1, 0);
        if (got <= 0) { CLOSE(c); continue; }
        req[got] = 0;
        char path[512] = "", *q = NULL;
        if (sscanf(req, "GET %511s", path) != 1) { reply(c, 404, "text/plain", "Not found", 9); CLOSE(c); continue; }
        if ((q = strchr(path, '?'))) *q++ = 0;
        off = 0;
        if (!strcmp(path, "/api/health"))        { add("{\"status\":\"ok\",\"stations\":%d}", n); reply(c, 200, "application/json", out, off); }
        else if (!strcmp(path, "/api/stations")) { json_stations(); reply(c, 200, "application/json", out, off); }
        else if (!strcmp(path, "/api/network"))  { json_network();  reply(c, 200, "application/json", out, off); }
        else if (!strcmp(path, "/api/route")) {
            char f[128], t[128], m[16], pr[16];
            qparam(q, "from", f, sizeof f); qparam(q, "to", t, sizeof t); qparam(q, "mode", m, sizeof m); qparam(q, "priority", pr, sizeof pr);
            json_route(f, t, pr, m); reply(c, 200, "application/json", out, off);
        } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
            FILE *fp = fopen("frontend/index.html", "rb");
            if (!fp) reply(c, 404, "text/plain", "frontend/index.html missing", 27);
            else { off = (int)fread(out, 1, sizeof out, fp); fclose(fp); reply(c, 200, "text/html; charset=utf-8", out, off); }
        } else reply(c, 404, "text/plain", "Not found", 9);
        CLOSE(c);
    }
}

int main(int argc, char **argv) {
    for (int i = 0; i < MAXN; i++) { xwalk[i] = -1; xmin[i] = -1; }
    for (int l = 0; l < MAXL; l++) headway[l] = -1;
    if (!load("data/stations.txt")) { fprintf(stderr, "Run from the project folder: data/stations.txt not found.\n"); return 1; }
    load_extras("data/extras.txt");
    load_fares("data/fares.txt");
    finish_load();
    if (argc > 1 && !strcmp(argv[1], "--cli")) { cli(); return 0; }
    const char *p = getenv("PORT"); int port = p ? atoi(p) : 0;
    serve(port > 0 ? port : 8080);
    return 0;
}
