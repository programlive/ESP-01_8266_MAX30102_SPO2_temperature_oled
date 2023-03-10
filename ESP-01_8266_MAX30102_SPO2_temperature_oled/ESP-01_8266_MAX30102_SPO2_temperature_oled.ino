/*****************************************************************
* Pulse rate and SPO2 meter using the MAX30102
* https://github.com/har-in-air/ESP8266_MAX30102_SPO2_PULSE_METER
* 
* 
* 
* This is a mashup of 
* 1. sensor initialization and readout code from Sparkfun 
* https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library
*  
*  2. spo2 & pulse rate analysis from 
* https://github.com/aromring/MAX30102_by_RF  
* (algorithm by  Robert Fraczkiewicz)
* I tweaked this to use 50Hz sample rate
* 
* 3. ESP8266 AP & Webserver code from Random Nerd tutorials
* https://randomnerdtutorials.com/esp8266-nodemcu-access-point-ap-web-server/
******************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
//#include <ESPAsyncWebServer.h>
#include <ESPAsyncWebSrv.h>
#include "algorithm_by_RF.h"
#include "MAX30105.h"
#include "heartRate.h"
#include "SSD1306Wire.h"        //(4针)0.96寸I2C通讯128x64OLED液晶屏模块1315驱动


#define SDA 1
#define SCL 3


SSD1306Wire display(0x3c, SDA, SCL);   // ADDRESS, SDA, SCL  -  SDA and SCL usually populate automatically based on your board's pins_arduino.h e.g. https://github.com/esp8266/Arduino/blob/master/variants/nodemcu/pins_arduino.h

const char* ssidAP = "SP02-Pulse";
const char* passwordAP = "";
int arr[128] = {0}; //显示数组

AsyncWebServer server(80);

// uncomment for test : measuring actual sample rate, or to display waveform on a serial plotter
//#define MODE_DEBUG  

MAX30105 sensor;


#ifdef MODE_DEBUG
uint32_t startTime;
#endif

uint32_t  aun_ir_buffer[RFA_BUFFER_SIZE]; //infrared LED sensor data
uint32_t  aun_red_buffer[RFA_BUFFER_SIZE];  //red LED sensor data
int32_t   n_heart_rate; 
float     n_spo2;
int       numSamples;
float temperature;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    html {
     font-family: Arial;
     margin: 0px;
     text-align: left;
    }
    td {
      color:grey;      
      padding: 10px;
      }
    table {
      background-color:black;
      margin-top : 70px;
      margin-left: auto;
      margin-right: auto;
      }    
    body {
      background-color:grey;      
      margin:0;
      padding:0;
      font-size: 2.0rem;
      }
    .field {
      font-size: 2.5rem;
      color : green;
      }
  </style>
</head>
<body>
<table>
  <tr>
    <td>SPO2</td>
    <td class="field" id="spo2" align="right">%SPO2%</td>
    <td>&#37;</td>
  </tr>
  <tr>
    <td>Pulse</td>
    <td class="field" id="heartrate" align="right">%HEARTRATE%</td>
    <td>bpm</td>
  </tr>
    <tr>
    <td>temperature</td>
    <td class="field" id="temperature" align="right">%TEMPERATURE%</td>
    <td>C</td>
  </tr>
</table>
</body>
<script>
setInterval(function ( ) {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      document.getElementById("spo2").innerHTML = this.responseText;
    }
  };
  xhttp.open("GET", "/spo2", true);
  xhttp.send();
}, 4000 ) ;
setInterval(function ( ) {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      document.getElementById("heartrate").innerHTML = this.responseText;
    }
  };
  xhttp.open("GET", "/heartrate", true);
  xhttp.send();
}, 4000 ) ;
setInterval(function ( ) {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      document.getElementById("temperature").innerHTML = this.responseText;
    }
  };
  xhttp.open("GET", "/temperature", true);
  xhttp.send();
}, 4000 ) ;
</script>
</html>)rawliteral";



String processor(const String& var){
    if(var == "SPO2"){
      return n_spo2 > 0 ? String(n_spo2) : String("00.00");
      }
    else if(var == "HEARTRATE"){
      return n_heart_rate > 0 ? String(n_heart_rate) : String("00");
      }
    return String();
    }


void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("SPO2/Pulse meter");

 // Initialising the UI will init the display too.
  display.init();
//  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);


  
  // ESP8266 tx power output 20.5dBm by default
  // we can lower this to reduce power supply noise caused by tx bursts
  WiFi.setOutputPower(12); 

  WiFi.softAP(ssidAP, passwordAP); 
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
     
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html, processor);
    });
  server.on("/spo2", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", n_spo2 > 0 ? String(n_spo2).c_str() : "00.00");
    });
  server.on("/heartrate", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", n_heart_rate > 0 ? String(n_heart_rate).c_str() : "00");
  });
  server.on("/temperature", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", temperature > 0 ? String(temperature).c_str() : "00");
  });
  server.begin();

  pinMode(LED_BUILTIN, OUTPUT);
  
  if (sensor.begin(Wire, I2C_SPEED_FAST) == false) {
    Serial.println("Error: MAX30102 not found, try cycling power to the board...");
    // indicate fault by blinking the board LED rapidly
    while (1){
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      delay(100);
      }
    }
    // ref Maxim AN6409, average dc value of signal should be within 0.25 to 0.75 18-bit range (max value = 262143)
    // You should test this as per the app note depending on application : finger, forehead, earlobe etc. It even
    // depends on skin tone.
    // I found that the optimum combination for my index finger was :
    // ledBrightness=30 and adcRange=2048, to get max dynamic range in the waveform, and a dc level > 100000
  byte ledBrightness = 60; // 0 = off,  255 = 50mA
  byte sampleAverage = 4; // 1, 2, 4, 8, 16, 32
  byte ledMode = 2; // 1 = Red only, 2 = Red + IR, 3 = Red + IR + Green (MAX30105 only)
  int sampleRate = 200; // 50, 100, 200, 400, 800, 1000, 1600, 3200
  int pulseWidth = 411; // 69, 118, 215, 411
  int adcRange = 4096; // 2048, 4096, 8192, 16384
  
  sensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange); 
  sensor.getINT1(); // clear the status registers by reading
  sensor.getINT2();
  numSamples = 0;

#ifdef MODE_DEBUG
  startTime = millis();
#endif
  }


#ifdef MODE_DEBUG
void loop(){
  sensor.check(); 

  while (sensor.available())   {
    numSamples++;
#if 0 
    // measure the sample rate FS  (in Hz) to be used by the RF algorithm
    //Serial.print("R[");
    //Serial.print(sensor.getFIFORed());
    //Serial.print("] IR[");
    //Serial.print(sensor.getFIFOIR());
    //Serial.print("] ");
    Serial.print((float)numSamples / ((millis() - startTime) / 1000.0), 2);
    Serial.println(" Hz");
#else 
    // display waveform on Arduino Serial Plotter window
    Serial.print(sensor.getFIFORed());
    Serial.print(" ");
    Serial.println(sensor.getFIFOIR());
#endif
    
    sensor.nextSample();
  }
}

#else // normal spo2 & heart-rate measure mode

void loop() {
  float ratio,correl; 
  int8_t  ch_spo2_valid;  
  int8_t  ch_hr_valid;  


  sensor.check();
  while (sensor.available())   {
      aun_red_buffer[numSamples] = sensor.getFIFORed(); 
      aun_ir_buffer[numSamples] = sensor.getFIFOIR();
      numSamples++;
      sensor.nextSample(); 


      if (numSamples == RFA_BUFFER_SIZE) {
        // calculate heart rate and SpO2 after RFA_BUFFER_SIZE samples (ST seconds of samples) using Robert's method
        rf_heart_rate_and_oxygen_saturation(aun_red_buffer, RFA_BUFFER_SIZE,aun_ir_buffer , &n_spo2, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid, &ratio, &correl);  //这个巨坑！！没有之一，red_buffer和ir_buffer得交换位置读数才显得正常，否则乱跳数字大部分为-999，但所有例程都没有改，有没人知道为什么
        Serial.printf("SP02 ");
        if (ch_spo2_valid) Serial.print(n_spo2); else Serial.print("x");
        Serial.print(", Pulse ");
        if (ch_hr_valid) Serial.print(n_heart_rate); else Serial.print("x");

        numSamples = 0;
        // toggle the board LED. This should happen every ST (= 4) seconds if MAX30102 has been configured correctly
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        temperature = sensor.readTemperature();
        Serial.print(", temp=");
        Serial.print(temperature, 4);Serial.print("°C");
        Serial.println();


        }

        //-------------oled---------------end
        // clear the display
        display.clear();
        // draw the current demo method
        display.setFont(ArialMT_Plain_16);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(0, 45, "H:" + String(n_heart_rate));
        display.drawString(60, 45, "S:" + String(n_spo2));
        display.setFont(ArialMT_Plain_10);
        display.drawString(0, 0, "T:" + String(temperature));

        //-------------oled---------------end

    }
        if (checkForBeat(aun_red_buffer[numSamples-1]) == true and aun_red_buffer[numSamples-1] > 20000){arr[127] =15;}else{arr[127] =40;}
        for(int i=0;i<127;i++){
          display.drawLine(i, arr[i], i+1, arr[i+1]);
          arr[i] = arr[i+1];
          }
        // write the buffer to the display oled
        display.display();
  }
  
#endif
