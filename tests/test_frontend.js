// UI tests: loads the real page in jsdom against the real C server.
// Run from the project folder:  gcc -O2 -o metro metro.c && cd tests && npm install && node test_frontend.js
// GRAPH=0 node test_frontend.js   tests the version without the route map.
const { JSDOM } = require('jsdom');
const { spawn } = require('child_process');
const path = require('path'), fs = require('fs'), os = require('os'), net = require('net');

const ROOT = path.resolve(__dirname, '..');
const GRAPH = process.env.GRAPH !== '0';
console.log(GRAPH ? 'VARIANT: with route map' : 'VARIANT: no route map');
let passed = 0, failed = 0;
const check = (c, m) => { if (c) passed++; else { failed++; console.log('  FAIL:', m); } };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const freePort = () => new Promise(res => { const s = net.createServer().listen(0, () => { const p = s.address().port; s.close(() => res(p)); }); });
async function until(fn, msg, ms = 4000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { try { if (fn()) return true; } catch (e) {} await sleep(30); }
  check(false, 'timeout waiting for: ' + msg); return false;
}
const NODATA = /\bfare\b|₹|distance|\d\s*km\b/i;     // must never appear in the results

async function startServer(stationsText) {
  let cwd = ROOT;
  if (stationsText) {
    cwd = fs.mkdtempSync(path.join(os.tmpdir(), 'metro-'));
    fs.cpSync(path.join(ROOT, 'data'), path.join(cwd, 'data'), { recursive: true });
    fs.symlinkSync(path.join(ROOT, 'frontend'), path.join(cwd, 'frontend'));
    fs.writeFileSync(path.join(cwd, 'data', 'stations.txt'), stationsText);
  }
  const port = await freePort();
  const proc = spawn(path.join(ROOT, 'metro'), [], { cwd, env: { ...process.env, PORT: String(port) }, stdio: 'ignore' });
  const base = 'http://127.0.0.1:' + port + '/';
  for (let i = 0; i < 50; i++) { try { await fetch(base + 'api/health'); break; } catch (e) { await sleep(100); } }
  return { base, stop() { proc.kill(); if (stationsText) fs.rmSync(cwd, { recursive: true, force: true }); } };
}
async function openPage(base) {
  const errors = []; const calls = { route: 0 };
  const dom = await JSDOM.fromURL(base, {
    runScripts: 'dangerously', resources: 'usable', pretendToBeVisual: true,
    beforeParse(w) {
      w.fetch = (u, o) => { if (String(u).includes('/api/route')) calls.route++; return fetch(new URL(u, base), o); };
      w.HTMLElement.prototype.scrollIntoView = function () {};
      w.addEventListener('error', e => errors.push(e.message));
    }
  });
  const w = dom.window, d = w.document;
  await until(() => d.getElementById('example'), 'page loaded'); await sleep(300);
  const $ = id => d.getElementById(id), R = $('result');
  const api = {
    w, d, $, R, errors, calls, dom,
    prio: p => { const r = $('p-' + p); r.checked = true; r.dispatchEvent(new w.Event('change', { bubbles: true })); },
    submit: async (a, b) => { if (a !== undefined) { $('from').value = a; $('to').value = b; } $('f').dispatchEvent(new w.Event('submit', { cancelable: true })); await until(() => R.querySelector('.sum') || R.querySelector('.err'), 'result'); await sleep(40); },
    openMap: async () => { const md = $('mapd'); if (!md.open) { md.open = true; md.dispatchEvent(new w.Event('toggle')); } await sleep(40); },
    sel: () => R.querySelector('tr.sel .rbtn').textContent,
    setOpen: async (key, v = true) => { const md = d.querySelector('details[data-key=' + key + ']'); md.open = v; md.dispatchEvent(new w.Event('toggle')); await sleep(40); },
  };
  return api;
}

