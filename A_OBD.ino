#include "driver/twai.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <LittleFS.h>

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
#define DOUBLE_MS      350     // double-press window (adds a log marker)

#define NUM_PAGES 14

// ---------------- Data logging ----------------
#define LOG_RIDE_MS      1000    // ride sample interval
#define LOG_CHARGE_MS    10000   // charge sample interval
#define LOG_FLUSH_MS     30000   // write RAM buffer to flash at least this often
#define LOG_END_MS       30000   // key off / charger out this long -> session ends
#define LOG_SWITCH_MS    5000    // ride <-> charge switch debounce
#define LOG_MIN_RIDE_S   30      // shorter sessions are discarded
#define LOG_MIN_CHARGE_S 60
#define LOG_RESERVE      49152   // keep this much flash free (oldest logs deleted)
#define LOG_BUF          40      // records held in RAM before writing
#define PACK_I_SIGN      1       // set to -1 if "energy used" and "regen" come out swapped

const char* AP_SSID = "A-OBD";
const char* AP_PASS = "12345678";

struct Telemetry {
  // battery
  uint16_t soc, soh, deltaSoc, voltage, imbalance;
  uint16_t maxCellSoh, minCellSoh;
  uint16_t cellV[14];
  uint16_t cellSoc[14];
  uint8_t  cellSoh[14];
  int16_t  cellTemp[5];
  int16_t  afeDieTemp, packCurrent;
  int16_t  eftTemp, eciTemp, senseTemp;
  uint16_t deratedDsgI, deratedChgI, deratedRegenI, dsgLimTemp;
  uint16_t chgCellRule, chgCmdRateLtd;
  int16_t  chgPostQuant;
  uint16_t soe, socEkf, maxSoc, minSocEst;
  int16_t  minV1Est, minV2Est;
  int      balancing;
  // drive / motor
  int      rpm, driveMode, rangeKm;
  int16_t  motorI, motorTemp, controllerTemp;
  int16_t  throttleOut, throttle1, throttle3;
  uint16_t pduVoltage;
  // environment
  uint16_t humidity;
  int16_t  ambientTemp;
  // charger
  uint32_t chargerOV, chargerOI, chargerIV;
  uint16_t chargerFan1, chargerFan2;
  // vehicle
  uint32_t odometer;            // metres
  uint8_t  bmsHwVer, bmsFw[3], bcmFw[3], gtwFw[3], motorFw[3];
  char     binId[16];
  // switches
  bool sideStand, frontBrake, rearBrake, highBeam, startSwitch;
  bool killSwitch, storageSwitch, horn;
  bool indicatorLeftSide, indicatorRightSide, indicatorCenterSide, keyOn;
  bool displayFan, forcedEco, smartEco;
  bool seatLock, chargerLock1, chargerLock2, chargerConnected;
};

Telemetry snap;
SemaphoreHandle_t mtx;
WebServer server(80);
unsigned long lastDiag = 0;
volatile uint32_t maxQueueBacklog = 0;

// OLED: 0.96" SSD1306 128x64 I2C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

bool     wifiOn   = false;
uint8_t  page     = 0;
uint8_t  holdPct  = 0;      // long-press progress (0 = hide)
uint32_t lastDraw = 0;
volatile uint32_t lastFrameMs = 0;   // last time any CAN frame arrived

// ---------------- Log file format ----------------
// /l/NNNNN.bin = 64-byte header (summary filled in when the session ends)
//                followed by 32-byte records.  All little-endian.
#define LOG_MAGIC 0x44424F41UL      // "AOBD"

struct __attribute__((packed)) LogHdr {
  uint32_t magic;
  uint8_t  ver, type, done, rsv;     // type 1 = ride, 2 = charge
  uint32_t epoch;                    // unix start time, 0 = unknown
  uint32_t dur;                      // seconds
  uint32_t nrec;
  uint32_t distM;                    // metres (odometer delta)
  int32_t  whUsed, whRegen;          // Wh x10
  uint32_t whChg;                    // Wh x10 delivered by charger
  uint16_t socStart, socEnd;         // 0.01 %
  int16_t  maxMotorT, maxCtrlT;      // C
  int16_t  maxCellT;                 // 0.01 C
  uint16_t minCell;                  // 0.1 mV
  uint16_t maxRpm;
  uint16_t peakW;                    // W (ride: battery, charge: charger)
  uint16_t endSpread;                // 0.1 mV
  uint16_t markers;
  uint8_t  pad[8];
};

struct __attribute__((packed)) LogRec {
  uint32_t t;                        // 0.1 s since session start
  uint16_t soc;                      // 0.01 %
  uint16_t volt;                     // 0.01 V
  int16_t  packI;                    // 0.01 A
  uint16_t cellMin, cellMax;         // 0.1 mV
  int16_t  cellTmax;                 // 0.01 C
  int16_t  rpm;
  int16_t  motorI;                   // 0.1 A
  int8_t   motorT, ctrlT, thr;       // C, C, %
  uint8_t  mode;
  uint32_t odo;                      // metres
  uint16_t flags;                    // see LF_* below
  uint16_t chgW;                     // charger output W
};
static_assert(sizeof(LogHdr) == 64, "LogHdr must be 64 bytes");
static_assert(sizeof(LogRec) == 32, "LogRec must be 32 bytes");

enum { LF_FBRAKE=1<<0, LF_RBRAKE=1<<1, LF_CHARGER=1<<2, LF_BAL=1<<3, LF_FECO=1<<4,
       LF_SECO=1<<5, LF_KEY=1<<6, LF_MARK=1<<7, LF_HBEAM=1<<8 };

bool     logOk = false;              // LittleFS mounted
bool     sessActive = false;
uint8_t  sessType = 0;
uint32_t sessId = 0, nextId = 1;
char     sessPath[16];
uint32_t sessStartMs = 0, sessEpoch = 0, sessRecs = 0;
uint32_t lastSample = 0, lastFlush = 0, pendSince = 0;
bool     pendingMarker = false;
LogRec   logBuf[LOG_BUF];
uint8_t  logN = 0;

bool     timeValid = false;          // phone has sent the time
uint32_t epochBase = 0;              // epoch at millis() == 0
struct Undated { uint32_t id, endMs; };
Undated  undated[8];
uint8_t  undatedN = 0;

LogHdr   lastSum;                    // summary shown on the OLED after a session
uint32_t summaryUntil = 0;
const char *toastMsg = nullptr;
uint32_t toastUntil = 0;

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

// ===================================================================
//  Web page (served from flash)
// ===================================================================
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
.hero.sm{padding:18px 16px 16px}.hero.sm h2{text-align:left}
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
.cap2{margin-top:2px;font-size:11px;color:var(--muted)}
.stats{display:grid;grid-template-columns:repeat(3,1fr);margin-top:22px;border-top:1px solid var(--line);padding-top:16px}
.stats.flat{margin-top:8px;border-top:0;padding-top:4px}
.stats.q4{grid-template-columns:repeat(4,1fr)}.stats.q4 .v{font-size:17px}
.stats>div{display:flex;flex-direction:column;align-items:center;gap:4px;padding:0 4px;min-width:0}
.stats>div+div{border-left:1px solid var(--line)}
.k{font-size:11px;font-weight:700;letter-spacing:.07em;text-transform:uppercase;color:var(--muted)}
.stats .v{font-size:20px;font-weight:700;letter-spacing:-.02em;white-space:nowrap}
.stats .v small{font-size:12px;font-weight:600;color:var(--muted);margin-left:2px;letter-spacing:0}
.stats .s{font-size:12px;color:var(--muted);min-height:16px}
.r{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:14px 0}
.r+.r,.r+.tr,.tr+.r,.tr+.tr{border-top:1px solid var(--line)}
.r>span:first-child{color:var(--muted);font-size:15px}
.r em{font-style:normal;font-size:11px;color:var(--muted);opacity:.8;margin-left:6px}
.r .v{font-size:18px;font-weight:700;letter-spacing:-.01em;white-space:nowrap}
.r .v small{font-size:13px;font-weight:600;color:var(--muted);margin-left:3px}
.r .mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:15px;letter-spacing:.02em}
.tr{padding:13px 0}
.trh{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:9px}
.trh span{color:var(--muted);font-size:15px}
.trh b{font-size:18px;letter-spacing:-.01em}.trh b small{font-size:13px;color:var(--muted);margin-left:2px}
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
.cell{background:var(--surface);border:1px solid var(--line);border-radius:18px;padding:13px 14px 12px;box-shadow:var(--shadow)}
.ch{display:flex;justify-content:space-between;align-items:center}
.cn{font-size:12px;font-weight:700;color:var(--muted);letter-spacing:.06em}
.hb{font-size:11px;font-weight:700;padding:3px 8px;border-radius:999px;background:var(--surface2);color:var(--muted)}
.hb.ok{background:var(--ok-soft);color:var(--ok)}.hb.warn{background:var(--warn-soft);color:var(--warn)}.hb.bad{background:var(--bad-soft);color:var(--bad)}
.cv{font-size:25px;font-weight:700;letter-spacing:-.03em;margin:9px 0 11px;white-space:nowrap}
.cv u{text-decoration:none;font-size:13px;color:var(--muted);margin-left:3px;font-weight:600;letter-spacing:0}
.cs{display:flex;justify-content:space-between;margin-top:10px;font-size:12px;color:var(--muted)}
.cs b{color:var(--text);font-weight:600}
.meter{height:6px;border-radius:3px;background:var(--track);overflow:hidden}
.meter i{display:block;height:100%;width:0;border-radius:3px;background:var(--ok);transition:width .5s,background .3s}
.note{margin:14px 4px 0;font-size:12px;color:var(--muted);line-height:1.5}
.pwr{display:flex;align-items:baseline;justify-content:center;gap:6px;margin-top:6px}
.pwr b{font-size:56px;font-weight:700;letter-spacing:-.04em;line-height:1}
.pwr u{text-decoration:none;font-size:22px;font-weight:600;color:var(--muted)}
.center{text-align:center}
.center .badge{display:inline-block;margin-top:12px}
.sel{width:100%;margin-top:8px;padding:12px 14px;border-radius:14px;border:1px solid var(--line);background:var(--surface2);color:var(--text);font:inherit;font-size:15px;font-weight:600}
.btns{display:flex;gap:8px;margin-top:12px}
.btn{flex:1;border:1px solid var(--line);background:var(--surface2);color:var(--text);border-radius:14px;padding:12px 0;font:inherit;font-size:14px;font-weight:700;cursor:pointer;transition:opacity .2s}
.btn.pri{background:var(--accent);border-color:transparent;color:#fff}
[data-theme=dark] .btn.pri{color:#0f1311}
.btn:disabled{opacity:.4;cursor:default}
.cst{margin:12px 2px 0;font-size:13px;color:var(--muted)}
.cname{display:flex;justify-content:space-between;align-items:baseline}
.cname b{font-size:15px}
.cname span{font-size:12px;color:var(--muted)}
#ccv{width:100%;height:240px;display:block;margin-top:12px}
[hidden]{display:none!important}
.seg{display:grid;grid-template-columns:1fr 1fr;gap:4px;padding:4px;margin-top:14px;border-radius:14px;background:var(--surface2);border:1px solid var(--line)}
.seg button{border:0;background:none;color:var(--muted);font:inherit;font-size:14px;font-weight:700;padding:9px 0;border-radius:10px;cursor:pointer;transition:all .2s}
.seg.s3{grid-template-columns:repeat(3,1fr)}
.seg button.on{background:var(--surface);color:var(--text);box-shadow:0 1px 3px rgba(0,0,0,.08)}
.sbar{height:8px;border-radius:4px;background:var(--track);overflow:hidden;margin-top:10px}
.sbar i{display:block;height:100%;width:0;border-radius:4px;background:var(--accent);transition:width .5s}
.stxt{margin-top:9px;font-size:13px;color:var(--muted)}
.rec{display:flex;align-items:center;justify-content:space-between;gap:10px}
.rec p{margin:0;font-size:14px;color:var(--muted);line-height:1.45}.rec p b{color:var(--text)}
.recdot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--bad);margin-right:8px;vertical-align:1px;animation:blink 1s infinite}
@keyframes blink{50%{opacity:.25}}
.btn.sm{flex:none;padding:9px 14px;font-size:13px}
.btn.del{color:var(--bad)}
.sess{display:flex;align-items:center;gap:12px;padding:13px 14px;background:var(--surface);border:1px solid var(--line);border-radius:18px;box-shadow:var(--shadow);margin-top:10px;cursor:pointer;transition:opacity .15s}
.sess:active{opacity:.6}
.si{width:40px;height:40px;border-radius:12px;display:grid;place-items:center;flex:none;background:var(--accent-soft);color:var(--accent)}
.si.c{background:var(--warn-soft);color:var(--warn)}
.si svg{width:20px;height:20px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.sb{flex:1;min-width:0}.sb b{display:block;font-size:15px;letter-spacing:-.01em}
.sb span{display:block;font-size:12px;color:var(--muted);margin-top:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.sv{text-align:right;flex:none}.sv b{display:block;font-size:17px;letter-spacing:-.02em}.sv small{font-size:11px;color:var(--muted)}
.empty{text-align:center;color:var(--muted);font-size:14px;padding:28px 12px;line-height:1.5}
.back{display:inline-flex;align-items:center;gap:4px;border:0;background:none;color:var(--accent);font:inherit;font-size:15px;font-weight:600;padding:14px 0 0;cursor:pointer}
.back svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:2.2;stroke-linecap:round;stroke-linejoin:round}
.stl{font-size:20px;font-weight:700;letter-spacing:-.02em;margin:0}
.sdt{font-size:13px;color:var(--muted);margin-top:3px}
.sg{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:14px}
.sg div{background:var(--surface2);border-radius:14px;padding:10px 12px}
.sg span{display:block;font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--muted)}
.sg b{display:block;font-size:18px;margin-top:3px;letter-spacing:-.01em;white-space:nowrap}.sg b small{font-size:12px;color:var(--muted);margin-left:2px}
.chips{display:flex;gap:6px;overflow-x:auto;padding-bottom:2px;scrollbar-width:none;-webkit-overflow-scrolling:touch}
.chips::-webkit-scrollbar{display:none}
.chip{flex:none;border:1px solid var(--line);background:var(--surface2);color:var(--muted);border-radius:999px;padding:7px 12px;font:inherit;font-size:13px;font-weight:600;cursor:pointer}
.chip.on{background:var(--accent-soft);color:var(--accent);border-color:var(--accent-line)}
#lcv{width:100%;height:250px;display:block;margin-top:10px;touch-action:pan-y}
.ro{display:flex;justify-content:space-between;gap:10px;font-size:13px;color:var(--muted);margin-top:8px;min-height:18px}
.ro b{color:var(--text)}
nav{position:fixed;left:0;right:0;bottom:0;z-index:30;border-top:1px solid var(--line);padding:8px 10px calc(8px + env(safe-area-inset-bottom))}
.nav{max-width:560px;margin:0 auto;display:grid;grid-template-columns:repeat(4,1fr);gap:4px}
.tab{border:0;background:none;color:var(--muted);display:flex;flex-direction:column;align-items:center;gap:4px;padding:9px 0 8px;border-radius:16px;font:inherit;font-size:11px;font-weight:600;cursor:pointer;transition:color .2s,background .2s}
.tab svg{width:22px;height:22px;fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}
.tab.active{color:var(--accent);background:var(--accent-soft)}
@media(max-width:350px){.big{font-size:48px}.ring{width:190px;height:190px}.stats .v{font-size:18px}.stats.q4 .v{font-size:15px}}
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
<div class="seg s3" id="bseg" hidden><button id="bs_cells" onclick="go('cells')">Cells</button><button id="bs_bms" onclick="go('bms')">BMS</button><button id="bs_charger" onclick="go('charger')">Charger</button></div>
<!-- ================= HOME ================= -->
<section class="panel active" id="home">
 <div class="card hero">
  <div class="ring"><svg viewBox="0 0 120 120"><circle class="trk" cx="60" cy="60" r="52"/><circle class="rb" id="rb" cx="60" cy="60" r="52" transform="rotate(-90 60 60)"/></svg>
   <div class="ringc"><div class="big"><span id="soc_i">--</span><span class="dec" id="soc_d"></span><span class="unit">%</span></div><div class="cap">State of charge</div><div class="cap2">actual, without buffer</div></div></div>
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

