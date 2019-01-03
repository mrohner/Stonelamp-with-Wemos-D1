
/*
  Neopixel stonelamp by Markus Rohner
  Version 2.1     15.6.2018 Added logging facility
  Version 2.0     11.3.2018 Blynk running on a different processor. This sketch without Blynk but with MQTT
  Version 1.1     17.5.2017

  Funtion: 12 activities
  
Aknowledgements:
  1. Adafruit Neopixel library: http://learn.adafruit.com/adafruit-neopixel-uberguide/neomatrix-library
  2. Arduino – LEDStrip effects for NeoPixel and FastLED: https://www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/
  
Pin assignments:
 * Wemos
D2 to DI 5050 RGB LED string 

Bill of material:
  -Wemos D1 mini
  -Adafruit Neopixelstring 60 LEDs
  -1000uF capacitor 
  -5V 4A Power Supply

*/
const bool DEBUG = 1; //Set to 1 for print 
#include <ESPEEPROM.h> //https://github.com/esp8266/Arduino/tree/master/libraries/EEPROM 9 from Ivan Grokhotkov. 
int eeAddress = 1;
struct MyObject {
  bool lamp_on;
  bool blinkon;
  long current_color;
  int current_activity;
  int brightness;
  int standard_speed;
};
MyObject stonelamp; //Variable to store custom object read from EEPROM.
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SimpleTimer.h>
SimpleTimer timer;
#include "WemoSwitch.h"
#include "WemoManager.h"
#include "CallbackFunction.h"
#include <Syslog.h>

WemoManager wemoManager;
WemoSwitch *light = NULL;

// RGB Lights
#include <Adafruit_NeoPixel.h>
#define NUM_LEDS 60
const int NUM_LEDS4 = NUM_LEDS/4;
#define PIN D2
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, PIN, NEO_GRB + NEO_KHZ800);

//const int long RED = 16711680;
//const int long GREEN = 65280;
const long BLUE = 255;
int brightness = 155;
int standard_speed = 50;
int current_activity = 0;
int this_activity = 0;
long current_color = BLUE;
bool lamp_on = false;
bool blinkon = false;

// Wireless settings
// Update these with values suitable for your network.
const char* ssid = "*****";
const char* password = "*****";
const char* mqtt_server = "192.168.178.59";
const char* mqtt_username = "mqtt";
const char* mqtt_password = "****";
char* InTopic = "Stonelamp/#"; //subscribe to topic to be notified about
char* OutTopic = "domoticz/in"; 
const int STONELAMP_IDX = 55;

// Syslog server connection info
#define SYSLOG_SERVER "192.168.178.59"
#define SYSLOG_PORT 514

// This device info
#define DEVICE_HOSTNAME "wemos62"
#define APP_NAME "Stonelamp"

// A UDP instance to let us send and receive packets over UDP
WiFiUDP udpClient;

// Create a new syslog instance with LOG_KERN facility
Syslog syslog(udpClient, SYSLOG_SERVER, SYSLOG_PORT, DEVICE_HOSTNAME, APP_NAME, LOG_KERN);

WiFiClient espClient;
PubSubClient client(espClient);
int counter = 0;

// Timing
volatile int WDTCount = 0;


