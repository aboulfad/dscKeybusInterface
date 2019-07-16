/*
 *  VirtualKeypad-Web 1.1 (esp8266)
 *
 *  Provides a virtual keypad web interface using the esp8266 as a standalone web server.
 *
 *  Usage:
 *    1. Install the following libraries directly from each Github repository:
 *         ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer
 *         ESPAsyncTCP: https://github.com/me-no-dev/ESPAsyncTCP
 *    2. Install the following libraries, available in the Arduino IDE Library Manager and
 *       the Platform.io Library Registry:
 *         ArduinoJson: https://github.com/bblanchon/ArduinoJson
 *         Chrono: https://github.com/SofaPirate/Chrono
 *    3. Set the WiFi SSID and password in the sketch.
 *    4. If desired, update the DNS hostname in the sketch.  By default, this is set to
 *       "dsc" and the web interface will be accessible at: http://dsc.local
 *    5. Set the esp8266 flash size to use 1M SPIFFS.
 *         Arduino IDE: Tools > Flash Size > 4M (1M SPIFFS)
 *    6. Upload the sketch.
 *    7. Upload the SPIFFS data containing the web server files:
 *         Arduino IDE: Tools > ESP8266 Sketch Data Upload
 *
 *  Release notes:
 *    1.1 - New: Fire, alarm, panic, stay arm, away arm, door chime buttons are now functional
 *          Bugfix: Set mDNS to update in loop()
 *          Updated: ArduinoOTA no longer included by default
 *    1.0 - Initial release
 *
 *  Wiring:
 *      DSC Aux(+) --- 5v voltage regulator --- esp8266 development board 5v pin (NodeMCU, Wemos)
 *
 *      DSC Aux(-) --- esp8266 Ground
 *
 *                                         +--- dscClockPin (esp8266: D1, D2, D8)
 *      DSC Yellow --- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *                                         +--- dscReadPin (esp8266: D1, D2, D8)
 *      DSC Green ---- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *  Virtual keypad (optional):
 *      DSC Green ---- NPN collector --\
 *                                      |-- NPN base --- 1k ohm resistor --- dscWritePin (esp8266: D1, D2, D8)
 *            Ground --- NPN emitter --/
 *
 *  Virtual keypad uses an NPN transistor to pull the data line low - most small signal NPN transistors should
 *  be suitable, for example:
 *   -- 2N3904
 *   -- BC547, BC548, BC549
 *
 *  Issues and (especially) pull requests are welcome:
 *  https://github.com/taligentx/dscKeybusInterface
 *
 *  Many thanks to Elektrik1 for contributing this example: https://github.com/Elektrik1
 *
 *  This example code is in the public domain.
 */

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <dscKeybusInterface.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <FS.h>
#include <SPIFFS.h>
#include <SPIFFSEditor.h>
#include <ArduinoJson.h>
#include <Chrono.h>

// WiFi settings
char wifiSSID[] = "";
char wifiPassword[] = "";

// DNS settings
char dnsHostname[] = "dsc";  // Sets the domain name - if set to "dsc", access via: http://dsc.local

// Configures the Keybus interface with the specified pins - dscWritePin is
// optional, leaving it out disables the virtual keypad
#define dscClockPin D1  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
#define dscReadPin D2   // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
#define dscWritePin D8  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)


// Initialize components
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
static AsyncClient * aClient = NULL;
Chrono ws_ping_pong(Chrono::SECONDS);
bool partitionChanged;
byte viewPartition = 1;
byte ligths_sent = 0x00;
byte last_open_zones[8];
byte partition = 0;
bool force_send_status_for_new_client = false;
const char* lcdPartition = "Partition ";


void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPassword);
  while (WiFi.status() != WL_CONNECTED) delay(100);
  Serial.print(F("WiFi connected: "));
  Serial.println(WiFi.localIP());

  if (!MDNS.begin(dnsHostname)) {
    Serial.println("Error setting up MDNS responder.");
    while (1) {
      delay(1000);
    }
  }

  SPIFFS.begin();
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.print(F("Web server started: http://"));
  Serial.print(dnsHostname);
  Serial.println(F(".local"));

  dsc.begin();
  ws_ping_pong.stop();

  Serial.println(F("DSC Keybus Interface is online."));
}