<!-- ================= CELLS ================= -->
<section class="panel" id="cells">
 <div class="card hero sm">
  <h2>Pack balance &middot; 14S</h2>
  <div class="stats flat">
   <div><span class="k">Lowest</span><span class="v" id="cmin">--</span><span class="s" id="cmin_c"></span></div>
   <div><span class="k">Highest</span><span class="v" id="cmax">--</span><span class="s" id="cmax_c"></span></div>
   <div><span class="k">Spread</span><span class="v" id="cspr">--</span><span class="s">max &minus; min</span></div>
  </div>
 </div>
 <div class="card"><h2>Cell health range</h2>
  <div class="r"><span>Maximum cell SoH</span><span class="v" id="cmaxs">--</span></div>
  <div class="r"><span>Minimum cell SoH</span><span class="v" id="cmins">--</span></div>
 </div>
 <div class="cells" id="cellgrid"></div>
 <p class="note">Bar shows each cell between the lowest and highest voltage. Badge shows cell health (SoH). Green is balanced, amber and red mean the cell is drifting from the pack average.</p>
</section>

<!-- ================= CHARGER ================= -->
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

<!-- ================= BMS ================= -->
<section class="panel" id="bms">
 <div class="card hero sm">
  <h2>Cell temperatures</h2>
  <div class="stats flat q4">
   <div><span class="k">T1</span><span class="v" id="ct0">--</span></div>
   <div><span class="k">T2</span><span class="v" id="ct1">--</span></div>
   <div><span class="k">T3</span><span class="v" id="ct2">--</span></div>
   <div><span class="k">T4</span><span class="v" id="ct3">--</span></div>
  </div>
 </div>
 <div class="card"><h2>BMS temperatures</h2>
  <div class="r"><span>AFE die</span><span class="v" id="die">--</span></div>
  <div class="r"><span>FET<em>probable</em></span><span class="v" id="eft">--</span></div>
  <div class="r"><span>ECI<em>probable</em></span><span class="v" id="eci">--</span></div>
  <div class="r"><span>Sense resistor<em>probable</em></span><span class="v" id="sr">--</span></div>
  <div class="r"><span>Cell temp 5<em>unused on NMC</em></span><span class="v" id="ct4">--</span></div>
 </div>
 <div class="card"><h2>Pack estimates</h2>
  <div class="r"><span>Pack current</span><span class="v" id="ci">--</span></div>
  <div class="r"><span>State of energy</span><span class="v" id="soe">--</span></div>
  <div class="r"><span>SoC (EKF)</span><span class="v" id="sekf">--</span></div>
  <div class="r"><span>Max SoC</span><span class="v" id="msoc">--</span></div>
  <div class="r"><span>Min SoC estimate</span><span class="v" id="minsoc">--</span></div>
  <div class="r"><span>Min V1 estimate</span><span class="v" id="v1">--</span></div>
  <div class="r"><span>Min V2 estimate</span><span class="v" id="v2">--</span></div>
 </div>
 <div class="card"><h2>Current limits</h2>
  <div class="r"><span>Derated discharge</span><span class="v" id="dd">--</span></div>
  <div class="r"><span>Derated charge</span><span class="v" id="dc">--</span></div>
  <div class="r"><span>Derated regen</span><span class="v" id="dr">--</span></div>
  <div class="r"><span>Discharge limit (temp)</span><span class="v" id="dl">--</span></div>
  <div class="r"><span>Charge algo &middot; cell rule</span><span class="v" id="cr">--</span></div>
  <div class="r"><span>Charge algo &middot; rate limited</span><span class="v" id="crl">--</span></div>
  <div class="r"><span>Charge algo &middot; post quantizer</span><span class="v" id="cpq">--</span></div>
 </div>
</section>

<!-- ================= MORE ================= -->
<section class="panel" id="more">
 <div class="card hero sm">
  <h2>Motor</h2>
  <div class="stats flat">
   <div><span class="k">Speed</span><span class="v" id="rpm">--</span><span class="s">RPM</span></div>
   <div><span class="k">Current</span><span class="v" id="mbi">--</span><span class="s">battery</span></div>
   <div><span class="k">Temp</span><span class="v" id="mt">--</span><span class="s">motor</span></div>
  </div>
 </div>
 <div class="card"><h2>Controller &amp; throttle</h2>
  <div class="r"><span>Controller temp</span><span class="v" id="ctemp">--</span></div>
  <div class="r"><span>PDU voltage</span><span class="v" id="pdu">--</span></div>
  <div class="tr"><div class="trh"><span>Throttle out</span><b id="thro">--</b></div><div class="meter"><i id="thro_b"></i></div></div>
  <div class="tr"><div class="trh"><span>Throttle 1</span><b id="thr1">--</b></div><div class="meter"><i id="thr1_b"></i></div></div>
  <div class="tr"><div class="trh"><span>Throttle 3</span><b id="thr3">--</b></div><div class="meter"><i id="thr3_b"></i></div></div>
 </div>
 <div class="card"><h2>Environment</h2>
  <div class="r"><span>Ambient temp</span><span class="v" id="amb">--</span></div>
  <div class="r"><span>Humidity</span><span class="v" id="hum">--</span></div>
 </div>
 <div class="card"><h2>Drive assists &amp; locks</h2><div id="sw2"></div></div>
 <div class="card"><h2>Vehicle &amp; firmware</h2>
  <div class="r"><span>Odometer</span><span class="v" id="odo">--</span></div>
  <div class="r"><span>BMS hardware</span><span class="v mono" id="bhw">--</span></div>
  <div class="r"><span>BMS firmware</span><span class="v mono" id="bfw">--</span></div>
  <div class="r"><span>BCM firmware</span><span class="v mono" id="cfw">--</span></div>
  <div class="r"><span>Gateway firmware</span><span class="v mono" id="gfw">--</span></div>
  <div class="r"><span>Motor firmware</span><span class="v mono" id="mfw">--</span></div>
  <div class="r"><span>Battery BIN</span><span class="v mono" id="bin">--</span></div>
 </div>
</section>

<!-- ================= CHART ================= -->
<section class="panel" id="chart">
<div class="seg" id="seg"><button id="sg_live" class="on" onclick="goSub('live')">Live</button><button id="sg_logs" onclick="goSub('logs')">Logs</button></div>

<div id="vLogs" hidden>
 <div class="card"><h2>Storage</h2>
  <div class="sbar"><i id="sbar"></i></div>
  <div class="stxt" id="stxt">--</div>
 </div>
 <div class="card"><div class="rec"><p id="recs">--</p><button class="btn sm" id="mkb" onclick="lmark()" hidden>Add marker</button></div></div>
 <div id="lglist"></div>
 <p class="note">Rides start when the motor turns with the key on and end 30 s after key off. Charges are recorded while the charger is connected. When storage is full the oldest session is deleted. Double-press the scanner button during a ride to add a marker.</p>
 <div class="btns" style="margin-top:14px"><button class="btn del" onclick="ldelAll()">Delete all logs</button></div>
</div>

<div id="vSess" hidden>
 <button class="back" onclick="history.back()"><svg viewBox="0 0 24 24"><path d="m15 18-6-6 6-6"/></svg>Logs</button>
 <div class="card">
  <p class="stl" id="stitle">--</p><div class="sdt" id="sdate"></div>
  <div class="sg" id="sgrid"></div>
 </div>
 <div class="card">
  <div class="chips" id="lchips"></div>
  <canvas id="lcv"></canvas>
  <div class="ro"><span id="ro_t">Tap the chart to read values</span><b id="ro_v"></b></div>
  <div class="btns">
   <button class="btn" onclick="lzoom(.5)">Zoom in</button>
   <button class="btn" onclick="lzoom(2)">Zoom out</button>
   <button class="btn" onclick="lzoom(0)">Reset</button>
  </div>
 </div>
 <div class="btns" style="margin-top:14px">
  <button class="btn pri" onclick="lcsv()">Download CSV</button>
  <button class="btn del" onclick="ldel()">Delete</button>
 </div>
</div>

<div id="vLive">
 <div class="card"><h2>Live chart</h2>
  <select class="sel" id="csig" aria-label="Signal"></select>
  <div class="btns">
   <button class="btn pri" id="cgo" onclick="chStart()">Start</button>
   <button class="btn" id="cstop" onclick="chStop(false)">Stop</button>
   <button class="btn" id="cdl" onclick="chCsv()" disabled>CSV</button>
  </div>
  <p class="cst" id="cst">Pick a signal and press Start. 45 s at 2 samples per second.</p>
 </div>
 <div class="card">
  <div class="cname"><b id="cname">--</b><span id="cunit"></span></div>
  <div class="stats q4" style="margin-top:12px;padding-top:12px">
   <div><span class="k">Now</span><span class="v" id="cnow">--</span></div>
   <div><span class="k">Min</span><span class="v" id="cmn">--</span></div>
   <div><span class="k">Max</span><span class="v" id="cmx">--</span></div>
   <div><span class="k">Avg</span><span class="v" id="cav">--</span></div>
  </div>
  <canvas id="ccv"></canvas>
 </div>
 <p class="note">Other tabs pause while the chart is open, so the scanner only serves one request at a time.</p>
</div>
</section>
</main>