void setup() {
  Serial.begin(115200);
  if (DEBUG) Serial.println(F("setup start"));
  setup_wifi();
  //WiFiClient WiFiclient;
  //const int httpPort = 8080;
  //next line to prevent MQTT connection error -2
  if(espClient.connect(mqtt_server, 80))Serial.println("Wifi client connected");
  client.setServer(mqtt_server, 8883);
  client.setCallback(callback);
 
// Init Neopixels
  strip.begin();
  all_lights(0);

//OTA
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);
  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("Wemos62-Stonelamp");
  // No authentication by default
  ArduinoOTA.setPassword((const char *)"070");
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) if (DEBUG) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) if (DEBUG) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) if (DEBUG) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) if (DEBUG) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) if (DEBUG) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  
  //EEPROM
  EEPROM.begin(128);
  EEPROM.get(eeAddress, stonelamp);
  lamp_on = stonelamp.lamp_on;
  blinkon = stonelamp.blinkon;
  current_color = stonelamp.current_color;
  current_activity = stonelamp.current_activity;
  brightness = stonelamp.brightness;
  standard_speed = stonelamp.standard_speed;
  publish(STONELAMP_IDX,lamp_on);

  wemoManager.begin();
  // Format: Alexa invocation name, local port no, on callback, off callback
  light = new WemoSwitch("stonelamp", 80, lightOn, lightOff);
  wemoManager.addDevice(*light);
  
  if (DEBUG) syslog.logf(LOG_INFO, "Entering Loop");
  timer.setInterval(1000L, ISRwatchdog);
  timer.setInterval(600000L, Publish_Status);//every 10 minutes
}


void loop() {
  if (!client.connected()) reconnect();
  wait(1);
  activity(current_activity);
}

 
void setup_wifi() {
  WiFi.mode(WIFI_STA); 
  delay(10);
  // We start by connecting to a WiFi network
  if (DEBUG) syslog.logf(LOG_INFO, "Connected to %s", ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    wait(500);
  }
  wait(500);
}


//{"idx":55,"nvalue":0,"svalue":""}
void publish(int idx, int nvalue){ 
  char output[130];
  snprintf_P(output, sizeof(output), PSTR("{\"idx\":%d,\"nvalue\":%d,\"svalue\":\"\"}"),idx,nvalue);
  client.publish(OutTopic,output);
}


void Publish_Status(){
  publish(STONELAMP_IDX,lamp_on);
}


void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    if (DEBUG) syslog.logf(LOG_INFO, "Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("Stonelamp",mqtt_username,mqtt_password)) {
      if (DEBUG) syslog.logf(LOG_INFO, "connected");
      counter = 0;
      // Once connected, publish an announcement...
      // ... and resubscribe
      client.subscribe(InTopic);
      delay(10);
      //client.subscribe(InTopic1);
    } else {
      if (DEBUG) {
        WiFi.localIP();
        syslog.logf(LOG_INFO, "Client State: %s",client.state());
      }
      ++counter;
      if (counter > 180) ESP.reset();
      // Wait 0.3 seconds before retrying
      wait(300);
      ArduinoOTA.handle();
    }
  }
}


void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonBuffer<500> jsonBuffer;
  //const char* json = "{\"CMD\":\"Color\",\"ARG\":16777215}";
  JsonObject& root = jsonBuffer.parseObject((char*)payload);
  int idx = root["idx"]; // 55
  int nvalue = root["nvalue"]; // request: 0 = off, 1 = on
  const char* CMD = root["CMD"]; // "Color"
  long argument = root["ARG"]; // 16777215
  String Command = String(CMD);
  if (DEBUG) {
  Serial.print("IDX: ");
  Serial.print(idx);
  Serial.print(" nvalue: ");
  Serial.print(nvalue);
  Serial.print(" CMD: ");
  Serial.print(Command);
  Serial.print(" ARG: ");
  Serial.println(argument);
  }
  if (idx == STONELAMP_IDX && nvalue != lamp_on) CMD_Power(nvalue);
  else if (Command == "Power" && argument != lamp_on) CMD_Power(argument); 
  else if (Command == "Color") CMD_Color(argument); 
  else if (Command == "Scheme")CMD_Activity(argument); 
  else if (Command == "Dimmer")CMD_Brightness(argument); 
  else if (Command == "Speed") CMD_Speed(argument); 
  else if (Command == "Blink") CMD_Blink(argument); 
}  


