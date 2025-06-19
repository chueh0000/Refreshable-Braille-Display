#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h> // Include the FS library
#include <ArduinoJson.h> // Include for JSON parsing/serialization

// Replace with your network credentials
const char* ssid = "R207";
const char* password = "33665107";

// Shift Register Pins (connected to ESP32)
const int DS_PIN = 32;   // Data Pin (DS)
const int SHCP_PIN = 33; // Shift Register Clock Pin (SHCP)
const int STCP_PIN = 25;  // Storage Register Clock Pin (STCP / Latch)

// Define the number of shift registers and outputs
const int NUM_SHIFT_REGISTERS = 2; // For 6 electromagnets (2 inputs each) = 12 outputs
const int NUM_SR_OUTPUTS = NUM_SHIFT_REGISTERS * 8; // Total outputs from shift registers

// Array to hold the state of all shift register outputs
byte shiftRegisterOutputs[NUM_SHIFT_REGISTERS];

// Structure to hold electromagnet state
struct Electromagnet {
  int input1_sr_pin; // Shift register output pin for L298N IN1
  int input2_sr_pin; // Shift register output pin for L298N IN2
  bool isOn;
  bool isReversed; // Indicates intended polarity when ON
  unsigned long turnOffTime; // For duration control (when ON)
  int durationMs; // Duration in milliseconds, 0 for continuous
  bool isReversingOff; // New: true if currently applying reverse current for turn-off
  unsigned long reverseOffEndTime; // New: time when reverse current should stop
};

// Define 6 electromagnets. Map them to shift register output pins.
// SR_PIN refers to the 0-15 index of the combined shift register outputs.
// Example: SR_PIN 0 is Q0 of SR1, SR_PIN 8 is Q0 of SR2
Electromagnet electromagnets[] = {
  {0, 1, false, false, 0, 0, false, 0}, // EM1: SR_OUT_0, SR_OUT_1
  {2, 3, false, false, 0, 0, false, 0}, // EM2: SR_OUT_2, SR_OUT_3
  {4, 5, false, false, 0, 0, false, 0}, // EM3: SR_OUT_4, SR_OUT_5
  {6, 7, false, false, 0, 0, false, 0}, // EM4: SR_OUT_6, SR_OUT_7
  {8, 9, false, false, 0, 0, false, 0}, // EM5: SR_OUT_8, SR_OUT_9 (from second SR)
  {10, 11, false, false, 0, 0, false, 0} // EM6: SR_OUT_10, SR_OUT_11 (from second SR)
};
const int NUM_ELECTROMAGNETS = sizeof(electromagnets) / sizeof(electromagnets[0]);

// --- Pattern Editor Variables ---
// A frame will consist of a simple array of (isOn, isReversed) for each electromagnet
struct PatternEMState {
    bool isOn;
    bool isReversed;
};
// A pattern frame now includes its own duration
struct PatternFrame {
    std::vector<PatternEMState> emStates;
    int duration; // Duration for this specific frame
};
// A pattern is a vector of PatternFrame objects.
std::vector<PatternFrame> currentLoadedPattern;
bool isPatternPlaying = false;
unsigned long lastFrameChangeTime = 0;
int currentPatternFrameIndex = 0;
// patternFrameDuration is no longer global, it's per-frame,
// but the 'play' command needs to re-initialize playback.

// Create an AsyncWebServer object on port 80
AsyncWebServer server(80);

// Function to write a byte array to the shift registers
void writeShiftRegisters() {
  digitalWrite(STCP_PIN, LOW); // Latch LOW to accept data
  for (int i = NUM_SHIFT_REGISTERS - 1; i >= 0; i--) { // Write from last SR to first
    shiftOut(DS_PIN, SHCP_PIN, MSBFIRST, shiftRegisterOutputs[i]);
  }
  digitalWrite(STCP_PIN, HIGH); // Latch HIGH to output data
}

// Function to set a specific output pin on the shift register
void setShiftRegisterPin(int sr_pin_index, bool state) {
  int sr_index = sr_pin_index / 8; // Which shift register
  int bit_index = sr_pin_index % 8; // Which bit within that shift register

  if (sr_index >= 0 && sr_index < NUM_SHIFT_REGISTERS) {
    if (state) {
      shiftRegisterOutputs[sr_index] |= (1 << bit_index); // Set bit
    } else {
      shiftRegisterOutputs[sr_index] &= ~(1 << bit_index); // Clear bit
    }
  }
}

