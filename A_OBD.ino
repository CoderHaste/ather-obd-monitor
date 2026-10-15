#include "driver/twai.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>

// ---------------- Pins ----------------
#define CAN_TX   10
#define CAN_RX   20
#define I2C_SDA  6      // OLED SDA
#define I2C_SCL  7      // OLED SCL
#define BTN_PIN  4      // push button between this pin and GND

// ---------------- Button timing ----------------
#define DEBOUNCE_MS    40
#define LONG_PRESS_MS  3000
#define HOLD_SHOW_MS   500     // progress bar starts after this

#define NUM_PAGES 8

const char* AP_SSID = "A-OBD";
const char* AP_PASS = "12345678";

struct Telemetry {
  uint16_t soc;
  uint16_t soh;
  uint16_t deltaSoc;
  uint16_t voltage;
  uint16_t imbalance;
  int      balancing;
  int      rpm;
  int      driveMode;
  bool     sideStand;
  bool     frontBrake, rearBrake, highBeam, startSwitch;
  bool     killSwitch, storageSwitch, horn;
  bool     indicatorLeftSide, indicatorRightSide, indicatorCenterSide, keyOn;
  int      rangeKm;
  uint16_t cellV[14];
  uint8_t  cellSoh[14];
  uint32_t chargerOV;
  uint32_t chargerOI;
  uint32_t chargerIV;
  int16_t  motorI;
  int16_t  motorTemp;
  bool     displayFan;
  uint16_t chargerFan1, chargerFan2;
  bool     forcedEco, smartEco;
  bool     seatLock, chargerLock1, chargerLock2;
  bool     chargerConnected;
};

Telemetry snap;
SemaphoreHandle_t mtx;
WebServer server(80);
unsigned long lastDiag = 0;
uint32_t maxQueueBacklog = 0;

// OLED: 0.96" SSD1306 128x64 I2C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

bool     wifiOn   = false;
uint8_t  page     = 0;
uint8_t  holdPct  = 0;      // long-press progress (0 = hide)
uint32_t lastDraw = 0;

inline const char* driveModeName(int v){
  switch(v){
    case 1: return "SPORTS";
    case 2: return "DRIVE";
    case 3: return "ECO";
    case 6: return "WARP";
    case 8: return "S. ECO";
    default: return "UNKNOWN";
  }
}

