// Escher - CNC Etch-a-Sketch
// Matt Welsh <mdw@mdw.la>
// https://www.teamsidney.com


#include <vector>
#include <Wire.h>
#include <AccelStepper.h>
#include <Adafruit_MotorShield.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "SPIFFS.h"
#include "EscherStepper.h"
#include "EscherParser.h"

// This file defines DEVICE_SECRET, WIFI_NETWORK, and WIFI_PASSWORD.
#include "EscherDeviceConfig.h"

// Define this to reverse the axes (e.g., if using gears to mate between
// the steppers and the Etch-a-Sketch).
#define REVERSE_AXES
#ifdef REVERSE_AXES
#define FORWARD_STEP BACKWARD
#define BACKWARD_STEP FORWARD
#else
#define FORWARD_STEP FORWARD
#define BACKWARD_STEP BACKWARD
#endif

// These should be calibrated for each device.
//#define BACKLASH_X 10
//#define BACKLASH_Y 15
#define BACKLASH_X 3
#define BACKLASH_Y 3
#define MAX_SPEED 100.0

Adafruit_MotorShield AFMS = Adafruit_MotorShield();
Adafruit_StepperMotor *myStepper1 = AFMS.getStepper(200, 1);
Adafruit_StepperMotor *myStepper2 = AFMS.getStepper(200, 2);
void forwardstep1() {
  myStepper1->onestep(FORWARD_STEP, SINGLE);
}
void backwardstep1() {
  myStepper1->onestep(BACKWARD_STEP, SINGLE);
}
void forwardstep2() {  
  myStepper2->onestep(FORWARD_STEP, DOUBLE);
}
void backwardstep2() {  
  myStepper2->onestep(BACKWARD_STEP, DOUBLE);
}

AccelStepper stepper1(forwardstep1, backwardstep1);
AccelStepper stepper2(forwardstep2, backwardstep2);
MultiStepper mstepper;
EscherStepper escher(mstepper, BACKLASH_X, BACKLASH_Y);
EscherParser parser(escher);

// We use some magic strings in this constant to ensure that we can easily strip it out of the binary.
const char BUILD_VERSION[] = ("__E5ch3r__ " __DATE__ " " __TIME__ " ___");

WiFiMulti wifiMulti;
HTTPClient http;
File fsUploadFile;

// Flash the LED.
void flashLed(int count) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(150);
    digitalWrite(LED_BUILTIN, LOW);
    delay(150);
  }
}

enum EtchState { STATE_INITIALIZING = 0, STATE_IDLE, STATE_READY, STATE_ETCHING, STATE_PAUSED };
EtchState etchState = STATE_INITIALIZING;

String etchStateString() {
  switch (etchState) {
    case STATE_INITIALIZING: return "initializing";
    case STATE_IDLE: return "idle";
    case STATE_READY: return "ready";
    case STATE_ETCHING: return "etching";
    case STATE_PAUSED: return "paused";
    default: return "unknown";
  }
}

// Initialization code.
void setup() {  
  Serial.begin(115200);
  Serial.printf("Starting: %s\n", BUILD_VERSION);
  pinMode(LED_BUILTIN, OUTPUT);

  // Initialize SPIFFS. We always format.
  if (!SPIFFS.format()) {
    Serial.println("Warning - SPIFFS format failed");  
  }
  if (!SPIFFS.begin(true)) {
    Serial.println("Warning - SPIFFS mount failed");
  }
  showFilesystemContents();

  stepper1.setMaxSpeed(MAX_SPEED);
  stepper2.setMaxSpeed(MAX_SPEED);
  mstepper.addStepper(stepper1);
  mstepper.addStepper(stepper2);
  AFMS.begin(); // Start the bottom shield

  // Bring up WiFi.
  wifiMulti.addAP(WIFI_NETWORK, WIFI_PASSWORD);

  Serial.println("Done with setup()");
}