<nav><div class="nav">
 <button class="tab active" id="tb_home" onclick="go('home')"><svg viewBox="0 0 24 24"><rect x="3" y="3" width="7" height="9" rx="1.8"/><rect x="14" y="3" width="7" height="5" rx="1.8"/><rect x="14" y="12" width="7" height="9" rx="1.8"/><rect x="3" y="16" width="7" height="5" rx="1.8"/></svg>Home</button>
 <button class="tab" id="tb_bat" onclick="go(batSub)"><svg viewBox="0 0 24 24"><rect x="2" y="7" width="18" height="10" rx="2.5"/><path d="M22 11v2M6 10.5v3M10 10.5v3M14 10.5v3"/></svg>Battery</button>
 <button class="tab" id="tb_more" onclick="go('more')"><svg viewBox="0 0 24 24"><path d="M4 6h9M19 6h1M4 12h2M12 12h8M4 18h11M19 18h1"/><circle cx="16" cy="6" r="2.2"/><circle cx="9" cy="12" r="2.2"/><circle cx="17" cy="18" r="2.2"/></svg>Vehicle</button>
 <button class="tab" id="tb_chart" onclick="go('chart')"><svg viewBox="0 0 24 24"><path d="M3 3v18h18"/><path d="m7 15 4-5 3 3 5-7"/></svg>Data</button>
</div></nav>

<script>
const $=id=>document.getElementById(id);
const T=(id,v)=>{const e=$(id);if(e)e.textContent=v};
const f=(n,d)=>Number(n).toFixed(d);
const U=(n,d,u)=>f(n,d)+'<small>'+u+'</small>';
const H=(id,html)=>{const e=$(id);if(e)e.innerHTML=html};
const DEG='\u00B0C';

/* ---------- theme ---------- */
function setTheme(t){
  document.documentElement.setAttribute('data-theme',t);
  document.querySelector('meta[name=theme-color]').content=t==='dark'?'#0f1311':'#f3f4f1';
  try{localStorage.setItem('aobd-theme',t)}catch(e){}
  if(typeof redrawChart==='function')try{redrawChart()}catch(e){}
}
$('theme').onclick=()=>setTheme(document.documentElement.getAttribute('data-theme')==='dark'?'light':'dark');

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
(function(){let h='';for(let i=0;i<14;i++)h+='<div class="cell"><div class="ch"><span class="cn">C'+(i+1)+'</span><span class="hb" id="ch'+i+'">--</span></div><div class="cv"><span id="cv'+i+'">--</span><u>V</u></div><div class="meter"><i id="cb'+i+'"></i></div><div class="cs"><span>SoC</span><b id="cs'+i+'">--</b></div></div>';$('cellgrid').innerHTML=h})();

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
function ok(){fails=0;setConn('live')}
function bad(){if(++fails>=2)setConn('off')}

/* ---------- refreshers ---------- */
const R={
 async home(){
  const j=await get('/d');
  const s=f(j.soc,2).split('.');T('soc_i',s[0]);T('soc_d','.'+s[1]);
  const rb=$('rb');rb.style.strokeDashoffset=326.73*(1-Math.max(0,Math.min(100,j.soc))/100);
  rb.setAttribute('class','rb'+(j.soc<15?' bad':j.soc<30?' warn':''));
  H('range',U(j.range,0,'km'));H('volt',U(j.volt,2,'V'));T('mode',j.mode);
  H('soh',U(j.soh,2,'%'));H('bdelta',U(j.bdelta,2,'%'));H('imb',U(j.imb,4,'V'));
  ['key','kill','storage','stand','dfan','start','fbrake','rbrake','horn','hbeam','indl','indr','indc'].forEach(k=>setTile(k,j[k]));
 },
 async cells(){
  const j=await get('/c'),v=j.v,h=j.h,sc=j.s,pos=v.filter(x=>x>0);
  H('cmaxs',U(j.maxS,2,'%'));H('cmins',U(j.minS,2,'%'));
  for(let i=0;i<14;i++)T('cs'+i,f(sc[i],2)+'%');
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
  const j=await get('/g');
  H('coutv',U(j.coutv,2,'V'));H('couti',U(j.couti,2,'A'));H('cinv',U(j.cinv,2,'V'));
  const w=j.watts;T('cw_n',w>=1000?f(w/1000,2):f(w,1));T('cw_u',w>=1000?'kW':'W');
  setBadge('cconnected',j.cconnected,'Charger connected','Not connected');
  setBadge('bal',j.bal,'Active','Idle');
  T('cfan1',j.fan1);T('cfan2',j.fan2);
  setBadge('clock1',j.clock1,'Locked','Unlocked');setBadge('clock2',j.clock2,'Locked','Unlocked');
 },
 async bms(){
  const j=await get('/b');
  for(let i=0;i<4;i++)H('ct'+i,U(j.ct[i],1,'\u00B0'));
  H('ct4',U(j.ct[4],2,DEG));
  ['die','eft','eci','sr'].forEach(k=>H(k,U(j[k],2,DEG)));
  H('ci',U(j.ci,2,'A'));
  ['soe','sekf','msoc','minsoc'].forEach(k=>H(k,U(j[k],2,'%')));
  H('v1',U(j.v1,4,'V'));H('v2',U(j.v2,4,'V'));
  ['dd','dc','dr','dl','cr','crl','cpq'].forEach(k=>H(k,U(j[k],2,'A')));
 },
 async more(){
  const j=await get('/m');
  T('rpm',j.rpm);H('mbi',U(j.mbi,1,'A'));H('mt',U(j.mt,0,DEG));
  H('ctemp',U(j.ctemp,0,DEG));H('pdu',U(j.pdu,2,'V'));
  ['thro','thr1','thr3'].forEach(k=>{H(k,U(j[k],0,'%'));$(k+'_b').style.width=Math.max(0,Math.min(100,j[k]))+'%'});
  H('amb',U(j.amb,2,DEG));H('hum',U(j.hum,2,'%'));
  setTile('feco',j.feco);setTile('seco',j.seco);setTile('seat',j.seat);
  H('odo',U(j.odo,1,'km'));
  T('bhw',j.bhw);T('bfw',j.bfw);T('cfw',j.cfw);T('gfw',j.gfw);T('mfw',j.mfw);T('bin',j.bin||'--');
 }
};
const TITLES={home:'Dashboard',cells:'Battery \u00B7 cells',charger:'Battery \u00B7 charger',bms:'Battery \u00B7 BMS',more:'Vehicle',chart:'Live chart'};
const BAT=['cells','bms','charger'];let batSub='cells';
let sub='live';

/* ---------- tabs + polling ---------- */
let cur='home',busy=false,timer;
/* ---------- navigation history (phone back button) ---------- */
function push(extra){
  const st=Object.assign({t:cur,s:sub},extra||{}),o=history.state;
  if(o&&o.t===st.t&&o.s===st.s&&(o.id||0)===(st.id||0))return;
  try{history.pushState(st,'')}catch(e){}
}
function go(t){if(t===cur&&!(t==='chart'&&sub==='sess'))return;if(t==='chart'&&sub==='sess')sub='logs';showTab(t);push()}
function goSub(s){if(s===sub)return;setSub(s);push()}
window.addEventListener('popstate',e=>{
  const st=e.state||{t:'home',s:'live'};
  if(st.t==='chart'){
    sub=st.s==='sess'?'logs':st.s;
    if(cur!=='chart')showTab('chart');else setSub(sub);
    if(st.s==='sess'&&st.id)openSess(st.id,true);
  }else showTab(st.t);
});
function showTab(t){
  if(cur==='chart'&&t!=='chart'){chStop(true);clearTimeout(lgTimer)}
  cur=t;
  document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('active',p.id===t));
  const nb=BAT.includes(t)?'bat':t;if(nb==='bat')batSub=t;
  document.querySelectorAll('.tab').forEach(b=>b.classList.toggle('active',b.id==='tb_'+nb));
  $('bseg').hidden=nb!=='bat';
  BAT.forEach(x=>$('bs_'+x).classList.toggle('on',x===t));
  T('sub',TITLES[t]);window.scrollTo(0,0);
  if(t==='chart'){clearTimeout(timer);setSub(sub)}else tick();
}
async function tick(){
  clearTimeout(timer);
  if(cur==='chart')return;
  if(busy||document.hidden){timer=setTimeout(tick,250);return}
  busy=true;
  try{await R[cur]();ok()}catch(e){bad()}
  busy=false;
  if(cur!=='chart')timer=setTimeout(tick,cur==='home'?1000:1500);
}
document.addEventListener('visibilitychange',()=>{if(!document.hidden)tick()});

/* ---------- live chart ---------- */
const EP={d:'/d',c:'/c',g:'/g',b:'/b',m:'/m'};
const GRP={d:'Battery',b:'Battery',g:'Charging',m:'Motor'};
const SIG={
 soc:['SoC','%','d'],volt:['Pack voltage','V','d'],ci:['Pack current','A','b'],imb:['Voltage imbalance','V','d'],
 'ct.0':['Cell temp 1',DEG,'b'],'ct.1':['Cell temp 2',DEG,'b'],'ct.2':['Cell temp 3',DEG,'b'],'ct.3':['Cell temp 4',DEG,'b'],
 watts:['Charging power','W','g'],couti:['Charging current','A','g'],
 rpm:['Motor RPM','rpm','m'],mbi:['Motor current','A','m'],thro:['Throttle','%','m'],mt:['Motor temp',DEG,'m'],ctemp:['Controller temp',DEG,'m']
};
const sel=$('csig');
(function(){let o='';for(const g of [...new Set(Object.values(GRP))]){o+='<optgroup label="'+g+'">';for(const k in SIG)if(GRP[SIG[k][2]]===g)o+='<option value="'+k+'">'+SIG[k][0]+'</option>';o+='</optgroup>'}sel.innerHTML=o;sel.value='rpm'})();
sel.onchange=()=>{if(!CH.run){CH.k=sel.value;CH.pts=[];$('cdl').disabled=true;chDraw()}};
const pick=(j,k)=>k.split('.').reduce((o,x)=>o==null?o:o[x],j);
const nf=v=>{const a=Math.abs(v);return a>=1000?v.toFixed(0):a>=100?v.toFixed(1):a>=1?v.toFixed(2):v.toFixed(4)};
const CH={k:'rpm',pts:[],t0:0,run:false,tm:0};
const cv=$('ccv'),g=cv.getContext('2d');

function chStart(){
  chStop(true);
  CH.k=sel.value;CH.pts=[];CH.t0=Date.now();CH.run=true;
  sel.disabled=true;$('cgo').disabled=true;$('cdl').disabled=true;
  T('cst','Recording 0 s / 45 s');chDraw();chPoll();
}
async function chPoll(){
  if(!CH.run)return;
  const st=Date.now(),el=(st-CH.t0)/1000;
  if(el>=45){chStop(false);return}
  try{const j=await get(EP[SIG[CH.k][2]]);const v=Number(pick(j,CH.k));if(isFinite(v))CH.pts.push({t:el,v:v});ok()}catch(e){bad()}
  if(!CH.run)return;
  T('cst','Recording '+Math.floor(el)+' s / 45 s \u00B7 '+CH.pts.length+' points');
  chDraw();
  CH.tm=setTimeout(chPoll,Math.max(0,500-(Date.now()-st)));
}
function chStop(silent){
  const was=CH.run;CH.run=false;clearTimeout(CH.tm);
  sel.disabled=false;$('cgo').disabled=false;$('cdl').disabled=!CH.pts.length;
  if(!silent&&was)T('cst',CH.pts.length?'Done \u00B7 '+CH.pts.length+' points recorded':'Stopped');
}
function chCsv(){
  const s=SIG[CH.k];let csv='time_s,'+s[0].replace(/[ ,]+/g,'_')+'_'+s[1].replace('\u00B0','deg')+'\n';
  for(const p of CH.pts)csv+=p.t.toFixed(1)+','+p.v+'\n';
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([csv],{type:'text/csv'}));
  a.download='aobd_'+CH.k.replace('.','_')+'.csv';a.click();
  setTimeout(()=>URL.revokeObjectURL(a.href),2000);
}
/* ---------- shared line-chart renderer ----------
   o = {t:[x], ys:[[y],...], x0, x1, xfmt, marks:[x], cross:index, dot:bool, empty:'text'} */
