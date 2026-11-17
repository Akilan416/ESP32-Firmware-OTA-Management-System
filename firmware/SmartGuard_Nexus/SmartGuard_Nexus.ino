/* NEXUS LAN edition: ESP32-S3. Set LAN_SSID / LAN_PASS below.
   Existing sensor logic retained; network and identification extended.
   ESP32-S3: one sketch, no LCD, no extra libraries.
   V1: ultrasonic proximity LEDs + close-distance beep.
   V2: V1 plus independent vibration alarm (fast blinking red).
   V3: potentiometer LED bar; red zone gives warning beeps.
   Compile each VERSION separately; first flash USB, next use browser OTA.
   Active buzzer via NPN assumed. Set PASSIVE_BUZZER true for passive piezo.
   POT: outer legs 3.3V/GND, middle GPIO8. NEVER connect pot to 5V.
   HC-SR04 ECHO GPIO2 through divider. All grounds common.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_app_desc.h>

#define FW_VERSION 3   // CHANGE TO 2 OR 3, then Export Compiled Binary
#if FW_VERSION < 1 || FW_VERSION > 3
#error "FW_VERSION must be 1, 2 or 3"
#endif

constexpr uint8_t TRIG=1, ECHO=2, VIB=10, BUZZER=11, POT=8;
constexpr uint8_t LEDS[6]={16,15,7,6,5,4}; // blue,blue,green,green,yellow,red
constexpr bool PASSIVE_BUZZER=false;
constexpr uint8_t BUZZER_ON=HIGH; // LOW only for an active-low driver module
constexpr uint16_t LIMIT_MM[6]={1000,800,600,400,250,120};
// Fill in the same 2.4 GHz network used by the laptop and other boards.
constexpr char LAN_SSID[]="NSFW";
constexpr char LAN_PASS[]="Wraith666";
constexpr char NODE_NAME[]="Security node";
constexpr char NODE_KIND[]="security";
String serialCommand;
String nodeId(){
 char id[18];uint64_t m=ESP.getEfuseMac();
 snprintf(id,sizeof(id),"%04X%08X",(unsigned)(m>>32),(unsigned)m);return String(id);
}
String buildFingerprint(){
 const esp_app_desc_t* d=esp_app_get_description();char out[65];
 for(int i=0;i<32;i++)snprintf(out+i*2,3,"%02x",d->app_elf_sha256[i]);
 out[64]=0;return String(out);
}
String nodeIP(){return WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():WiFi.softAPIP().toString();}
int nodeChip(){
#ifdef CONFIG_IDF_TARGET_ESP32S3
 return 9;
#else
 return 0;
#endif
}
String identityFields(){
 return String("\"device_id\":\"")+nodeId()+"\",\"name\":\""+NODE_NAME+
 "\",\"kind\":\""+NODE_KIND+"\",\"chip_id\":"+nodeChip()+
 ",\"ip\":\""+nodeIP()+"\",\"elf_sha256\":\""+buildFingerprint()+"\"";
}
void serialIdentity(){
 while(Serial.available()){
  char c=Serial.read();
  if(c=='\n'){
   serialCommand.trim();
   if(serialCommand=="NEXUS_INFO")Serial.println(String("NEXUS_INFO {")+identityFields()+"}");
   serialCommand="";
  }else if(serialCommand.length()<64)serialCommand+=c;
 }
}

WebServer server(80);
volatile bool vibrationEdge=false;
portMUX_TYPE vibMux=portMUX_INITIALIZER_UNLOCKED;
bool buzzerOn=false, closeAlarm=false, potAlarm=false;
bool uploading=false, uploadOK=false;
uint32_t lastUpload=0, muteVibrationUntil=0, vibrationUntil=0, lastVibration=0;
uint32_t lastPing=0,lastPot=0,lastLog=0;
uint16_t distanceMM=2000, samples[3]={2000,2000,2000};
uint8_t sampleIndex=0,level=0;
int potValue=0;

// Explicit prototypes keep Arduino sketch preprocessing straightforward.
void IRAM_ATTR vibrationISR();
void setBuzzer(bool on);
void showBar(uint8_t n);
void readDistance();
void runApplication();
void startServer();
void handleUpload();
void handleDone();
void handleInfo();

void IRAM_ATTR vibrationISR(){
 portENTER_CRITICAL_ISR(&vibMux); vibrationEdge=true; portEXIT_CRITICAL_ISR(&vibMux);
}
void setBuzzer(bool on){
 if(on==buzzerOn)return;
 buzzerOn=on;
 if(PASSIVE_BUZZER){if(on)tone(BUZZER,2200);else noTone(BUZZER);}
 else digitalWrite(BUZZER,on?BUZZER_ON:!BUZZER_ON);
 // Ignore vibration from the buzzer itself and brief mechanical settling.
 muteVibrationUntil=millis()+200;
}
void showBar(uint8_t n){for(uint8_t i=0;i<6;i++)digitalWrite(LEDS[i],i<n?HIGH:LOW);}
void readDistance(){
 if(millis()-lastPing<60)return;
 lastPing=millis();
 digitalWrite(TRIG,LOW);delayMicroseconds(2);
 digitalWrite(TRIG,HIGH);delayMicroseconds(10);digitalWrite(TRIG,LOW);
 uint32_t width=pulseIn(ECHO,HIGH,25000); // bounded wait: OTA remains serviced
 uint16_t mm=2000;
 if(width>=120 && width<=23200){uint32_t d=width*343UL/2000UL;mm=d<2000?d:2000;}
 samples[sampleIndex]=mm;sampleIndex=(sampleIndex+1)%3;
 uint16_t a=samples[0],b=samples[1],c=samples[2],t;
 if(a>b){t=a;a=b;b=t;}if(b>c){t=b;b=c;c=t;}if(a>b){t=a;a=b;b=t;}
 distanceMM=b;
 uint8_t next=0;
 for(uint8_t i=0;i<6;i++){
  if(distanceMM < LIMIT_MM[i]+(i<level?30:0))next=i+1;else break;
 }
 level=next;
 if(distanceMM<120)closeAlarm=true;
 if(distanceMM>150)closeAlarm=false;
}
void runApplication(){
 uint32_t now=millis();
#if FW_VERSION == 3
 if(now-lastPot>=20){
  lastPot=now;
  potValue=(potValue*3+analogRead(POT))/4;
  // 0..5 LEDs are the normal zone. Sixth (red) is the warning zone.
  const int thresholds[6]={200,850,1500,2150,2800,3450};
  uint8_t next=0;
  for(uint8_t i=0;i<6;i++){if(potValue>=thresholds[i]-(i<level?60:0))next=i+1;else break;}
  level=next;potAlarm=(level==6);
 }
 showBar(level);
 setBuzzer(potAlarm && (now%700<140));
#else
 readDistance();now=millis();
#if FW_VERSION == 2
 bool edge;
 portENTER_CRITICAL(&vibMux);edge=vibrationEdge;vibrationEdge=false;portEXIT_CRITICAL(&vibMux);
 if(edge && !buzzerOn && (int32_t)(now-muteVibrationUntil)>=0 && now-lastVibration>=300){
  lastVibration=now;vibrationUntil=now+2500;
  Serial.println("VIBRATION ALARM: fast red blink + buzzer");
 }
 bool vibrationAlarm=(int32_t)(now-vibrationUntil)<0;
 if(vibrationAlarm){
  showBar(level>5?5:level);
  digitalWrite(LEDS[5],((now/80)%2)?HIGH:LOW);
  setBuzzer(true);
 }else
#endif
 {
  showBar(level);
  setBuzzer(closeAlarm && (now%700<140));
 }
#endif
 if(now-lastLog>=1000){
  lastLog=now;
  Serial.printf("V%d distance=%u mm pot=%d LEDs=%u buzzer=%d vibDO=%d\n",
    FW_VERSION,distanceMM,potValue,level,buzzerOn,digitalRead(VIB));
 }
}
const char PAGE[] PROGMEM=R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>SmartGuard OTA</title></head>
<body style="font:18px sans-serif;max-width:600px;margin:30px auto;padding:16px">
<h2>SmartGuard OTA</h2><pre id="info">Loading...</pre>
<p>Select the application .ino.bin compiled for ESP32-S3.</p>
<input type="file" id="file" accept=".bin"><button id="send" onclick="send()">Upload firmware</button><p id="msg"></p>
<script>
let busy=false;
async function refresh(){if(busy)return;try{let r=await fetch('/info');document.getElementById('info').textContent=JSON.stringify(await r.json(),null,2);}catch(e){}}
setInterval(refresh,2000);refresh();
async function send(){let f=document.getElementById('file').files[0];if(!f)return;
busy=true;document.getElementById('send').disabled=true;let m=document.getElementById('msg');m.textContent='Uploading. Keep power connected.';
let d=new FormData();d.append('firmware',f);
try{let r=await fetch('/update',{method:'POST',body:d});let t=await r.text();m.textContent=t==='OK'?'Success. Refresh this page after reboot and refresh.':'FAILED: '+t;}
catch(e){m.textContent='Connection lost. Check the board IP and the reported version; retry if unchanged.';}
busy=false;document.getElementById('send').disabled=false;}
</script></body></html>)HTML";
void handleInfo(){
 String json="{"+identityFields()+",\"version\":"+String(FW_VERSION)+
 ",\"distance_mm\":"+String(distanceMM)+",\"pot_adc\":"+String(potValue)+
 ",\"led_level\":"+String(level)+",\"buzzer\":"+(buzzerOn?"true":"false")+
 ",\"vibration_pin\":"+String(digitalRead(VIB))+"}";
 server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",json);
}
void handleUpload(){
 HTTPUpload& u=server.upload();lastUpload=millis();
 if(u.status==UPLOAD_FILE_START){
  uploadOK=false;uploading=true;setBuzzer(false);showBar(0);
  Serial.println("OTA started; sensing paused.");
  if(!Update.begin(UPDATE_SIZE_UNKNOWN))Update.printError(Serial);
 }else if(u.status==UPLOAD_FILE_WRITE){
  if(!Update.hasError() && Update.write(u.buf,u.currentSize)!=u.currentSize)Update.printError(Serial);
 }else if(u.status==UPLOAD_FILE_END){
  uploadOK=Update.end(true);if(!uploadOK)Update.printError(Serial);
 }else if(u.status==UPLOAD_FILE_ABORTED){
  Update.abort();uploadOK=false;uploading=false;Serial.println("OTA aborted");
 }
}
void handleDone(){
 bool ok=uploadOK && !Update.hasError();
 server.sendHeader("Connection","close");
 server.send(ok?200:500,"text/plain",ok?"OK":"Update failed. Check Serial Monitor and OTA partition scheme.");
 delay(300);if(ok)ESP.restart();
 uploading=false;
}
void startServer(){
 WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);
 WiFi.begin(LAN_SSID,LAN_PASS);
 Serial.print("Joining shared Wi-Fi");
 uint32_t started=millis();
 while(WiFi.status()!=WL_CONNECTED && millis()-started<15000){delay(250);Serial.print('.');}
 if(WiFi.status()!=WL_CONNECTED){
  WiFi.mode(WIFI_AP_STA);
  String recovery="Nexus-"+nodeId().substring(6);
  WiFi.softAP(recovery.c_str(),"guard1234");
  Serial.println("\nLAN unavailable. Recovery AP: "+recovery);
 }
 server.on("/",HTTP_GET,[](){server.send_P(200,"text/html",PAGE);});
 server.on("/info",HTTP_GET,handleInfo);
 server.on("/update",HTTP_POST,handleDone,handleUpload);
 server.begin();Serial.println("\nOTA page: http://"+nodeIP());
 Serial.println(String("NEXUS_INFO {")+identityFields()+"}");
}
void setup(){
 Serial.begin(115200);delay(300);
 for(uint8_t p:LEDS){pinMode(p,OUTPUT);digitalWrite(p,LOW);}
 pinMode(BUZZER,OUTPUT);digitalWrite(BUZZER,!BUZZER_ON);
 pinMode(TRIG,OUTPUT);digitalWrite(TRIG,LOW);pinMode(ECHO,INPUT);
 pinMode(VIB,INPUT_PULLUP);
#if FW_VERSION == 3
 analogReadResolution(12);analogSetPinAttenuation(POT,ADC_11db);
 potValue=analogRead(POT);
#endif
 // Independent startup beep: also tests the buzzer circuit.
 setBuzzer(true);delay(180);setBuzzer(false);delay(400);
#if FW_VERSION == 2
 attachInterrupt(digitalPinToInterrupt(VIB),vibrationISR,CHANGE);
#endif
 startServer();
 Serial.printf("Running version %d. No LCD required.\n",FW_VERSION);
}
void loop(){
 serialIdentity();
 server.handleClient();
 if(uploading){
  if(millis()-lastUpload>30000){Update.abort();uploading=false;uploadOK=false;}
  delay(1);return;
 }
 runApplication();delay(1);
}