// Checkin to Firebase.
void checkin() {
  Serial.print("MAC address ");
  Serial.println(WiFi.macAddress());
  Serial.print("IP address is ");
  Serial.println(WiFi.localIP().toString());

  String url = "https://firestore.googleapis.com/v1beta1/projects/team-sidney/databases/(default)/documents:commit";
  http.setTimeout(1000);
  http.addHeader("Content-Type", "application/json");
  http.begin(url);

  // With Firestore, in order to get a server-generated timestamp,
  // we need to issue a 'commit' request with two operations: an
  // update and a transform. This results in some gnarly code.

  StaticJsonDocument<1024> checkinDoc;
  JsonObject root = checkinDoc.to<JsonObject>();
  JsonArray writes = root.createNestedArray("writes");

  // First entry - the update operation.
  JsonObject first = writes.createNestedObject();
  JsonObject update = first.createNestedObject("update");

  String docName = "projects/team-sidney/databases/(default)/documents/escher/root/secret/"
                   + DEVICE_SECRET + "/devices/" + WiFi.macAddress();
  update["name"] = docName;

  // Here's the contents of the document we're writing.
  JsonObject fields = update.createNestedObject("fields");
  JsonObject buildversion = fields.createNestedObject("version");
  buildversion["stringValue"] = BUILD_VERSION;
  JsonObject status = fields.createNestedObject("status");
  status["stringValue"] = etchStateString();
  JsonObject mac = fields.createNestedObject("mac");
  mac["stringValue"] = WiFi.macAddress();
  JsonObject ip = fields.createNestedObject("ip");
  ip["stringValue"] = WiFi.localIP().toString();
  JsonObject rssi = fields.createNestedObject("rssi");
  rssi["integerValue"] = String(WiFi.RSSI());
  JsonObject backlash_x = fields.createNestedObject("backlash_x");
  backlash_x["integerValue"] = String(BACKLASH_X);
  JsonObject backlash_y = fields.createNestedObject("backlash_y");
  backlash_y["integerValue"] = String(BACKLASH_Y);

  // Second entry - the transform operation.
  JsonObject second = writes.createNestedObject();
  JsonObject transform = second.createNestedObject("transform");
  transform["document"] = docName;
  JsonArray fieldTransforms = transform.createNestedArray("fieldTransforms");
  JsonObject st = fieldTransforms.createNestedObject();
  st["fieldPath"] = "updateTime";
  // This is the magic key/value to get Firebase to generate a server timestamp.
  st["setToServerValue"] = "REQUEST_TIME";
  JsonObject currentDocument = second.createNestedObject("currentDocument");
  currentDocument["exists"] = true;

  // Now serialize it.
  String payload;
  serializeJson(root, payload);

  Serial.print("[HTTP] POST " + url + "\n");
  Serial.print(payload + "\n");

  int httpCode = http.sendRequest("POST", payload);
  if (httpCode == 200) {
    Serial.printf("[HTTP] Checkin response code: %d\n", httpCode);
  } else {
    Serial.printf("[HTTP] failed, status code %d: %s\n",
      httpCode, http.errorToString(httpCode).c_str());
  }
  http.end();

  // Now read configuration.
  readConfig();
}

// Read desired configuration from Firebase.
// XXX XXX XXX - Update this to conform to the new protocol.
void readConfig() {
  Serial.println("readConfig called");

  String url = String("https://firestore.googleapis.com/v1beta1/") +
               String("projects/team-sidney/databases/(default)/documents/escher/root/secret/") +
               DEVICE_SECRET + String("/devices/") + WiFi.macAddress() + String("/commands/etch");
  http.setTimeout(1000);
  http.addHeader("Content-Type", "application/json");
  http.begin(url);
  Serial.print("[HTTP] GET " + url + "\n");
  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.printf("[HTTP] failed, error: %s\n", http.errorToString(httpCode).c_str());
    return;
  }

  String payload = http.getString();
  Serial.printf("[HTTP] readConfig response code: %d\n", httpCode);
  Serial.println(payload);

#if 0
  // Parse JSON config.
  DeserializationError err = deserializeJson(curConfigDocument, payload);
  Serial.print("Deserialize returned: ");
  Serial.println(err.c_str());

    JsonObject cc = curConfigDocument.as<JsonObject>();
    nextConfig.numPixels = cc["numPixels"];
    if (nextConfig.numPixels == 0) {
      nextConfig.numPixels = NUMPIXELS;
    }
    nextConfig.dataPin = cc["dataPin"];
    if (nextConfig.dataPin == 0) {
      nextConfig.dataPin = DEFAULT_DATA_PIN;
    }
    nextConfig.clockPin = cc["clockPin"];
    if (nextConfig.clockPin == 0) {
      nextConfig.clockPin = DEFAULT_CLOCK_PIN;
    }
    memcpy(nextConfig.mode, (const char *)cc["mode"], sizeof(nextConfig.mode));
    nextConfig.enabled = (cc["enabled"] == true);
    nextConfig.speed = cc["speed"];
    nextConfig.brightness = cc["brightness"];
    nextConfig.colorChange = cc["colorChange"];
    nextConfig.color1 = strip->Color(cc["red"], cc["green"], cc["blue"]);
    nextConfig.color2 = strip->Color(cc["red2"], cc["green2"], cc["blue2"]);
    memcpy(nextConfig.firmwareVersion, (const char *)cc["version"], sizeof(nextConfig.firmwareVersion));

    // If the firmware version needs to be updated, kick off the update.
    if (strcmp(nextConfig.firmwareVersion, BUILD_VERSION) &&
        strcmp(nextConfig.firmwareVersion, "none") &&
        strcmp(nextConfig.firmwareVersion, "") &&
        strcmp(nextConfig.firmwareVersion, "current")) {
      Serial.printf("readConfig: next firmware version %s triggering update\n", nextConfig.firmwareVersion);
      needsFirmwareUpdate = true;
    }

    xSemaphoreGive(configMutex);
  } else {
    Serial.println("Warning - readConfig() unable to get config mutex");
  }