void loop() {

  MDNS.update();

  //ping-pong WebSocket to keep connection open
  if (ws_ping_pong.isRunning() && ws_ping_pong.elapsed() > 5 * 60) {
    ws.pingAll();
    ws_ping_pong.restart();
  }

  if (dsc.loop() && (dsc.statusChanged || force_send_status_for_new_client)) {  // Processes data only when a valid Keybus command has been read
    dsc.statusChanged = false;  // Resets the status flag

    // If the Keybus data buffer is exceeded, the sketch is too busy to process all Keybus commands.  Call
    // loop() more often, or increase dscBufferSize in the library: src/dscKeybusInterface.h
    if (dsc.bufferOverflow) {
      Serial.println(F("Keybus buffer overflow"));
      dsc.bufferOverflow = false;
    }

    // Checks status for the currently viewed partition
    partition = viewPartition - 1;

    setLights(partition);
    setStatus(partition);

    if (dsc.fireChanged[partition]) {
      dsc.fireChanged[partition] = false;  // Resets the fire status flag
      printFire(partition);
    }

    // Zone status is stored in the openZones[] and openZonesChanged[] arrays using 1 bit per zone, up to 64 zones
    //   openZones[0] and openZonesChanged[0]: Bit 0 = Zone 1 ... Bit 7 = Zone 8
    //   openZones[1] and openZonesChanged[1]: Bit 0 = Zone 9 ... Bit 7 = Zone 16
    //   ...
    //   openZones[7] and openZonesChanged[7]: Bit 0 = Zone 57 ... Bit 7 = Zone 64
    if (dsc.openZonesStatusChanged || force_send_status_for_new_client) {
      dsc.openZonesStatusChanged = false;  // Resets the open zones status flag

      if (ws.count()) {
        char outas[512];
        StaticJsonDocument<200> doc;
        JsonObject root = doc.to<JsonObject>();
        root["open_zone_0"] = dsc.openZones[0];
        root["open_zone_1"] = dsc.openZones[1];
        root["open_zone_2"] = dsc.openZones[2];
        root["open_zone_3"] = dsc.openZones[3];
        root["open_zone_4"] = dsc.openZones[4];
        root["open_zone_5"] = dsc.openZones[5];
        root["open_zone_6"] = dsc.openZones[6];
        root["open_zone_7"] = dsc.openZones[7];
        serializeJson(root, outas);
        ws.textAll(outas);
      }
    }

    // Zone alarm status is stored in the alarmZones[] and alarmZonesChanged[] arrays using 1 bit per zone, up to 64 zones
    //   alarmZones[0] and alarmZonesChanged[0]: Bit 0 = Zone 1 ... Bit 7 = Zone 8
    //   alarmZones[1] and alarmZonesChanged[1]: Bit 0 = Zone 9 ... Bit 7 = Zone 16
    //   ...
    //   alarmZones[7] and alarmZonesChanged[7]: Bit 0 = Zone 57 ... Bit 7 = Zone 64
    if (dsc.alarmZonesStatusChanged || force_send_status_for_new_client) {
      dsc.alarmZonesStatusChanged = false;  // Resets the alarm zones status flag

      if (ws.count()) {
        char outas[512];
        StaticJsonDocument<200> doc;
        JsonObject root = doc.to<JsonObject>();
        root["alarm_zone_0"] = dsc.alarmZones[0];
        root["alarm_zone_1"] = dsc.alarmZones[1];
        root["alarm_zone_2"] = dsc.alarmZones[2];
        root["alarm_zone_3"] = dsc.alarmZones[3];
        root["alarm_zone_4"] = dsc.alarmZones[4];
        root["alarm_zone_5"] = dsc.alarmZones[5];
        root["alarm_zone_6"] = dsc.alarmZones[6];
        root["alarm_zone_7"] = dsc.alarmZones[7];
        serializeJson(root, outas);
        ws.textAll(outas);
      }
    }

    if (dsc.powerChanged) {
      dsc.powerChanged = false;  // Resets the power trouble status flag
      if (dsc.powerTrouble) {
        //        lcd.clear();
        //        lcd.print(0, 0, "Power trouble");
      }
      else {
        //        lcd.clear();
        //        lcd.print(0, 0, "Power restored");
      }
    }

    if (dsc.batteryChanged) {
      dsc.batteryChanged = false;  // Resets the battery trouble status flag
      if (dsc.batteryTrouble) {
        //        lcd.clear();
        //        lcd.print(0, 0, "Battery trouble");
      }
      else {
        //        lcd.clear();
        //        lcd.print(0, 0, "Battery restored");
      }
    }
    if (force_send_status_for_new_client) {
      force_send_status_for_new_client = false;
    }
  }
}