// Function to update the L298N inputs based on electromagnet state
void updateElectromagnet(int emIndex) {
  Electromagnet& em = electromagnets[emIndex];
  Serial.print("DEBUG: updateElectromagnet(EM "); Serial.print(emIndex + 1);
  Serial.print(") - isOn: "); Serial.print(em.isOn ? "true" : "false");
  Serial.print(", isReversed: "); Serial.print(em.isReversed ? "true" : "false"); // This is the ON polarity
  Serial.print(", isReversingOff: "); Serial.println(em.isReversingOff ? "true" : "false");

  if (em.isOn) { // If the electromagnet is intended to be ON
    em.isReversingOff = false; // Ensure it's not in reversing state
    if (em.isReversed) { // If ON is reversed
      setShiftRegisterPin(em.input1_sr_pin, LOW);
      setShiftRegisterPin(em.input2_sr_pin, HIGH);
      Serial.print("DEBUG: EM "); Serial.print(emIndex + 1); Serial.println(" ON - Reversed (IN1:LOW, IN2:HIGH)");
    } else { // If ON is normal
      setShiftRegisterPin(em.input1_sr_pin, HIGH);
      setShiftRegisterPin(em.input2_sr_pin, LOW);
      Serial.print("DEBUG: EM "); Serial.print(emIndex + 1); Serial.println(" ON - Normal (IN1:HIGH, IN2:LOW)");
    }
  } else if (em.isReversingOff) { // If currently applying reverse current to turn off
    // Apply the reverse of the 'isReversed' polarity for the short pulse
    if (em.isReversed) { // If intended ON state is reversed, reverse pulse should be normal (IN1=HIGH, IN2=LOW)
      setShiftRegisterPin(em.input1_sr_pin, HIGH);
      setShiftRegisterPin(em.input2_sr_pin, LOW);
      Serial.print("DEBUG: EM "); Serial.print(emIndex + 1); Serial.println(" REVERSING OFF - Normal Pulse (IN1:HIGH, IN2:LOW)");
    } else { // If intended ON state is normal, reverse pulse should be reversed (IN1=LOW, IN2=HIGH)
      setShiftRegisterPin(em.input1_sr_pin, LOW);
      setShiftRegisterPin(em.input2_sr_pin, HIGH);
      Serial.print("DEBUG: EM "); Serial.print(emIndex + 1); Serial.println(" REVERSING OFF - Reversed Pulse (IN1:LOW, IN2:HIGH)");
    }
  }
  else { // Fully OFF (neither isOn nor isReversingOff)
    setShiftRegisterPin(em.input1_sr_pin, LOW);
    setShiftRegisterPin(em.input2_sr_pin, LOW);
    Serial.print("DEBUG: EM "); Serial.print(emIndex + 1); Serial.println(" FULLY OFF (IN1:LOW, IN2:LOW)");
  }
  writeShiftRegisters(); // Update the shift registers
}

// Function to apply a pattern frame to electromagnets
void applyPatternFrame(int frameIndex) {
    if (frameIndex < 0 || frameIndex >= currentLoadedPattern.size()) {
        Serial.println("ERROR: Invalid pattern frame index.");
        return;
    }
    Serial.print("DEBUG: Applying pattern frame: "); Serial.println(frameIndex);
    const auto& frameData = currentLoadedPattern[frameIndex]; // Get the whole frame data

    for (int i = 0; i < NUM_ELECTROMAGNETS; ++i) {
        if (i < frameData.emStates.size()) { // Ensure index is within bounds of frame data
            electromagnets[i].isOn = frameData.emStates[i].isOn;
            electromagnets[i].isReversed = frameData.emStates[i].isReversed;
            electromagnets[i].durationMs = 0; // Pattern frames are momentary, not duration controlled individually
            electromagnets[i].isReversingOff = false; // Not reversing off during pattern play
            electromagnets[i].turnOffTime = 0; // Not duration controlled
            updateElectromagnet(i);
        } else {
            // Handle case where pattern frame might have fewer EMs than total, e.g., turn off excess
            electromagnets[i].isOn = false;
            electromagnets[i].isReversed = false;
            electromagnets[i].durationMs = 0;
            electromagnets[i].isReversingOff = false;
            electromagnets[i].turnOffTime = 0;
            updateElectromagnet(i);
        }
    }
}