#endif
  http.end();
}


// Dump debug information on the filesystem contents.
void showFilesystemContents() {
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String fileName = file.name();
    size_t fileSize = file.size();
    Serial.printf("FS file %s, size: %d\n", fileName.c_str(), fileSize);
    file = root.openNextFile();
  }
}

#if 0
// XXX XXX - EXAMPLE CODE ONLY. Use this as basis for fetching
// gcode document from Firebase into SPIFFS.
void handleUpload() {
  Serial.println("handleUpload called");
  if (etchState == STATE_ETCHING) {
    Serial.println("Warning - cannot accept upload while etching.");
    return;
  }

  HTTPUpload& upload = server.upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    Serial.printf("handleUpload filename: %s\n", filename.c_str());
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
    
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Serial.printf("handleUpload received %d bytes\n", upload.currentSize);
    if (fsUploadFile) {
      fsUploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile) {
      fsUploadFile.close();
    }
    Serial.printf("handleUpload completed write of %d bytes\n", upload.totalSize);
  }
}


// XXX XXX - EXAMPLE CODE ONLY. Use this as basis for initializing
// etching.
void handleEtch() {
  Serial.println("handleEtch called");
  server.sendHeader("Access-Control-Allow-Origin", "*"); // Permit CORS.

  // First check that we are ready.
  if (!(etchState == STATE_READY)) {
    server.send(500, "text/plain", "State must be ready to start etching");
  }

  if (parser.Open("/cmddata.txt")) {
    server.send(200, "text/plain", "Etching started.");
    stepper1.setCurrentPosition(0);
    stepper2.setCurrentPosition(0);
    stepper1.enableOutputs();
    stepper2.enableOutputs();
    etchState = STATE_ETCHING;
  } else {
    server.send(500, "text/plain", "No cmddata.txt found -- use /upload first");
    etchState = STATE_IDLE;
  }
}
#endif

// Run the Etcher.
bool runEtcher() {
  // First see if the Escher controller is ready for more.
  if (escher.run()) {
    return true;
  }

  // Feed more commands to Escher. Returns false when file is complete.
  return parser.Feed();
}

unsigned long lastCheckin = 0;
unsigned long lastBlink = 0;
String gcodeUrl = "";
String gcodeHash = "";

// Main loop.
void loop() {
  if (etchState == STATE_INITIALIZING) {
    if (wifiMulti.run() != WL_CONNECTED) {
      Serial.println("Waiting for WiFi connection...");
      delay(1000);
      return;
    } else {
      Serial.println("WiFi initialized, setting state to idle.");
      etchState = STATE_IDLE;
    }
    
  } else if (etchState == STATE_IDLE || etchState == STATE_READY || etchState == STATE_PAUSED) {
    // Do periodic checkins.
    if (millis() - lastCheckin >= 10000) {
      lastCheckin = millis();
      checkin();
    }
     
  } else if (etchState == STATE_ETCHING) {
    // Avoid doing checkins; only run etcher until done.
    if (!runEtcher()) {
      Serial.println("Etcher completed.");
      myStepper1->release();
      myStepper2->release();
      stepper1.disableOutputs();
      stepper2.disableOutputs();
      etchState = STATE_IDLE;
    }
  }
}

// XXX XXX XXX TODO(mdw)
// 1. Update checkin() to conform to the new protocol.
// 2. Update readConfig() to use the new command protocol. Get rid of all ideas of stop/pause/resume.
// 3. When readConfig() gets a command to start etching, write code to fetch gcode from the given url
//    and store it in SPIFFS.
// 4. Modify EscherParser to parse gcode directly.