const char htmlPage[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en" data-theme="light"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#f3f4f1">
<title>A OBD Scanner</title>
<script>try{if(localStorage.getItem('aobd-theme')==='dark')document.documentElement.setAttribute('data-theme','dark')}catch(e){}</script>
<style>
:root{
--bg:#f3f4f1;--surface:#fff;--surface2:#f1f3f0;--track:#e6e9e5;--text:#16201b;--muted:#69746d;--line:rgba(22,32,27,.08);
--glass:rgba(243,244,241,.84);--off:#c9cfca;
--accent:#1d7a57;--accent-soft:#e2f1e9;--accent-line:rgba(29,122,87,.28);--accent-ring:rgba(29,122,87,.16);
--ok:#1d7a57;--ok-soft:#e2f1e9;--warn:#b87408;--warn-soft:#fbf0d9;--bad:#cc3d34;--bad-soft:#fbe6e4;
--shadow:0 1px 2px rgba(22,32,27,.04),0 10px 28px rgba(22,32,27,.06);--r:22px}
[data-theme=dark]{
--bg:#0f1311;--surface:#171c19;--surface2:#1e2521;--track:#262e29;--text:#eef2ef;--muted:#93a198;--line:rgba(255,255,255,.08);
--glass:rgba(15,19,17,.84);--off:#3a443e;
--accent:#4cc38f;--accent-soft:rgba(76,195,143,.14);--accent-line:rgba(76,195,143,.32);--accent-ring:rgba(76,195,143,.18);
--ok:#4cc38f;--ok-soft:rgba(76,195,143,.14);--warn:#e5a93d;--warn-soft:rgba(229,169,61,.15);--bad:#ef6b61;--bad-soft:rgba(239,107,97,.15);
--shadow:0 1px 2px rgba(0,0,0,.3),0 10px 28px rgba(0,0,0,.35)}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{background:var(--bg)}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 -apple-system,BlinkMacSystemFont,"SF Pro Text","Segoe UI",Roboto,Inter,system-ui,sans-serif;-webkit-font-smoothing:antialiased;font-variant-numeric:tabular-nums;padding-bottom:calc(92px + env(safe-area-inset-bottom));transition:background .3s,color .3s}
header,nav{background:var(--glass);-webkit-backdrop-filter:saturate(1.6) blur(18px);backdrop-filter:saturate(1.6) blur(18px)}
header{position:sticky;top:0;z-index:20;border-bottom:1px solid var(--line);padding-top:env(safe-area-inset-top)}
.bar{max-width:560px;margin:0 auto;display:flex;align-items:center;justify-content:space-between;padding:12px 16px}
.brand{display:flex;align-items:center;gap:11px}
.logo{width:38px;height:38px;border-radius:12px;display:grid;place-items:center;color:#fff;background:linear-gradient(145deg,#1d7a57,#3fae84);box-shadow:0 6px 14px rgba(29,122,87,.32)}
.logo svg{width:20px;height:20px;fill:currentColor}
.brand b{display:block;font-size:18px;letter-spacing:-.02em;line-height:1.1}
.brand small{display:block;color:var(--muted);font-size:12px;margin-top:1px}
.right{display:flex;align-items:center;gap:8px}
.pill{display:inline-flex;align-items:center;gap:6px;font-size:12px;font-weight:600;padding:7px 11px;border-radius:999px;background:var(--surface);border:1px solid var(--line);color:var(--muted)}
.pill i{width:7px;height:7px;border-radius:50%;background:var(--muted)}
.pill.live{color:var(--ok)}.pill.live i{background:var(--ok);animation:pulse 2s infinite}
.pill.off{color:var(--bad)}.pill.off i{background:var(--bad)}
@keyframes pulse{0%{box-shadow:0 0 0 0 var(--accent-line)}70%,100%{box-shadow:0 0 0 7px transparent}}
.icon{width:36px;height:36px;border-radius:50%;border:1px solid var(--line);background:var(--surface);color:var(--text);display:grid;place-items:center;padding:0;cursor:pointer}
.icon svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
[data-theme=dark] .moon,[data-theme=light] .sun{display:none}
main{max-width:560px;margin:0 auto;padding:4px 16px 0;transition:opacity .3s}
body.stale main{opacity:.45}
.panel{display:none}.panel.active{display:block;animation:in .35s ease}
@keyframes in{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--r);box-shadow:var(--shadow);padding:18px;margin-top:14px}
.card h2{margin:0 0 6px;font-size:12px;font-weight:700;letter-spacing:.09em;text-transform:uppercase;color:var(--muted)}
.hero{padding:24px 16px 18px;text-align:center}
.ring{position:relative;width:216px;height:216px;margin:0 auto}
.ring svg{width:100%;height:100%;display:block}
.trk{fill:none;stroke:var(--track);stroke-width:9}
.rb{fill:none;stroke:var(--ok);stroke-width:9;stroke-linecap:round;stroke-dasharray:326.73;stroke-dashoffset:326.73;transition:stroke-dashoffset .8s cubic-bezier(.2,.8,.2,1),stroke .4s}
.rb.warn{stroke:var(--warn)}.rb.bad{stroke:var(--bad)}
.ringc{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.big{font-size:58px;font-weight:700;letter-spacing:-.04em;line-height:1}
.big .dec{font-size:24px;font-weight:600;color:var(--muted);letter-spacing:0}
.big .unit{font-size:22px;font-weight:600;color:var(--muted);margin-left:2px;letter-spacing:0}
.cap{margin-top:8px;font-size:11px;font-weight:700;letter-spacing:.09em;text-transform:uppercase;color:var(--muted)}
.stats{display:grid;grid-template-columns:repeat(3,1fr);margin-top:22px;border-top:1px solid var(--line);padding-top:16px}
.stats>div{display:flex;flex-direction:column;align-items:center;gap:4px;padding:0 4px;min-width:0}
.stats>div+div{border-left:1px solid var(--line)}
.k{font-size:11px;font-weight:700;letter-spacing:.07em;text-transform:uppercase;color:var(--muted)}
.stats .v{font-size:20px;font-weight:700;letter-spacing:-.02em;white-space:nowrap}
.stats .v small{font-size:12px;font-weight:600;color:var(--muted);margin-left:2px;letter-spacing:0}
.stats .s{font-size:12px;color:var(--muted);min-height:16px}
.r{display:flex;justify-content:space-between;align-items:center;padding:14px 0}
.r+.r{border-top:1px solid var(--line)}
.r span:first-child{color:var(--muted);font-size:15px}
.r .v{font-size:18px;font-weight:700;letter-spacing:-.01em}
.r .v small{font-size:13px;font-weight:600;color:var(--muted);margin-left:3px}
.badge{font-size:13px;font-weight:700;padding:5px 12px;border-radius:999px;background:var(--surface2);color:var(--muted);transition:all .2s}
.badge.on{background:var(--accent-soft);color:var(--accent)}
.gh{margin:18px 0 9px;font-size:12px;font-weight:600;color:var(--muted)}
.gh:first-of-type{margin-top:8px}
.tiles{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.tile{display:flex;align-items:center;gap:11px;padding:12px 13px;border-radius:15px;background:var(--surface2);border:1px solid transparent;transition:background .25s,border-color .25s}
.tile i{width:10px;height:10px;border-radius:50%;background:var(--off);flex:none;transition:all .25s}
.tile b{display:block;font-size:14px;font-weight:600;line-height:1.2}
.tile span{display:block;font-size:12px;color:var(--muted);margin-top:2px}
.tile.on{background:var(--accent-soft);border-color:var(--accent-line)}
.tile.on i{background:var(--accent);box-shadow:0 0 0 4px var(--accent-ring)}
.tile.on span{color:var(--accent);font-weight:700}
.cells{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}
.cell{background:var(--surface);border:1px solid var(--line);border-radius:18px;padding:13px 14px 14px;box-shadow:var(--shadow)}
.ch{display:flex;justify-content:space-between;align-items:center}
.cn{font-size:12px;font-weight:700;color:var(--muted);letter-spacing:.06em}
.hb{font-size:11px;font-weight:700;padding:3px 8px;border-radius:999px;background:var(--surface2);color:var(--muted)}
.hb.ok{background:var(--ok-soft);color:var(--ok)}.hb.warn{background:var(--warn-soft);color:var(--warn)}.hb.bad{background:var(--bad-soft);color:var(--bad)}
.cv{font-size:25px;font-weight:700;letter-spacing:-.03em;margin:9px 0 11px;white-space:nowrap}
.cv u{text-decoration:none;font-size:13px;color:var(--muted);margin-left:3px;font-weight:600;letter-spacing:0}
.meter{height:6px;border-radius:3px;background:var(--track);overflow:hidden}
.meter i{display:block;height:100%;width:0;border-radius:3px;background:var(--ok);transition:width .5s,background .3s}
.note{margin:14px 4px 0;font-size:12px;color:var(--muted);line-height:1.5}
.pwr{display:flex;align-items:baseline;justify-content:center;gap:6px;margin-top:6px}
.pwr b{font-size:56px;font-weight:700;letter-spacing:-.04em;line-height:1}
.pwr u{text-decoration:none;font-size:22px;font-weight:600;color:var(--muted)}
.center{text-align:center}
.center .badge{display:inline-block;margin-top:12px}
nav{position:fixed;left:0;right:0;bottom:0;z-index:30;border-top:1px solid var(--line);padding:8px 12px calc(8px + env(safe-area-inset-bottom))}
.nav{max-width:560px;margin:0 auto;display:grid;grid-template-columns:repeat(4,1fr);gap:4px}
.tab{border:0;background:none;color:var(--muted);display:flex;flex-direction:column;align-items:center;gap:4px;padding:9px 0 8px;border-radius:16px;font:inherit;font-size:11px;font-weight:600;cursor:pointer;transition:color .2s,background .2s}
.tab svg{width:22px;height:22px;fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}
.tab.active{color:var(--accent);background:var(--accent-soft)}
@media(max-width:350px){.big{font-size:48px}.ring{width:190px;height:190px}.stats .v{font-size:18px}}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style></head><body>
<header><div class="bar">
 <div class="brand"><div class="logo"><svg viewBox="0 0 24 24"><path d="M13.5 2 4 13.5h6.5L9.5 22 20 9.5h-6.8z"/></svg></div>
  <div><b>A OBD</b><small id="sub">Dashboard</small></div></div>
 <div class="right"><span class="pill" id="conn"><i></i><span id="connt">Connecting</span></span>
  <button class="icon" id="theme" aria-label="Toggle dark mode">
   <svg class="moon" viewBox="0 0 24 24"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>
   <svg class="sun" viewBox="0 0 24 24"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>
  </button></div>
</div></header>

<main>
<section class="panel active" id="home">
 <div class="card hero">
  <div class="ring"><svg viewBox="0 0 120 120"><circle class="trk" cx="60" cy="60" r="52"/><circle class="rb" id="rb" cx="60" cy="60" r="52" transform="rotate(-90 60 60)"/></svg>
   <div class="ringc"><div class="big"><span id="soc_i">--</span><span class="dec" id="soc_d"></span><span class="unit">%</span></div><div class="cap">State of charge</div></div></div>
  <div class="stats">
   <div><span class="k">Range</span><span class="v" id="range">--</span></div>
   <div><span class="k">Voltage</span><span class="v" id="volt">--</span></div>
   <div><span class="k">Mode</span><span class="v" id="mode">--</span></div>
  </div>
 </div>
 <div class="card"><h2>Battery health</h2>
  <div class="r"><span>State of health</span><span class="v" id="soh">--</span></div>
  <div class="r"><span>Delta SoC</span><span class="v" id="bdelta">--</span></div>
  <div class="r"><span>Voltage imbalance</span><span class="v" id="imb">--</span></div>
 </div>
 <div class="card"><h2>Switches &amp; inputs</h2><div id="sw"></div></div>
</section>

<section class="panel" id="cells">
 <div class="card hero" style="padding-bottom:16px">
  <h2 style="text-align:left">Pack balance &middot; 14S</h2>
  <div class="stats" style="margin-top:8px;border-top:0;padding-top:4px">
   <div><span class="k">Lowest</span><span class="v" id="cmin">--</span><span class="s" id="cmin_c"></span></div>
   <div><span class="k">Highest</span><span class="v" id="cmax">--</span><span class="s" id="cmax_c"></span></div>
   <div><span class="k">Spread</span><span class="v" id="cspr">--</span><span class="s">max &minus; min</span></div>
  </div>
 </div>
 <div class="cells" id="cellgrid"></div>
 <p class="note">Bar shows each cell between the lowest and highest voltage. Badge shows cell health (SoH). Green is balanced, amber and red mean the cell is drifting from the pack average.</p>
</section>

<section class="panel" id="charger">
 <div class="card hero center">
  <h2 style="text-align:center">Charging power</h2>
  <div class="pwr"><b id="cw_n">--</b><u id="cw_u">W</u></div>
  <span class="badge" id="cconnected">--</span>
  <div class="stats">
   <div><span class="k">Out V</span><span class="v" id="coutv">--</span></div>
   <div><span class="k">Out A</span><span class="v" id="couti">--</span></div>
   <div><span class="k">In V</span><span class="v" id="cinv">--</span></div>
  </div>
 </div>
 <div class="card"><h2>Charger status</h2>
  <div class="r"><span>Cell balancing</span><span class="badge" id="bal">--</span></div>
  <div class="r"><span>Fan speed 1</span><span class="v" id="cfan1">--</span></div>
  <div class="r"><span>Fan speed 2</span><span class="v" id="cfan2">--</span></div>
  <div class="r"><span>Charger lock 1</span><span class="badge" id="clock1">--</span></div>
  <div class="r"><span>Charger lock 2</span><span class="badge" id="clock2">--</span></div>
 </div>
</section>

<section class="panel" id="more">
 <div class="card hero">
  <h2 style="text-align:left">Motor</h2>
  <div class="stats" style="margin-top:8px;border-top:0;padding-top:4px">
   <div><span class="k">Speed</span><span class="v" id="rpm">--</span><span class="s">RPM</span></div>
   <div><span class="k">Current</span><span class="v" id="mbi">--</span><span class="s">battery</span></div>
   <div><span class="k">Temp</span><span class="v" id="mt">--</span><span class="s">motor</span></div>
  </div>
 </div>
 <div class="card"><h2>Drive assists &amp; locks</h2><div id="sw2"></div></div>
</section>
</main>

<nav><div class="nav">
 <button class="tab active" id="tb_home" onclick="showTab('home')"><svg viewBox="0 0 24 24"><rect x="3" y="3" width="7" height="9" rx="1.8"/><rect x="14" y="3" width="7" height="5" rx="1.8"/><rect x="14" y="12" width="7" height="9" rx="1.8"/><rect x="3" y="16" width="7" height="5" rx="1.8"/></svg>Dashboard</button>
 <button class="tab" id="tb_cells" onclick="showTab('cells')"><svg viewBox="0 0 24 24"><rect x="2" y="7" width="18" height="10" rx="2.5"/><path d="M22 11v2M6 10.5v3M10 10.5v3M14 10.5v3"/></svg>Cells</button>
 <button class="tab" id="tb_charger" onclick="showTab('charger')"><svg viewBox="0 0 24 24"><path d="M13 2 4 14h7l-1 8 9-12h-7z"/></svg>Charger</button>
 <button class="tab" id="tb_more" onclick="showTab('more')"><svg viewBox="0 0 24 24"><path d="M4 6h9M19 6h1M4 12h2M12 12h8M4 18h11M19 18h1"/><circle cx="16" cy="6" r="2.2"/><circle cx="9" cy="12" r="2.2"/><circle cx="17" cy="18" r="2.2"/></svg>More</button>
</div></nav>

<script>
const $=id=>document.getElementById(id);
const T=(id,v)=>{const e=$(id);if(e)e.textContent=v};
const f=(n,d)=>Number(n).toFixed(d);
const U=(n,d,u)=>f(n,d)+'<small>'+u+'</small>';
const H=(id,html)=>{const e=$(id);if(e)e.innerHTML=html};

/* ---------- theme ---------- */
function setTheme(t){
  document.documentElement.setAttribute('data-theme',t);
  document.querySelector('meta[name=theme-color]').content=t==='dark'?'#0f1311':'#f3f4f1';
  try{localStorage.setItem('aobd-theme',t)}catch(e){}
}
$('theme').onclick=()=>setTheme(document.documentElement.getAttribute('data-theme')==='dark'?'light':'dark');
setTheme(document.documentElement.getAttribute('data-theme'));

/* ---------- on/off tiles ---------- */
const TL={};
function tile(id,label,on,off){TL[id]=[on,off];return '<div class="tile" id="t_'+id+'"><i></i><div><b>'+label+'</b><span id="s_'+id+'">--</span></div></div>'}
function group(title,items){return '<div class="gh">'+title+'</div><div class="tiles">'+items.map(a=>tile.apply(null,a)).join('')+'</div>'}
function setTile(id,v){const t=$('t_'+id);if(!t)return;t.classList.toggle('on',!!v);T('s_'+id,v?TL[id][0]:TL[id][1])}
function setBadge(id,v,on,off){const e=$(id);if(!e)return;e.classList.toggle('on',!!v);e.textContent=v?on:off}

$('sw').innerHTML=
  group('Ignition & safety',[['key','Key','On','Off'],['kill','Kill switch','Pressed','Released'],['storage','Storage mode','Pressed','Released'],['stand','Side stand','Down','Up'],['dfan','Display fan','On','Off']])+
  group('Rider controls',[['start','Start switch','Pressed','Released'],['fbrake','Front brake','Pressed','Released'],['rbrake','Rear brake','Pressed','Released'],['horn','Horn','Pressed','Released']])+
  group('Lights',[['hbeam','High beam','On','Off'],['indl','Left signal','On','Off'],['indr','Right signal','On','Off'],['indc','Center signal','On','Off']]);
$('sw2').innerHTML=group('Eco modes',[['feco','Forced eco','On','Off'],['seco','Smart eco','On','Off']])+group('Locks',[['seat','Seat lock','Locked','Unlocked']]);

/* ---------- cells grid ---------- */
(function(){let h='';for(let i=0;i<14;i++)h+='<div class="cell"><div class="ch"><span class="cn">C'+(i+1)+'</span><span class="hb" id="ch'+i+'">--</span></div><div class="cv"><span id="cv'+i+'">--</span><u>V</u></div><div class="meter"><i id="cb'+i+'"></i></div></div>';$('cellgrid').innerHTML=h})();

/* ---------- networking ---------- */
async function get(u){
  const c=new AbortController(),t=setTimeout(()=>c.abort(),3000);
  try{const r=await fetch(u,{cache:'no-store',signal:c.signal});if(!r.ok)throw 0;return await r.json()}
  finally{clearTimeout(t)}
}
let fails=0;
function setConn(s){
  $('conn').className='pill '+s;
  T('connt',s==='live'?'Live':s==='off'?'Offline':'Connecting');
  document.body.classList.toggle('stale',s==='off');
}

/* ---------- refreshers ---------- */
const R={
 async home(){
  const j=await get('/data');
  const s=f(j.soc,2).split('.');T('soc_i',s[0]);T('soc_d','.'+s[1]);
  const rb=$('rb');rb.style.strokeDashoffset=326.73*(1-Math.max(0,Math.min(100,j.soc))/100);
  rb.setAttribute('class','rb'+(j.soc<15?' bad':j.soc<30?' warn':''));
  H('range',U(j.range,0,'km'));H('volt',U(j.volt,2,'V'));T('mode',j.mode);
  H('soh',U(j.soh,2,'%'));H('bdelta',U(j.bdelta,2,'%'));H('imb',U(j.imb,4,'V'));
  setTile('key',j.key);setTile('kill',j.kill);setTile('storage',j.storage);setTile('stand',j.stand);setTile('dfan',j.dfan);
  setTile('start',j.start);setTile('fbrake',j.fbrake);setTile('rbrake',j.rbrake);setTile('horn',j.horn);
  setTile('hbeam',j.hbeam);setTile('indl',j.indl);setTile('indr',j.indr);setTile('indc',j.indc);
 },
 async cells(){
  const j=await get('/cells'),v=j.v,h=j.h,pos=v.filter(x=>x>0);
  if(!pos.length)return;
  const mn=Math.min(...pos),mx=Math.max(...pos),avg=pos.reduce((a,b)=>a+b,0)/pos.length,span=Math.max(mx-mn,0.02);
  H('cmin',U(mn,4,'V'));H('cmax',U(mx,4,'V'));H('cspr',U(mx-mn,4,'V'));
  T('cmin_c','Cell '+(v.indexOf(mn)+1));T('cmax_c','Cell '+(v.indexOf(mx)+1));
  for(let i=0;i<14;i++){
    T('cv'+i,f(v[i],4));
    const hb=$('ch'+i);hb.textContent=f(h[i],0)+'%';hb.className='hb '+(h[i]>90?'ok':h[i]>70?'warn':'bad');
    const b=$('cb'+i),dev=Math.abs(v[i]-avg);
    b.style.width=(v[i]>0?30+(v[i]-mn)/span*70:0)+'%';
    b.style.background=dev<0.01?'var(--ok)':dev<0.03?'var(--warn)':'var(--bad)';
  }
 },
 async charger(){
  const j=await get('/charger');
  H('coutv',U(j.coutv,2,'V'));H('couti',U(j.couti,2,'A'));H('cinv',U(j.cinv,2,'V'));
  const w=j.watts;T('cw_n',w>=1000?f(w/1000,2):f(w,1));T('cw_u',w>=1000?'kW':'W');
  setBadge('cconnected',j.cconnected,'Charger connected','Not connected');
  setBadge('bal',j.bal,'Active','Idle');
  T('cfan1',j.fan1);T('cfan2',j.fan2);
  setBadge('clock1',j.clock1,'Locked','Unlocked');setBadge('clock2',j.clock2,'Locked','Unlocked');
 },
 async more(){
  const j=await get('/data');
  T('rpm',j.rpm);setTile('feco',j.feco);setTile('seco',j.seco);
  const c=await get('/charger');
  setTile('seat',c.seat);H('mbi',U(c.mbi,1,'A'));H('mt',U(c.mt,0,'\u00B0C'));
 }
};
const TITLES={home:'Dashboard',cells:'Cell info',charger:'Charger',more:'Motor & more'};

/* ---------- tabs + polling ---------- */
let cur='home',busy=false,timer;
function showTab(t){
  cur=t;
  document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('active',p.id===t));
  document.querySelectorAll('.tab').forEach(b=>b.classList.toggle('active',b.id==='tb_'+t));
  T('sub',TITLES[t]);window.scrollTo(0,0);tick();
}
async function tick(){
  clearTimeout(timer);
  if(busy||document.hidden){timer=setTimeout(tick,250);return}
  busy=true;
  try{await R[cur]();fails=0;setConn('live')}
  catch(e){if(++fails>=2)setConn('off')}
  busy=false;
  timer=setTimeout(tick,cur==='home'?1000:1500);
}
document.addEventListener('visibilitychange',()=>{if(!document.hidden)tick()});
tick();
</script>
</body></html>
)HTML";

