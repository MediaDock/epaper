
// ============================================================
//  Configuration — edit these values
// ============================================================
#define WIFI_SSID          "EPaper"
#define WIFI_PASSWORD      "epaperpass"
#define IMAGE_INTERVAL_MS   10000   // default slide interval (overridable via web UI)
// 0 = landscape 400×300 (recommended)  |  2 = landscape 180°
// Portrait modes (1, 3) not supported — display width must be divisible by 8.
#define DISPLAY_ROTATION    0

// ============================================================
//  Display
// ============================================================
#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <vector>

#define CS_PIN   10
#define BUSY_PIN  3
#define RES_PIN   4
#define DC_PIN    9

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
  GxEPD2_420_GDEY042T81(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

WebServer server(80);

// ============================================================
//  State  (dispW/dispH set from display.width/height in setup)
// ============================================================
std::vector<String> imageList;
int           currentIndex  = 0;
unsigned long lastSwap      = 0;
File          uploadFile;
int           dispW         = 400;
int           dispH         = 300;
unsigned long imageInterval = IMAGE_INTERVAL_MS;

// ============================================================
//  Embedded web UI
// ============================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head>
<title>EPaper Gallery</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box}
body{font-family:sans-serif;max-width:640px;margin:40px auto;padding:0 20px;color:#222}
h1{margin-bottom:2px}
.sub{color:#777;font-size:13px;margin-top:0}
.card{background:#f7f7f7;padding:16px;border-radius:8px;margin:12px 0}
h3{margin:0 0 10px}
input[type=file]{display:block;margin:6px 0 10px}
button{background:#222;color:#fff;padding:7px 14px;border:none;border-radius:4px;cursor:pointer}
button:hover{background:#444}
.del{background:#b00;font-size:12px;padding:3px 10px}
.row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #e0e0e0}
.row:last-child{border:none}
canvas{display:block;max-width:100%;border:1px solid #ccc;margin:8px 0;image-rendering:pixelated}
#prog{width:100%;margin-top:6px;display:none}
#status{margin-top:5px;font-size:13px;color:#555}
#adj{display:none;margin-top:10px}
.sl{display:grid;grid-template-columns:80px 1fr 42px;align-items:center;gap:4px;margin:3px 0}
.sl label{font-size:12px;color:#444}
.sl input[type=range]{width:100%}
.sl span{font-size:12px;text-align:right;color:#666;font-variant-numeric:tabular-nums}
.cfg{display:flex;align-items:center;gap:10px;flex-wrap:wrap;font-size:14px}
input[type=number]{width:72px;padding:5px;border:1px solid #ccc;border-radius:4px}
#setMsg{font-size:13px;color:#555}
</style>
</head>
<body>
<h1>EPaper Gallery</h1>
<p class="sub">Upload any image — resized, dithered, and previewed in the browser. Only 15 KB is sent to the device.</p>

<canvas id="proc" style="display:none"></canvas>

<div class="card">
  <h3>Upload Image</h3>
  <input type="file" id="fi" accept="image/*">
  <div id="adj">
    <div class="sl"><label>Brightness</label><input type="range" id="bri" min="-100" max="100" value="0"   oninput="sv('briV',this.value);rp()"><span id="briV">0</span></div>
    <div class="sl"><label>Contrast</label>  <input type="range" id="con" min="-100" max="100" value="0"   oninput="sv('conV',this.value);rp()"><span id="conV">0</span></div>
    <div class="sl"><label>Threshold</label> <input type="range" id="thr" min="50"  max="200" value="128"  oninput="sv('thrV',this.value);rp()"><span id="thrV">128</span></div>
    <div class="sl"><label>Scale</label>     <input type="range" id="scl" min="50"  max="300" value="100" step="5" oninput="sv('sclV',(this.value/100).toFixed(1)+'×');rp()"><span id="sclV">1.0×</span></div>
    <div class="sl"><label>Offset X</label>  <input type="range" id="ox"  min="-200" max="200" value="0"  oninput="sv('oxV',this.value);rp()"><span id="oxV">0</span></div>
    <div class="sl"><label>Offset Y</label>  <input type="range" id="oy"  min="-200" max="200" value="0"  oninput="sv('oyV',this.value);rp()"><span id="oyV">0</span></div>
    <p style="font-size:12px;color:#888;margin:8px 0 2px">Preview — exact display output:</p>
    <canvas id="prev"></canvas>
    <button id="upBtn" style="margin-top:8px">Upload to device</button>
  </div>
  <progress id="prog" max="100" value="0"></progress>
  <div id="status"></div>
</div>

<div class="card">
  <h3>Settings</h3>
  <div class="cfg">
    <span>Slide duration</span>
    <input type="number" id="dur" min="0" max="3600" value="10"> seconds (0 = static)
    <button onclick="saveDur()">Save</button>
    <span id="setMsg"></span>
  </div>
</div>

<div class="card">
  <h3>Images on device</h3>
  <div id="list"><em>Loading…</em></div>
</div>

<script>
let W=400,H=300,curImg=null,packed=null,fname=null;

// load display dimensions and duration from device
fetch('/config').then(r=>r.json()).then(c=>{
  W=c.w; H=c.h;
  document.getElementById('proc').width=W;
  document.getElementById('proc').height=H;
  document.getElementById('prev').width=W;
  document.getElementById('prev').height=H;
  const hw=Math.round(W/2), hh=Math.round(H/2);
  const ox=document.getElementById('ox');
  ox.min=-hw; ox.max=hw;
  const oy=document.getElementById('oy');
  oy.min=-hh; oy.max=hh;
});
fetch('/settings').then(r=>r.json()).then(s=>{
  document.getElementById('dur').value=s.interval;
});

function sv(id,v){document.getElementById(id).textContent=v;}

function rp(){
  if(!curImg)return;
  const bri=+document.getElementById('bri').value;
  const con=+document.getElementById('con').value;
  const thr=+document.getElementById('thr').value;
  const scl=+document.getElementById('scl').value/100;
  const ox=+document.getElementById('ox').value;
  const oy=+document.getElementById('oy').value;

  // draw image to processing canvas with scale + offset
  const proc=document.getElementById('proc');
  const ctx=proc.getContext('2d');
  ctx.fillStyle='white'; ctx.fillRect(0,0,W,H);
  const fit=Math.min(W/curImg.width, H/curImg.height);
  const sw=curImg.width*fit*scl, sh=curImg.height*fit*scl;
  ctx.drawImage(curImg, (W-sw)/2+ox, (H-sh)/2+oy, sw, sh);

  // grayscale + brightness + contrast
  const id=ctx.getImageData(0,0,W,H);
  const cf=1+con/100;
  const g=new Float32Array(W*H);
  for(let i=0;i<W*H;i++){
    let v=id.data[i*4]*0.299+id.data[i*4+1]*0.587+id.data[i*4+2]*0.114;
    v=(v-128)*cf+128; // contrast
    v+=bri;           // brightness
    g[i]=Math.max(0,Math.min(255,v));
  }

  // Floyd-Steinberg dither
  const out=new Uint8Array(W*H);
  for(let y=0;y<H;y++){
    for(let x=0;x<W;x++){
      const idx=y*W+x;
      const old=g[idx], nv=old<thr?0:255, err=old-nv;
      out[idx]=nv<128?1:0;
      if(x+1<W)         g[idx+1]  +=err*7/16;
      if(y+1<H){
        if(x>0)         g[idx+W-1]+=err*3/16;
                        g[idx+W]  +=err*5/16;
        if(x+1<W)       g[idx+W+1]+=err*1/16;
      }
    }
  }

  // pack to 1-bit
  packed=new Uint8Array(W*H/8);
  for(let i=0;i<W*H;i++) if(out[i]) packed[i>>3]|=(0x80>>(i&7));

  // render preview
  const pctx=document.getElementById('prev').getContext('2d');
  const pid=pctx.createImageData(W,H);
  for(let i=0;i<W*H;i++){
    const v=out[i]?0:255;
    pid.data[i*4]=pid.data[i*4+1]=pid.data[i*4+2]=v; pid.data[i*4+3]=255;
  }
  pctx.putImageData(pid,0,0);
}

document.getElementById('fi').addEventListener('change',function(e){
  const f=e.target.files[0]; if(!f)return;
  fname=f.name.replace(/\.[^/.]+$/,'')+'.raw';
  const img=new Image(), url=URL.createObjectURL(f);
  img.onload=function(){
    URL.revokeObjectURL(url);
    curImg=img;
    document.getElementById('adj').style.display='block';
    document.getElementById('status').textContent='';
    rp();
  };
  img.src=url;
});

document.getElementById('upBtn').addEventListener('click',function(){
  if(!packed)return;
  const fd=new FormData();
  fd.append('file',new Blob([packed],{type:'application/octet-stream'}),fname);
  const xhr=new XMLHttpRequest();
  const prog=document.getElementById('prog'), stat=document.getElementById('status');
  stat.textContent='Uploading…';
  xhr.upload.onprogress=ev=>{
    if(ev.lengthComputable){prog.style.display='block';prog.value=ev.loaded/ev.total*100;}
  };
  xhr.onload=()=>{
    prog.style.display='none'; prog.value=0;
    stat.textContent=xhr.responseText;
    document.getElementById('fi').value='';
    document.getElementById('adj').style.display='none';
    curImg=packed=null; load();
  };
  xhr.open('POST','/upload'); xhr.send(fd);
});

function saveDur(){
  const v=parseInt(document.getElementById('dur').value);
  if(isNaN(v)||v<0)return;
  fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({interval:v})}).then(r=>r.text()).then(t=>{
      const el=document.getElementById('setMsg');
      el.textContent=t; setTimeout(()=>el.textContent='',2000);
  });
}

function load(){
  fetch('/list').then(r=>r.json()).then(files=>{
    const el=document.getElementById('list');
    if(!files.length){el.innerHTML='<em>No images yet.</em>';return;}
    el.innerHTML=files.map(f=>
      `<div class="row"><span>${f}</span><button class="del" onclick="del('${f}')">Delete</button></div>`
    ).join('');
  });
}
function del(n){
  if(!confirm('Delete '+n+'?'))return;
  fetch('/delete?file='+encodeURIComponent(n)).then(()=>load());
}
load();
</script>
</body></html>
)rawliteral";

// ============================================================
//  Display helper
// ============================================================
void showMessage(const char* top, const char* bottom = nullptr) {
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(top, 0, 0, &tbx, &tby, &tbw, &tbh);
    uint16_t cx = ((display.width()  - tbw) / 2) - tbx;
    uint16_t cy = ((display.height() - tbh) / 2) - tby;
    if (bottom) cy -= tbh / 2 + 6;
    display.setCursor(cx, cy);
    display.print(top);
    if (bottom) {
      display.getTextBounds(bottom, 0, 0, &tbx, &tby, &tbw, &tbh);
      cx = ((display.width() - tbw) / 2) - tbx;
      display.setCursor(cx, cy + tbh + 12);
      display.print(bottom);
    }
  } while (display.nextPage());
}

// ============================================================
//  Raw 1-bit renderer
//  Format: row-major, MSB = leftmost pixel, 1 = black
// ============================================================
bool displayRAW(const String& path) {
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.println("Cannot open: " + path); return false; }

  int expected = dispW * dispH / 8;
  if ((int)f.size() != expected) {
    Serial.printf("Bad size %d, expected %d\n", (int)f.size(), expected);
    f.close(); return false;
  }

  int rowBytes = dispW / 8;
  uint8_t row[50];  // 400/8 = 50 bytes max

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    f.seek(0);
    for (int y = 0; y < dispH; y++) {
      if (f.read(row, rowBytes) != rowBytes) break;
      for (int x = 0; x < dispW; x++) {
        if ((row[x >> 3] >> (7 - (x & 7))) & 1)
          display.drawPixel(x, y, GxEPD_BLACK);
      }
    }
  } while (display.nextPage());

  f.close();
  return true;
}

// ============================================================
//  Scan LittleFS for .raw files
// ============================================================
void scanImages() {
  imageList.clear();
  File root  = LittleFS.open("/");
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name  = entry.name();
      String lower = name; lower.toLowerCase();
      if (lower.endsWith(".raw")) {
        if (!name.startsWith("/")) name = "/" + name;
        imageList.push_back(name);
      }
    }
    entry = root.openNextFile();
  }
}

// ============================================================
//  Settings  (persisted in /settings.json)
// ============================================================
void loadSettings() {
  File f = LittleFS.open("/settings.json", "r");
  if (!f) return;
  String s = f.readString(); f.close();
  int p = s.indexOf("\"interval\":");
  if (p >= 0) {
    long v = s.substring(p + 11).toInt();
    if (v >= 0) imageInterval = (unsigned long)v * 1000UL;
  }
}

void saveSettings(long intervalSec) {
  File f = LittleFS.open("/settings.json", "w");
  if (f) { f.print("{\"interval\":" + String(intervalSec) + "}"); f.close(); }
}

// ============================================================
//  Web server handlers
// ============================================================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleConfig() {
  server.send(200, "application/json",
    "{\"w\":" + String(dispW) + ",\"h\":" + String(dispH) + "}");
}

void handleGetSettings() {
  server.send(200, "application/json",
    "{\"interval\":" + String(imageInterval / 1000UL) + "}");
}

void handleSaveSettings() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  String body = server.arg("plain");
  int p = body.indexOf("\"interval\":");
  if (p >= 0) {
    long v = body.substring(p + 11).toInt();
    if (v >= 0) {
      imageInterval = (unsigned long)v * 1000UL;
      saveSettings(v);
      server.send(200, "text/plain", "Saved");
      return;
    }
  }
  server.send(400, "text/plain", "Invalid");
}