void changedPartition(byte partition) {
  partitionChanged = true;
  setStatus(partition);
  setLights(partition);
  partitionChanged = false;
}


void setLights(byte partition) {
  if ((dsc.lights[partition] != ligths_sent || force_send_status_for_new_client) && ws.count()) {
    char outas[128];
    StaticJsonDocument<200> doc;
    JsonObject root = doc.to<JsonObject>();
    root["status_packet"] = dsc.lights[partition];
    serializeJson(root, outas);
    ws.textAll(outas);
    ligths_sent = dsc.lights[partition];
  }
}


void setStatus(byte partition) {
  static byte lastStatus[8];
  if (!partitionChanged && dsc.status[partition] == lastStatus[partition] && !force_send_status_for_new_client) return;
  lastStatus[partition] = dsc.status[partition];

  if (ws.count()) {
    char outas[128];
    StaticJsonDocument<200> doc;
    JsonObject root = doc.to<JsonObject>();

    switch (dsc.status[partition]) {
      case 0x01: root["lcd_lower"] = "Ready"; break;
      case 0x02: root["lcd_lower"] = "Stay zones open"; break;
      case 0x03: root["lcd_lower"] = "Zones open"; break;
      case 0x04: root["lcd_lower"] = "Armed stay"; break;
      case 0x05: root["lcd_lower"] = "Armed away"; break;
      case 0x07: root["lcd_lower"] = "Failed to arm"; break;
      case 0x08: root["lcd_lower"] = "Exit delay"; break;
      case 0x09: root["lcd_lower"] = "No entry delay"; break;
      case 0x0B: root["lcd_lower"] = "Quick exit"; break;
      case 0x0C: root["lcd_lower"] = "Entry delay"; break;
      case 0x0D: root["lcd_lower"] = "Alarm memory"; break;
      case 0x10: root["lcd_lower"] = "Keypad lockout"; break;
      case 0x11: root["lcd_lower"] = "Alarm"; break;
      case 0x14: root["lcd_lower"] = "Auto-arm"; break;
      case 0x16: root["lcd_lower"] = "No entry delay"; break;
      case 0x22: root["lcd_lower"] = "Alarm memory"; break;
      case 0x33: root["lcd_lower"] = "Busy"; break;
      case 0x3D: root["lcd_lower"] = "Disarmed"; break;
      case 0x3E: root["lcd_lower"] = "Disarmed"; break;
      case 0x40: root["lcd_lower"] = "Keypad blanked"; break;
      case 0x8A: root["lcd_lower"] = "Activate zones"; break;
      case 0x8B: root["lcd_lower"] = "Quick exit"; break;
      case 0x8E: root["lcd_lower"] = "Invalid option"; break;
      case 0x8F: root["lcd_lower"] = "Invalid code"; break;
      case 0x9E: root["lcd_lower"] = "Enter * code"; break;
      case 0x9F: root["lcd_lower"] = "Access code"; break;
      case 0xA0: root["lcd_lower"] = "Zone bypass"; break;
      case 0xA1: root["lcd_lower"] = "Trouble menu"; break;
      case 0xA2: root["lcd_lower"] = "Alarm memory"; break;
      case 0xA3: root["lcd_lower"] = "Door chime on"; break;
      case 0xA4: root["lcd_lower"] = "Door chime off"; break;
      case 0xA5: root["lcd_lower"] = "Master code"; break;
      case 0xA6: root["lcd_lower"] = "Access codes"; break;
      case 0xA7: root["lcd_lower"] = "Enter new code"; break;
      case 0xA9: root["lcd_lower"] = "User function"; break;
      case 0xAA: root["lcd_lower"] = "Time and Date"; break;
      case 0xAB: root["lcd_lower"] = "Auto-arm time"; break;
      case 0xAC: root["lcd_lower"] = "Auto-arm on"; break;
      case 0xAD: root["lcd_lower"] = "Auto-arm off"; break;
      case 0xAF: root["lcd_lower"] = "System test"; break;
      case 0xB0: root["lcd_lower"] = "Enable DLS"; break;
      case 0xB2: root["lcd_lower"] = "Command output"; break;
      case 0xB7: root["lcd_lower"] = "Installer code"; break;
      case 0xB8: root["lcd_lower"] = "Enter * code"; break;
      case 0xB9: root["lcd_lower"] = "Zone tamper"; break;
      case 0xBA: root["lcd_lower"] = "Zones low batt."; break;
      case 0xC6: root["lcd_lower"] = "Zone fault menu"; break;
      case 0xC8: root["lcd_lower"] = "Service required"; break;
      case 0xD0: root["lcd_lower"] = "Keypads low batt"; break;
      case 0xD1: root["lcd_lower"] = "Wireless low bat"; break;
      case 0xE4: root["lcd_lower"] = "Installer menu"; break;
      case 0xE5: root["lcd_lower"] = "Keypad slot"; break;
      case 0xE6: root["lcd_lower"] = "Input: 2 digits"; break;
      case 0xE7: root["lcd_lower"] = "Input: 3 digits"; break;
      case 0xE8: root["lcd_lower"] = "Input: 4 digits"; break;
      case 0xEA: root["lcd_lower"] = "Code: 2 digits"; break;
      case 0xEB: root["lcd_lower"] = "Code: 4 digits"; break;
      case 0xEC: root["lcd_lower"] = "Input: 6 digits"; break;
      case 0xED: root["lcd_lower"] = "Input: 32 digits"; break;
      case 0xEE: root["lcd_lower"] = "Input: option"; break;
      case 0xF0: root["lcd_lower"] = "Function key 1"; break;
      case 0xF1: root["lcd_lower"] = "Function key 2"; break;
      case 0xF2: root["lcd_lower"] = "Function key 3"; break;
      case 0xF3: root["lcd_lower"] = "Function key 4"; break;
      case 0xF4: root["lcd_lower"] = "Function key 5"; break;
      case 0xF8: root["lcd_lower"] = "Keypad program"; break;
      default: root["lcd_lower"] = dsc.status[partition];
    }
    serializeJson(root, outas);
    ws.textAll(outas);
  }
}