Telemetry getSnap(){
  Telemetry t;
  xSemaphoreTake(mtx, portMAX_DELAY);
  t = snap;
  xSemaphoreGive(mtx);
  return t;
}

void handleRoot(){ server.send_P(200, "text/html", htmlPage); }
char jsonBuf[512];
void handleData(){
  Telemetry t = getSnap();
  int n=snprintf(jsonBuf,sizeof(jsonBuf),
    "{\"soc\":%d.%02d,\"soh\":%d.%02d,\"bdelta\":%d.%02d,\"volt\":%d.%02d,\"imb\":%d.%04d,\"rpm\":%d,\"mode\":\"%s\",\"range\":%d,\"fbrake\":%s,\"rbrake\":%s,\"hbeam\":%s,\"start\":%s,\"kill\":%s,\"storage\":%s,\"horn\":%s,\"indl\":%s,\"indr\":%s,\"indc\":%s,\"key\":%s,\"stand\":%s,\"dfan\":%s,\"feco\":%s,\"seco\":%s}",
    t.soc/100,abs(t.soc%100), t.soh/100,abs(t.soh%100), t.deltaSoc/100,abs(t.deltaSoc%100),
    t.voltage/100,abs(t.voltage%100), t.imbalance/10000,abs(t.imbalance%10000),
    t.rpm,driveModeName(t.driveMode),t.rangeKm,
    t.frontBrake?"true":"false",t.rearBrake?"true":"false",t.highBeam?"true":"false",t.startSwitch?"true":"false",
    t.killSwitch?"true":"false",t.storageSwitch?"true":"false",t.horn?"true":"false",
    t.indicatorLeftSide?"true":"false",t.indicatorRightSide?"true":"false",t.indicatorCenterSide?"true":"false",t.keyOn?"true":"false",t.sideStand?"true":"false",
    t.displayFan?"true":"false",t.forcedEco?"true":"false",t.smartEco?"true":"false");
  if(n<0||n>=(int)sizeof(jsonBuf)){server.send(500,"application/json","{}");return;}
  server.send(200,"application/json",jsonBuf);
}