void setup() {
  Serial.begin(115200);

  // Set shift register control pins as OUTPUT
  pinMode(DS_PIN, OUTPUT);
  pinMode(SHCP_PIN, OUTPUT);
  pinMode(STCP_PIN, OUTPUT);

  // Initialize all shift register outputs to LOW
  for (int i = 0; i < NUM_SHIFT_REGISTERS; i++) {
    shiftRegisterOutputs[i] = 0;
  }
  writeShiftRegisters(); // Apply initial state

  // Initialize FS
  if (!LittleFS.begin(true)) {
    Serial.println("An Error has occurred while mounting FS");
    return;
  }
  Serial.println("SPIFFS mounted successfully");


  // Connect to Wi-Fi
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Serve static files
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // Handle control requests for individual electromagnets
  server.on("/control", HTTP_GET, [](AsyncWebServerRequest *request){
    // Stop pattern playback if individual control is used
    isPatternPlaying = false; 

    if (request->hasArg("index") && request->hasArg("state")) { // duration and reversed are optional for some states
      int index = request->arg("index").toInt();
      String state = request->arg("state");
      
      bool reversedFromClient = false; // This parameter is now only used by "on" state to set polarity directly
      if (request->hasArg("reversed")) {
          reversedFromClient = request->arg("reversed") == "true";
      }

      int duration = 0;
      if (request->hasArg("duration")) {
          duration = request->arg("duration").toInt();
      }

      if (index >= 0 && index < NUM_ELECTROMAGNETS) {
        Electromagnet& em = electromagnets[index];
        em.durationMs = duration; // Always update duration with what client sends

        if (state == "on") {
          Serial.print("API Call: EM "); Serial.print(index + 1); Serial.println(" - State ON received.");
          em.isOn = true;
          em.isReversingOff = false; // Ensure not in reversing state
          // When turning ON, we use the polarity provided by the client (which should be the desired ON polarity)
          em.isReversed = reversedFromClient; 
          if (em.durationMs > 0) {
            em.turnOffTime = millis() + em.durationMs;
          } else {
            em.turnOffTime = 0; // Continuous
          }
          Serial.print("EM "); Serial.print(index + 1); Serial.print(" ON, Polarity: "); Serial.print(em.isReversed ? "Reversed" : "Normal"); Serial.print(", Duration: "); Serial.print(em.durationMs); Serial.println("ms");
        } else if (state == "off") { // When turning OFF
          Serial.print("API Call: EM "); Serial.print(index + 1); Serial.println(" - State OFF received.");
          // IMPORTANT: Do NOT update em.isReversed here.
          // It must retain its last set ON polarity so updateElectromagnet can apply the *opposite* for the pulse.
          em.isOn = false;
          em.turnOffTime = 0; // Cancel any pending turn-off
          em.isReversingOff = true; // Initiate reverse current for 1 sec
          em.reverseOffEndTime = millis() + 100; // 0.1 second (100 ms)
          Serial.print("EM "); Serial.print(index + 1); Serial.println(" initiating OFF with reverse current.");
        } else if (state == "toggle_polarity") { // This is the only place client explicitly toggles polarity
            Serial.print("API Call: EM "); Serial.print(index + 1); Serial.println(" - Toggle Polarity received.");
            em.isReversed = !em.isReversed; // Toggle the stored polarity state
            Serial.print("EM "); Serial.print(index + 1); Serial.print(" polarity toggled to: "); Serial.println(em.isReversed ? "Reversed" : "Normal");
            // If the electromagnet is ON, re-evaluate its turn-off time with the new polarity
            if (em.isOn) {
                if (em.durationMs > 0) {
                    em.turnOffTime = millis() + em.durationMs;
                } else {
                    em.turnOffTime = 0; // Continuous
                }
            }
        }
        else {
          request->send(400, "text/plain", "Invalid state parameter");
          Serial.print("API Call: EM "); Serial.print(index + 1); Serial.println(" - Invalid state parameter.");
          return;
        }
        updateElectromagnet(index); // Apply changes to shift register immediately

        // Send JSON response with updated state
        String jsonResponse = "{";
        jsonResponse += "\"index\": " + String(index) + ",";
        jsonResponse += "\"isOn\": " + String(em.isOn ? "true" : "false") + ",";
        jsonResponse += "\"isReversed\": " + String(em.isReversed ? "true" : "false") + ","; // Send actual em.isReversed
        jsonResponse += "\"durationMs\": " + String(em.durationMs) + ",";
        jsonResponse += "\"isReversingOff\": " + String(em.isReversingOff ? "true" : "false"); // Include new state
        jsonResponse += "}";
        request->send(200, "application/json", jsonResponse);

      } else {
        request->send(400, "text/plain", "Invalid electromagnet index");
        Serial.println("Invalid electromagnet index: " + String(index));
      }
    } else {
      request->send(400, "text/plain", "Missing parameters");
      Serial.println("API Call: Missing parameters.");
    }
  });

  // New endpoint to provide initial electromagnet states to the client
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest *request){
    String jsonArray = "[";
    for (int i = 0; i < NUM_ELECTROMAGNETS; i++) {
      jsonArray += "{";
      jsonArray += "\"index\":" + String(i) + ",";
      jsonArray += "\"isOn\":" + String(electromagnets[i].isOn ? "true" : "false") + ",";
      jsonArray += "\"isReversed\":" + String(electromagnets[i].isReversed ? "true" : "false") + ",";
      jsonArray += "\"durationMs\":" + String(electromagnets[i].durationMs) + ",";
      jsonArray += "\"isReversingOff\": " + String(electromagnets[i].isReversingOff ? "true" : "false"); // Include new state
      jsonArray += "}";
      if (i < NUM_ELECTROMAGNETS - 1) {
        jsonArray += ",";
      }
    }
    jsonArray += "]";
    request->send(200, "application/json", jsonArray);
  });

  // --- New Pattern Editor Endpoints ---
  server.on("/pattern", HTTP_GET, [](AsyncWebServerRequest *request){
    String action = request->arg("action");

    if (action == "save") {
      String patternData = request->arg("data");
      
      Serial.print("Received pattern save request. Data length: "); Serial.println(patternData.length());

      // Attempt to parse JSON
      DynamicJsonDocument doc(patternData.length() * 2); // Heuristic: allocate twice the data length for safety. Adjust as needed.
      DeserializationError error = deserializeJson(doc, patternData);

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        request->send(400, "text/plain", "Invalid JSON data for pattern save.");
        return;
      }

      currentLoadedPattern.clear(); // Clear previous pattern
      JsonArray patternArray = doc.as<JsonArray>();

      for (JsonVariant frameVariant : patternArray) {
        JsonObject frameObj = frameVariant.as<JsonObject>();
        PatternFrame newFrame;
        newFrame.duration = frameObj["duration"].as<int>(); // Extract frame duration

        JsonArray emStatesArray = frameObj["emStates"].as<JsonArray>();
        for (JsonVariant emStateVariant : emStatesArray) {
          JsonObject emStateObj = emStateVariant.as<JsonObject>();
          PatternEMState emState;
          emState.isOn = emStateObj["isOn"].as<bool>();
          emState.isReversed = emStateObj["isReversed"].as<bool>();
          newFrame.emStates.push_back(emState);
        }
        currentLoadedPattern.push_back(newFrame);
      }
      Serial.print("Pattern saved. Frames: "); Serial.println(currentLoadedPattern.size());
      request->send(200, "text/plain", "Pattern saved successfully.");

    } else if (action == "load") {
      // Create JSON to send back the pattern
      DynamicJsonDocument doc(currentLoadedPattern.size() * (NUM_ELECTROMAGNETS * 20 + 30) + 50); // Heuristic for size
      JsonArray patternArray = doc.to<JsonArray>();

      for (const auto& frame : currentLoadedPattern) {
        JsonObject frameObj = patternArray.add<JsonObject>();
        frameObj["duration"] = frame.duration; // Add frame duration
        JsonArray emStatesArray = frameObj.createNestedArray("emStates");
        for (const auto& emState : frame.emStates) {
          JsonObject emStateObj = emStatesArray.add<JsonObject>();
          emStateObj["isOn"] = emState.isOn;
          emStateObj["isReversed"] = emState.isReversed;
        }
      }

      String jsonResponse;
      serializeJson(doc, jsonResponse);

      Serial.print("Pattern load request. Sending frames: "); Serial.println(currentLoadedPattern.size());
      request->send(200, "application/json", jsonResponse); // Send just the pattern array

    } else if (action == "play") {
        if (currentLoadedPattern.empty()) {
            request->send(400, "text/plain", "No pattern to play.");
            return;
        }
        // Stop any ongoing individual electromagnet duration or reverse pulses
        for(int i = 0; i < NUM_ELECTROMAGNETS; ++i) {
            electromagnets[i].isOn = false;
            electromagnets[i].durationMs = 0;
            electromagnets[i].isReversingOff = false;
            electromagnets[i].turnOffTime = 0;
            electromagnets[i].reverseOffEndTime = 0;
            updateElectromagnet(i); // Ensure they are physically off or clear pending operations
        }

        isPatternPlaying = true;
        currentPatternFrameIndex = 0;
        lastFrameChangeTime = millis();
        applyPatternFrame(currentPatternFrameIndex); // Apply first frame immediately
        Serial.println("Pattern playback started.");
        request->send(200, "text/plain", "Pattern playing.");
    } else if (action == "stop") {
        isPatternPlaying = false;
        // Turn off all electromagnets when pattern stops
        for (int i = 0; i < NUM_ELECTROMAGNETS; ++i) {
            electromagnets[i].isOn = false;
            electromagnets[i].isReversed = false; // Reset to default polarity when off
            electromagnets[i].durationMs = 0;
            electromagnets[i].isReversingOff = false;
            electromagnets[i].turnOffTime = 0;
            electromagnets[i].reverseOffEndTime = 0; // Clear any pending pulse end time
            updateElectromagnet(i);
        }
        Serial.println("Pattern playback stopped.");
        request->send(200, "text/plain", "Pattern stopped.");
    } else {
      request->send(400, "text/plain", "Invalid pattern action.");
    }
  });

  // Endpoint for IP address
  server.on("/ip", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", WiFi.localIP().toString());
  });

  // Start the server
  server.begin();
  Serial.println("HTTP Async server started");
}