void printFire(byte partition) {
  if (dsc.fire[partition]) {
    //    lcd.clear();
    //    byte position = strlen(lcdPartition);
    //    lcd.print(0, 0, lcdPartition);
    //    lcd.print(position, 0, partition + 1);
    //    lcd.print(0, 1, "Fire alarm");
  } else {
    //    lcd.clear();
    //    byte position = strlen(lcdPartition);
    //    lcd.print(0, 0, lcdPartition);
    //    lcd.print(position, 0, partition + 1);
    //    lcd.print(0, 1, "Fire alarm off");
  }
}


void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->printf("{\"connected_id\": %u}", client->id());
    force_send_status_for_new_client = true;

    client->ping();
    ws_ping_pong.restart();

  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
    if (ws.count() <= 0) {
      ws_ping_pong.stop();
    }

  } else if (type == WS_EVT_ERROR) {
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);

  } else if (type == WS_EVT_PONG) {
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char*)data : "");

  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";
    if (info->final && info->index == 0 && info->len == len) {
      //the whole message is in a single frame and we got all of it's data
      //Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);

      if (info->opcode == WS_TEXT) {
        for (size_t i = 0; i < info->len; i++) {
          msg += (char) data[i];
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if (info->opcode == WS_TEXT) {
        StaticJsonDocument<200> doc;
        auto err = deserializeJson(doc, msg);
        if (!err) {
          JsonObject root = doc.as<JsonObject>();
          if (root.containsKey("btn_single_click")) {
            char *tmp = (char *)root["btn_single_click"].as<char*>();
            char * const sep_at = strchr(tmp, '_');
            if (sep_at != NULL)            {
              *sep_at = '\0';
              dsc.write(sep_at + 1);
            }
          }
        }
      }

    } else {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if (info->index == 0) {
        if (info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);

      if (info->opcode == WS_TEXT) {
        for (size_t i = 0; i < info->len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for (size_t i = 0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if ((info->index + len) == info->len) {
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if (info->final) {
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
          if (info->message_opcode == WS_TEXT)
            client->text("I got your text message");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}