char cellBuf[512];
void handleCells(){
  Telemetry t = getSnap();
  char *p=cellBuf; int rem=sizeof(cellBuf);
  int n=snprintf(p,rem,"{\"v\":["); if(n<0)return; p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%d.%04d%s",t.cellV[i]/10000,abs(t.cellV[i]%10000),i<13?",":""); if(n<0)return; p+=n; rem-=n; }
  n=snprintf(p,rem,"],\"h\":["); if(n<0)return; p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%d%s",t.cellSoh[i],i<13?",":""); if(n<0)return; p+=n; rem-=n; }
  snprintf(p,rem,"]}");
  server.send(200,"application/json",cellBuf);
}

char chargerBuf[384];
void handleCharger(){
  Telemetry t = getSnap();
  const int32_t motorI=t.motorI;
  const int32_t motorMag=motorI<0?-motorI:motorI;
  const char *motorSign=motorI<0?"-":"";
  const uint64_t powerProduct=(uint64_t)t.chargerOV*(uint64_t)t.chargerOI;
  const uint32_t wattsWhole=(uint32_t)(powerProduct/1000000ULL);
  const unsigned wattsFrac=(unsigned)((powerProduct%1000000ULL)/10000ULL);
  snprintf(chargerBuf,sizeof(chargerBuf),
    "{\"coutv\":%d.%03d,\"couti\":%d.%03d,\"cinv\":%d.%03d,\"mbi\":%s%d.%01d,\"mt\":%d,\"bal\":%d,\"watts\":%u.%02u,\"fan1\":%d,\"fan2\":%d,\"cconnected\":%s,\"seat\":%s,\"clock1\":%s,\"clock2\":%s}",
    (int)(t.chargerOV/1000),abs((int)(t.chargerOV%1000)),
    (int)(t.chargerOI/1000),abs((int)(t.chargerOI%1000)),
    (int)(t.chargerIV/1000),abs((int)(t.chargerIV%1000)),
    motorSign,(int)(motorMag/10),abs((int)(motorMag%10)),
    (int)t.motorTemp, t.balancing, wattsWhole, wattsFrac,
    t.chargerFan1, t.chargerFan2,
    t.chargerConnected?"true":"false",t.seatLock?"true":"false",t.chargerLock1?"true":"false",t.chargerLock2?"true":"false");
  server.send(200,"application/json",chargerBuf);
}