function plot(cv,o){
  const r=cv.getBoundingClientRect();if(!r.width)return;
  const d=window.devicePixelRatio||1,W=r.width,Hh=r.height,w0=Math.round(W*d),h0=Math.round(Hh*d);
  if(cv.width!==w0||cv.height!==h0){cv.width=w0;cv.height=h0}
  const g=cv.getContext('2d');g.setTransform(d,0,0,d,0,0);g.clearRect(0,0,W,Hh);
  const cs=getComputedStyle(document.documentElement),C=n=>cs.getPropertyValue(n).trim();
  const L=50,Rt=10,Tp=10,B=24,pw=W-L-Rt,ph=Hh-Tp-B,x0=o.x0,x1=o.x1>o.x0?o.x1:o.x0+1;
  const X=t=>L+pw*(t-x0)/(x1-x0);
  g.font='11px -apple-system,system-ui,sans-serif';g.fillStyle=C('--muted');g.strokeStyle=C('--line');g.lineWidth=1;
  g.textAlign='center';
  for(let k=0;k<=3;k++){const t=x0+(x1-x0)*k/3;g.fillText(o.xfmt(t),Math.min(Math.max(X(t),L+16),W-Rt-16),Hh-6)}
  const TT=o.t;let a=0,b=TT.length-1;
  while(a<b&&TT[a]<x0)a++;while(b>a&&TT[b]>x1)b--;
  if(!TT.length||TT[a]>x1||TT[b]<x0){g.fillText(o.empty||'',L+pw/2,Tp+ph/2);return}
  let mn=Infinity,mx=-Infinity;
  for(const y of o.ys)for(let i=a;i<=b;i++){const v=y[i];if(v<mn)mn=v;if(v>mx)mx=v}
  let lo=mn,hi=mx;if(hi-lo<1e-6){const e=Math.abs(hi)*0.01||0.5;lo-=e;hi+=e}
  const pad=(hi-lo)*0.12;lo-=pad;hi+=pad;
  const Y=v=>Tp+ph*(1-(v-lo)/(hi-lo));
  g.textAlign='right';
  for(let i=0;i<=3;i++){const v=lo+(hi-lo)*i/3,y=Y(v);g.beginPath();g.moveTo(L,y);g.lineTo(W-Rt,y);g.stroke();g.fillText(nf(v),L-6,y+4)}
  if(o.marks&&o.marks.length){
    g.save();g.setLineDash([4,4]);g.strokeStyle=C('--warn');g.lineWidth=1.5;
    for(const m of o.marks)if(m>=x0&&m<=x1){g.beginPath();g.moveTo(X(m),Tp);g.lineTo(X(m),Tp+ph);g.stroke()}
    g.restore();
  }
  const cols=[C('--accent'),C('--warn')],step=Math.max(1,Math.floor((b-a)/(pw*1.5)));
  g.save();g.beginPath();g.rect(L,Tp-4,pw,ph+8);g.clip();
  o.ys.forEach((y,si)=>{
    g.beginPath();let first=true;
    for(let i=a;;i+=step){if(i>b)i=b;const px=X(TT[i]),py=Y(y[i]);first?g.moveTo(px,py):g.lineTo(px,py);first=false;if(i===b)break}
    g.strokeStyle=cols[si%2];g.lineWidth=2;g.lineJoin='round';g.lineCap='round';g.stroke();
    if(o.ys.length===1){g.lineTo(X(TT[b]),Tp+ph);g.lineTo(X(TT[a]),Tp+ph);g.closePath();g.globalAlpha=.12;g.fillStyle=cols[0];g.fill();g.globalAlpha=1}
  });
  g.restore();
  const dotAt=(i)=>{o.ys.forEach((y,si)=>{g.beginPath();g.arc(X(TT[i]),Y(y[i]),4.5,0,7);g.fillStyle=cols[si%2];g.fill();g.strokeStyle=C('--surface');g.lineWidth=2;g.stroke()})};
  if(o.cross!=null&&o.cross>=a&&o.cross<=b){
    g.strokeStyle=C('--muted');g.lineWidth=1;g.beginPath();g.moveTo(X(TT[o.cross]),Tp);g.lineTo(X(TT[o.cross]),Tp+ph);g.stroke();
    dotAt(o.cross);
  } else if(o.dot)dotAt(b);
}

function chDraw(){
  const s=SIG[CH.k]||SIG.rpm,P=CH.pts;
  T('cname',s[0]);T('cunit',s[1]);
  plot(cv,{t:P.map(p=>p.t),ys:[P.map(p=>p.v)],x0:0,x1:45,xfmt:x=>Math.round(x)+' s',dot:true,
    empty:CH.run?'Waiting for data\u2026':'Press Start to record'});
  if(!P.length){['cnow','cmn','cmx','cav'].forEach(id=>T(id,'--'));return}
  let mn=Infinity,mx=-Infinity,sum=0;for(const p of P){if(p.v<mn)mn=p.v;if(p.v>mx)mx=p.v;sum+=p.v}
  H('cnow',nf(P[P.length-1].v));H('cmn',nf(mn));H('cmx',nf(mx));H('cav',nf(sum/P.length));
}

/* ---------- data logs ---------- */
let LG=null,LGM={},lgTimer=0,SV=null,LV={k:'',x0:0,x1:1,ci:-1};
const p2=n=>String(n).padStart(2,'0');
const fdur=s=>{s=Math.round(s);return s>=3600?Math.floor(s/3600)+'h '+p2(Math.floor(s/60)%60)+'m':Math.floor(s/60)+'m '+p2(s%60)+'s'};
const fclk=s=>{s=Math.max(0,Math.round(s));return s>=3600?Math.floor(s/3600)+':'+p2(Math.floor(s/60)%60)+':'+p2(s%60):Math.floor(s/60)+':'+p2(s%60)};
const fdate=ep=>ep?new Date(ep*1000).toLocaleString([],{weekday:'short',day:'numeric',month:'short',hour:'2-digit',minute:'2-digit'}):'Date unknown';
const fldt=ep=>{const z=new Date(ep*1000);return z.getFullYear()+'-'+p2(z.getMonth()+1)+'-'+p2(z.getDate())+' '+p2(z.getHours())+':'+p2(z.getMinutes())+':'+p2(z.getSeconds())};
const mb=b=>(b/1048576).toFixed(2)+' MB';
const ICON={1:'<svg viewBox="0 0 24 24"><path d="M3.3 17a9 9 0 1 1 17.4 0"/><path d="m12 14 4-4"/></svg>',2:'<svg viewBox="0 0 24 24"><path d="M13 2 4 14h7l-1 8 9-12h-7z"/></svg>'};

function setSub(s){
  if(s!=='live'&&CH.run)chStop(true);
  clearTimeout(lgTimer);
  sub=s;
  $('vLive').hidden=s!=='live';$('vLogs').hidden=s!=='logs';$('vSess').hidden=s!=='sess';
  $('seg').hidden=s==='sess';
  $('sg_live').classList.toggle('on',s==='live');$('sg_logs').classList.toggle('on',s!=='live');
  T('sub',s==='live'?'Live chart':s==='logs'?'Logs':'Session');
  window.scrollTo(0,0);
  if(s==='live')requestAnimationFrame(chDraw);
  if(s==='logs')loadLogs();
}
function redrawChart(){if(cur!=='chart')return;if(sub==='live')chDraw();else if(sub==='sess')drawLog()}

async function loadLogs(){
  clearTimeout(lgTimer);
  if(cur!=='chart'||sub!=='logs')return;
  try{const j=await get('/logs');ok();LG=j;renderLogs(j)}catch(e){bad()}
  if(cur==='chart'&&sub==='logs')lgTimer=setTimeout(loadLogs,5000);
}
function renderLogs(j){
  if(j.err){T('stxt','Storage not available');H('recs','Logging is off. In Arduino IDE pick a partition scheme with SPIFFS space and re-flash.');H('lglist','');return}
  const free=Math.max(0,j.total-j.used);
  $('sbar').style.width=(j.used/j.total*100).toFixed(1)+'%';
  T('stxt',mb(j.used)+' of '+mb(j.total)+' used \u00b7 about '+Math.floor(free/115200)+' h of riding left');
  if(j.rec.on){
    H('recs','<span class="recdot"></span><b>Recording '+(j.rec.ty==2?'charge':'ride')+'</b><br>'+fdur(j.rec.du)+' \u00b7 '+j.rec.n+' points');
    $('mkb').hidden=j.rec.ty!=1;
  }else{H('recs','Not recording. Rides and charges are saved automatically.');$('mkb').hidden=true}
  LGM={};j.s.forEach(s=>LGM[s.id]=s);
  const list=j.s.slice().sort((a,b)=>b.id-a.id);
  if(!list.length){H('lglist','<div class="empty">No sessions yet.<br>Go for a ride or plug in the charger.</div>');return}
  H('lglist',list.map(s=>{
    const ride=s.ty!=2,net=(s.wu-s.wr)/10,km=s.di/1000,soc=f(s.s0/100,0)+'\u2192'+f(s.s1/100,0);
    const title=ride?'Ride \u00b7 '+km.toFixed(1)+' km':'Charge \u00b7 +'+(s.wc/10000).toFixed(2)+' kWh';
    const right=ride&&s.di>=100?'<b>'+(net*1000/s.di).toFixed(0)+'</b><small>Wh/km</small>':'<b>'+soc+'</b><small>% SoC</small>';
    return '<div class="sess" onclick="openSess('+s.id+')"><div class="si'+(ride?'':' c')+'">'+ICON[ride?1:2]+'</div><div class="sb"><b>'+title+'</b><span>'+fdate(s.ep)+' \u00b7 '+fdur(s.du)+(s.mk?' \u00b7 '+s.mk+' marker'+(s.mk>1?'s':''):'')+'</span></div><div class="sv">'+right+'</div></div>';
  }).join(''));
}

async function lmark(){
  const b=$('mkb');
  try{const j=await get('/mark');b.textContent=j.ok?'Added \u2713':'Not recording'}catch(e){b.textContent='Failed'}
  setTimeout(()=>b.textContent='Add marker',1500);
}
async function ldelAll(){
  if(!confirm('Delete all saved logs? This cannot be undone.'))return;
  try{await get('/logdel?id=all')}catch(e){}
  loadLogs();
}

