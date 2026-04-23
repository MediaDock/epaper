
// ============================================================
//  Configuration — edit these three values
// ============================================================
#define WIFI_SSID         "EPaper-Gallery"
#define WIFI_PASSWORD     "epaperpass"
#define IMAGE_INTERVAL_MS  10000          // milliseconds between slides

// ============================================================
//  Display
// ============================================================
#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>

#define CS_PIN   10
#define BUSY_PIN  3
#define RES_PIN   4
#define DC_PIN    9

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
  GxEPD2_420_GDEY042T81(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

// ============================================================
//  WiFi + web server + filesystem
// ============================================================
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <vector>

WebServer server(80);

// ============================================================
//  Constants & state
// ============================================================
#define DISP_W    400
#define DISP_H    300
#define RAW_BYTES (DISP_W * DISP_H / 8)   // 15 000 bytes per image

std::vector<String> imageList;             // absolute paths, e.g. "/photo.raw"
int           currentIndex = 0;
unsigned long lastSwap     = 0;
File          uploadFile;

// ============================================================
//  Embedded web UI
//  - accepts any browser-supported image format (JPEG, PNG, …)
//  - resizes to 400×300 with letterboxing on the client
//  - applies Floyd-Steinberg dithering in JS
//  - uploads only the resulting 15 KB 1-bit raw bitmap
// ============================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head>
  <title>EPaper Gallery</title>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    *{box-sizing:border-box}
    body{font-family:sans-serif;max-width:640px;margin:40px auto;padding:0 20px;color:#222}
    h1{margin-bottom:2px}p.sub{color:#777;font-size:13px;margin-top:0}
    .card{background:#f7f7f7;padding:18px;border-radius:8px;margin:14px 0}
    h3{margin:0 0 12px}
    input[type=file]{display:block;margin:8px 0 12px}
    button{background:#222;color:#fff;padding:8px 18px;border:none;border-radius:4px;cursor:pointer}
    button:hover{background:#444}
    .del{background:#b00;font-size:12px;padding:4px 10px}
    .row{display:flex;justify-content:space-between;align-items:center;
         padding:9px 0;border-bottom:1px solid #e0e0e0}
    .row:last-child{border:none}
    canvas{display:block;max-width:100%;border:1px solid #ccc;margin-top:10px;
           image-rendering:pixelated}
    #prog{width:100%;margin-top:8px;display:none}
    #status{margin-top:6px;font-size:13px;color:#555}
    #previewWrap{display:none;margin-top:12px}
    label.sl{font-size:13px;color:#444}
    input[type=range]{width:100%;margin:4px 0 10px}
  </style>
</head>
<body>
  <h1>EPaper Gallery</h1>
  <p class="sub">
    Upload any image (JPEG, PNG, …). It is resized to 400×300 and
    Floyd-Steinberg dithered <em>in your browser</em> before the
    15 KB 1-bit file is sent to the device.
  </p>

  <!-- hidden processing canvas -->
  <canvas id="proc" width="400" height="300" style="display:none"></canvas>

  <div class="card">
    <h3>Upload Image</h3>
    <input type="file" id="fi" accept="image/*">

    <div id="previewWrap">
      <label class="sl">Threshold: <span id="thrVal">128</span></label>
      <input type="range" id="thr" min="50" max="200" value="128">
      <p style="font-size:12px;color:#888;margin:0 0 6px">
        Preview (this is exactly what will appear on the display):
      </p>
      <canvas id="prev" width="400" height="300"></canvas>
      <br><br>
      <button id="upBtn">Upload to device</button>
    </div>

    <progress id="prog" max="100" value="0"></progress>
    <div id="status"></div>
  </div>

  <div class="card">
    <h3>Images on device</h3>
    <div id="list"><em>Loading…</em></div>
  </div>

  <script>
  const W = 400, H = 300;
  let gray = null;        // Float32Array — grayscale of current image
  let packed = null;      // Uint8Array   — 15 000-byte result
  let fname = null;

  // ── grayscale extraction (called once when image is chosen) ───────────
  function extractGray(imageData) {
    const g = new Float32Array(W * H);
    for (let i = 0; i < W * H; i++) {
      g[i] = imageData.data[i*4]   * 0.299
           + imageData.data[i*4+1] * 0.587
           + imageData.data[i*4+2] * 0.114;
    }
    return g;
  }

  // ── Floyd-Steinberg dither ─────────────────────────────────────────────
  function dither(grayIn, threshold) {
    const g = new Float32Array(grayIn);   // work on a copy
    const out = new Uint8Array(W * H);    // 1 = black
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        const idx = y * W + x;
        const old = g[idx];
        const nv  = old < threshold ? 0 : 255;
        const err = old - nv;
        out[idx]  = nv === 0 ? 1 : 0;
        if (x+1 < W)           g[idx+1]   += err * 7/16;
        if (y+1 < H) {
          if (x > 0)           g[idx+W-1] += err * 3/16;
                               g[idx+W]   += err * 5/16;
          if (x+1 < W)         g[idx+W+1] += err * 1/16;
        }
      }
    }
    return out;
  }

  // ── pack 1-bit array → Uint8Array ─────────────────────────────────────
  function pack(dithered) {
    const bytes = new Uint8Array(W * H / 8);
    for (let i = 0; i < W * H; i++) {
      if (dithered[i]) bytes[i >> 3] |= (0x80 >> (i & 7));
    }
    return bytes;
  }

  // ── render dithered preview onto canvas ───────────────────────────────
  function renderPreview(dithered) {
    const ctx = document.getElementById('prev').getContext('2d');
    const id  = ctx.createImageData(W, H);
    for (let i = 0; i < W * H; i++) {
      const v = dithered[i] ? 0 : 255;
      id.data[i*4] = id.data[i*4+1] = id.data[i*4+2] = v;
      id.data[i*4+3] = 255;
    }
    ctx.putImageData(id, 0, 0);
  }

  // ── recompute when threshold slider moves ─────────────────────────────
  document.getElementById('thr').addEventListener('input', function() {
    document.getElementById('thrVal').textContent = this.value;
    if (!gray) return;
    const d = dither(gray, +this.value);
    packed = pack(d);
    renderPreview(d);
  });

  // ── file chosen ───────────────────────────────────────────────────────
  document.getElementById('fi').addEventListener('change', function(e) {
    const file = e.target.files[0];
    if (!file) return;
    fname = file.name.replace(/\.[^/.]+$/, '') + '.raw';

    const img = new Image();
    const url = URL.createObjectURL(file);
    img.onload = function() {
      URL.revokeObjectURL(url);

      const proc = document.getElementById('proc');
      const ctx  = proc.getContext('2d');
      ctx.fillStyle = 'white';
      ctx.fillRect(0, 0, W, H);

      // scale to fit, preserve aspect ratio
      const scale = Math.min(W / img.width, H / img.height);
      const sw = img.width  * scale;
      const sh = img.height * scale;
      ctx.drawImage(img, (W-sw)/2, (H-sh)/2, sw, sh);

      gray = extractGray(ctx.getImageData(0, 0, W, H));
      const thr = +document.getElementById('thr').value;
      const d   = dither(gray, thr);
      packed    = pack(d);
      renderPreview(d);
      document.getElementById('previewWrap').style.display = 'block';
      document.getElementById('status').textContent = '';
    };
    img.src = url;
  });

  // ── upload ────────────────────────────────────────────────────────────
  document.getElementById('upBtn').addEventListener('click', function() {
    if (!packed) return;
    const fd  = new FormData();
    fd.append('file', new Blob([packed], {type:'application/octet-stream'}), fname);
    const xhr  = new XMLHttpRequest();
    const prog = document.getElementById('prog');
    const stat = document.getElementById('status');
    stat.textContent = 'Uploading…';
    xhr.upload.onprogress = ev => {
      if (ev.lengthComputable) { prog.style.display='block'; prog.value=ev.loaded/ev.total*100; }
    };
    xhr.onload = () => {
      prog.style.display='none'; prog.value=0;
      stat.textContent = xhr.responseText;
      document.getElementById('fi').value='';
      document.getElementById('previewWrap').style.display='none';
      gray = packed = null;
      load();
    };
    xhr.open('POST', '/upload');
    xhr.send(fd);
  });

  // ── image list ────────────────────────────────────────────────────────
  function load() {
    fetch('/list').then(r=>r.json()).then(files => {
      const el = document.getElementById('list');
      if (!files.length) { el.innerHTML='<em>No images yet.</em>'; return; }
      el.innerHTML = files.map(f =>
        `<div class="row"><span>${f}</span>
         <button class="del" onclick="del('${f}')">Delete</button></div>`
      ).join('');
    });
  }
  function del(n) {
    if (!confirm('Delete '+n+'?')) return;
    fetch('/delete?file='+encodeURIComponent(n)).then(()=>load());
  }
  load();
  </script>
</body></html>
)rawliteral";

// ============================================================
//  Display helper: one or two centered text lines
// ============================================================
void showMessage(const char* top, const char* bottom = nullptr) {
  display.setRotation(1);
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
//  Format: RAW_BYTES bytes, row-major, MSB = leftmost pixel, 1 = black
// ============================================================
bool displayRAW(const String& path) {
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.println("Cannot open: " + path); return false; }
  if ((int)f.size() != RAW_BYTES) {
    Serial.println("Bad size: " + String(f.size()));
    f.close(); return false;
  }

  uint8_t row[DISP_W / 8];   // 50 bytes per row

  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    f.seek(0);
    for (int y = 0; y < DISP_H; y++) {
      if (f.read(row, sizeof(row)) != sizeof(row)) break;
      for (int x = 0; x < DISP_W; x++) {
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
      String name = entry.name();
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
//  Web server handlers
// ============================================================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
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
    scanImages();
    currentIndex = 0;
    server.send(200, "text/plain", "Deleted");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void handleUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    String path = "/" + u.filename;
    Serial.println("Upload start: " + path);
    LittleFS.remove(path);
    uploadFile = LittleFS.open(path, "w");
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    if (uploadFile) { uploadFile.close(); Serial.println("Upload done"); }
    scanImages();
    currentIndex = 0;
    lastSwap = millis();
  }
}

void handleUploadDone() {
  server.send(200, "text/plain",
    "Saved — " + String(imageList.size()) + " image(s) on device");
}

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  display.init(115200, true, 50, false);

  if (!LittleFS.begin(true)) {
    showMessage("LittleFS Error");
    while (true) delay(1000);
  }

  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.println("SSID: " WIFI_SSID);
  Serial.print("IP:   "); Serial.println(ip);

  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/list",   HTTP_GET,  handleList);
  server.on("/delete", HTTP_GET,  handleDelete);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
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

  if (!imageList.empty() && millis() - lastSwap >= IMAGE_INTERVAL_MS) {
    currentIndex = (currentIndex + 1) % (int)imageList.size();
    displayRAW(imageList[currentIndex]);
    lastSwap = millis();
  }
}