void setupCAN(){
  pinMode(CAN_RX, INPUT_PULLUP);
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX,(gpio_num_t)CAN_RX,TWAI_MODE_NORMAL);
  g.rx_queue_len=128; g.tx_queue_len=0;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  ESP_ERROR_CHECK(twai_driver_install(&g,&t,&f));
  ESP_ERROR_CHECK(twai_start());
}

void IRAM_ATTR applyFrame(Telemetry &t, const twai_message_t &m){
  if(m.extd||m.rtr) return;
  const uint8_t *d=m.data;
  switch(m.identifier){
    case 0x131: if(m.data_length_code<6)return; t.soc=d[4]+(d[5]<<8); t.balancing=((d[2]>>4)|((d[3]&0x0F)<<4))&1; t.chargerConnected=((d[2]>>6)&1); break;
    case 0x14D: if(m.data_length_code<6)return; t.soh=d[4]+(d[5]<<8); break;
    case 0x13D: if(m.data_length_code<6)return; t.deltaSoc=d[4]+(d[5]<<8); break;
    case 0x141: if(m.data_length_code<8)return; t.voltage=d[0]+(d[1]<<8); t.imbalance=d[2]+(d[3]<<8); break;
    case 0x200: if(m.data_length_code<8)return; t.rangeKm=d[6]; t.displayFan=((d[7]>>4)&1); break;
    case 0x510: if(m.data_length_code<4)return; { int16_t raw=(int16_t)(d[2]+(d[3]<<8)); t.rpm=raw*5/6; } break;
    case 0x101: if(m.data_length_code<5)return; t.driveMode=d[1]; t.smartEco=((d[3]>>6)&1); t.forcedEco=((d[4]>>2)&1); break;
    case 0x102:
      if(m.data_length_code<5)return;
      t.sideStand=(d[4]&1);
      t.frontBrake=((d[0]>>0)&1); t.rearBrake=((d[0]>>1)&1); t.highBeam=((d[0]>>2)&1);
      t.startSwitch=((d[0]>>4)&1); t.killSwitch=((d[0]>>5)&1); t.storageSwitch=((d[0]>>6)&1);
      t.horn=((d[1]>>1)&1); t.indicatorLeftSide=((d[2]>>0)&1); t.indicatorRightSide=((d[2]>>1)&1); t.indicatorCenterSide=((d[2]>>2)&1); t.keyOn=((d[4]>>3)&1);
      break;
    case 0x103:
      if(m.data_length_code<2)return;
      t.seatLock=((d[1]>>1)&1); t.chargerLock2=((d[1]>>2)&1); t.chargerLock1=((d[1]>>3)&1);
      break;
    case 0x208:
      if(m.data_length_code<4)return;
      t.chargerFan1=d[0]+(d[1]<<8); t.chargerFan2=d[2]+(d[3]<<8);
      break;
    case 0x132: if(m.data_length_code<8)return; t.cellV[0]=d[0]+(d[1]<<8); t.cellV[1]=d[2]+(d[3]<<8); t.cellV[2]=d[4]+(d[5]<<8); t.cellV[3]=d[6]+(d[7]<<8); break;
    case 0x133: if(m.data_length_code<8)return; t.cellV[4]=d[0]+(d[1]<<8); t.cellV[5]=d[2]+(d[3]<<8); t.cellV[6]=d[4]+(d[5]<<8); t.cellV[7]=d[6]+(d[7]<<8); break;
    case 0x134: if(m.data_length_code<8)return; t.cellV[8]=d[0]+(d[1]<<8); t.cellV[9]=d[2]+(d[3]<<8); t.cellV[10]=d[4]+(d[5]<<8); t.cellV[11]=d[6]+(d[7]<<8); break;
    case 0x135: if(m.data_length_code<4)return; t.cellV[12]=d[0]+(d[1]<<8); t.cellV[13]=d[2]+(d[3]<<8); break;
    case 0x14e: if(m.data_length_code<8)return; t.cellSoh[0]=d[0]; t.cellSoh[1]=d[1]; t.cellSoh[2]=d[2]; t.cellSoh[3]=d[3]; t.cellSoh[4]=d[4]; t.cellSoh[5]=d[5]; t.cellSoh[6]=d[6]; t.cellSoh[7]=d[7]; break;
    case 0x14f: if(m.data_length_code<6)return; t.cellSoh[8]=d[0]; t.cellSoh[9]=d[1]; t.cellSoh[10]=d[2]; t.cellSoh[11]=d[3]; t.cellSoh[12]=d[4]; t.cellSoh[13]=d[5]; break;
    case 0x201:
      if(m.data_length_code<8)return;
      t.chargerOV=d[0]|(d[1]<<8)|(d[2]<<16)|((uint32_t)d[3]<<24);
      t.chargerOI=d[4]|(d[5]<<8)|(d[6]<<16)|((uint32_t)d[7]<<24);
      break;
    case 0x203:
      if(m.data_length_code<4)return;
      t.chargerIV=d[0]|(d[1]<<8)|(d[2]<<16)|((uint32_t)d[3]<<24);
      break;
    case 0x18c:
      if(m.data_length_code<5)return;
      t.motorI=((d[2]>>5)|((d[3]&31)<<3))+((((d[3]>>5)|((d[4]&31)<<3))&1)*256)-(((d[3]>>5)&1)*512);
      t.motorTemp=(((d[4]&63)<<2)|(d[3]>>6))-40;
      break;
  }
}