async function openSess(id,fromPop){
  setSub('sess');SV=null;if(!fromPop)push({id});
  T('stitle','Loading\u2026');T('sdate','');H('sgrid','');H('lchips','');
  try{
    const r=await fetch('/log?id='+id,{cache:'no-store'});if(!r.ok)throw 0;
    SV=parseLog(await r.arrayBuffer(),id);
    renderSess();
  }catch(e){T('stitle','Could not load this session')}
}
function parseLog(ab,id){
  const dv=new DataView(ab),n=Math.max(0,Math.floor((ab.byteLength-64)/32)),pis=(LG&&LG.pis)||1;
  const F=()=>new Float32Array(n),D=()=>new Float64Array(n);
  const d={id,n,ty:dv.getUint8(5),ep:dv.getUint32(8,true),t:D(),soc:F(),V:F(),I:F(),P:F(),cmin:F(),cmax:F(),ct:F(),
    rpm:F(),mi:F(),mt:F(),ctt:F(),thr:F(),mode:new Uint8Array(n),odo:D(),fl:new Uint16Array(n),cw:F(),marks:[]};
  for(let i=0;i<n;i++){
    const o=64+i*32;
    d.t[i]=dv.getUint32(o,true)/10;d.soc[i]=dv.getUint16(o+4,true)/100;d.V[i]=dv.getUint16(o+6,true)/100;
    d.I[i]=dv.getInt16(o+8,true)/100;d.P[i]=d.V[i]*d.I[i]*pis;
    d.cmin[i]=dv.getUint16(o+10,true)/10000;d.cmax[i]=dv.getUint16(o+12,true)/10000;d.ct[i]=dv.getInt16(o+14,true)/100;
    d.rpm[i]=dv.getInt16(o+16,true);d.mi[i]=dv.getInt16(o+18,true)/10;
    d.mt[i]=dv.getInt8(o+20);d.ctt[i]=dv.getInt8(o+21);d.thr[i]=dv.getInt8(o+22);d.mode[i]=dv.getUint8(o+23);
    d.odo[i]=dv.getUint32(o+24,true)/1000;d.fl[i]=dv.getUint16(o+28,true);d.cw[i]=dv.getUint16(o+30,true);
    if(d.fl[i]&128)d.marks.push(d.t[i]);
  }
  return d;
}
const LSIG={
  pwr:['Power','W',d=>[d.P]],cw:['Charger power','W',d=>[d.cw]],soc:['SoC','%',d=>[d.soc]],v:['Voltage','V',d=>[d.V]],
  i:['Pack current','A',d=>[d.I]],rpm:['Motor RPM','rpm',d=>[d.rpm]],thr:['Throttle','%',d=>[d.thr]],
  mt:['Motor temp',DEG,d=>[d.mt]],ctt:['Controller temp',DEG,d=>[d.ctt]],ct:['Cell temp',DEG,d=>[d.ct]],
  cell:['Cell min / max','V',d=>[d.cmin,d.cmax]]
};
function renderSess(){
  const d=SV,s=LGM[d.id],ride=d.ty!=2;
  T('stitle',(ride?'Ride':'Charge')+(s?ride?' \u00b7 '+(s.di/1000).toFixed(1)+' km':' \u00b7 +'+(s.wc/10000).toFixed(2)+' kWh':''));
  T('sdate',fdate(d.ep||(s&&s.ep))+(s?'':' \u00b7 in progress'));
  const c=(k,v,u)=>'<div><span>'+k+'</span><b>'+v+(u?'<small>'+u+'</small>':'')+'</b></div>';
  let h='';
  if(s){
    const net=(s.wu-s.wr)/10,soc=f(s.s0/100,1)+' \u2192 '+f(s.s1/100,1);
    if(ride){
      h=c('Duration',fdur(s.du))+c('Distance',(s.di/1000).toFixed(1),'km')+
        c('Energy used',net.toFixed(0),'Wh')+c('Efficiency',s.di>=100?(net*1000/s.di).toFixed(0):'--','Wh/km')+
        c('Regen',(s.wr/10).toFixed(0),'Wh')+c('SoC',soc,'%')+
        c('Peak power',(s.pw/1000).toFixed(2),'kW')+c('Top RPM',s.rp)+
        c('Max motor',s.mt,DEG)+c('Max controller',s.ct,DEG)+
        c('Max cell temp',(s.cl/100).toFixed(1),DEG)+c('Lowest cell',s.mc?(s.mc/10000).toFixed(3):'--','V');
      if(s.mk)h+=c('Markers',s.mk);
    }else{
      h=c('Duration',fdur(s.du))+c('Energy added',(s.wc/10000).toFixed(2),'kWh')+
        c('SoC',soc,'%')+c('Peak power',(s.pw/1000).toFixed(2),'kW')+
        c('Max cell temp',(s.cl/100).toFixed(1),DEG)+c('End cell spread',(s.sp/10).toFixed(1),'mV');
    }
  }else h=c('Points',d.n)+c('Duration',d.n?fdur(d.t[d.n-1]):'--');
  H('sgrid',h);
  const keys=ride?['pwr','soc','v','i','rpm','thr','mt','ctt','ct','cell']:['cw','soc','v','i','cell','ct'];
  LV={k:keys[0],x0:0,x1:d.n?Math.max(d.t[d.n-1],1):1,ci:-1};
  H('lchips',keys.map(k=>'<button class="chip'+(k===LV.k?' on':'')+'" data-c="'+k+'">'+LSIG[k][0]+'</button>').join(''));
  requestAnimationFrame(drawLog);
}
$('lchips').addEventListener('click',e=>{
  const b=e.target.closest('[data-c]');if(!b)return;
  LV.k=b.dataset.c;
  document.querySelectorAll('#lchips .chip').forEach(x=>x.classList.toggle('on',x===b));
  drawLog();
});
function drawLog(){
  const d=SV;if(!d)return;
  const def=LSIG[LV.k],ys=def[2](d);
  plot($('lcv'),{t:d.t,ys,x0:LV.x0,x1:LV.x1,xfmt:fclk,marks:d.marks,cross:LV.ci>=0?LV.ci:null,empty:'No data in this session'});
  if(LV.ci>=0&&LV.ci<d.n){
    const i=LV.ci;
    T('ro_t',fclk(d.t[i])+(d.ep?' \u00b7 '+new Date((d.ep+d.t[i])*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'}):''));
    T('ro_v',ys.map(a=>nf(a[i])).join(' / ')+' '+def[1]);
  }else{T('ro_t','Tap the chart to read values');T('ro_v','')}
}
function lpick(e){
  const d=SV;if(!d||!d.n)return;
  const cvs=$('lcv'),r=cvs.getBoundingClientRect(),pw=r.width-60;
  let t=LV.x0+(e.clientX-r.left-50)/pw*(LV.x1-LV.x0);
  t=Math.max(LV.x0,Math.min(LV.x1,t));
  let lo=0,hi=d.n-1;while(lo<hi){const m=(lo+hi)>>1;if(d.t[m]<t)lo=m+1;else hi=m}
  if(lo>0&&Math.abs(d.t[lo-1]-t)<Math.abs(d.t[lo]-t))lo--;
  LV.ci=lo;drawLog();
}
(function(){
  const c=$('lcv');
  c.addEventListener('pointerdown',e=>{try{c.setPointerCapture(e.pointerId)}catch(_){}lpick(e)});
  c.addEventListener('pointermove',e=>{if(e.buttons)lpick(e)});
})();
function lzoom(k){
  const d=SV;if(!d||!d.n)return;
  const full=Math.max(d.t[d.n-1],1);
  if(!k){LV.x0=0;LV.x1=full;drawLog();return}
  const ctr=LV.ci>=0?d.t[LV.ci]:(LV.x0+LV.x1)/2;
  const span=Math.min(full,Math.max((LV.x1-LV.x0)*k,Math.min(20,full)));
  LV.x0=Math.max(0,Math.min(full-span,ctr-span/2));LV.x1=LV.x0+span;
  drawLog();
}
function lcsv(){
  const d=SV;if(!d||!d.n)return;
  const cols='time_s'+(d.ep?',local_time':'')+',soc_pct,pack_v,pack_a,power_w,cell_min_v,cell_max_v,cell_temp_max_c,motor_rpm,motor_a,motor_c,controller_c,throttle_pct,drive_mode,odometer_km,charger_w,front_brake,rear_brake,charger,balancing,forced_eco,smart_eco,key,high_beam,marker';
  const out=[cols];
  for(let i=0;i<d.n;i++){
    const fl=d.fl[i],b=k=>(fl>>k)&1;
    out.push([d.t[i].toFixed(1)].concat(d.ep?[fldt(d.ep+d.t[i])]:[],[
      d.soc[i].toFixed(2),d.V[i].toFixed(2),d.I[i].toFixed(2),d.P[i].toFixed(0),d.cmin[i].toFixed(4),d.cmax[i].toFixed(4),
      d.ct[i].toFixed(2),d.rpm[i],d.mi[i].toFixed(1),d.mt[i],d.ctt[i],d.thr[i],d.mode[i],d.odo[i].toFixed(3),d.cw[i],
      b(0),b(1),b(2),b(3),b(4),b(5),b(6),b(8),b(7)]).join(','));
  }
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([out.join('\n')+'\n'],{type:'text/csv'}));
  const stamp=d.ep?fldt(d.ep).slice(0,16).replace(/[-: ]/g,''):String(d.id).padStart(5,'0');
  a.download='aobd_'+(d.ty==2?'charge':'ride')+'_'+stamp+'.csv';a.click();
  setTimeout(()=>URL.revokeObjectURL(a.href),2000);
}
async function ldel(){
  if(!SV||!confirm('Delete this session?'))return;
  try{await get('/logdel?id='+SV.id)}catch(e){}
  history.back();
}

window.addEventListener('resize',redrawChart);
fetch('/time?t='+Math.floor(Date.now()/1000),{cache:'no-store'}).catch(()=>{});

setTheme(document.documentElement.getAttribute('data-theme'));
try{history.replaceState({t:'home',s:'live'},'')}catch(e){}
tick();
</script>
</body></html>
)HTML";

// ===================================================================
//  Fixed-point formatting (C3 has no FPU -> avoid float printf)
// ===================================================================
static const uint32_t P10[] = {1, 10, 100, 1000, 10000};

// writes v / 10^dec with exactly 'dec' decimals, returns end of string
static char* fx(char *b, int32_t v, uint8_t dec){
  uint32_t u;
  if(v < 0){ *b++ = '-'; u = (uint32_t)(-(int64_t)v); } else u = (uint32_t)v;
  uint32_t ip = u / P10[dec], fp = u % P10[dec];
  char tmp[10]; int n = 0;
  do { tmp[n++] = '0' + ip % 10; ip /= 10; } while(ip);
  while(n) *b++ = tmp[--n];
  if(dec){
    *b++ = '.';
    for(int i = dec - 1; i >= 0; i--){ b[i] = '0' + fp % 10; fp /= 10; }
    b += dec;
  }
  *b = 0;
  return b;
}

static void verStr(char *b, const uint8_t *v){ snprintf(b, 12, "%u.%u.%u", v[0], v[1], v[2]); }

static void binStr(char *out, const Telemetry &t){
  int n = 0;
  for(int i = 0; i < 14; i++){
    char c = t.binId[i]; if(!c) break;
    out[n++] = ((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')) ? c : '?';
  }
  out[n] = 0;
}

// ===================================================================
//  Tiny JSON writer (one static buffer, no heap / String)
// ===================================================================
static char  jb[1024];
static char *jp;
static bool  jfirst;

static inline void jc(char c){ if(jp < jb + sizeof(jb) - 1) *jp++ = c; }
static void jputs(const char *s){ while(*s) jc(*s++); }
static void jBegin(){ jp = jb; jfirst = true; jc('{'); }
static void jKey(const char *k){ if(!jfirst) jc(','); jfirst = false; jc('"'); jputs(k); jputs("\":"); }
static void jFx(int32_t v, uint8_t d){ char b[16]; fx(b, v, d); jputs(b); }
static void jNum(const char *k, int32_t v, uint8_t d = 0){ jKey(k); jFx(v, d); }
static void jBool(const char *k, bool v){ jKey(k); jputs(v ? "true" : "false"); }
static void jStr(const char *k, const char *s){ jKey(k); jc('"'); jputs(s); jc('"'); }
static void jSend(){
  jc('}'); *jp = 0;
  server.send_P(200, "application/json", jb, jp - jb);
}

Telemetry getSnap(){
  Telemetry t;
  xSemaphoreTake(mtx, portMAX_DELAY);
  t = snap;
  xSemaphoreGive(mtx);
  return t;
}

// ===================================================================
//  HTTP handlers - one small endpoint per tab
// ===================================================================
void handleRoot(){ server.send_P(200, "text/html", htmlPage); }

// /d  -> Home
void handleHome(){
  Telemetry t = getSnap();
  jBegin();
  jNum("soc", t.soc, 2);      jNum("soh", t.soh, 2);   jNum("bdelta", t.deltaSoc, 2);
  jNum("volt", t.voltage, 2); jNum("imb", t.imbalance, 4);
  jNum("range", t.rangeKm);   jStr("mode", driveModeName(t.driveMode));
  jBool("key", t.keyOn);      jBool("kill", t.killSwitch);   jBool("storage", t.storageSwitch);
  jBool("stand", t.sideStand);jBool("dfan", t.displayFan);
  jBool("start", t.startSwitch); jBool("fbrake", t.frontBrake); jBool("rbrake", t.rearBrake);
  jBool("horn", t.horn);      jBool("hbeam", t.highBeam);
  jBool("indl", t.indicatorLeftSide); jBool("indr", t.indicatorRightSide); jBool("indc", t.indicatorCenterSide);
  jSend();
}

// /c  -> Cells
void handleCells(){
  Telemetry t = getSnap();
  jBegin();
  jKey("v"); jc('['); for(int i=0;i<14;i++){ if(i) jc(','); jFx(t.cellV[i], 4); }   jc(']');
  jKey("h"); jc('['); for(int i=0;i<14;i++){ if(i) jc(','); jFx(t.cellSoh[i], 0); } jc(']');
  jKey("s"); jc('['); for(int i=0;i<14;i++){ if(i) jc(','); jFx(t.cellSoc[i], 2); } jc(']');
  jNum("maxS", t.maxCellSoh, 2); jNum("minS", t.minCellSoh, 2);
  jSend();
}

// /g  -> Charger
void handleCharger(){
  Telemetry t = getSnap();
  const uint64_t uW = (uint64_t)t.chargerOV * (uint64_t)t.chargerOI;   // mV * mA
  jBegin();
  jNum("coutv", t.chargerOV / 10, 2); jNum("couti", t.chargerOI / 10, 2); jNum("cinv", t.chargerIV / 10, 2);
  jNum("watts", (int32_t)(uW / 10000ULL), 2);
  jBool("cconnected", t.chargerConnected); jBool("bal", t.balancing);
  jNum("fan1", t.chargerFan1); jNum("fan2", t.chargerFan2);
  jBool("clock1", t.chargerLock1); jBool("clock2", t.chargerLock2);
  jSend();
}

// /b  -> BMS
void handleBms(){
  Telemetry t = getSnap();
  jBegin();
  jKey("ct"); jc('['); for(int i=0;i<5;i++){ if(i) jc(','); jFx(t.cellTemp[i], 2); } jc(']');
  jNum("die", t.afeDieTemp, 2); jNum("eft", t.eftTemp, 2); jNum("eci", t.eciTemp, 2); jNum("sr", t.senseTemp, 2);
  jNum("ci", t.packCurrent, 2);
  jNum("soe", t.soe, 2); jNum("sekf", t.socEkf, 2); jNum("msoc", t.maxSoc, 2); jNum("minsoc", t.minSocEst, 2);
  jNum("v1", t.minV1Est, 4); jNum("v2", t.minV2Est, 4);
  jNum("dd", t.deratedDsgI, 2); jNum("dc", t.deratedChgI, 2); jNum("dr", t.deratedRegenI, 2);
  jNum("dl", t.dsgLimTemp, 2);  jNum("cr", t.chgCellRule, 2); jNum("crl", t.chgCmdRateLtd, 2);
  jNum("cpq", t.chgPostQuant, 2);
  jSend();
}

// /m  -> Motor & more
void handleMore(){
  Telemetry t = getSnap();
  char b[16];
  jBegin();
  jNum("rpm", t.rpm); jNum("mbi", t.motorI, 1); jNum("mt", t.motorTemp); jNum("ctemp", t.controllerTemp);
  jNum("thro", t.throttleOut); jNum("thr1", t.throttle1); jNum("thr3", t.throttle3);
  jNum("pdu", t.pduVoltage / 10, 2);
  jNum("hum", t.humidity, 2); jNum("amb", t.ambientTemp, 2);
  jBool("feco", t.forcedEco); jBool("seco", t.smartEco); jBool("seat", t.seatLock);
  jNum("odo", (int32_t)(t.odometer / 100), 1);
  jNum("bhw", t.bmsHwVer);
  verStr(b, t.bmsFw);   jStr("bfw", b);
  verStr(b, t.bcmFw);   jStr("cfw", b);
  verStr(b, t.gtwFw);   jStr("gfw", b);
  verStr(b, t.motorFw); jStr("mfw", b);
  binStr(b, t);         jStr("bin", b);
  jSend();
}

// ===================================================================
//  CAN
// ===================================================================
// ===================================================================
//  Data logging (LittleFS)  -  all file access happens in loop()
// ===================================================================
static inline int8_t  c8(int32_t v){  return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
static inline int16_t c16(int32_t v){ return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint32_t epochNow(){ return epochBase + millis() / 1000; }

static void logPath(char *b, uint32_t id){ snprintf(b, 16, "/l/%05lu.bin", (unsigned long)id); }

static uint32_t idFromName(const char *n){
  const char *s = strrchr(n, '/');
  return (uint32_t)strtoul(s ? s + 1 : n, nullptr, 10);
}

static bool readHdr(const char *path, LogHdr &h){
  File f = LittleFS.open(path, "r");
  if(!f) return false;
  bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == LOG_MAGIC;
  f.close();
  return ok;
}

static bool writeHdr(const char *path, const LogHdr &h){
  File f = LittleFS.open(path, "r+");
  if(!f) return false;
  f.seek(0);
  f.write((const uint8_t*)&h, sizeof(h));
  f.close();
  return true;
}

// Reads every record of a session and fills in its summary header.
static bool summarize(const char *path, LogHdr &h, uint32_t epochOverride){
  File f = LittleFS.open(path, "r+");
  if(!f) return false;
  if(f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != LOG_MAGIC){ f.close(); return false; }
  if(epochOverride) h.epoch = epochOverride;

  static LogRec rb[16];
  int64_t eUsed = 0, eRegen = 0, eChg = 0;
  uint32_t n = 0, prevT = 0, firstOdo = 0, lastOdo = 0, peak = 0;
  int16_t mMot = -128, mCtl = -128, mCell = -32768;
  uint16_t minCell = 0xFFFF, maxRpm = 0, marks = 0;
  LogRec last = {};
  const uint32_t dtCap = h.type == 2 ? 150 : 30;     // ignore gaps (0.1 s units)

  for(;;){
    size_t got = f.read((uint8_t*)rb, sizeof(rb)) / sizeof(LogRec);
    if(!got) break;
    for(size_t i = 0; i < got; i++){
      const LogRec &r = rb[i];
      if(n == 0){ h.socStart = r.soc; prevT = r.t; }
      uint32_t dt = r.t - prevT; if(dt > dtCap) dt = 0;
      prevT = r.t;
      int32_t p = (int32_t)r.volt * r.packI * PACK_I_SIGN;      // 1e-4 W
      if(p > 0) eUsed += (int64_t)p * dt; else eRegen -= (int64_t)p * dt;
      eChg += (int64_t)r.chgW * dt;
      uint32_t pw = h.type == 2 ? r.chgW : (p > 0 ? (uint32_t)(p / 10000) : 0);
      if(pw > peak) peak = pw;
      if(r.odo){ if(!firstOdo) firstOdo = r.odo; lastOdo = r.odo; }
      if(r.motorT > mMot) mMot = r.motorT;
      if(r.ctrlT  > mCtl) mCtl = r.ctrlT;
      if(r.cellTmax > mCell) mCell = r.cellTmax;
      if(r.cellMin && r.cellMin < minCell) minCell = r.cellMin;
      uint16_t ar = r.rpm < 0 ? -r.rpm : r.rpm;
      if(ar > maxRpm) maxRpm = ar;
      if(r.flags & LF_MARK) marks++;
      last = r; n++;
    }
  }
  h.nrec     = n;
  h.dur      = last.t / 10;
  h.socEnd   = last.soc;
  h.distM    = lastOdo >= firstOdo ? lastOdo - firstOdo : 0;
  h.whUsed   = (int32_t)(eUsed  / 36000000LL);
  h.whRegen  = (int32_t)(eRegen / 36000000LL);
  h.whChg    = (uint32_t)(eChg  / 3600LL);
  h.maxMotorT = n ? mMot : 0; h.maxCtrlT = n ? mCtl : 0; h.maxCellT = n ? mCell : 0;
  h.minCell  = minCell == 0xFFFF ? 0 : minCell;
  h.maxRpm   = maxRpm;
  h.peakW    = peak > 65535 ? 65535 : peak;
  h.endSpread = (last.cellMax > last.cellMin && last.cellMin) ? last.cellMax - last.cellMin : 0;
  h.markers  = marks;
  h.done     = 1;
  f.seek(0);
  f.write((const uint8_t*)&h, sizeof(h));
  f.close();
  return n > 0;
}

// Deletes the oldest finished session. Returns false if none could be deleted.
static bool deleteOldest(){
  File root = LittleFS.open("/l");
  if(!root) return false;
  uint32_t oldest = 0xFFFFFFFF;
  for(File f = root.openNextFile(); f; f = root.openNextFile()){
    uint32_t id = idFromName(f.name());
    f.close();
    if(id && id != (sessActive ? sessId : 0) && id < oldest) oldest = id;
  }
  root.close();
  if(oldest == 0xFFFFFFFF) return false;
  char p[16]; logPath(p, oldest);
  return LittleFS.remove(p);
}

static void ensureSpace(){
  for(int i = 0; i < 50 && LittleFS.totalBytes() - LittleFS.usedBytes() < LOG_RESERVE; i++)
    if(!deleteOldest()) break;
}

static void logFlush(){
  if(!logN || !sessActive) return;
  ensureSpace();
  File f = LittleFS.open(sessPath, "a");
  if(f){ f.write((const uint8_t*)logBuf, logN * sizeof(LogRec)); f.close(); }
  logN = 0;
  lastFlush = millis();
}

static void logStart(uint8_t type, uint32_t now){
  ensureSpace();
  sessId = nextId++;
  logPath(sessPath, sessId);
  LogHdr h = {};
  h.magic = LOG_MAGIC; h.ver = 1; h.type = type;
  h.epoch = timeValid ? epochNow() : 0;
  File f = LittleFS.open(sessPath, "w");
  if(!f) return;
  f.write((const uint8_t*)&h, sizeof(h));
  f.close();
  sessActive = true; sessType = type;
  sessStartMs = now; sessEpoch = h.epoch; sessRecs = 0;
  lastSample = now - 100000; lastFlush = now; logN = 0;
  pendingMarker = false;
}

static void logEnd(uint32_t now){
  logFlush();
  sessActive = false;
  LogHdr h;
  bool ok = summarize(sessPath, h, sessEpoch);
  uint32_t minDur = sessType == 2 ? LOG_MIN_CHARGE_S : LOG_MIN_RIDE_S;
  if(!ok || h.dur < minDur){ LittleFS.remove(sessPath); return; }
  lastSum = h;
  summaryUntil = now + 6000;
  if(!h.epoch && undatedN < 8) undated[undatedN++] = { sessId, now };
}

static void addRecord(const Telemetry &t, uint32_t now){
  if(logN >= LOG_BUF) logFlush();
  LogRec &r = logBuf[logN++];
  uint16_t cmin = 0xFFFF, cmax = 0;
  for(int i = 0; i < 14; i++){
    uint16_t v = t.cellV[i];
    if(!v) continue;
    if(v < cmin) cmin = v;
    if(v > cmax) cmax = v;
  }
  int16_t tmax = t.cellTemp[0];
  for(int i = 1; i < 4; i++) if(t.cellTemp[i] > tmax) tmax = t.cellTemp[i];
  uint64_t uW = (uint64_t)t.chargerOV * t.chargerOI;

  r.t        = (now - sessStartMs) / 100;
  r.soc      = t.soc;
  r.volt     = t.voltage;
  r.packI    = t.packCurrent;
  r.cellMin  = cmin == 0xFFFF ? 0 : cmin;
  r.cellMax  = cmax;
  r.cellTmax = tmax;
  r.rpm      = c16(t.rpm);
  r.motorI   = t.motorI;
  r.motorT   = c8(t.motorTemp);
  r.ctrlT    = c8(t.controllerTemp);
  r.thr      = c8(t.throttleOut);
  r.mode     = (uint8_t)t.driveMode;
  r.odo      = t.odometer;
  r.chgW     = uW / 1000000ULL > 65535 ? 65535 : (uint16_t)(uW / 1000000ULL);
  r.flags    = (t.frontBrake ? LF_FBRAKE : 0) | (t.rearBrake ? LF_RBRAKE : 0) |
               (t.chargerConnected ? LF_CHARGER : 0) | (t.balancing ? LF_BAL : 0) |
               (t.forcedEco ? LF_FECO : 0) | (t.smartEco ? LF_SECO : 0) |
               (t.keyOn ? LF_KEY : 0) | (pendingMarker ? LF_MARK : 0) | (t.highBeam ? LF_HBEAM : 0);
  pendingMarker = false;
  sessRecs++;
}

// Called every 200 ms: starts/ends sessions and takes samples.
static void logTick(const Telemetry &t, uint32_t now){
  if(!logOk) return;
  uint32_t lf = lastFrameMs;
  bool alive = lf && (now - lf) < 3000;

  uint8_t want = 0;
  if(alive){
    if(t.chargerConnected) want = 2;
    else if(t.keyOn && ((sessActive && sessType == 1) || t.rpm != 0)) want = 1;
  }

  if(!sessActive){
    if(want) logStart(want, now);
    pendSince = 0;
  } else if(want != sessType){
    if(!pendSince) pendSince = now;
    if(now - pendSince >= (want ? LOG_SWITCH_MS : LOG_END_MS)){
      logEnd(now);
      pendSince = 0;
      if(want) logStart(want, now);
    }
  } else pendSince = 0;

  if(!sessActive) return;
  uint32_t iv = sessType == 1 ? LOG_RIDE_MS : LOG_CHARGE_MS;
  if(alive && (pendingMarker || now - lastSample >= iv)){
    addRecord(t, now);
    lastSample = now;
  }
  if(logN && now - lastFlush >= LOG_FLUSH_MS) logFlush();
}

static bool addMarker(){
  if(!sessActive) return false;
  pendingMarker = true;
  return true;
}

// Mount the filesystem, finish sessions interrupted by power loss.
static void logInit(){
  logOk = LittleFS.begin(true);
  if(!logOk){ Serial.println("[LOG] LittleFS mount failed - logging disabled"); return; }
  LittleFS.mkdir("/l");
  static uint32_t pending[32]; uint8_t np = 0;
  File root = LittleFS.open("/l");
  for(File f = root.openNextFile(); f; f = root.openNextFile()){
    uint32_t id = idFromName(f.name());
    LogHdr h;
    bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == LOG_MAGIC;
    f.close();
    if(id >= nextId) nextId = id + 1;
    if((!ok || !h.done) && np < 32) pending[np++] = id;
  }
  root.close();
  for(uint8_t i = 0; i < np; i++){
    char p[16]; logPath(p, pending[i]);
    LogHdr h;
    bool ok = summarize(p, h, 0);
    uint32_t minDur = h.type == 2 ? LOG_MIN_CHARGE_S : LOG_MIN_RIDE_S;
    if(!ok || h.dur < minDur) LittleFS.remove(p);
  }
  Serial.printf("[LOG] ready, %u of %u bytes used\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

// ---------------- Log endpoints ----------------
// /logs -> storage info, recording state and every finished session's summary
void handleLogs(){
  char b[300];
  if(!logOk){ server.send(200, "application/json", "{\"err\":1}"); return; }
  if(sessActive) logFlush();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  int n = snprintf(b, sizeof(b),
    "{\"used\":%u,\"total\":%u,\"pis\":%d,\"rec\":{\"on\":%d,\"ty\":%u,\"id\":%lu,\"du\":%lu,\"n\":%lu},\"s\":[",
    (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(), PACK_I_SIGN,
    sessActive ? 1 : 0, sessType, (unsigned long)sessId,
    (unsigned long)(sessActive ? (millis() - sessStartMs) / 1000 : 0), (unsigned long)sessRecs);
  server.sendContent(b, n);
  bool first = true;
  File root = LittleFS.open("/l");
  for(File f = root.openNextFile(); f; f = root.openNextFile()){
    uint32_t id = idFromName(f.name());
    LogHdr h;
    bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == LOG_MAGIC && h.done;
    f.close();
    if(!ok || (sessActive && id == sessId)) continue;
    n = snprintf(b, sizeof(b),
      "%s{\"id\":%lu,\"ty\":%u,\"ep\":%lu,\"du\":%lu,\"n\":%lu,\"di\":%lu,\"wu\":%ld,\"wr\":%ld,\"wc\":%lu,"
      "\"s0\":%u,\"s1\":%u,\"mt\":%d,\"ct\":%d,\"cl\":%d,\"mc\":%u,\"rp\":%u,\"pw\":%u,\"sp\":%u,\"mk\":%u}",
      first ? "" : ",", (unsigned long)id, h.type, (unsigned long)h.epoch, (unsigned long)h.dur,
      (unsigned long)h.nrec, (unsigned long)h.distM, (long)h.whUsed, (long)h.whRegen, (unsigned long)h.whChg,
      h.socStart, h.socEnd, h.maxMotorT, h.maxCtrlT, h.maxCellT, h.minCell, h.maxRpm, h.peakW,
      h.endSpread, h.markers);
    server.sendContent(b, n);
    first = false;
  }
  root.close();
  server.sendContent("]}");
  server.sendContent("");
}

// /log?id=N -> raw binary session file (the page parses it)
void handleLog(){
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  if(!logOk || !id){ server.send(404, "text/plain", "no log"); return; }
  if(sessActive && id == sessId) logFlush();
  char p[16]; logPath(p, id);
  File f = LittleFS.open(p, "r");
  if(!f){ server.send(404, "text/plain", "no log"); return; }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

// /logdel?id=N or id=all
void handleLogDel(){
  if(!logOk){ server.send(200, "application/json", "{\"ok\":0}"); return; }
  String a = server.arg("id");
  if(a == "all"){
    static uint32_t ids[128]; uint8_t k = 0;
    File root = LittleFS.open("/l");
    for(File f = root.openNextFile(); f && k < 128; f = root.openNextFile()){ ids[k++] = idFromName(f.name()); f.close(); }
    root.close();
    for(uint8_t i = 0; i < k; i++){
      if(sessActive && ids[i] == sessId) continue;
      char p[16]; logPath(p, ids[i]); LittleFS.remove(p);
    }
  } else {
    uint32_t id = strtoul(a.c_str(), nullptr, 10);
    if(id && !(sessActive && id == sessId)){ char p[16]; logPath(p, id); LittleFS.remove(p); }
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

// /time?t=<unix seconds>  - the page sends the phone's clock
void handleTime(){
  uint32_t t = strtoul(server.arg("t").c_str(), nullptr, 10);
  if(t > 1600000000UL){
    epochBase = t - millis() / 1000;
    timeValid = true;
    uint32_t now = millis();
    for(uint8_t i = 0; i < undatedN; i++){            // date sessions recorded before
      char p[16]; logPath(p, undated[i].id);
      LogHdr h;
      if(readHdr(p, h) && !h.epoch){
        h.epoch = epochNow() - (now - undated[i].endMs) / 1000 - h.dur;
        writeHdr(p, h);
      }
    }
    undatedN = 0;
    if(sessActive && !sessEpoch) sessEpoch = epochNow() - (now - sessStartMs) / 1000;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

// /mark -> adds a marker to the session being recorded
void handleMark(){
  bool ok = addMarker();
  if(ok){ toastMsg = "MARKER ADDED"; toastUntil = millis() + 1200; }
  server.send(200, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
}

void setupCAN(){
  pinMode(CAN_RX, INPUT_PULLUP);
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX,(gpio_num_t)CAN_RX,TWAI_MODE_NORMAL);
  g.rx_queue_len = 128; g.tx_queue_len = 0;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  ESP_ERROR_CHECK(twai_driver_install(&g,&t,&f));
  ESP_ERROR_CHECK(twai_start());
}

#define U16(i) ((uint16_t)(d[i] | (d[(i)+1] << 8)))
#define S16(i) ((int16_t)(d[i] | (d[(i)+1] << 8)))
#define U32(i) ((uint32_t)d[i] | ((uint32_t)d[(i)+1] << 8) | ((uint32_t)d[(i)+2] << 16) | ((uint32_t)d[(i)+3] << 24))

void applyFrame(Telemetry &t, const twai_message_t &m){
  if(m.extd || m.rtr) return;
  const uint8_t *d = m.data;
  const uint8_t n = m.data_length_code;
  switch(m.identifier){
    case 0x131: if(n<6) return; t.soc=U16(4); t.balancing=((d[2]>>4)|((d[3]&0x0F)<<4))&1; t.chargerConnected=(d[2]>>6)&1; break;
    case 0x14D: if(n<6) return; t.soh=U16(4); t.maxCellSoh=U16(4); if(n>=8) t.minCellSoh=U16(6); break;
    case 0x13D: if(n<6) return; t.deltaSoc=U16(4); break;
    case 0x141: if(n<4) return; t.voltage=U16(0); t.imbalance=U16(2); break;
    case 0x140:
      if(n<4) return;
      t.humidity=U16(0); t.ambientTemp=S16(2);
      if(n>=6) t.socEkf=U16(4);
      break;
    case 0x200: if(n<8) return; t.rangeKm=d[6]; t.displayFan=(d[7]>>4)&1; break;
    case 0x510: if(n<4) return; t.rpm=(int)S16(2)*5/6; break;
    case 0x101: if(n<5) return; t.driveMode=d[1]; t.smartEco=(d[3]>>6)&1; t.forcedEco=(d[4]>>2)&1; break;
    case 0x102:
      if(n<5) return;
      t.frontBrake=d[0]&1; t.rearBrake=(d[0]>>1)&1; t.highBeam=(d[0]>>2)&1;
      t.startSwitch=(d[0]>>4)&1; t.killSwitch=(d[0]>>5)&1; t.storageSwitch=(d[0]>>6)&1;
      t.horn=(d[1]>>1)&1;
      t.indicatorLeftSide=(d[2]>>1)&1; t.indicatorRightSide=d[2]&1; t.indicatorCenterSide=(d[2]>>2)&1;
      t.sideStand=d[4]&1; t.keyOn=(d[4]>>3)&1;
      if(n>=8) t.pduVoltage=U16(6);
      break;
    case 0x103: if(n<2) return; t.seatLock=(d[1]>>1)&1; t.chargerLock2=(d[1]>>2)&1; t.chargerLock1=(d[1]>>3)&1; break;
    case 0x208: if(n<4) return; t.chargerFan1=U16(0); t.chargerFan2=U16(2); break;
    case 0x132: if(n<8) return; for(int i=0;i<4;i++) t.cellV[i]   =U16(i*2); break;
    case 0x133: if(n<8) return; for(int i=0;i<4;i++) t.cellV[4+i] =U16(i*2); break;
    case 0x134: if(n<8) return; for(int i=0;i<4;i++) t.cellV[8+i] =U16(i*2); break;
    case 0x135:
      if(n<4) return;
      t.cellV[12]=U16(0); t.cellV[13]=U16(2);
      if(n>=8){ t.afeDieTemp=S16(4); t.packCurrent=S16(6); }
      break;
    case 0x136: if(n<8) return; for(int i=0;i<4;i++) t.cellTemp[i]=S16(i*2); break;
    case 0x137: if(n<8) return; t.cellTemp[4]=S16(0); t.eftTemp=S16(2); t.eciTemp=S16(4); t.senseTemp=S16(6); break;
    case 0x139: if(n<6) return; t.deratedDsgI=U16(0); t.deratedChgI=U16(2); t.deratedRegenI=U16(4); break;
    case 0x142: if(n<8) return; t.minV1Est=S16(0); t.minV2Est=S16(2); t.minSocEst=U16(4); t.dsgLimTemp=U16(6); break;
    case 0x143: if(n<8) return; for(int i=0;i<4;i++) t.cellSoc[i]  =U16(i*2); break;
    case 0x144: if(n<8) return; for(int i=0;i<4;i++) t.cellSoc[4+i]=U16(i*2); break;
    case 0x145: if(n<8) return; for(int i=0;i<4;i++) t.cellSoc[8+i]=U16(i*2); break;
    case 0x146: if(n<6) return; t.cellSoc[12]=U16(0); t.cellSoc[13]=U16(2); t.maxSoc=U16(4); break;
    case 0x148: if(n<6) return; t.chgCellRule=U16(0); t.chgCmdRateLtd=U16(2); t.chgPostQuant=S16(4); break;
    case 0x14E: if(n<8) return; for(int i=0;i<8;i++) t.cellSoh[i]=d[i]; break;
    case 0x14F: if(n<6) return; for(int i=0;i<6;i++) t.cellSoh[8+i]=d[i]; break;
    case 0x151: if(n<6) return; t.soe=U16(4); break;
    case 0x162: if(n<7) return; t.bmsHwVer=d[1]; t.bmsFw[0]=d[4]; t.bmsFw[1]=d[5]; t.bmsFw[2]=d[6]; break;
    case 0x163: if(n<8) return; for(int i=0;i<6;i++) t.binId[i]=(char)d[2+i]; break;
    case 0x164: if(n<8) return; for(int i=0;i<8;i++) t.binId[6+i]=(char)d[i]; t.binId[14]=0; break;
    case 0x107: if(n<7) return; t.bcmFw[0]=d[4]; t.bcmFw[1]=d[5]; t.bcmFw[2]=d[6]; break;
    case 0x216: if(n<7) return; t.gtwFw[0]=d[4]; t.gtwFw[1]=d[5]; t.gtwFw[2]=d[6]; break;
    case 0x70B: if(n<4) return; t.motorFw[0]=d[1]; t.motorFw[1]=d[2]; t.motorFw[2]=d[3]; break;
    case 0x212: if(n<4) return; t.odometer=U32(0); break;
    case 0x201: if(n<8) return; t.chargerOV=U32(0); t.chargerOI=U32(4); break;
    case 0x203: if(n<4) return; t.chargerIV=U32(0); break;
    case 0x18C:
      if(n<5) return;
      t.motorI=((d[2]>>5)|((d[3]&31)<<3))+((((d[3]>>5)|((d[4]&31)<<3))&1)*256)-(((d[3]>>5)&1)*512);
      t.motorTemp=(((d[4]&63)<<2)|(d[3]>>6))-40;
      if(n>=6) t.controllerTemp=(((d[5]&63)<<2)|(d[4]>>6))-40;
      break;
    case 0x38C:
      if(n<8) return;
      t.throttleOut=((d[3]>>2)|((d[4]&3)<<6))-(((d[4]>>1)&1)*256);
      t.throttle3  =((d[5]>>4)|((d[6]&15)<<4))-(((d[6]>>3)&1)*256);
      t.throttle1  =((d[6]>>4)|((d[7]&15)<<4))-(((d[7]>>3)&1)*256);
      break;
  }
}

// Blocks until a frame arrives, drains the queue, publishes one snapshot,
// then sleeps 2 ms so the (lower priority) loop / WiFi always get CPU on
// the single-core C3. 128-frame queue covers >30 ms at full bus load.
void canTask(void *){
  static Telemetry local;
  local = getSnap();
  twai_message_t m;
  for(;;){
    if(twai_receive(&m, pdMS_TO_TICKS(100)) != ESP_OK) continue;
    uint32_t cnt = 0;
    do { applyFrame(local, m); cnt++; } while(cnt < 128 && twai_receive(&m, 0) == ESP_OK);
    xSemaphoreTake(mtx, portMAX_DELAY);
    snap = local;
    xSemaphoreGive(mtx);
    lastFrameMs = millis();
    if(cnt > maxQueueBacklog) maxQueueBacklog = cnt;
    vTaskDelay(pdMS_TO_TICKS(2));
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
#define FONT_MAIN  u8g2_font_6x10_tf
#define FONT_SMALL u8g2_font_5x8_tf

void drawHeader(const char *title, bool showPage){
  u8g2.setFont(FONT_MAIN);
  u8g2.drawStr(0, 9, title);
  int rx = 124;
  if(showPage){
    char b[8]; snprintf(b, sizeof(b), "%u/%u", page+1, NUM_PAGES);
    int w = u8g2.getStrWidth(b);
    u8g2.drawStr(128 - w, 9, b);
    rx = 128 - w - 6;
  }
  if(sessActive && (millis() / 500) % 2) u8g2.drawDisc(rx, 5, 2);   // blinking REC dot
  u8g2.drawHLine(0, 11, 128);
  if(holdPct > 0) u8g2.drawBox(0, 10, (128 * holdPct) / 100, 3);   // long-press progress
}

// left label, right-aligned value, row 0..4
void drawRow(int row, const char *label, const char *val){
  int y = 23 + 10*row;
  u8g2.setFont(FONT_MAIN);
  u8g2.drawStr(0, y, label);
  u8g2.drawStr(128 - u8g2.getStrWidth(val), y, val);
}

// compact row (small font) 0..6
void drawRowSm(int row, const char *label, const char *val){
  int y = 20 + 7*row;
  u8g2.setFont(FONT_SMALL);
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

// row with fixed-point value + unit (space separated)
void rowV(int row, const char *label, int32_t v, uint8_t dec, const char *unit){
  char b[22]; char *e = fx(b, v, dec);
  if(unit && *unit){ *e++ = ' '; strcpy(e, unit); }
  drawRow(row, label, b);
}
// grid cell with fixed-point value + unit (no space)
void gridV(int idx, const char *label, int32_t v, uint8_t dec, const char *unit){
  char b[16]; char *e = fx(b, v, dec);
  strcpy(e, unit);
  drawGrid(idx, label, b);
}

inline const char* onOff(bool b){ return b ? "ON" : "off"; }

void pageBattery(const Telemetry &t){
  drawHeader("BATTERY", true);
  rowV(0, "SoC",       t.soc, 2, "%");
  rowV(1, "SoH",       t.soh, 2, "%");
  rowV(2, "Delta SoC", t.deltaSoc, 2, "%");
  rowV(3, "Voltage",   t.voltage, 2, "V");
  rowV(4, "Imbalance", t.imbalance, 4, "V");
}

void pageDrive(const Telemetry &t){
  drawHeader("DRIVE", true);
  drawRow(0, "Mode", driveModeName(t.driveMode));
  rowV(1, "Range",      t.rangeKm, 0, "km");
  rowV(2, "Motor RPM",  t.rpm, 0, "");
  rowV(3, "Motor I",    t.motorI, 1, "A");
  rowV(4, "Motor Temp", t.motorTemp, 0, "C");
}

void pageController(const Telemetry &t){
  char b[24]; char *e;
  drawHeader("CONTROLLER", true);
  rowV(0, "Ctrl Temp", t.controllerTemp, 0, "C");
  rowV(1, "Throttle",  t.throttleOut, 0, "%");
  e = fx(b, t.throttle1, 0); strcpy(e, "% / "); e += 4; e = fx(e, t.throttle3, 0); strcpy(e, "%");
  drawRow(2, "Thr 1 / 3", b);
  rowV(3, "PDU Volt",  t.pduVoltage / 10, 2, "V");
  e = fx(b, t.ambientTemp / 10, 1); strcpy(e, "C "); e += 2; e = fx(e, t.humidity / 100, 0); strcpy(e, "%");
  drawRow(4, "Amb / Hum", b);
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

// 14 cells in 3 columns x 5 rows.  mode 0 = voltage, 1 = SoH, 2 = SoC
void pageCells(const Telemetry &t, uint8_t mode){
  static const char *titles[] = {"CELL VOLTAGE", "CELL SoH", "CELL SoC"};
  drawHeader(titles[mode], true);
  u8g2.setFont(FONT_SMALL);
  char b[16];
  for(int i=0;i<14;i++){
    int col = i / 5, row = i % 5;
    int x = col * 43, y = 23 + 10*row;
    int idx = i + 1;
    b[0] = idx >= 10 ? '1' : ' '; b[1] = '0' + idx % 10; b[2] = ' ';
    char *e;
    if(mode == 0)      fx(b+3, t.cellV[i] / 10, 3);
    else if(mode == 1){ e = fx(b+3, t.cellSoh[i], 0); e[0] = '%'; e[1] = 0; }
    else               fx(b+3, t.cellSoc[i] / 10, 1);
    u8g2.drawStr(x, y, b);
  }
}

void pageCharger1(const Telemetry &t){
  char b[24];
  drawHeader("CHARGER", true);
  rowV(0, "Out V", t.chargerOV / 10, 2, "V");
  rowV(1, "Out I", t.chargerOI / 10, 2, "A");
  rowV(2, "In V",  t.chargerIV / 100, 1, "V");
  const uint64_t uW = (uint64_t)t.chargerOV * (uint64_t)t.chargerOI;
  if(uW >= 1000000000ULL){ char *e = fx(b, (int32_t)(uW / 10000000ULL), 2); strcpy(e, " kW"); }
  else                   { char *e = fx(b, (int32_t)(uW / 100000ULL), 1);   strcpy(e, " W");  }
  drawRow(3, "Power", b);
  drawRow(4, "Connected", t.chargerConnected ? "YES" : "no");
}

void pageCharger2(const Telemetry &t){
  drawHeader("CHARGER 2", true);
  drawRow(0, "Balancing", t.balancing ? "ACTIVE" : "idle");
  rowV(1, "Fan 1", t.chargerFan1, 0, "");
  rowV(2, "Fan 2", t.chargerFan2, 0, "");
  drawRow(3, "Lock 1", t.chargerLock1 ? "LOCKED" : "open");
  drawRow(4, "Lock 2", t.chargerLock2 ? "LOCKED" : "open");
}

void pageBmsTemp(const Telemetry &t){
  drawHeader("BMS TEMP", true);
  u8g2.drawVLine(64, 14, 50);
  gridV(0, "T1",  t.cellTemp[0] / 10, 1, "C");
  gridV(1, "T2",  t.cellTemp[1] / 10, 1, "C");
  gridV(2, "T3",  t.cellTemp[2] / 10, 1, "C");
  gridV(3, "T4",  t.cellTemp[3] / 10, 1, "C");
  gridV(4, "Die", t.afeDieTemp / 10, 1, "C");
  gridV(5, "FET", t.eftTemp / 10, 1, "C");
  gridV(6, "ECI", t.eciTemp / 10, 1, "C");
  gridV(7, "Sns", t.senseTemp / 10, 1, "C");
}

void pageBmsPack(const Telemetry &t){
  drawHeader("BMS PACK", true);
  rowV(0, "Pack I",   t.packCurrent, 2, "A");
  rowV(1, "SoE",      t.soe, 2, "%");
  rowV(2, "SoC EKF",  t.socEkf, 2, "%");
  rowV(3, "Max SoC",  t.maxSoc, 2, "%");
  rowV(4, "Min SoC",  t.minSocEst, 2, "%");
}

void pageLimits(const Telemetry &t){
  drawHeader("LIMITS", true);
  rowV(0, "Dsg I",    t.deratedDsgI, 2, "A");
  rowV(1, "Chg I",    t.deratedChgI, 2, "A");
  rowV(2, "Regen I",  t.deratedRegenI, 2, "A");
  rowV(3, "Dsg T-lim",t.dsgLimTemp, 2, "A");
  rowV(4, "Chg Rule", t.chgCellRule, 2, "A");
}

void pageVehicle(const Telemetry &t){
  char b[20];
  drawHeader("VEHICLE", true);
  char *e = fx(b, (int32_t)(t.odometer / 100), 1); strcpy(e, " km");
  drawRowSm(0, "Odometer", b);
  snprintf(b, sizeof(b), "%u", t.bmsHwVer); drawRowSm(1, "BMS HW", b);
  verStr(b, t.bmsFw);   drawRowSm(2, "BMS FW", b);
  verStr(b, t.bcmFw);   drawRowSm(3, "BCM FW", b);
  verStr(b, t.gtwFw);   drawRowSm(4, "Gateway FW", b);
  verStr(b, t.motorFw); drawRowSm(5, "Motor FW", b);
  binStr(b, t);         drawRowSm(6, "BIN", b[0] ? b : "--");
}

void pageWifi(){
  char b[24];
  drawHeader("WiFi ON", false);
  drawRow(0, "SSID", AP_SSID);
  drawRow(1, "Pass", AP_PASS);
  drawRow(2, "IP", "192.168.4.1");
  snprintf(b, sizeof(b), "%d", WiFi.softAPgetStationNum()); drawRow(3, "Clients", b);
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
  const char *ver = "v6";
  u8g2.drawStr((128 - u8g2.getStrWidth(ver)) / 2, 38, ver);
  u8g2.drawHLine(24, 43, 80);
  const char *by = "by PM27";
  u8g2.drawStr((128 - u8g2.getStrWidth(by)) / 2, 56, by);
  u8g2.sendBuffer();
}

static void durStr(char *b, uint32_t s){
  if(s >= 3600) snprintf(b, 12, "%luh %02lum", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
  else          snprintf(b, 12, "%lum %02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void socRange(char *b, uint16_t a, uint16_t z){
  char *e = fx(b, a / 100, 0); strcpy(e, "->"); e += 2;
  e = fx(e, z / 100, 0); strcpy(e, "%");
}

// Shown for a few seconds after a ride or charge has been saved
void pageSummary(){
  const LogHdr &h = lastSum;
  char b[22];
  if(h.type == 2){
    drawHeader("CHARGE SAVED", false);
    char *e = fx(b, (int32_t)(h.whChg / 100), 2); strcpy(e, " kWh"); drawRow(0, "Added", b);
    socRange(b, h.socStart, h.socEnd);   drawRow(1, "SoC", b);
    durStr(b, h.dur);                    drawRow(2, "Time", b);
    rowV(3, "Peak", h.peakW / 10, 2, "kW");
    rowV(4, "Max cell", h.maxCellT / 10, 1, "C");
  } else {
    drawHeader("RIDE SAVED", false);
    int32_t net = h.whUsed - h.whRegen;                          // Wh x10
    rowV(0, "Distance", (int32_t)(h.distM / 100), 1, "km");
    rowV(1, "Energy", net / 10, 0, "Wh");
    if(h.distM >= 100) rowV(2, "Efficiency", net * 100 / (int32_t)h.distM, 0, "Wh/km");
    else               drawRow(2, "Efficiency", "--");
    socRange(b, h.socStart, h.socEnd);   drawRow(3, "SoC", b);
    durStr(b, h.dur);                    drawRow(4, "Time", b);
  }
}

void drawScreen(){
  u8g2.clearBuffer();
  uint32_t now = millis();
  if(toastMsg && now < toastUntil){
    u8g2.setFont(FONT_MAIN);
    u8g2.drawRFrame(4, 20, 120, 26, 4);
    u8g2.drawStr((128 - u8g2.getStrWidth(toastMsg)) / 2, 37, toastMsg);
  } else if(summaryUntil && now < summaryUntil){
    pageSummary();
  } else if(wifiOn){
    pageWifi();
  } else {
    Telemetry t = getSnap();
    switch(page){
      case 0:  pageBattery(t);    break;
      case 1:  pageDrive(t);      break;
      case 2:  pageController(t); break;
      case 3:  pageInputs1(t);    break;
      case 4:  pageInputs2(t);    break;
      case 5:  pageCells(t, 0);   break;
      case 6:  pageCells(t, 1);   break;
      case 7:  pageCells(t, 2);   break;
      case 8:  pageCharger1(t);   break;
      case 9:  pageCharger2(t);   break;
      case 10: pageBmsTemp(t);    break;
      case 11: pageBmsPack(t);    break;
      case 12: pageLimits(t);     break;
      case 13: pageVehicle(t);    break;
    }
  }
  u8g2.sendBuffer();
}

// ======================= Button =======================
// short press  -> next page (only when WiFi is off)
// double press -> add a marker to the current log
// hold 3 sec   -> toggle WiFi
void handleButton(){
  static bool     lastRaw    = HIGH;
  static bool     stable     = HIGH;
  static uint32_t lastChange = 0;
  static uint32_t pressStart = 0;
  static bool     longDone   = false;
  static bool     pendPage   = false;
  static uint32_t pendAt     = 0;

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
      if(!longDone){
        if(pendPage && now - pendAt < DOUBLE_MS){        // second tap -> marker
          pendPage = false;
          toastMsg = addMarker() ? "MARKER ADDED" : "NOT RECORDING";
          toastUntil = now + 1200;
        } else {
          pendPage = true; pendAt = now;
        }
      }
      lastDraw = 0;               // redraw right away
    }
  }

  if(pendPage && now - pendAt >= DOUBLE_MS){             // it was a single tap
    pendPage = false;
    summaryUntil = 0;                                    // a tap also dismisses the summary
    if(!wifiOn) page = (page + 1) % NUM_PAGES;
    lastDraw = 0;
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

  server.on("/",  handleRoot);
  server.on("/d", handleHome);
  server.on("/c", handleCells);
  server.on("/g", handleCharger);
  server.on("/b", handleBms);
  server.on("/m", handleMore);
  server.on("/logs",   handleLogs);
  server.on("/log",    handleLog);
  server.on("/logdel", handleLogDel);
  server.on("/time",   handleTime);
  server.on("/mark",   handleMark);
  // WiFi stays OFF at boot; hold the button 3 sec to start it

  logInit();
  setupCAN();
  xTaskCreate(canTask, "can", 4096, NULL, 2, NULL);   // C3 is single core
  delay(2000);   // keep the welcome screen visible
}

void loop(){
  handleButton();
  if(wifiOn) server.handleClient();

  uint32_t now = millis();

  static uint32_t lastLogTick = 0;
  if(now - lastLogTick >= 200){
    lastLogTick = now;
    Telemetry t = getSnap();
    logTick(t, now);
  }

  uint32_t interval = (holdPct > 0) ? 100 : (wifiOn ? 1000 : 250);
  if(sessActive && interval > 500) interval = 500;       // keep the REC dot blinking
  if(lastDraw == 0 || now - lastDraw >= interval){
    lastDraw = now ? now : 1;
    drawScreen();
  }

  if(now - lastDiag >= 5000){
    lastDiag = now;
    twai_status_info_t st;
    if(twai_get_status_info(&st) == ESP_OK){
      Serial.printf("[DIAG] rx_missed:%lu rx_overrun:%lu arb_lost:%lu max_batch:%lu heap:%lu\n",
        (unsigned long)st.rx_missed_count, (unsigned long)st.rx_overrun_count,
        (unsigned long)st.arb_lost_count, (unsigned long)maxQueueBacklog,
        (unsigned long)ESP.getFreeHeap());
    }
    maxQueueBacklog = 0;
  }
  vTaskDelay(pdMS_TO_TICKS(1));
}