void loop() {
  // Check for electromagnets that need to be turned off after a duration (individual control)
  // Only handle individual durations if pattern is not playing
  if (!isPatternPlaying) {
      for (int i = 0; i < NUM_ELECTROMAGNETS; i++) {
        Electromagnet& em = electromagnets[i];
        if (em.isOn && em.durationMs > 0 && millis() >= em.turnOffTime) {
          Serial.print("DEBUG: EM "); Serial.print(i + 1); Serial.println(" - Duration elapsed, setting to off and initiating reverse.");
          em.isOn = false;
          em.turnOffTime = 0;
          // When turning off due to duration, initiate reverse current
          em.isReversingOff = true; 
          em.reverseOffEndTime = millis() + 100; // 0.1 second (100 ms)
          updateElectromagnet(i);
        }

        // Check for reverse-off pulse completion
        if (!em.isOn && em.isReversingOff && millis() >= em.reverseOffEndTime) {
          Serial.print("DEBUG: EM "); Serial.print(i + 1); Serial.println(" - Reverse pulse ended, fully turning off.");
          em.isReversingOff = false; // Stop reverse current
          em.durationMs = 0; // IMPORTANT: Reset duration here when fully off
          updateElectromagnet(i); // Turn off completely
        }
      }
  }

  // Pattern playback logic
  if (isPatternPlaying && !currentLoadedPattern.empty()) {
      const auto& currentFrame = currentLoadedPattern[currentPatternFrameIndex];
      // Use the duration specific to the current frame
      if (millis() - lastFrameChangeTime >= currentFrame.duration) { 
          currentPatternFrameIndex++;
          if (currentPatternFrameIndex >= currentLoadedPattern.size()) {
              currentPatternFrameIndex = 0; // Loop pattern
          }
          applyPatternFrame(currentPatternFrameIndex); // Apply the next frame
          lastFrameChangeTime = millis();
      }
  }
}