void canTask(void *){
  Telemetry local;
  xSemaphoreTake(mtx, portMAX_DELAY);
  local = snap;
  xSemaphoreGive(mtx);
  uint32_t backlog = 0;
  for(;;){
    twai_message_t msg;
    if(twai_receive(&msg,0)==ESP_OK){
      applyFrame(local, msg);
      backlog++;
    } else {
      if(backlog>0){
        xSemaphoreTake(mtx, portMAX_DELAY);
        snap = local;
        xSemaphoreGive(mtx);
        if(backlog>maxQueueBacklog) maxQueueBacklog=backlog;
        backlog=0;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

// ======================= WiFi on/off =======================
void startWifi(){
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP); delay(100);
  WiFi.softAP(AP_SSID,AP_PASS,6,0,4);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  server.begin();
  wifiOn = true;
}

void stopWifi(){
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
}

// ======================= OLED =======================
#define FONT_MAIN u8g2_font_6x10_tf
#define FONT_SMALL u8g2_font_5x8_tf

void drawHeader(const char *title, bool showPage){
  u8g2.setFont(FONT_MAIN);
  u8g2.drawStr(0, 9, title);
  if(showPage){
    char b[8]; snprintf(b, sizeof(b), "%u/%u", page+1, NUM_PAGES);
    u8g2.drawStr(128 - u8g2.getStrWidth(b), 9, b);
  }
  u8g2.drawHLine(0, 11, 128);
  // long-press progress bar replaces the underline
  if(holdPct > 0) u8g2.drawBox(0, 10, (128 * holdPct) / 100, 3);
}

// left label, right-aligned value, row 0..4
void drawRow(int row, const char *label, const char *val){
  int y = 23 + 10*row;
  u8g2.setFont(FONT_MAIN);
  u8g2.drawStr(0, y, label);
  u8g2.drawStr(128 - u8g2.getStrWidth(val), y, val);
}

// 2-column grid, idx 0..7
void drawGrid(int idx, const char *label, const char *val){
  int col = idx & 1, row = idx >> 1;
  int x = col ? 68 : 0;
  int y = 25 + 12*row;
  u8g2.setFont(FONT_MAIN);
  u8g2.drawStr(x, y, label);
  u8g2.drawStr(x + 60 - u8g2.getStrWidth(val), y, val);
}

inline const char* onOff(bool b){ return b ? "ON" : "off"; }

void pageBattery(const Telemetry &t){
  char b[24];
  drawHeader("BATTERY", true);
  snprintf(b,sizeof(b),"%.2f %%", t.soc/100.0f);       drawRow(0,"SoC",b);
  snprintf(b,sizeof(b),"%.2f %%", t.soh/100.0f);       drawRow(1,"SoH",b);
  snprintf(b,sizeof(b),"%.2f %%", t.deltaSoc/100.0f);  drawRow(2,"Delta SoC",b);
  snprintf(b,sizeof(b),"%.2f V",  t.voltage/100.0f);   drawRow(3,"Voltage",b);
  snprintf(b,sizeof(b),"%.4f V",  t.imbalance/10000.0f); drawRow(4,"Imbalance",b);
}

void pageDrive(const Telemetry &t){
  char b[24];
  drawHeader("DRIVE", true);
  drawRow(0,"Mode",driveModeName(t.driveMode));
  snprintf(b,sizeof(b),"%d km", t.rangeKm);            drawRow(1,"Range",b);
  snprintf(b,sizeof(b),"%d", t.rpm);                   drawRow(2,"Motor RPM",b);
  snprintf(b,sizeof(b),"%.1f A", t.motorI/10.0f);      drawRow(3,"Motor I",b);
  snprintf(b,sizeof(b),"%d C", (int)t.motorTemp);      drawRow(4,"Motor Temp",b);
}

void pageInputs1(const Telemetry &t){
  drawHeader("INPUTS 1", true);
  u8g2.drawVLine(64, 14, 50);
  drawGrid(0,"Key",  onOff(t.keyOn));
  drawGrid(1,"Start",onOff(t.startSwitch));
  drawGrid(2,"FBrk", onOff(t.frontBrake));
  drawGrid(3,"RBrk", onOff(t.rearBrake));
  drawGrid(4,"Beam", onOff(t.highBeam));
  drawGrid(5,"Horn", onOff(t.horn));
  drawGrid(6,"Kill", onOff(t.killSwitch));
  drawGrid(7,"Stor", onOff(t.storageSwitch));
}

void pageInputs2(const Telemetry &t){
  drawHeader("INPUTS 2", true);
  u8g2.drawVLine(64, 14, 50);
  drawGrid(0,"IndL", onOff(t.indicatorLeftSide));
  drawGrid(1,"IndR", onOff(t.indicatorRightSide));
  drawGrid(2,"IndC", onOff(t.indicatorCenterSide));
  drawGrid(3,"Stand",t.sideStand ? "DOWN" : "UP");
  drawGrid(4,"DFan", onOff(t.displayFan));
  drawGrid(5,"FEco", onOff(t.forcedEco));
  drawGrid(6,"SEco", onOff(t.smartEco));
  drawGrid(7,"Seat", t.seatLock ? "LOCK" : "open");
}

// 14 cells in 3 columns x 5 rows
void pageCells(const Telemetry &t, bool voltage){
  drawHeader(voltage ? "CELL VOLTAGE" : "CELL SoH", true);
  u8g2.setFont(FONT_SMALL);
  char b[16];
  for(int i=0;i<14;i++){
    int col = i / 5, row = i % 5;
    int x = col * 43, y = 23 + 10*row;
    if(voltage) snprintf(b,sizeof(b),"%2d %.3f", i+1, t.cellV[i]/10000.0f);
    else        snprintf(b,sizeof(b),"%2d %3d%%", i+1, t.cellSoh[i]);
    u8g2.drawStr(x, y, b);
  }
}

void pageCharger1(const Telemetry &t){
  char b[24];
  drawHeader("CHARGER", true);
  snprintf(b,sizeof(b),"%.2f V", t.chargerOV/1000.0f); drawRow(0,"Out V",b);
  snprintf(b,sizeof(b),"%.2f A", t.chargerOI/1000.0f); drawRow(1,"Out I",b);
  snprintf(b,sizeof(b),"%.1f V", t.chargerIV/1000.0f); drawRow(2,"In V",b);
  float w = (t.chargerOV/1000.0f) * (t.chargerOI/1000.0f);
  if(w >= 1000.0f) snprintf(b,sizeof(b),"%.2f kW", w/1000.0f);
  else             snprintf(b,sizeof(b),"%.1f W", w);
  drawRow(3,"Power",b);
  drawRow(4,"Connected", t.chargerConnected ? "YES" : "no");
}

void pageCharger2(const Telemetry &t){
  char b[24];
  drawHeader("CHARGER 2", true);
  snprintf(b,sizeof(b),"%d", t.balancing);   drawRow(0,"Balancing",b);
  snprintf(b,sizeof(b),"%u", t.chargerFan1); drawRow(1,"Fan 1",b);
  snprintf(b,sizeof(b),"%u", t.chargerFan2); drawRow(2,"Fan 2",b);
  drawRow(3,"Lock 1", t.chargerLock1 ? "LOCKED" : "open");
  drawRow(4,"Lock 2", t.chargerLock2 ? "LOCKED" : "open");
}

void pageWifi(){
  char b[24];
  drawHeader("WiFi ON", false);
  snprintf(b,sizeof(b),"%s", AP_SSID);                     drawRow(0,"SSID",b);
  snprintf(b,sizeof(b),"%s", AP_PASS);                     drawRow(1,"Pass",b);
  drawRow(2,"IP","192.168.4.1");
  snprintf(b,sizeof(b),"%d", WiFi.softAPgetStationNum());  drawRow(3,"Clients",b);
  u8g2.setFont(FONT_SMALL);
  u8g2.drawStr(0, 63, "Hold 3s to turn WiFi off");
}

void showMsg(const char *msg){
  u8g2.clearBuffer();
  u8g2.setFont(FONT_MAIN);
  u8g2.drawStr((128 - u8g2.getStrWidth(msg))/2, 36, msg);
  u8g2.sendBuffer();
}

void drawWelcome(){
  u8g2.clearBuffer();
  u8g2.drawRFrame(0, 0, 128, 64, 4);

  u8g2.setFont(u8g2_font_helvB10_tr);
  const char *title = "A OBD Scanner";
  u8g2.drawStr((128 - u8g2.getStrWidth(title)) / 2, 24, title);

  u8g2.setFont(FONT_MAIN);
  const char *ver = "v4";
  u8g2.drawStr((128 - u8g2.getStrWidth(ver)) / 2, 38, ver);

  u8g2.drawHLine(24, 43, 80);

  const char *by = "by CoderHaste";
  u8g2.drawStr((128 - u8g2.getStrWidth(by)) / 2, 56, by);
  u8g2.sendBuffer();
}

void drawScreen(){
  u8g2.clearBuffer();
  if(wifiOn){
    pageWifi();
  } else {
    Telemetry t = getSnap();
    switch(page){
      case 0: pageBattery(t);  break;
      case 1: pageDrive(t);    break;
      case 2: pageInputs1(t);  break;
      case 3: pageInputs2(t);  break;
      case 4: pageCells(t, true);  break;
      case 5: pageCells(t, false); break;
      case 6: pageCharger1(t); break;
      case 7: pageCharger2(t); break;
    }
  }
  u8g2.sendBuffer();
}

// ======================= Button =======================
// short press  -> next page (only when WiFi is off)
// hold 3 sec   -> toggle WiFi
void handleButton(){
  static bool     lastRaw   = HIGH;
  static bool     stable    = HIGH;
  static uint32_t lastChange = 0;
  static uint32_t pressStart = 0;
  static bool     longDone  = false;

  uint32_t now = millis();
  bool raw = digitalRead(BTN_PIN);
  if(raw != lastRaw){ lastRaw = raw; lastChange = now; }

  if((now - lastChange) >= DEBOUNCE_MS && raw != stable){
    stable = raw;
    if(stable == LOW){            // pressed
      pressStart = now;
      longDone = false;
    } else {                      // released
      holdPct = 0;
      if(!longDone && !wifiOn){
        page = (page + 1) % NUM_PAGES;
      }
      lastDraw = 0;               // redraw right away
    }
  }

  if(stable == LOW && !longDone){
    uint32_t held = now - pressStart;
    if(held >= LONG_PRESS_MS){
      longDone = true;
      holdPct = 0;
      if(!wifiOn){ showMsg("Starting WiFi..."); startWifi(); }
      else       { showMsg("WiFi off...");      stopWifi();  }
      lastDraw = 0;
    } else if(held >= HOLD_SHOW_MS){
      holdPct = (uint8_t)(((held - HOLD_SHOW_MS) * 100UL) / (LONG_PRESS_MS - HOLD_SHOW_MS));
      if(holdPct == 0) holdPct = 1;
    }
  }
}

// ======================= Arduino =======================
void setup(){
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);

  mtx = xSemaphoreCreateMutex();

  Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.begin();
  u8g2.setBusClock(400000);
  drawWelcome();

  server.on("/",handleRoot);
  server.on("/data",handleData);
  server.on("/cells",handleCells);
  server.on("/charger",handleCharger);
  // WiFi stays OFF at boot; hold the button 3 sec to start it

  setupCAN();
  xTaskCreatePinnedToCore(canTask,"can",4096,NULL,2,NULL,0);
  delay(2000);   // keep the welcome screen visible
}

void loop(){
  handleButton();
  if(wifiOn) server.handleClient();

  uint32_t now = millis();
  uint32_t interval = (holdPct > 0) ? 100 : (wifiOn ? 1000 : 250);
  if(lastDraw == 0 || now - lastDraw >= interval){
    lastDraw = now ? now : 1;
    drawScreen();
  }

  if(now-lastDiag>=5000){
    lastDiag=now;
    twai_status_info_t st;
    if(twai_get_status_info(&st)==ESP_OK){
      Serial.printf("[DIAG] rx_missed:%lu rx_overrun:%lu arb_lost:%lu max_backlog:%lu heap:%lu\n",
        (unsigned long)st.rx_missed_count,(unsigned long)st.rx_overrun_count,
        (unsigned long)st.arb_lost_count,(unsigned long)maxQueueBacklog,
        (unsigned long)ESP.getFreeHeap());
    }
    maxQueueBacklog=0;
  }
  vTaskDelay(pdMS_TO_TICKS(1));
}
