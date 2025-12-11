/* 
  ESP32 ECG + Live Web Plot + BPM + Tap & Manual
  - Uses WebServer + WebSocketsServer
  - ADC on GPIO34 (ADC1 channel)
  - Sampling ~500Hz (adjust SAMPLE_PERIOD_US)
  - Streams sample batches via WebSocket to client
  - Detects peaks and computes BPM (simple algorithm with refractory period)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Ticker.h>

/////////////////////  CONFIG  /////////////////////
const char* AP_SSID = "Pulse-ESP32";
const char* AP_PASS = "12345678";

const int ADC_PIN = 34;           // GPIO34 (ADC1_CH6). Change if needed.
const int ADC_READ_RES = 4095;    // 12-bit by default
const float VREF = 3.3;           // ESP32 ADC reference approx (use calibration if needed)

const unsigned long SAMPLE_PERIOD_US = 2000; // 2000us => 500Hz
const int BATCH_SEND_MS = 100;    // send samples every 100 ms
const int MAX_BATCH_SAMPLES = 200; // safety cap

// Peak detection params
const int MIN_PEAK_DISTANCE_MS = 250; // refractory period (ms) - avoid double peaks
const float PEAK_THRESHOLD_REL = 0.25; // relative threshold fraction of dynamic range

////////////////////////////////////////////////////

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
Ticker sampleTicker;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Circular buffer for samples to send
#define SAMPLE_BUFFER_SIZE 1024
volatile uint16_t sampleBuf[SAMPLE_BUFFER_SIZE];
volatile uint32_t sampleTime[SAMPLE_BUFFER_SIZE];
volatile int sampleHead = 0;
volatile int sampleTail = 0;

// runtime variables for BPM detection
volatile unsigned long lastSampleMs = 0;
volatile unsigned long lastPeakMs = 0;
volatile float lastValue = 0;
volatile float baseline = 0; // moving baseline
volatile float maxRecent = 0;
volatile float minRecent = 0;
volatile bool firstSamples = true;

// peak intervals for BPM
#define MAX_PEAKS 16
volatile unsigned long peakTimes[MAX_PEAKS];
volatile int peakCount = 0;

unsigned long lastBatchSend = 0;
unsigned long lastBpmCalcMs = 0;
volatile float currentBpm = 0;

////////////////////////////////////////////////////////////////////////
// Sampling function (called by Ticker at regular intervals)
////////////////////////////////////////////////////////////////////////
void onSample() {
  unsigned long t = millis();
  int raw = analogRead(ADC_PIN);
  // store sample
  portENTER_CRITICAL(&mux);
  sampleBuf[sampleHead] = raw;
  sampleTime[sampleHead] = t;
  sampleHead = (sampleHead + 1) % SAMPLE_BUFFER_SIZE;
  // avoid overwrite
  if (sampleHead == sampleTail) sampleTail = (sampleTail + 1) % SAMPLE_BUFFER_SIZE;
  portEXIT_CRITICAL(&mux);

  // simple baseline tracking and peak detection (lightweight)
  float v = raw; // use raw ADC counts to avoid float ADC conversion too often
  if (firstSamples) {
    baseline = v;
    maxRecent = v;
    minRecent = v;
    firstSamples = false;
  }
  // sliding min/max (very simple)
  if (v > maxRecent) maxRecent = v;
  if (v < minRecent) minRecent = v;
  // smooth baseline (exponential)
  baseline = baseline * 0.995 + v * 0.005;

  float dynamicRange = maxRecent - minRecent;
  float threshold = baseline + PEAK_THRESHOLD_REL * ( (dynamicRange>0)? dynamicRange : 50);

  // detect rising edge crossing threshold and local max
  static bool above = false;
  if (!above && v > threshold) {
    // potential start of peak
    above = true;
  }
  if (above) {
    // detect peak when value starts falling
    static float peakCandidate = 0;
    static unsigned long peakCandidateTime = 0;
    if (v > peakCandidate) {
      peakCandidate = v;
      peakCandidateTime = t;
    }
    if (v < peakCandidate - 2) { // slight fall -> confirm peak (2 ADC counts hysteresis)
      // check refractory
      if (peakCandidateTime - lastPeakMs > MIN_PEAK_DISTANCE_MS) {
        // register peak
        portENTER_CRITICAL(&mux);
        if (peakCount < MAX_PEAKS) {
          peakTimes[peakCount++] = peakCandidateTime;
        } else {
          // shift left
          for (int i = 1; i < MAX_PEAKS; ++i) peakTimes[i-1] = peakTimes[i];
          peakTimes[MAX_PEAKS-1] = peakCandidateTime;
        }
        portEXIT_CRITICAL(&mux);
        lastPeakMs = peakCandidateTime;
      }
      // reset
      peakCandidate = 0;
      peakCandidateTime = 0;
      above = false;
      // reset recent min/max slowly
      maxRecent *= 0.95;
      minRecent *= 0.98;
    }
  } else {
    // decay recent extremes slowly
    maxRecent *= 0.999;
    minRecent += (minRecent>0? -0.0001:0);
  }
}

////////////////////////////////////////////////////////////////////////
// Web & WebSocket handling
////////////////////////////////////////////////////////////////////////
void handleRoot() {
  server.send_P(200, "text/html", R"rawliteral(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 ECG Live</title>
<style>
  body{font-family:Arial;background:#f2ebe2;margin:0;padding:12px}
  .card{max-width:900px;margin:auto;background:#fff;padding:14px;border-radius:12px;box-shadow:0 4px 18px rgba(0,0,0,0.08)}
  h1{margin:0 0 8px 0}
  #canvas{width:100%;height:200px;border:1px solid #ddd;display:block}
  .row{display:flex;gap:12px;align-items:center;margin-top:10px}
  .big{font-size:28px;font-weight:700}
  button{padding:8px 12px;border-radius:8px;border:none;background:#111;color:#fff;cursor:pointer}
  #tap{background:#d33}
  label{display:block;font-size:13px;color:#444}
  .small{font-size:12px;color:#666}
</style>
</head>
<body>
<div class="card">
  <h1>ESP32 ECG Live & Pulse</h1>
  <canvas id="canvas"></canvas>
  <div class="row">
    <div>
      <div class="small">BPM (ESP32):</div>
      <div id="bpm" class="big">--</div>
    </div>
    <div>
      <div class="small">Samples/sec ~</div>
      <div id="rate">--</div>
    </div>
    <div style="flex:1"></div>
    <div>
      <button id="btnClear">Clear</button>
    </div>
  </div>

  <hr>
  <div style="display:flex;gap:12px;align-items:center;margin-top:8px">
    <button id="tap" onclick="tapNow()">TAP</button>
    <button onclick="setMode('tap')">Tap Mode</button>
    <button onclick="setMode('manual')">Manual Mode</button>
    <div id="tapInfo" style="margin-left:12px;color:#333">Tap count: <span id="tapCount">0</span></div>
  </div>

  <div id="manualControls" style="display:none;margin-top:10px">
    <label>Beats counted</label>
    <input id="beats" type="number" value="30">
    <label>Seconds</label>
    <input id="secs" type="number" value="15">
    <button onclick="calcManual()">Calculate</button>
    <div>Manual BPM: <span id="manualBpm">--</span></div>
  </div>

  <div style="margin-top:10px;font-size:12px;color:#666">
    Note: This is an experimental DIY system. Verify signals with oscilloscope before connecting to humans.
  </div>
</div>

<script>
let ws;
let canvas = document.getElementById('canvas');
let ctx = canvas.getContext('2d');
let width, height, scaleX = 2;
function fitCanvas(){ width = canvas.clientWidth; height = canvas.clientHeight = 200; canvas.width = width*devicePixelRatio; canvas.height = height*devicePixelRatio; ctx.scale(devicePixelRatio, devicePixelRatio); }
fitCanvas(); window.addEventListener('resize', ()=>{ fitCanvas(); drawGrid(); });

let buffer = [];
const MAX_POINTS = 600;
let bpmDisplay = document.getElementById('bpm');
let rateDisplay = document.getElementById('rate');

function drawGrid(){
  ctx.clearRect(0,0,width,height);
  ctx.fillStyle = '#fff'; ctx.fillRect(0,0,width,height);
  ctx.strokeStyle = '#eee';
  for(let x=0;x<width;x+=20){ ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,height); ctx.stroke(); }
  for(let y=0;y<height;y+=20){ ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(width,y); ctx.stroke(); }
}
drawGrid();

function draw(){
  drawGrid();
  if(buffer.length==0) return;
  ctx.strokeStyle='#d33';
  ctx.lineWidth=1.5;
  ctx.beginPath();
  let minV=0, maxV=4095;
  for(let i=0;i<buffer.length;i++){
    let v = buffer[i];
    let x = (i / MAX_POINTS) * width;
    // map v (0..4095) to y (height..0) centered
    let y = height - ( (v - 0) / (4095 - 0) ) * height;
    if(i==0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  }
  ctx.stroke();
}

function appendSamples(arr){
  buffer = buffer.concat(arr);
  if(buffer.length>MAX_POINTS) buffer = buffer.slice(buffer.length-MAX_POINTS);
  draw();
}

function connectWs(){
  let loc = window.location.hostname;
  ws = new WebSocket('ws://'+loc+':81/');
  ws.onopen = ()=>{ console.log('ws open'); }
  ws.onmessage = (evt)=>{
    // messages either: "BPM:xx" or "S:raw,raw,raw,..." (comma separated)
    let msg = evt.data;
    if(msg.startsWith('BPM:')){
      let v = msg.substring(4);
      bpmDisplay.innerText = v;
    } else if(msg.startsWith('S:')){
      let payload = msg.substring(2);
      let arr = payload.split(',').map(x=>parseInt(x));
      appendSamples(arr);
    } else if(msg.startsWith('R:')){
      rateDisplay.innerText = msg.substring(2);
    }
  };
  ws.onclose = ()=>{ console.log('ws closed, reconnect in 1s'); setTimeout(connectWs,1000); }
}
connectWs();

// TAP & Manual mode
let tapTimes=[];
function tapNow(){
  const now = Date.now();
  tapTimes.push(now);
  document.getElementById('tapCount').innerText = tapTimes.length;
  // simple client bpm
  if(tapTimes.length>=2){
    let intervals = [];
    for(let i=1;i<tapTimes.length;i++) intervals.push(tapTimes[i]-tapTimes[i-1]);
    let avgMs = intervals.reduce((a,b)=>a+b,0)/intervals.length;
    let bpm = 60000/avgMs;
    document.getElementById('manualBpm').innerText = bpm.toFixed(1);
  }
}
function setMode(m){
  if(m==='tap'){ document.getElementById('manualControls').style.display='none'; }
  else { document.getElementById('manualControls').style.display='block'; }
}
function calcManual(){
  let beats = Number(document.getElementById('beats').value);
  let secs = Number(document.getElementById('secs').value);
  if(!secs) return;
  let bpm = (beats * 60) / secs;
  document.getElementById('manualBpm').innerText = bpm.toFixed(1);
}

document.getElementById('btnClear').onclick = ()=>{ buffer=[]; drawGrid(); }
</script>
</body>
</html>
)rawliteral");
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  // not used for incoming messages in this project
  if(type == WStype_CONNECTED) {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("Client connected: %d - %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
  }
}

////////////////////////////////////////////////////////////////////////
// helper: send batched samples to all clients every BATCH_SEND_MS
////////////////////////////////////////////////////////////////////////
void sendBatchIfNeeded() {
  unsigned long now = millis();
  if (now - lastBatchSend < BATCH_SEND_MS) return;
  lastBatchSend = now;

  // gather up to MAX_BATCH_SAMPLES samples from buffer
  uint16_t tmp[MAX_BATCH_SAMPLES];
  int cnt = 0;
  portENTER_CRITICAL(&mux);
  while (sampleTail != sampleHead && cnt < MAX_BATCH_SAMPLES) {
    tmp[cnt] = sampleBuf[sampleTail];
    sampleTail = (sampleTail + 1) % SAMPLE_BUFFER_SIZE;
    cnt++;
  }
  portEXIT_CRITICAL(&mux);

  if (cnt == 0) return;
  // format as CSV numbers to save overhead
  String out = "S:";
  for (int i=0;i<cnt;i++) {
    out += String(tmp[i]);
    if (i < cnt-1) out += ",";
  }
  webSocket.broadcastTXT(out);
}

////////////////////////////////////////////////////////////////////////
// compute BPM from recorded peakTimes (called periodically in loop)
////////////////////////////////////////////////////////////////////////
void computeBpmFromPeaks() {
  portENTER_CRITICAL(&mux);
  int pc = peakCount;
  if (pc < 2) { portEXIT_CRITICAL(&mux); return; }
  // compute average RR
  unsigned long sum = 0;
  for (int i=1;i<pc;i++) sum += (peakTimes[i] - peakTimes[i-1]);
  float avgMs = (float)sum / (pc-1);
  float bpm = 60000.0 / avgMs;
  currentBpm = bpm;
  // clear peaks older than last 8
  if (pc > MAX_PEAKS-2) {
    // keep last half
    int keep = pc/2;
    for (int i=0;i<keep;i++) peakTimes[i] = peakTimes[pc - keep + i];
    peakCount = keep;
  }
  portEXIT_CRITICAL(&mux);

  // broadcast BPM to clients
  String msg = "BPM:" + String((int)round(currentBpm));
  webSocket.broadcastTXT(msg);
}

////////////////////////////////////////////////////////////////////////
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  // start AP
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(IP);

  // start WebServer
  server.on("/", handleRoot);
  server.begin();
  Serial.println("HTTP server started");

  // websocket
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  Serial.println("WebSocket server started");

  // start sampling ticker
  sampleTicker.attach_us(SAMPLE_PERIOD_US, onSample);

  lastBatchSend = millis();
  lastBpmCalcMs = millis();
}

void loop() {
  server.handleClient();
  webSocket.loop();

  sendBatchIfNeeded();

  // compute BPM every 1000ms
  if (millis() - lastBpmCalcMs > 1000) {
    computeBpmFromPeaks();
    // send rate approx
    String rate = "R:" + String(1000.0 / (float)SAMPLE_PERIOD_US * 1000.0 / 1000.0, 0); // rough samples/sec
    webSocket.broadcastTXT(rate);
    lastBpmCalcMs = millis();
  }
}