void activity(int activity_button) {
  if (lamp_on) {
    this_activity = activity_button;
    strip.setBrightness(brightness);
    switch (activity_button) {
      case 0:  // no activity
        all_lights(current_color);
        if (blinkon) {
          wait(standard_speed*2);
          strip.setBrightness(0);
          strip.show();
          wait(standard_speed*2);
          strip.setBrightness(brightness);
         }
      break;
      case 1:  // colorWipe
      colorWipe(0); 
      colorWipe(current_color); 
    break;
    case 2:  // theaterChase
      theaterChase(strip.Color(random(255), random(255), random(255)));
    break;
    case 3:  // rainbow
      rainbow();
    break;
    case 4:  // rainbowCycle
      rainbowCycle();
    break;
    case 5:  // theaterChaseRainbow
      theaterChaseRainbow();
    break;
    case 6:  // CylonBounce
      CylonBounce(splitColor (current_color,'r'), splitColor (current_color,'g'), splitColor (current_color,'b'), 3);
    break;
    case 7:  // TwinkleRandom
      TwinkleRandom(20,false);
    break;
    case 8:  // Sparkle
      Sparkle(random(255), random(255), random(255));
     break;
    case 9:  // RunningLights
      RunningLights(splitColor (current_color,'r'), splitColor (current_color,'g'), splitColor (current_color,'b'));
    break;
    case 10:  // Fire
      Fire(55,120);// was 55,120
    break;
    case 11:  // fade
      FadeInOut(splitColor (current_color,'r'), splitColor (current_color,'g'), splitColor (current_color,'b'));
    break;
    case 12:  // rotate
      rotate();
    break; 
   }
 }
 else {
  strip.setBrightness(0);
  strip.show();
 }
}


void CMD_Power(int argument) { //ON Off
    if (argument == 1) lamp_on = 1;
    else {
      all_lights(0);
      lamp_on = 0;
    }
    printStatus();
    Publish_Status();
}


void CMD_Activity(int argument) { //Activity slider
   current_activity = argument;
   printStatus();
}

  
void CMD_Blink(int argument) { //Blink
  if (argument) blinkon = true;
  else blinkon = false;
  printStatus();
}


void CMD_Brightness(int argument) { // Brightness slider
  brightness = argument;
  strip.setBrightness(brightness);
  strip.show();
  printStatus();
}


void CMD_Speed(int argument) { //Speed
  standard_speed = argument;
  printStatus();
}


void CMD_Color(long argument) { //RGB light
  current_color = argument;
  printStatus();  
}


void ISRwatchdog() {
  WDTCount++;
  if (WDTCount == 5) {
    if (DEBUG) Serial.println(F("WDT reset"));
    ESP.reset();
  }
  yield();
}


void setPixel(int Pixel, byte red, byte green, byte blue) {
  strip.setPixelColor(Pixel, strip.Color(red, green, blue));
} 


void lightOn() {
  CMD_Power(1); 
}


void lightOff() {
  CMD_Power(0); 
}


// Fill the dots one after the other with a color (1)
void colorWipe(uint32_t c) {
  for (int i = 0; i < NUM_LEDS4; i++) {
    if (!lamp_on || this_activity != current_activity) break;
    for (int j = 0; j < 4; j++) {
    strip.setPixelColor(i+j*NUM_LEDS4,c);
    }
    strip.show();
    wait(standard_speed/3);
 }
}


//Theatre-style crawling lights. (2)
void theaterChase(uint32_t c) {
  for (int j = 0; j < 10; j++) { //do 10 cycles of chasing
    if (!lamp_on || this_activity != current_activity) break;
    for (int q = 0; q < 3; q++) {
      if (!lamp_on || this_activity != current_activity) break;
      for (int i = 0; i < NUM_LEDS4; i = i + 3) {
        if (!lamp_on || this_activity != current_activity) break;
        for (int m = 0; m < 4; m++) {
        strip.setPixelColor(i + q + m*NUM_LEDS4, c);  //turn every third pixel on
        }
      }
      strip.show();
      wait(standard_speed*3);
      for (int i = 0; i < NUM_LEDS; i = i + 3) {
        if (!lamp_on || this_activity != current_activity) break;
        for (int m = 0; m < 4; m++) {
        strip.setPixelColor(i + q + m*NUM_LEDS4, 0);      //turn every third pixel off
        }
      }
    }
  }
}


