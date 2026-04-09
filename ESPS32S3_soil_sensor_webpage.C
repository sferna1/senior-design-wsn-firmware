
// With auto check gui, auto poll every 10 secs (change " if(millis()-last>10000){" ), manual refresh button, historicals
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>

const char* ssid = "SF_SoilSensor_ESP32";
const char* password = "12345678";

#define RXD2 5
#define TXD2 4
HardwareSerial RS485(2);

#define SLAVE_ID 0x01
#define BAUD 9600

uint16_t registers[] = {0x0006,0x0012,0x0013,0x0015,0x001E,0x001F,0x0020};
const char* names[] = {"pH","Moisture","Temperature","EC","Nitrogen","Phosphorus","Potassium"};

float scale[] = {0.01,0.1,0.1,1,1,1,1};
float minVal[] = {4.0,0,5,0,0,0,0};
float maxVal[] = {8.5,100,40,3000,1999,1999,1999};
const char* units[] = {"pH","%RH","°C","µs/cm","mg/kg","mg/kg","mg/kg"};

float values[7];
bool valid[7];

WebServer server(80);
unsigned long lastPollTime = 0;

uint16_t crc16(uint8_t *data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (int i=0;i<len;i++) {
    crc ^= data[i];
    for (int j=0;j<8;j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

void buildRequest(uint8_t *frame, uint16_t reg) {
  frame[0]=SLAVE_ID; frame[1]=0x03;
  frame[2]=reg>>8; frame[3]=reg&0xFF;
  frame[4]=0x00; frame[5]=0x01;
  uint16_t crc=crc16(frame,6);
  frame[6]=crc&0xFF; frame[7]=crc>>8;
}

bool readRegister(uint16_t reg, uint16_t &value) {
  uint8_t req[8];
  buildRequest(req, reg);

  while (RS485.available()) RS485.read();

  RS485.write(req,8);
  RS485.flush();
  delay(30);

  uint8_t resp[7];
  int i=0;
  unsigned long t=millis();

  while (millis()-t < 300) {
    if (RS485.available()) {
      resp[i++] = RS485.read();
      if (i>=7) break;
    }
  }

  if (i<7) return false;

  uint16_t crc_calc = crc16(resp,5);
  uint16_t crc_recv = resp[5] | (resp[6]<<8);

  if (crc_calc != crc_recv) return false;

  value = (resp[3]<<8) | resp[4];
  return true;
}

void pollSensors() {
  for (int i=0;i<7;i++) {
    uint16_t raw;
    if (readRegister(registers[i], raw)) {
      values[i] = raw * scale[i];
      valid[i] = true;
    } else {
      valid[i] = false;
    }
    delay(50);
  }
  lastPollTime = millis();
}

void handleData() {
  String json="{";
  json += "\"time\":" + String(lastPollTime) + ",";
  for(int i=0;i<7;i++){
    json+="\""+String(names[i])+"\":";
    if(valid[i]) json+=String(values[i],2);
    else json+="null";
    if(i<6) json+=",";
  }
  json+="}";
  server.send(200,"application/json",json);
}

void handleRoot() {
String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Soil Monitor</title>

<style>
body {
  background:#0f172a;
  color:#e5e7eb;
  font-family:system-ui;
  margin:0;
  text-align:center;
}

.header {
  background:#020617;
  padding:15px;
  font-size:20px;
  color:#34d399;
}

.container {
  padding:10px;
}

.grid {
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(140px,1fr));
  gap:10px;
}

.card {
  background:#1e293b;
  padding:12px;
  border-radius:12px;
}

.value {
  font-size:22px;
  margin-top:5px;
}

.ok {color:#34d399;}
.bad {color:#f87171;}

button {
  margin:15px;
  padding:10px 20px;
  font-size:16px;
  border:none;
  border-radius:10px;
  background:#34d399;
  color:#020617;
}
button:disabled {
  background: #6b7280;
  color: #111827;
}
canvas {
  margin-top:15px;
  background:#020617;
  border-radius:10px;
}

.timestamp {
  font-size:12px;
  color:#94a3b8;
}
</style>
</head>

<body>

<div class="header">🌿 ESP32 Soil Sensor Dashboard</div>

<div class="container">

<button id="refreshBtn" onclick="manualRefresh()">Manual Refresh</button>

<div class="timestamp" id="time">Last update: --</div>

<div class="grid" id="cards"></div>

<canvas id="chart" width="350" height="150"></canvas>

</div>

<script>
const names=["pH","Moisture","Temperature","EC","Nitrogen","Phosphorus","Potassium"];
const units=["pH","%RH","°C","µs/cm","mg/kg","mg/kg","mg/kg"];
const minV=[4,0,5,0,0,0,0];
const maxV=[8.5,100,40,3000,1999,1999,1999];
function manualRefresh(){
  const btn = document.getElementById("refreshBtn");

  btn.innerText = "Refreshing...";
  btn.disabled = true;

  fetch('http://192.168.4.1/refresh')
    .then(() => setTimeout(update, 300))
    .finally(() => {
      btn.innerText = "Manual Refresh";
      btn.disabled = false;
    });
}

// history arrays
let history = names.map(()=>[]);

function update(){
 fetch('/data')
 .then(r=>r.json())
 .then(d=>{

   // timestamp
   document.getElementById("time").innerText =
     "Last update: " + new Date().toLocaleTimeString();

   let html="";

   for(let i=0;i<names.length;i++){
     let v=d[names[i]];
     let txt="—", cls="bad", stat="NO DATA";

     if(v!==null){
       txt=v.toFixed(2);

       // store history (limit 30 points)
       history[i].push(v);
       if(history[i].length>30) history[i].shift();

       if(v<minV[i]||v>maxV[i]){
         stat="OUT OF RANGE"; cls="bad";
       } else {
         stat="OK"; cls="ok";
       }
     }

     html+=`
     <div class="card">
       <div>${names[i]}</div>
       <div class="value ${cls}">${txt}</div>
       <div>${units[i]}</div>
       <div class="${cls}">${stat}</div>
     </div>`;
   }

   document.getElementById("cards").innerHTML=html;

   drawChart();
 });
}

function drawChart(){
 const c=document.getElementById("chart");
 const ctx=c.getContext("2d");

 ctx.clearRect(0,0,c.width,c.height);

 for(let i=0;i<history.length;i++){
   let data=history[i];
   if(data.length<2) continue;

   ctx.beginPath();

   for(let x=0;x<data.length;x++){
     let y = c.height - (data[x]/100)*c.height;
     let px = x*(c.width/30);

     if(x===0) ctx.moveTo(px,y);
     else ctx.lineTo(px,y);
   }

   ctx.stroke();
 }
}

setInterval(update,3000);
update();
</script>

</body>
</html>
)rawliteral";

server.send(200,"text/html",page);
}

void setup() {
  Serial.begin(115200);
  RS485.begin(BAUD, SERIAL_8N1, RXD2, TXD2);
  server.on("/refresh", handleRefresh);
  WiFi.softAP(ssid,password);
  Serial.println(WiFi.softAPIP());

  server.on("/",handleRoot);
  server.on("/data",handleData);
  server.begin();
}
void handleRefresh() {
  pollSensors();  // force immediate read
  Serial.println("MANUAL REFRESH TRIGGERED");
  server.send(200, "text/plain", "OK");
}
void loop() {
  server.handleClient();

  static unsigned long last=0;
  if(millis()-last>10000){
    last=millis();
    pollSensors();
  }
}
