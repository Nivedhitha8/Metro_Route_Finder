# Metro Route Finder (C, graph algorithms) - version without graphs

A DSA project: stations are graph vertices, tracks are weighted edges. The C program is also the web
server, so the whole site is one small binary (no database, no runtime dependencies).

## What it does
- **Priorities**: Fastest (default), Fewest Interchanges, Shortest. (The API still also accepts `all`, `walking` and `cheapest`.)
- Extra detail sits in **dropdowns, closed by default**: the three options side by side, the route map, a comparison of all
  routes, and "About the numbers". Routes are named by what they win (Fastest, Fewest Interchanges, Shortest) or
  "Alternative 1", never "Route A".
- **A search runs only when you press Find route** (or Enter, or Try an example). Picking a station, swapping or changing a
  station after a result never searches by itself; the old result is greyed out with a reminder. Changing the priority
  re-ranks the routes already found, instantly.
- **Fastest, Fewest Interchanges and Shortest side by side** (in a dropdown). When they are the same route (this is the normal
  case on this network) the page says so instead of showing three copies.
- Every route shows **estimated travel time, stops and interchanges**. Fares and distances are not displayed on the page
  (the API and data files still support them, see below).
- The other real routes (alternatives), a comparison table and an explanation of the trade-offs.
- Station-by-station directions with interchange instructions.
- Light theme by default, optional dark theme (remembered in the browser). Responsive on phones.
## How routes are found (nothing is random or hard-coded)
Every route is a real path through the stations in `data/stations.txt`. Different priorities can, and often do,
return the *same* route. They only differ when the network really offers a trade-off.

| Need | Algorithm |
|---|---|
| Is the destination reachable? Minimum number of stops | BFS, O(V + E) |
| Best route for a priority | Dijkstra with a binary min-heap on (station, line) pairs, O((V + E) log V) |
| Alternative routes | DFS / backtracking over routes that never visit a station twice |

A route is described by numbers (time, distance, stops, interchanges, walking, fare). A priority is only the
**order** in which they are compared; the first is minimised and the next ones break ties:

| Priority | Order compared |
|---|---|
| Fastest | time, interchanges, distance, stops |
| Fewest Interchanges | interchanges, time, distance, stops |
| Shortest | **distance (km)**, stops, time, interchanges |
| Less Walking (API only) | walking, interchanges, time |
| Cheapest (API only; hidden in the page) | fare (verified rules only), stops, time |

If a number is not known for the whole network it counts as 0 for every route and the next one decides. So:
- **Shortest = least km only when every track segment has a km value.** Otherwise it means fewest stops and the
  page says so. A straight-line or estimated distance is never substituted.
- Alternatives slower than 2x the fastest route + 10 minutes are hidden as impractical (the priority winners are kept).

## Data honesty: what is and is not available
Nothing is invented. Missing data shows as **Not available**.
- **Distance (km): not loaded yet, and not shown on the page.** `data/stations.txt` had no distances and no verified per-segment source was
  found (public pages only give whole-line lengths, and they disagree). Add the official km after the minutes:
  `Guindy,3,1.25` (minutes to the next station, then km to the next station). Fill **every** segment and the API
  returns `km` and Shortest Path switches to least distance. Restart the program.
- **Travel time**: the minutes in `data/stations.txt` are approximate sample values and are labelled as estimates.
  It is in-train time. Waiting and interchange time need `data/extras.txt` (headways and transfer minutes); the
  longer estimate appears only when every needed value exists. Replace the sample minutes with timetable values for
  daily-use accuracy.
- **Fares**: no fare rules are loaded and none are shown on the page (the official table could not be verified). See `data/fares.txt`.
- **Walking**: not in the data. *Less Walking* uses fewer interchanges as a stand-in until `data/extras.txt` has it.

## Speed
A search takes about 10 microseconds (median) and under 0.2 ms in the worst case on this network, measured by the
server itself. A full HTTP request takes under 1 ms locally. The tests fail if the 99th percentile exceeds 50 ms.

## Project files
- `metro.c` - graph, algorithms, JSON API and web server
- `data/stations.txt` - lines, stations, minutes (and optional km)
- `data/extras.txt` - optional: interchange walking/transfer time, train headways, closed stations
- `data/fares.txt` - fare rules (inactive until verified)
- `frontend/index.html` - the website (plain HTML/CSS/JavaScript, no libraries)
- `tests/` - developer tests (not used by Docker/Render)
- `Dockerfile`, `render.yaml`, `run-local.sh`, `run-local.bat` - run and deploy

## Run on localhost
1. Install **gcc** (Windows: MinGW-w64 or MSYS2; Linux/macOS: usually present or `xcode-select --install`).
2. Double-click `run-local.bat` (Windows) or run `./run-local.sh`.
3. Open http://localhost:8080

Console version: `gcc -O2 -o metro metro.c` then `./metro --cli`
(Windows: `gcc -O2 -o metro.exe metro.c -lws2_32`, then `metro.exe --cli`).
Start the program from the project folder so it can find `data/` and `frontend/`.

## API
- `GET /api/health`
- `GET /api/stations`
- `GET /api/network` - lines with stations, all tracks, interchanges, and which data is available
- `GET /api/route?from=Guindy&to=Egmore&priority=all`
  `priority` = `all | fastest | cheapest | transfers | walking | shortest` (the older `mode=time|stops` still works).
  Keeps the original fields (`stops`, `minutes`, `interchanges`, `path`) for the recommended route and adds
  `routes[]` (numbers, path, segments; `km` is `null` when unknown), `best{priority: route index}`,
  `recommended`, `min_stops` and `elapsed_us`. Errors: `{"ok":false,"error":"..."}`.

## Tests
```
gcc -O2 -o metro metro.c
python3 tests/test_backend.py          # server vs an independent brute-force solver, all station pairs, speed
cd tests && npm install && node test_frontend.js   # real page in jsdom (dev only)
```

## Deploy on Render
No database, no secrets, nothing to configure.
1. Push this folder to a GitHub repository (the `Dockerfile` must be at the repo root).
2. On [render.com](https://render.com): **New +** > **Web Service** > connect the repo
   (or **New +** > **Blueprint**, which reads `render.yaml`).
3. **Runtime**: Docker. Leave Build/Start commands empty. Free plan is fine.
4. **Health Check Path**: `/api/health` (already set by the Blueprint).
5. **Create Web Service**. Your site is live at `https://<name>.onrender.com`.

**Redeploy after changes:** commit and push to the connected branch. Render rebuilds automatically when
Auto-Deploy is on (default). Otherwise: service > **Manual Deploy** > **Deploy latest commit**
(use **Clear build cache & deploy** if a build looks stale).

Render sets `PORT`; the server binds `0.0.0.0:$PORT`. The frontend uses relative `/api/...` URLs.
On the free plan the service sleeps when idle, so the first request can take 30-60 s.
Local Docker: `docker build -t metro . && docker run -e PORT=8080 -p 8080:8080 metro`

## Prepared for later (only built when reliable data exists)
- **Closed stations / disruption rerouting**: `closed | Station` in `data/extras.txt` makes every search route
  around it. There is no live feed.
- **Walking / feeder-bus links**, **accessibility preferences**, **interactive map**: need data that is not
  available yet (walking distances, lift data, coordinates). `/api/network` already returns stations, tracks and lines.