// (3)
void rainbow() {
  uint16_t i, j;
  for (j = 0; j < 256; j+=2) {
    if (!lamp_on || this_activity != current_activity) break;
    for (i = 0; i < NUM_LEDS4; i++) {
      if (!lamp_on || this_activity != current_activity) break;
      for (int m = 0; m < 4; m++) {
        strip.setPixelColor(i + m*NUM_LEDS4,Wheel((i + j) & 255));
        strip.show();
        wait(standard_speed/5);
      }
     }
   }
}


// Slightly different, this makes the rainbow equally distributed throughout (4)
void rainbowCycle() {
  uint16_t i, j;
  for (j = 0; j < 256 * 5; j++) { // 5 cycles of all colors on wheel
    if (!lamp_on || this_activity != current_activity) break;
    for (i = 0; i < NUM_LEDS4; i++) {
      if (!lamp_on || this_activity != current_activity) break;
      for (int m = 0; m < 4; m++) {
        strip.setPixelColor(i + m*NUM_LEDS4, Wheel(((i * 256 / NUM_LEDS4) + j) & 255));
       }
     }
    strip.show();
    wait(standard_speed/2);
  }
}


//Theatre-style crawling lights with rainbow effect (5)
void theaterChaseRainbow() {
  for (int j = 0; j < 256; j+=2) {   // cycle all 256 colors in the wheel
    if (!lamp_on || this_activity != current_activity) break;
    for (int q = 0; q < 3; q++) {
      if (!lamp_on || this_activity != current_activity) break;
      for (int i = 0; i < NUM_LEDS4; i = i + 3) {
        if (!lamp_on || this_activity != current_activity) break;
        for (int m = 0; m < 4; m++) {
        strip.setPixelColor(i + q + m*NUM_LEDS4, Wheel( (i + j) % 255)); //turn every third pixel on
        }
      }
      strip.show();
      wait(standard_speed*3);
      for (int i = 0; i < NUM_LEDS4; i = i + 3) {
        if (!lamp_on || this_activity != current_activity) break;
        for (int m = 0; m < 4; m++) {
        strip.setPixelColor(i + q + m*NUM_LEDS4, 0);      //turn every third pixel off
        }
      }
    }
  }
}


// (6)
void CylonBounce(byte red, byte green, byte blue, int EyeSize){
  for(int i = 0; i < NUM_LEDS4-EyeSize-2; i++) {
    if (!lamp_on || this_activity != current_activity) break;
    all_lights(0);
     for (int m = 0; m < 4; m++) {
       setPixel(i+ m*NUM_LEDS4, red/10, green/10, blue/10);
     }
    for(int j = 1; j <= EyeSize; j++) {
      if (!lamp_on || this_activity != current_activity) break;
       for (int m = 0; m < 4; m++) {
         setPixel(i + j + m*NUM_LEDS4, red, green, blue); 
       }
    }
      for (int m = 0; m < 4; m++) {
        setPixel(i + EyeSize + 1 + m*NUM_LEDS4, red/10, green/10, blue/10);
      }
    strip.show();
    wait(standard_speed/2);
  }
  wait(standard_speed);

  for(int i = NUM_LEDS4-EyeSize-2; i > 0; i--) {
    if (!lamp_on || this_activity != current_activity) break;
    all_lights(0);
    for (int m = 0; m < 4; m++) {
      setPixel(i + m*NUM_LEDS4, red/10, green/10, blue/10);
    }
    for(int j = 1; j <= EyeSize; j++) {
      if (!lamp_on || this_activity != current_activity) break;
      for (int m = 0; m < 4; m++) {
        setPixel(i + j + m*NUM_LEDS4, red, green, blue); 
      }
    }
    for (int m = 0; m < 4; m++) {
      setPixel(i +EyeSize + 1 + m*NUM_LEDS4, red/10, green/10, blue/10);
    }
    strip.show();
    wait(standard_speed/2);
  }
  wait(standard_speed);
}


