"""Which functions of Woody.exe are named nowhere in docs/, src/ or tools/ ("not analysed yet").

A function counts as covered when any address inside it (0x4xxxxx) appears in a doc, a source file or a tool.
Markers in disasm_full.txt that are only jump targets (no direct caller, no pointer to them anywhere in the image)
are merged into the preceding real function, so a switch case of a covered function is covered too.

usage: coverage.py out/disasm_full.txt game/Woody.exe [--min N] [--all] [--html [FILE]]
  prints the totals for the game code (< 0x480000) and every uncovered real function of at least N instructions
  (default 40) with its callers, callees (with the docs that name them) and its strings; --all includes the
  library code above 0x480000 (Cryo sound/BigFile/HNM, window framework, CRT). --html also writes the coverage map
  (default out/coverage.html; it holds addresses and strings of the original, so it stays out of the repository): every
  function by address, coloured ported (named in src/), read (named only in docs/ or tools/), helper (not named, only
  called from ported/read/helper code), library (>= 0x480000, replaced by the port) or open, with the open list."""
import sys, re, glob, bisect, collections
import pefile

args = [a for a in sys.argv[1:] if not a.startswith('--')]
min_n = int(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 40
if '--min' in sys.argv: args.remove(str(min_n))
html = None
if '--html' in sys.argv:
    k = sys.argv.index('--html'); nxt = sys.argv[k + 1] if k + 1 < len(sys.argv) else ''
    html = nxt if nxt.lower().endswith(('.html', '.htm')) else 'out/coverage.html'
    if html in args: args.remove(html)
show_all = '--all' in sys.argv
L = open(args[0], encoding='utf-8').read().split('\n')

starts = [(int(m.group(1), 16), i) for i, l in enumerate(L) for m in [re.match(r'; ===== function ([0-9a-f]{8})', l)] if m]
F = {}
for k, (a, i) in enumerate(starts):
    j = starts[k + 1][1] if k + 1 < len(starts) else len(L)
    ins = [l for l in L[i:j] if re.match(r'[0-9a-f]{8}  ', l)]
    F[a] = dict(n=len(ins), end=int(ins[-1][:8], 16) if ins else a,
                calls={int(m.group(1), 16) for l in ins for m in [re.search(r'call    0x([0-9a-f]+)$', l)] if m},
                strs=[l.split('  ; ', 1)[1].strip() for l in ins if '  ; ' in l])
A = sorted(F)
callers = collections.defaultdict(set)
for a, f in F.items():
    for c in f['calls']: callers[c].add(a)

pe = pefile.PE(args[1]); raw = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
def dw(i): return raw[i] | raw[i + 1] << 8 | raw[i + 2] << 16 | raw[i + 3] << 24
# switch jump tables (jmp dword ptr [reg*4 + T]): their slots point at case labels of the function that owns the jmp,
# so a case label referenced only from such a table is a jump target like any other, not a function of its own
jt_slots = set(); jt_data = set()   # jt_data: table addresses (a table the disassembler took for code is data, not a function)
for l in L:
    m = re.match(r'[0-9a-f]{8}  mov\s+\w+, byte ptr \[\w+ \+ 0x([0-9a-f]+)\]', l)
    if m: jt_data.add(int(m.group(1), 16))
    m = re.match(r'([0-9a-f]{8})  jmp     dword ptr \[\w+\*4 \+ 0x([0-9a-f]+)\]', l)
    if not m: continue
    at, t = int(m.group(1), 16), int(m.group(2), 16)
    jt_data.add(t); i = t - base
    while 0 <= i < len(raw) - 3:
        v = dw(i)
        if not at - 0x4000 < v < at + 0x4000: break   # case labels are code next to the jmp
        jt_slots.add(i); i += 4
ptrs = set()
for i in range(len(raw) - 3):
    if i in jt_slots: continue
    v = dw(i)
    if v in F: ptrs.add(v)
ptrs -= jt_data
real = sorted(a for a in A if a in callers or a in ptrs or a == A[0])
def owner(x): return real[bisect.bisect_right(real, x) - 1]

named = collections.defaultdict(set); in_src = set()   # in_src: named by the port's own code (src/)
for fn in glob.glob('docs/*.md') + glob.glob('src/*.[ch]') + glob.glob('tools/*.py') + glob.glob('tools/native/*.c'):
    if fn.replace('\\', '/').endswith('tools/coverage.py'): continue
    for m in re.finditer(r'\b0x(4[0-9a-f]{5})\b', open(fn, encoding='utf-8', errors='ignore').read(), re.I):
        a = int(m.group(1), 16)
        k = bisect.bisect_right(A, a) - 1
        if k >= 0 and a <= F[A[k]]['end'] + 16:
            named[owner(a)].add(fn.replace('\\', '/').split('/')[-1])
            if fn.replace('\\', '/').startswith('src/'): in_src.add(owner(a))

size = collections.Counter()
for a in A: size[owner(a)] += F[a]['n']
game = [o for o in real if o < 0x480000]
tot = sum(size[o] for o in game); cov = sum(size[o] for o in game if o in named)
print('game code (< 0x480000): %d instr, named %d (%.1f%%), %d functions not named (%d instr)'
      % (tot, cov, 100.0 * cov / tot, sum(1 for o in game if o not in named), tot - cov))
lib = [o for o in real if o >= 0x480000]
print('library code (>= 0x480000): %d instr, named %d' % (sum(size[o] for o in lib), sum(size[o] for o in lib if o in named)))

def doc(a): return ','.join(sorted(named.get(a, ()))[:3])   # .get: indexing the defaultdict would mark a as named and hide it from the list
for o in (real if show_all else game):
    if o in named or size[o] < min_n: continue
    k = A.index(o); body = [o]
    for a in A[k + 1:]:
        if a in real: break
        body.append(a)
    cs = sorted({owner(c) for c in callers.get(o, ())})
    cl = sorted({c for a in body for c in F[a]['calls']})
    ss = [s for a in body for s in F[a]['strs'] if not s.startswith('0x')]
    print('%06x %4d  from: %s  to: %s  %s' % (o, size[o], ' '.join('%x(%s)' % (c, doc(c)) for c in cs[:3]) or ('(pointer)' if o in ptrs else '-'),
          ' '.join('%x(%s)' % (c, doc(c)) for c in cl[:5]), ' | '.join(dict.fromkeys(ss))[:120]))

# ---- the coverage map (--html): one row = 4 KB, each function as wide as it is long
if html:
    import json, os
    from html import escape
    HTML = r"""<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WoodyRE coverage map</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@500;700;800&family=JetBrains+Mono:wght@400;600&display=swap">
<style>
:root{--bg:#f6f7f5;--panel:#fff;--fg:#1d2321;--mut:#5d6763;--line:#dde2de;--hl:#1d2321;@LIGHT@;color-scheme:light;
--display:"Archivo","Helvetica Neue",Arial,sans-serif;--body:system-ui,-apple-system,"Segoe UI",sans-serif;--mono:"JetBrains Mono",ui-monospace,Consolas,monospace}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#141816;--panel:#1b201e;--fg:#e4e8e5;--mut:#98a39e;--line:#2c3431;--hl:#fff;@DARK@;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#141816;--panel:#1b201e;--fg:#e4e8e5;--mut:#98a39e;--line:#2c3431;--hl:#fff;@DARK@;color-scheme:dark}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 var(--body)}
main{max-width:1120px;margin:0 auto;padding:28px 16px 56px;display:grid;gap:28px}main>*{min-width:0}
h1{font:800 clamp(26px,4vw,38px)/1.05 var(--display);letter-spacing:-.01em;margin:0}
h2{font:700 19px/1.2 var(--display);margin:0 0 10px}
.lede{color:var(--mut);max-width:68ch;margin:8px 0 0}
.eyebrow{font:600 12px var(--mono);letter-spacing:.08em;text-transform:uppercase;color:var(--mut);margin:0 0 8px}
.bar{display:flex;height:18px;border-radius:4px;overflow:hidden;gap:1px;background:var(--line);margin-bottom:10px}.bar i{display:block;min-width:2px}
.legend{list-style:none;margin:14px 0 0;padding:0;display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:8px 22px}
.legend li{display:grid;grid-template-columns:14px auto 1fr;column-gap:8px;align-items:baseline;min-width:0}
.sw{width:14px;height:14px;border-radius:3px;display:block;align-self:center}
.nm{font-weight:600}.pc{font:600 13px var(--mono);font-variant-numeric:tabular-nums;justify-self:end}.ds{grid-column:2/4;color:var(--mut);font-size:13px}
.s-ported{background:var(--s-ported)}.s-read{background:var(--s-read)}.s-helper{background:var(--s-helper)}
.s-library{background:var(--s-library)}.s-open{background:var(--s-open)}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:6px}
.wrap{padding:10px 10px 10px 0;overflow:hidden}canvas{display:block;width:100%;cursor:crosshair;touch-action:manipulation}
#info{position:sticky;bottom:0;font:13px/1.5 var(--mono);padding:10px 12px;min-height:3.2em;overflow-wrap:anywhere;box-shadow:0 -4px 12px rgba(0,0,0,.08)}
#info .st{font-weight:600}#info .m{color:var(--mut)}
.tw{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--line);vertical-align:top}
th{font:600 12px var(--mono);letter-spacing:.06em;text-transform:uppercase;color:var(--mut)}
td.mono{font:12.5px var(--mono);white-space:nowrap}td.num{font:12.5px var(--mono);text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
tbody tr td:first-child{box-shadow:inset 3px 0 0 var(--s-open)}
.how{color:var(--mut);font-size:13px;margin:8px 0 0}code{font:12.5px var(--mono)}
</style></head><body><main>
<header><p class="eyebrow">Woody Woodpecker &middot; Woody.exe &middot; @NF@ functions &middot; @KB@ KB of code</p>
<h1>WoodyRE coverage map</h1>
<p class="lede">Every function of the original Woody.exe, sorted by what the port and its docs do with it. Shares are of the code's bytes.</p>
<ul class="legend">@LEGEND@</ul></header>
@SECTIONS@
<div id="info" class="panel"><span class="m">Point at or tap a function for its address, size, first string, class and the files that cite it.</span></div>
<p class="how">One row is 4 KB; each function is as wide as it is long. The game code runs up to 0x480000; above it sit the Cryo sound library, BigFile, the HNM player, the Golem window framework and the MSVC runtime, which the port replaces.</p>
<section><h2>Open</h2>
<div class="panel tw"><table><thead><tr><th>Function</th><th>Bytes</th><th>Strings</th></tr></thead><tbody>@OPEN@</tbody></table></div>
<p class="how">Reached by the game, at least @MIN@ instructions, largest first.</p>@MORE@
<p class="how">Made by <code>python tools/coverage.py out/disasm_full.txt game/Woody.exe --html</code> from the addresses <code>docs/</code>, <code>src/</code> and <code>tools/</code> cite. See docs/TODO.md, "Not analysed yet".</p></section>
</main>
<script>
const DATA=@DATA@,N=@N@,ROW=4096,H=7,G=1,LBL=64,info=document.getElementById('info'),HINT=info.innerHTML;let pinned=null;
function css(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim()}
function esc(t){return String(t).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}
const maps=[...document.querySelectorAll('canvas[data-p]')].map(cv=>{const D=DATA[cv.dataset.p];
const rows=[...new Set(D.flatMap(f=>{const a=Math.floor(f[0]/ROW),b=Math.floor((f[0]+Math.max(f[1],1)-1)/ROW),r=[];for(let k=a;k<=b;k++)r.push(k);return r}))].sort((a,b)=>a-b);
return {cv,D,rows,ri:new Map(rows.map((r,k)=>[r,k]))}});
function span(m,f,cb){let a=f[0],n=f[1];while(n>0){const r=Math.floor(a/ROW),o=a-r*ROW,k=Math.min(n,ROW-o);cb(m.ri.get(r),o,k);a+=k;n-=k}}
function draw(m){const {cv,D,rows}=m,w=cv.parentNode.clientWidth-10,dpr=window.devicePixelRatio||1,h=rows.length*(H+G);
cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);cv.style.height=h+'px';const x=cv.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);
const col=N.map(s=>css('--s-'+s));x.font='9px '+css('--mono');x.fillStyle=css('--mut');
rows.forEach((r,k)=>{if(k%8==0||(k>0&&rows[k-1]!=r-1))x.fillText('0x'+(r*ROW).toString(16),6,k*(H+G)+H)});
const sc=(w-LBL)/ROW;for(const f of D){x.fillStyle=col[f[2]];span(m,f,(k,o,n)=>x.fillRect(LBL+o*sc,k*(H+G),Math.max(n*sc-0.35,0.6),H))}
if(pinned&&pinned.m===m){x.strokeStyle=css('--hl');x.lineWidth=1.5;span(m,pinned.f,(k,o,n)=>x.strokeRect(LBL+o*sc-1,k*(H+G)-1,Math.max(n*sc,1)+2,H+2))}}
function drawAll(){maps.forEach(draw)}
function at(m,e){const b=m.cv.getBoundingClientRect(),k=Math.floor((e.clientY-b.top)/(H+G));if(k<0||k>=m.rows.length||e.clientX-b.left<LBL)return null;
const a=m.rows[k]*ROW+(e.clientX-b.left-LBL)/((b.width-LBL)/ROW);let lo=0,hi=m.D.length-1,f=null;
while(lo<=hi){const i=(lo+hi)>>1;if(m.D[i][0]<=a){f=m.D[i];lo=i+1}else hi=i-1}return f&&a<f[0]+f[1]?f:null}
function show(m,f){if(!f){info.innerHTML=HINT;return}
info.innerHTML=esc(m.cv.dataset.n)+' &middot; <span class="st" style="color:var(--s-'+N[f[2]]+')">'+N[f[2]]+'</span> &middot; 0x'+f[0].toString(16)+
' &middot; '+f[1].toLocaleString()+' bytes<br>'+esc(f[3]||'sub_'+f[0].toString(16))+''+
(f[5]?'<br><span class="m">cited in '+esc(f[5])+'</span>':'')}
maps.forEach(m=>{m.cv.addEventListener('mousemove',e=>{if(!pinned)show(m,at(m,e))});
m.cv.addEventListener('mouseleave',()=>{if(!pinned)show(m,null)});
m.cv.addEventListener('click',e=>{const f=at(m,e),old=pinned;pinned=f&&!(old&&old.f===f)?{m,f}:null;show(m,pinned&&pinned.f);draw(m);if(old&&old.m!==m)draw(old.m)})});
let rt=0;addEventListener('resize',()=>{clearTimeout(rt);rt=setTimeout(drawAll,60)});
try{matchMedia('(prefers-color-scheme: dark)').addEventListener('change',drawAll)}catch(e){}
new MutationObserver(drawAll).observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});
drawAll();if(document.fonts)document.fonts.ready.then(drawAll);
</script></body></html>
"""
    CLASSES = ['ported', 'read', 'helper', 'library', 'open']
    WHAT = {'ported': "named in the port's code (src/)", 'read': 'named only in docs/ or tools/',
            'helper': 'not named, only called from ported, read or helper code',
            'library': 'Cryo sound, BigFile, HNM, Golem window framework, MSVC runtime (>= 0x480000): replaced by the port',
            'open': 'reached by the game, nobody has looked at it yet'}
    COL = {'ported': ('#1f8a4c', '#38b56a'), 'read': ('#62b47e', '#4f9466'), 'helper': ('#a9d6b6', '#2f5a3e'),
           'library': ('#7d8794', '#5e6772'), 'open': ('#d2453c', '#f0625a')}
    AI = {a: k for k, a in enumerate(A)}
    calls_by = collections.defaultdict(set)   # function -> the functions that call it
    for o in real:
        k = AI[o]
        for a in A[k:]:
            if a != o and a in real: break
            for c in F[a]['calls']:
                if c in F: calls_by[owner(c)].add(o)
    cls = {o: 'library' if o >= 0x480000 else 'ported' if o in in_src else 'read' if o in named else 'open' for o in real}
    grew = True
    while grew:
        grew = False
        for o in real:
            if cls[o] == 'open' and calls_by[o] and all(cls[c] in ('ported', 'read', 'helper') for c in calls_by[o]):
                cls[o] = 'helper'; grew = True
    rows, by, opens = [], collections.Counter(), []
    for i, o in enumerate(real):
        k = AI[o]; end = F[o]['end']; ss = list(F[o]['strs'])
        for a in A[k + 1:]:
            if a in real: break
            end = max(end, F[a]['end']); ss += F[a]['strs']
        ss = [s for s in ss if not s.startswith('0x')]
        n = (real[i + 1] if i + 1 < len(real) else end + 1) - o
        n = max(1, min(n, end + 16 - o))
        rows.append([o, n, CLASSES.index(cls[o]), ss[0][:60] if ss else '', '', ' '.join(sorted(named.get(o, ())))])
        by[cls[o]] += n
        if cls[o] == 'open' and size[o] >= min_n: opens.append((n, o, ss))
    tot = sum(by.values()) or 1
    light = ';'.join('--s-%s:%s' % (c, COL[c][0]) for c in CLASSES); dark = ';'.join('--s-%s:%s' % (c, COL[c][1]) for c in CLASSES)
    bar = ''.join('<i class="s-%s" style="flex:%d" title="%s %.1f %%"></i>' % (c, by[c], c, 100.0 * by[c] / tot) for c in CLASSES if by[c])
    sec = ('<section><h2>Woody.exe</h2><div class="bar" aria-hidden="true">%s</div><div class="panel map"><div class="wrap">'
           '<canvas data-p="woody" data-n="Woody.exe" role="img" aria-label="Map of every function of Woody.exe by address, '
           'coloured by class"></canvas></div></div></section>' % bar)
    legend = ''.join('<li><i class="sw s-%s"></i><span class="nm">%s</span><span class="pc">%.1f %%</span><span class="ds">%s &middot; '
                     '%s functions, %s KB</span></li>' % (c, c, 100.0 * by[c] / tot, escape(WHAT[c]),
                     format(sum(1 for o in real if cls[o] == c), ','), format(round(by[c] / 1024), ',')) for c in CLASSES)
    opens.sort(reverse=True)
    trs = ''.join('<tr><td class="mono">%#x</td><td class="num">%s</td><td>%s</td></tr>'
                  % (o, format(n, ','), escape('; '.join(dict.fromkeys(s[:40] for s in ss))[:160])) for n, o, ss in opens[:80]) \
        or '<tr><td colspan="3">None: every function the game reaches is named, a helper or library code.</td></tr>'
    more = '<p class="how">%d more: coverage.py without --html lists them all.</p>' % (len(opens) - 80) if len(opens) > 80 else ''
    page = HTML.replace('@LIGHT@', light).replace('@DARK@', dark).replace('@LEGEND@', legend).replace('@SECTIONS@', sec) \
        .replace('@OPEN@', trs).replace('@MORE@', more).replace('@MIN@', str(min_n)).replace('@NF@', format(len(real), ',')) \
        .replace('@KB@', format(round(tot / 1024), ',')).replace('@N@', json.dumps(CLASSES)) \
        .replace('@DATA@', json.dumps({'woody': rows}, separators=(',', ':')))
    if os.path.dirname(html): os.makedirs(os.path.dirname(html), exist_ok=True)
    open(html, 'w', encoding='utf-8', newline='\n').write(page)
    print('->', html)