void handleList() {
  scanImages();
  String json = "[";
  for (size_t i = 0; i < imageList.size(); i++) {
    if (i) json += ",";
    String name = imageList[i];
    if (name.startsWith("/")) name = name.substring(1);
    json += "\"" + name + "\"";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleDelete() {
  if (!server.hasArg("file")) { server.send(400, "text/plain", "Missing file"); return; }
  String path = server.arg("file");
  if (!path.startsWith("/")) path = "/" + path;
  if (LittleFS.remove(path)) {
    scanImages(); currentIndex = 0;
    server.send(200, "text/plain", "Deleted");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void handleUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    String path = "/" + u.filename;
    Serial.println("Upload: " + path);
    LittleFS.remove(path);
    uploadFile = LittleFS.open(path, "w");
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    if (uploadFile) { uploadFile.close(); }
    scanImages(); currentIndex = 0; lastSwap = millis();
  }
}

void handleUploadDone() {
  // Validate file size matches display dimensions
  String path = "/" + server.upload().filename;
  if (LittleFS.exists(path)) {
    int sz = (int)LittleFS.open(path, "r").size();
    int expected = dispW * dispH / 8;
    if (sz != expected) {
      LittleFS.remove(path);
      server.send(400, "text/plain",
        "Wrong size (" + String(sz) + " B, expected " + String(expected) + " B). "
        "Make sure the browser loaded /config before uploading.");
      return;
    }
  }
  server.send(200, "text/plain",
    "Saved — " + String(imageList.size()) + " image(s) on device");
}

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  display.init(115200, true, 50, false);
  display.setRotation(DISPLAY_ROTATION);
  dispW = display.width();
  dispH = display.height();

  if (!LittleFS.begin(true)) {
    showMessage("LittleFS Error");
    while (true) delay(1000);
  }

  loadSettings();

  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("SSID: %s  IP: %s  dispW: %d  dispH: %d\n",
    WIFI_SSID, ip.toString().c_str(), dispW, dispH);

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/config",  HTTP_GET,  handleConfig);
  server.on("/settings",HTTP_GET,  handleGetSettings);
  server.on("/settings",HTTP_POST, handleSaveSettings);
  server.on("/list",    HTTP_GET,  handleList);
  server.on("/delete",  HTTP_GET,  handleDelete);
  server.on("/upload",  HTTP_POST, handleUploadDone, handleUpload);
  server.begin();

  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%s", ip.toString().c_str());
  showMessage(WIFI_SSID, ipStr);

  scanImages();
  if (!imageList.empty()) {
    displayRAW(imageList[0]);
    lastSwap = millis();
  }
}

// ============================================================
//  Loop
// ============================================================
void loop() {
  server.handleClient();

  if (!imageList.empty() && imageInterval > 0 && millis() - lastSwap >= imageInterval) {
    currentIndex = (currentIndex + 1) % (int)imageList.size();
    displayRAW(imageList[currentIndex]);
    lastSwap = millis();
  }
}