(async () => {
  const srv = await startServer();
  try {
    const P = await openPage(srv.base); const { w, d, $, R } = P;

    console.log('1. Initial state');
    check(d.documentElement.dataset.theme === 'light', 'light theme by default');
    check($('p-fastest').checked, 'Fastest is the default priority');
    check([...d.querySelectorAll('.chips label')].map(l => l.textContent).join('|') === 'Fastest|Fewest Interchanges|Shortest', 'exactly three priorities: Fastest, Fewest Interchanges, Shortest');
    check(!d.querySelector('#p-all') && !d.querySelector('#p-walking') && !d.querySelector('#p-cheapest'), 'no All Routes / Less Walking / Cheapest');
    check(d.querySelector('label[for=p-transfers]').textContent === 'Fewest Interchanges', 'named Fewest Interchanges');
    check(R.innerHTML.trim() === '', 'no result before searching');

    console.log('2. Nothing is searched until "Find route" is pressed');
    const pickFirst = async id => { const inp = $(id); inp.dispatchEvent(new w.Event('focus')); await sleep(20); const li = $('dd-' + id).querySelector('li[data-i]'); li.dispatchEvent(new w.MouseEvent('mousedown', { bubbles: true, cancelable: true })); await sleep(20); };
    await pickFirst('from'); await pickFirst('to');
    check($('from').value !== '' && $('to').value !== '', 'stations chosen from the dropdown: ' + $('from').value + ' / ' + $('to').value);
    check(P.calls.route === 0 && R.innerHTML.trim() === '', 'choosing stations does not search');
    $('swap').click(); await sleep(30);
    check(P.calls.route === 0 && R.innerHTML.trim() === '', 'swap does not search');
    P.prio('shortest'); await sleep(30);
    check(P.calls.route === 0 && R.innerHTML.trim() === '', 'changing priority before a search does not search');
    P.prio('fastest');
    await P.submit('Wimco Nagar Depot', 'St. Thomas Mount');
    check(P.calls.route === 1, 'pressing Find route searches exactly once');
    const trip1 = R.querySelector('.trip').textContent;
    check(trip1.startsWith('Wimco Nagar Depot'), 'result shown');
    check($('stalehint').hidden && !R.classList.contains('stale'), 'fresh result is not marked stale');
    $('from').value = 'Guindy'; $('from').dispatchEvent(new w.Event('input', { bubbles: true })); await sleep(20);
    check(P.calls.route === 1 && R.querySelector('.trip').textContent === trip1, 'editing a station does not replace the result');
    check(!$('stalehint').hidden && R.classList.contains('stale'), 'old result greyed out with a "press Find route" reminder');
    $('swap').click(); await sleep(20);
    check(P.calls.route === 1, 'swap after a result still does not search');
    $('from').value = 'Wimco Nagar Depot'; $('to').value = 'St. Thomas Mount'; $('from').dispatchEvent(new w.Event('input', { bubbles: true })); await sleep(20);
    check($('stalehint').hidden, 'reminder clears when the stations match the result again');

    console.log('3. Result content and dropdowns');
    check(R.querySelectorAll('.sum > div').length === 3, 'three summary tiles');
    check(/^Travel time \(est\.\)\|Stops\|Interchanges?$/.test([...R.querySelectorAll('.sum span')].map(x => x.textContent).join('|')), 'tiles: time, stops, interchanges');
    check(!NODATA.test(R.textContent), 'no fare/distance/km text in the results: ' + (R.textContent.match(NODATA) || [''])[0]);
    check(!/\bRoute [A-Z]\b/.test(R.textContent) && !R.querySelector('.badge'), 'no "Route A/B" labels anywhere');
    check(!/All Routes|Less Walking|Cheapest|Walking|Default pick/.test(R.textContent), 'no All Routes / Less Walking / Cheapest / walking text');
    check(R.querySelector('#h-rec').textContent === 'Recommended for: Fastest', 'heading names the priority');
    const keys = GRAPH ? ['opt', 'map', 'cmp', 'notes'] : ['opt', 'cmp', 'notes'];
    const dds = keys.map(k => d.querySelector('details[data-key=' + k + ']'));
    check(dds.every(x => x && !x.open), 'dropdowns exist and are closed by default: ' + keys.join(','));
    check(dds.map(x => x.querySelector('summary').textContent).join('|').includes('Fastest, Fewest Interchanges and Shortest') && /Compare all routes \(\d+\)/.test(R.textContent) && R.textContent.includes('About the numbers'), 'dropdown titles');
    check(R.querySelector('.sum').closest('section').querySelector('details[data-key=notes]'), 'About the numbers is a dropdown inside the result card');
    const order = keys.filter(k => k !== 'notes').map(k => d.querySelector('details[data-key=' + k + ']'));
    check(order.every((x, i) => i === 0 || (order[i - 1].compareDocumentPosition(x) & 4)), 'dropdown order: ' + keys.filter(k => k !== 'notes').join(' > '));
    check(![...R.querySelectorAll('table.cmp th')].some(t => /fare|distance|walking/i.test(t.textContent)), 'comparison table has no fare/distance/walking column');
    check([...R.querySelectorAll('table.cmp th')].map(t => t.textContent).join('|') === 'Route|Travel time (est.)|Stops|Interchanges', 'table columns');
    check(R.querySelectorAll('table.cmp tbody tr').length >= 2, 'alternatives listed');
    const names = [...R.querySelectorAll('.rbtn')].map(b => b.textContent);
    check(names.every(n => /^(Fastest|Fewest Interchanges|Shortest|Alternative \d)/.test(n)), 'routes have plain names: ' + names.join(' / '));
    check(R.textContent.includes('sample values'), 'times labelled as sample/estimate');
    await P.setOpen('cmp'); await P.setOpen('opt');
    P.prio('transfers'); await sleep(40);
    check(d.querySelector('details[data-key=cmp]').open && d.querySelector('details[data-key=opt]').open, 'dropdowns stay open when the page redraws');
    P.prio('fastest'); await sleep(30);
    await P.setOpen('cmp', false); await P.setOpen('opt', false);

    console.log('4. Route map: a dropdown placed before the comparison');
    const src = await (await fetch(srv.base)).text();
    if (GRAPH) {
      const md = $('mapd'), table = R.querySelector('table.cmp');
      check(md && md.tagName === 'DETAILS' && md.querySelector('summary').textContent === 'Route map', 'Route map is a <details> dropdown');
      check(!md.open && !R.querySelector('#graph svg'), 'closed by default, nothing drawn yet');
      check(md.querySelector('summary').textContent === 'Route map', 'dropdown titled Route map');
      check(md.compareDocumentPosition(table) & 4, 'map comes before the comparison');
      check(md.compareDocumentPosition(R.querySelector('#h-dir')) & 4, 'and before the directions');
      await P.openMap();
      const svg = R.querySelector('#graph svg');
      check(svg, 'opening draws the map');
      check(!svg.hasAttribute('width') && svg.getAttribute('viewBox'), 'scales to any screen width (no fixed pixel width)');
      const nodes = [...svg.querySelectorAll('circle.nd, rect.nd')];
      check(nodes.length <= 6, 'simple: only key stations drawn (' + nodes.length + ' nodes)');
      const nt = nodes.map(n => n.querySelector('title').textContent);
      check(nt.some(t => t.includes('Wimco Nagar Depot (start)')) && nt.some(t => t.includes('St. Thomas Mount (destination)')), 'start and destination drawn');
      check(nt.filter(t => t.includes('interchange')).length === 2, 'both interchanges drawn');
      check(!svg.querySelector('.minor') && !$('allnames'), 'no per-station label clutter or toggles');
      check(R.querySelectorAll('#legend li').length >= 6 && R.querySelector('#legend').textContent.includes('Blue line'), 'compact legend with the real lines');
      // every drawn stretch is a real stretch of a real route
      const api = await (await fetch(srv.base + 'api/route?from=Wimco%20Nagar%20Depot&to=St.%20Thomas%20Mount')).json();
      const real = new Set(api.routes.flatMap(r => r.segments.flatMap(s => [`${s.line}|${s.from}|${s.to}|${s.stops}|${s.minutes}`, `${s.line}|${s.to}|${s.from}|${s.stops}|${s.minutes}`])));
      const edges = [...svg.querySelectorAll('line.e title')].map(t => t.textContent);
      const bad = edges.filter(t => { const m = t.match(/^(\w+) line: (.*) to (.*) \((\d+) stops?, (\d+) min\)$/); return !m || !real.has(`${m[1]}|${m[2]}|${m[3]}|${m[4]}|${m[5]}`); });
      check(edges.length >= 3 && bad.length === 0, 'every drawn stretch matches the real route data (' + edges.length + ' stretches, ' + bad.length + ' bad)');
      check(!svg.querySelector('.chiptx') && !/\bRoute [A-Z]\b/.test(svg.textContent), 'no route letters on the map');
      check(svg.querySelectorAll('line.e.sel').length > 0 && svg.querySelectorAll('line.e.dim').length > 0, 'selected route solid, others dashed');
      // narrow screen: redraw as if the container were 320 px wide
      Object.defineProperty($('graph'), 'clientWidth', { configurable: true, value: 320 });
      w.dispatchEvent(new w.Event('resize')); await sleep(300);
      const vb = R.querySelector('#graph svg').getAttribute('viewBox').split(' ').map(Number);
      check(vb[2] < 420, 'phone width: map fits in about one screen width (viewBox ' + vb[2] + ')');
      Object.defineProperty($('graph'), 'clientWidth', { configurable: true, value: 900 });
      w.dispatchEvent(new w.Event('resize')); await sleep(300);
      check(/drawGraph/.test(src), 'page contains the map code');
    } else {
      check(!/drawGraph|gscroll|<svg viewBox|id="legend"|mapd/.test(src), 'no map code in the page source');
      check(!$('mapd') && !R.querySelector('#graph') && !R.querySelector('svg') && !/graph|route map/i.test(R.innerHTML), 'no map or graph anywhere in the results');
    }

    console.log('5. Each priority updates the recommendation (and the map highlight)');
    const edgeCount = GRAPH ? R.querySelectorAll('#graph line.e').length : 0;
    for (const p of ['fastest', 'transfers', 'shortest', 'fastest']) {
      P.prio(p); await sleep(40);
      const label = { fastest: 'Fastest', transfers: 'Fewest Interchanges', shortest: 'Shortest' }[p];
      check(R.querySelector('#h-rec').textContent === 'Recommended for: ' + label, p + ': heading');
      check(R.querySelector('.why').textContent.length > 30, p + ': explanation');
      check(P.calls.route === 1, p + ': no new search needed');
      if (GRAPH) {
        check(R.querySelectorAll('#graph line.e.sel').length > 0 && R.querySelectorAll('#graph line.e').length === edgeCount, p + ': selected route highlighted, same stretches drawn');
        check(R.querySelectorAll('#graph line.e.dim').length > 0, p + ': other routes dashed');
      }
      check(!NODATA.test(R.textContent) && !/\bRoute [A-Z]\b/.test(R.textContent), p + ': no fare/distance, no route letters');
    }
    P.prio('shortest'); await sleep(30);
    check(/shortest route: <strong>|shortest route: \d+ stops/.test(R.querySelector('.why').innerHTML) || /This is the shortest route/.test(R.querySelector('.why').textContent), 'shortest explained');
    P.prio('transfers'); await sleep(30);
    check(R.querySelector('.tips').textContent.includes('Fastest vs Fewest Interchanges'), 'trade-off explained');
    P.prio('fastest');

    console.log('6. Fastest / Fewest Interchanges / Shortest: identical or different');
    await P.submit('Guindy', 'Little Mount');
    check(R.querySelectorAll('.opt').length === 1 && /All three options are the same route/.test(R.querySelector('.same').textContent) && !/\bRoute [A-Z]\b/.test(R.querySelector('.same').textContent), 'adjacent stations: all three identical, said clearly');
    await P.submit('Puratchi Thalaivar Dr. M.G. Ramachandran Central', 'Ekkattuthangal');
    check(R.querySelectorAll('.opt').length >= 2 && !/All three options are the same/.test(R.querySelector('.same').textContent), 'Central -> Ekkattuthangal: options really differ');
    const ot = [...R.querySelectorAll('.opt')].map(o => o.textContent).join('|');
    check(/Fastest/.test(ot) && /Fewest Interchanges/.test(ot) && !NODATA.test(ot), 'differing options labelled, no fare/distance');
    await P.submit('Wimco Nagar Depot', 'Chennai International Airport');
    check(R.querySelectorAll('.opt').length === 1, 'Wimco -> Airport: all three identical');

    console.log('7. Directions and route selection');
    await P.submit('Guindy', 'Egmore'); P.prio('fastest'); await sleep(30);
    check(R.querySelector('#h-dir').textContent === 'Directions', 'Directions heading has no route letter');
    const dir = R.querySelector('#h-dir').closest('section');
    check(dir.querySelector('.xchg') && /Change at Puratchi/.test(dir.querySelector('.xchg').textContent), 'interchange instruction');
    check(dir.querySelectorAll('.stop').length >= 10 && /toward/.test(dir.textContent), 'station-by-station list with direction');
    check(!/not available|walking/i.test(dir.querySelector('.xchg').textContent), 'interchange instruction has no "not available" clutter');
    const before = P.sel(), btns = [...R.querySelectorAll('.rbtn')];
    if (GRAPH) { await P.openMap(); }
    await P.setOpen('cmp'); btns[btns.length - 1].click(); await sleep(40);
    check(P.sel() !== before || btns.length === 1, 'clicking a route selects it');
    check(R.querySelector('#h-dir').closest('section').querySelector('.sub').textContent.startsWith(P.sel()), 'directions follow the selection (' + P.sel() + ')');
    check(d.querySelector('details[data-key=cmp]').open, 'compare dropdown stays open after choosing a route');
    if (GRAPH) check($('mapd').open && R.querySelector('#graph svg') && R.querySelectorAll('#graph line.e.sel').length > 0, 'map stays open and follows the selection');
    P.prio('fastest'); await sleep(30); check(/^Fastest/.test(P.sel()), 'priority change resets the selection to its recommendation');

    console.log('8. Errors');
    await P.submit('Nowhere', 'Guindy'); check(R.querySelector('.err'), 'invalid station');
    await P.submit('Guindy', 'Guindy'); check(R.querySelector('.err').textContent.includes('same station'), 'same station');
    await P.submit('Guindy', 'Egmore'); check(R.querySelector('.sum'), 'recovers after an error');

    console.log('9. Theme toggle keeps everything');
    await P.submit('Wimco Nagar Depot', 'St. Thomas Mount'); P.prio('transfers'); await sleep(30);
    await P.setOpen('cmp'); if (GRAPH) await P.openMap();
    const snap = { from: $('from').value, to: $('to').value, prio: d.querySelector('input[name=prio]:checked').value, sel: P.sel(), html: R.innerHTML, open: GRAPH ? $('mapd').open : null, cmp: d.querySelector('details[data-key=cmp]').open };
    $('theme').click(); await sleep(20);
    check(d.documentElement.dataset.theme === 'dark' && $('theme').getAttribute('aria-pressed') === 'true', 'dark mode on');
    check($('from').value === snap.from && $('to').value === snap.to && d.querySelector('input[name=prio]:checked').value === snap.prio, 'stations and priority kept');
    check(P.sel() === snap.sel && R.innerHTML === snap.html, 'results (and map) untouched by the theme switch');
    check(d.querySelector('details[data-key=cmp]').open === snap.cmp && (!GRAPH || $('mapd').open === snap.open), 'dropdowns keep their state');
    check(!(GRAPH && [...R.querySelectorAll('#graph svg [style]')].some(n => /#[0-9a-f]{3,6}/i.test(n.getAttribute('style')))), 'map has no hard-coded colours');
    const css = d.querySelector('style').textContent;
    const darkBlock = css.slice(css.indexOf(':root[data-theme="dark"]{'), css.indexOf('*{box-sizing'));
    const lightBlock = css.slice(css.indexOf(':root{'), css.indexOf(':root[data-theme="dark"]{'));
    const vars = b => new Set([...b.matchAll(/--([\w-]+):/g)].map(m => m[1]));
    const missing = [...vars(lightBlock)].filter(v => !vars(darkBlock).has(v) && v !== 'serif');
    check(missing.length === 0, 'dark theme redefines every colour variable (missing: ' + missing.join(',') + ')');
    $('theme').click(); await sleep(20);
    check(d.documentElement.dataset.theme === 'light' && R.innerHTML === snap.html && w.localStorage.getItem('metro-theme') === 'light', 'back to light, same result, choice remembered');

    console.log('10. Responsive CSS and contrast');
    check(/@media \(max-width:700px\)/.test(css) && /\.sum div\{padding-left:10px\}/.test(css), 'mobile rules present');
    check(/overflow-x:auto/.test(css), 'wide table scrolls inside its own box');
    const parse = b => { const o = {}; for (const m of b.matchAll(/--([\w-]+):\s*(#[0-9a-fA-F]{6})/g)) o[m[1]] = m[2]; return o; };
    const lum = h => { const c = [1, 3, 5].map(i => parseInt(h.substr(i, 2), 16) / 255).map(v => v <= .03928 ? v / 12.92 : Math.pow((v + .055) / 1.055, 2.4)); return .2126 * c[0] + .7152 * c[1] + .0722 * c[2]; };
    const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + .05) / (Math.min(x, y) + .05); };
    for (const [nm, th] of [['light', parse(lightBlock)], ['dark', { ...parse(lightBlock), ...parse(darkBlock) }]])
      for (const [fg, bg] of [['ink', 'panel'], ['soft', 'panel'], ['muted', 'panel'], ['btn-ink', 'btn'], ['err', 'panel'], ['accent', 'panel'], ['l-blue', 'panel'], ['l-green', 'panel'], ['ink', 'label-halo']]) {
        const r = ratio(th[fg], th[bg]); check(r >= (fg.startsWith('l-') ? 3 : 4.5), nm + ' contrast ' + fg + ' on ' + bg + ' = ' + r.toFixed(2));
      }
    check(P.errors.length === 0, 'no JS errors: ' + P.errors.join('; '));
    P.dom.window.close();

    console.log('11. Network where Fastest and Fewest Interchanges differ');
    const s2 = await startServer('#line Red\nS,10\nT,0\n\n#line Blue\nS,1\nX,0\n\n#line Green\nX,1\nY,0\n\n#line Orange\nY,1\nT,0\n');
    try {
      const Q = await openPage(s2.base); const { $: q$, R: R2 } = Q;
      await Q.submit('S', 'T'); Q.prio('fastest'); await sleep(30);
      const f = R2.querySelector('.rec-head').textContent; const fs_ = R2.querySelector('.sum').textContent;
      Q.prio('transfers'); await sleep(30);
      check(R2.querySelector('.rec-head').textContent !== f && R2.querySelector('.sum').textContent !== fs_, 'recommendation and numbers change between Fastest and Fewest Interchanges');
      check(R2.querySelectorAll('.opt').length >= 2, 'different options shown separately');
      check(/saves 2 interchanges but is 7 min longer/.test(R2.querySelector('.tips').textContent), 'trade-off sentence: ' + R2.querySelector('.tips').textContent.slice(0, 120));
      if (GRAPH) {
        await Q.openMap();
        check(R2.querySelectorAll('#graph line.e.sel').length === 1 && R2.querySelectorAll('#graph line.e.dim').length > 0, 'map highlights the direct route, dashes the other');
        Q.prio('fastest'); await sleep(40);
        check(R2.querySelectorAll('#graph line.e.sel').length === 3, 'map highlights the 3-stretch fastest route');
      }
      Q.prio('fastest'); await sleep(30);
      check(R2.querySelectorAll('.rbtn').length >= 2 && !/\bRoute [A-Z]\b/.test(R2.textContent), 'plain route names in the trade-off network');
      check(R2.querySelectorAll('.xchg').length === 2, 'two interchange instructions');
      check(Q.errors.length === 0, 'no JS errors (network 2)');
      Q.dom.window.close();
    } finally { s2.stop(); }

    console.log('12. Even with km data in the file, the page shows no distance');
    const s3 = await startServer('#line Red\nS,2,5.0\nM,2,5.0\nT,0\n\n#line Blue\nS,6,9.0\nT,0\n\n#line Green\nS,1,2.5\nX,1,2.5\nT,0\n');
    try {
      const Z = await openPage(s3.base); const { R: R3 } = Z;
      await Z.submit('S', 'T');
      check(!NODATA.test(R3.textContent.replace(/least distance/g, '')), 'no km or distance figures shown');
      Z.prio('shortest'); await sleep(30);
      check(!/\d\s*km/i.test(R3.textContent), 'still no km figure after choosing Shortest');
      check(/least distance/.test(R3.querySelector('.why').textContent), 'Shortest follows the data but shows no number: ' + R3.querySelector('.why').textContent.slice(0, 80));
      Z.dom.window.close();
    } finally { s3.stop(); }
  } finally { srv.stop(); }
  console.log('\n' + passed + ' checks passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