// (7)
void TwinkleRandom(int Count,boolean OnlyOne) {
  all_lights(0);
   for (int i=0; i<Count; i++) {
     if (!lamp_on || this_activity != current_activity) break;
     setPixel(random(NUM_LEDS),random(0,255),random(0,255),random(0,255));
     strip.show();
     wait(standard_speed);
     if(OnlyOne) { 
       all_lights(0); 
     }
   }
   wait(standard_speed/2);
}


// (8)
void Sparkle(byte red, byte green, byte blue) {
  all_lights(0);
  int Pixel = random(NUM_LEDS);
  setPixel(Pixel,red,green,blue);
  strip.show();
  wait(standard_speed*2);
  setPixel(Pixel,0,0,0);
}


// (9)
void RunningLights(byte red, byte green, byte blue) {
  int Position=0;
  for(int i=0; i<NUM_LEDS4*2; i++)  {
    if (!lamp_on || this_activity != current_activity) break;
      Position++; // = 0; //Position + Rate;
      for(int i=0; i<NUM_LEDS4; i++) {
        if (!lamp_on || this_activity != current_activity) break;
          for (int m = 0; m < 4; m++) {
            if (!lamp_on || this_activity != current_activity) break;
            setPixel(i + m*NUM_LEDS4,((sin(i+Position) * 127 + 128)/255)*red,
                   ((sin(i+Position) * 127 + 128)/255)*green,
                   ((sin(i+Position) * 127 + 128)/255)*blue);
        }
      }
      strip.show();
      wait(standard_speed*2);
  }
}


// (10)
void Fire(int Cooling, int Sparking) {
  static byte heat[NUM_LEDS];
  int cooldown;
  // Step 1.  Cool down every cell a little
  for (int m = 0; m < 4; m++) {
    for( int i = 0; i < NUM_LEDS4; i++) {
      cooldown = random(0, ((Cooling * 10) / NUM_LEDS4) + 2);
      if(cooldown>heat[i + m*NUM_LEDS4]) {
        heat[i + m*NUM_LEDS4]=0;
     } else {
        heat[i + m*NUM_LEDS4]=heat[i + m*NUM_LEDS4]-cooldown;
      }
    }
  }
  // Step 2.  Heat from each cell drifts 'up' and diffuses a little
  for (int m = 0; m < 4; m++) {
    for( int k = NUM_LEDS4 - 1; k >= 2; k--) {
      if (!lamp_on || this_activity != current_activity) break;
      heat[k + m*NUM_LEDS4] = (heat[k + m*NUM_LEDS4 - 1] + heat[k + m*NUM_LEDS4 - 2] + heat[k + m*NUM_LEDS4 - 2]) / 3;
    }
  }
  // Step 3.  Randomly ignite new 'sparks' near the bottom
  for (int m = 0; m < 4; m++) {
    if( random(255) < Sparking ) {
      int y = random(3); //was 7
      heat[y + m*NUM_LEDS4] = heat[y + m*NUM_LEDS4] + random(160,255);
    }
  }
  // Step 4.  Convert heat to LED colors
  for( int j = 0; j < NUM_LEDS; j++) {
    if (!lamp_on || this_activity != current_activity) break;
    setPixelHeatColor(j, heat[j]);
  }
  strip.show();
  wait(standard_speed);
}


