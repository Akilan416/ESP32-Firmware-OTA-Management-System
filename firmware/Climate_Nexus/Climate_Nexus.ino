/* ESP32 Dev Module: example DHT11 + IR + DC motor (L298N) node.
 ASSUMED WIRING -- adapt to your friend's actual circuit BEFORE uploading:
 DHT11 data=27 (3.3V logic, pull-up if bare sensor), IR DO=26 (3.3V logic).
 L298N IN1=25, IN2=33, ENA=32 (remove ENA jumper).
 Motor power from its rated external supply; common GND. Not a mains fan circuit.
 Motor ON at >=30 C, OFF at <=28 C. By default IR presence is also required,
 held for 15 seconds after the last detection. Invalid DHT reading -> motor OFF.
 Install DHT sensor library by Adafruit and Adafruit Unified Sensor.
 Fill in LAN_SSID/LAN_PASS. OTA preserves connectivity only if retained in future builds.
 This is an example replacement, NOT an automatic merge of existing fan firmware.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_app_desc.h>
#include <DHT.h>

#define FW_VERSION 1   // Increment after implementing a later climate-node update
constexpr uint8_t DHT_PIN=27, IR_PIN=26, MOTOR_IN1=25, MOTOR_IN2=33, MOTOR_EN=32;
constexpr bool REQUIRE_IR=true; // false = temperature alone controls the motor
constexpr uint8_t IR_ACTIVE=LOW;
constexpr float MOTOR_ON_C=30.0f, MOTOR_OFF_C=28.0f;
constexpr uint32_t PRESENCE_HOLD_MS=15000;
DHT dht(DHT_PIN,DHT11);
float temperature=NAN, humidity=NAN;
bool motorOn=false, temperatureDemand=false, irDetected=false, seenPresence=false;
uint32_t lastDht=0,lastPresence=0;
// Fill in the same 2.4 GHz network used by the laptop and other boards.
constexpr char LAN_SSID[]="YOUR_WIFI_NAME";
constexpr char LAN_PASS[]="YOUR_WIFI_PASSWORD";
constexpr char NODE_NAME[]="Climate & motor";
constexpr char NODE_KIND[]="climate";
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
bool uploading=false, uploadOK=false;
uint32_t lastUpload=0;
void startServer();
void handleUpload();
void handleDone();
void handleInfo();
void setMotor(bool on){
 if(on==motorOn)return;
 motorOn=on;
 digitalWrite(MOTOR_EN,LOW);
 digitalWrite(MOTOR_IN1,on?HIGH:LOW);digitalWrite(MOTOR_IN2,LOW);
 digitalWrite(MOTOR_EN,on?HIGH:LOW);
}
void runApplication(){
 uint32_t now=millis();
 irDetected=(digitalRead(IR_PIN)==IR_ACTIVE);
 if(irDetected){lastPresence=now;seenPresence=true;}
 if(now-lastDht>=2000){
  lastDht=now;temperature=dht.readTemperature();humidity=dht.readHumidity();
  if(isnan(temperature)||isnan(humidity))temperatureDemand=false;
  else if(temperature>=MOTOR_ON_C)temperatureDemand=true;
  else if(temperature<=MOTOR_OFF_C)temperatureDemand=false;
 }
 bool presence=seenPresence && now-lastPresence<PRESENCE_HOLD_MS;
 setMotor(temperatureDemand && (!REQUIRE_IR || presence));
}
const char PAGE[] PROGMEM=R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>SmartGuard OTA</title></head>
<body style="font:18px sans-serif;max-width:600px;margin:30px auto;padding:16px">
<h2>SmartGuard OTA</h2><pre id="info">Loading...</pre>
<p>Select the application .ino.bin compiled for ESP32.</p>
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
 ",\"temperature_c\":"+(isnan(temperature)?String("null"):String(temperature,1))+
 ",\"humidity\":"+(isnan(humidity)?String("null"):String(humidity,1))+
 ",\"ir_detected\":"+(irDetected?"true":"false")+
 ",\"motor_on\":"+(motorOn?"true":"false")+"}";
 server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",json);
}
void handleUpload(){
 HTTPUpload& u=server.upload();lastUpload=millis();
 if(u.status==UPLOAD_FILE_START){
  uploadOK=false;uploading=true;setMotor(false);
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
 pinMode(MOTOR_EN,OUTPUT);pinMode(MOTOR_IN1,OUTPUT);pinMode(MOTOR_IN2,OUTPUT);
 digitalWrite(MOTOR_EN,LOW);digitalWrite(MOTOR_IN1,LOW);digitalWrite(MOTOR_IN2,LOW);
 pinMode(IR_PIN,INPUT_PULLUP);dht.begin();
 startServer();
 Serial.println("Climate node ready: DHT11 + IR + motor");
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