void setPixelHeatColor (int Pixel, byte temperature) {
  // Scale 'heat' down from 0-255 to 0-191
  byte t192 = round((temperature/255.0)*191);
  // calculate ramp up from
  byte heatramp = t192 & 0x3F; // 0..63
  heatramp <<= 2; // scale up to 0..252
  // figure out which third of the spectrum we're in:
  if( t192 > 0x80) {                     // hottest
    setPixel(Pixel, 255, 255, heatramp);
  } else if( t192 > 0x40 ) {             // middle
    setPixel(Pixel, 255, heatramp, 0);
  } else {                               // coolest
    setPixel(Pixel, heatramp, 0, 0);
  }
}


//(11)
void FadeInOut(byte red, byte green, byte blue){
  float r, g, b;
      
  for(int k = 20; k < 256; k=k+1) {
    if (!lamp_on || this_activity != current_activity) break; 
    r = (k/256.0)*red;
    g = (k/256.0)*green;
    b = (k/256.0)*blue;
    all_lights(r,g,b);
    wait(standard_speed/6);
  }
     
  for(int k = 255; k >= 20; k=k-2) {
    if (!lamp_on || this_activity != current_activity) break;
    r = (k/256.0)*red;
    g = (k/256.0)*green;
    b = (k/256.0)*blue;
    all_lights(r,g,b);
    wait(standard_speed/6);
  }
}


//Rotate (12)
void rotate() {
  all_lights(0);
  for (int m = 0; m < 4; m++) {
    for (int i = m*NUM_LEDS4; i < (m+1)*NUM_LEDS4; i++) {
      if (!lamp_on || this_activity != current_activity) break;
        strip.setPixelColor(i,current_color);
       }
      strip.show();
      wait(standard_speed*2);
    for (int i = m*NUM_LEDS4; i < (m+1)*NUM_LEDS4; i++) {
      if (!lamp_on || this_activity != current_activity) break;
        strip.setPixelColor(i,0);
       }
      strip.show();
  }
}


void all_lights(int g, int r, int b) {
  wait(1);
    for (int x = 0; x < NUM_LEDS; x++) {
      strip.setPixelColor(x, g, r, b);
    }
    strip.show();
}


void all_lights(int color) {
  wait(1);
    for (int x = 0; x < NUM_LEDS; x++) {
      strip.setPixelColor(x,color);
    }
    strip.show();
}


// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  } else if (WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  } else {
    WheelPos -= 170;
    return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
  }
}


/**
   splitColor() - Receive a uint32_t value, and spread into bits.
*/
byte splitColor ( uint32_t c, char value )
{
  switch ( value ) {
    case 'r': return (uint8_t)(c >> 16);
    case 'g': return (uint8_t)(c >>  8);
    case 'b': return (uint8_t)(c >>  0);
    default:  return 0;
  }
}


void printStatus() {
  if (DEBUG) {
    Serial.print(F(" ON: "));
    Serial.print(lamp_on);
    Serial.print(F(" Blink: "));
    Serial.print(blinkon);
    Serial.print(F(" Activity: "));
    Serial.print(current_activity);
    Serial.print(F(" Color: "));
    Serial.print(current_color);
    Serial.print(F(" Brightness: "));
    Serial.print(brightness);
    Serial.print(F(" Speed: "));
    Serial.println(standard_speed);
  }
  EEPROM.begin(128);
  stonelamp.lamp_on = lamp_on;
  stonelamp.blinkon = blinkon;
  stonelamp.current_color = current_color;
  stonelamp.current_activity = current_activity;
  stonelamp.brightness = brightness;
  stonelamp.standard_speed = standard_speed;
  EEPROM.put(eeAddress, stonelamp);
  EEPROM.commit();
}


void wait (int ms) {
  client.loop();
  for(long i = 0;i <= ms * 30000; i++) asm ( "nop \n" ); //80kHz Wemos D1
  client.loop();
  ArduinoOTA.handle();
  timer.run();
  wemoManager.serverLoop();
  yield();
  WDTCount = 0;
}
