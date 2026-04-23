//Copyright (C) 2025 [battiemme]

//This program is free software: you can redistribute it and/or modify
//it under the terms of the GNU Affero General Public License as
//published by the Free Software Foundation, either version 3 of the
//License, or (at your option) any later version.

//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU Affero General Public License for more details.

//You should have received a copy of the GNU Affero General Public License
//along with this program.  If not, see <https://www.gnu.org/licenses/>.

//V2 Added dual MIDI track selection and INMP441 microphone support for note recognition (BETA)

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <driver/i2s_std.h>
#include <driver/i2s_types.h>
#include <driver/gpio.h>
#include <math.h>

i2s_chan_handle_t i2s_rx_handle = NULL;

// LED Configuration
#define LED_PIN     2
#define NUM_LEDS    72
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define MAX_NOTES_PER_TRACK 1200

// I2S Microphone Pins (INMP441)
#define I2S_SD      GPIO_NUM_33  // DATA
#define I2S_WS      GPIO_NUM_25  // WORD SELECT (Clock)
#define I2S_SCK     GPIO_NUM_32  // BIT CLOCK
#define I2S_PORT    I2S_NUM_0

// Audio Processing Constants
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512
#define FFT_SIZE 256

CRGB leds[NUM_LEDS];

// WiFi Configuration
const char* ssid = "Pianethor";
const char* password = "12345678";

WebServer server(80);

// Global Variables
uint8_t globalBrightness = 128;
bool isPlaying = false;
bool isPaused = false;
bool microphoneEnabled = false;
bool awaitingSound = false;
bool debugMode = false;
float playbackSpeed = 1.0; // Default speed (100%). At 100% each MIDI tick lasts exactly as it should
unsigned long playStartTime = 0;
unsigned long pauseTime = 0;
unsigned long totalPauseTime = 0;
int currentNoteIndex = 0;
String loadedFileName = "";
unsigned long noteOffDuration = 50; // Tempo in ms che una nota deve stare spenta prima di poter essere ripresa
bool isResumeFromSlider = false;  // Track if we are resuming from a slider
unsigned long bootSessionId = 0; // unique per device boot, used to key ephemeral UI state on client

// Volume detection variables
float volumeThreshold = 300.0; // Maximum volume threshold required to trigger next note
float minimumVolumeThreshold = 100.0; // Minimum volume threshold - volume must drop below this before rising above volumeThreshold to advance
float calibrationPeak = 0.0;
float calibration05s = 0.0;
float calibration1s = 0.0;
float calibration2s = 0.0;
bool isCalibrating = false;
unsigned long calibrationStartTime = 0;
unsigned long thresholdTimeout = 300; // Timeout in milliseconds before detecting next threshold cross (reduced for better responsiveness)
unsigned long lastThresholdDetectionTime = 0; // Track last time we detected threshold crossing
bool microphoneFirstNoteShown = false;  // Track if first note has been shown for microphone
unsigned long lastProcessedNoteGroup = ULONG_MAX;  // Track the last note group we processed to avoid re-processing
unsigned long lastNoteEndTime = 0;  // Track when the last note group ended to prevent overlapping detections
bool volumeBelowMinimumThreshold = false; // Track if volume has dropped below minimum threshold since last note trigger

// Loading state
bool isLoading = false;
int loadingProgress = 0;

// Default Colors
CRGB leftHandColor = CRGB::Red;
CRGB rightHandColor = CRGB::Blue;

// Note Structure
struct Note {
  int ledIndex;
  bool isLeftHand;
  unsigned long startTime;
  unsigned long duration;
  int midiNote;
  int velocity;
  int trackSource;
};

// Track Info Structure
struct TrackInfo {
  int trackIndex;
  String trackName;
  int noteCount;
  int minNote;
  int maxNote;
  bool hasNotes;
  int programNumber;  // Program Change number per determinare lo strumento
};

int selectedTrackIndex2 = -1;
int trackTranspose1 = 0;
int trackTranspose2 = 0;

std::vector<Note> notes;
std::vector<int> activeLeds;
std::vector<unsigned long> ledEndTimes;
std::vector<unsigned long> ledStartTimes;
std::vector<unsigned long> ledOffTimes;  // Track when an LED was turned off for noteOffDuration
std::vector<TrackInfo> availableTracks;

bool hasLoadedFile = false;
bool hasAnalyzedTracks = false;
int selectedTrackIndex = -1;

// MIDI Configuration
int splitPoint = 60;
float beatsPerMinute = 120.0;
int ticksPerQuarter = 480;
float msPerTick = (60000.0 / beatsPerMinute) / ticksPerQuarter;

// Function prototypes
void startupAnimation();
void updateActiveLeds();
void clearActiveLeds();
void testSingleLed(int ledIndex);
void clearAllLeds();
String getMainPage();
void handleFileUpload();
void analyzeMIDITracks();
void processMusicXMLFile(File& file);
void initMicrophone();
float detectVolume();
void playMusic();
void setupWebServer();
void saveNotesToSPIFFS();
void loadNotesFromSPIFFS();
void printMemoryInfo();
void processSelectedTrack(File& file, int targetTrack, int trackSlot = 1);
void audioProcessingTask(void *parameter);
float detectPitch();
int pitchToMIDI(float frequency);
void highlightNoteWithFeedback(int correctNote);

void setup() {
  Serial.begin(115200);
  
  // Wait for stabilization
  delay(1000);
  // create a boot-unique session id (changes on device reboot)
  bootSessionId = millis();
  
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(globalBrightness);
  
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization error");
    return;
  }
  
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  
  setupWebServer();
  server.begin();
  Serial.println("Server started");
  
  activeLeds.resize(NUM_LEDS, -1);
  ledEndTimes.resize(NUM_LEDS, 0);
  ledStartTimes.resize(NUM_LEDS, 0);
  ledOffTimes.resize(NUM_LEDS, 0);
  
  startupAnimation();
  
  Serial.printf("Free memory at startup: %zu bytes\n", ESP.getFreeHeap());
  
  // Don't load file automatically if it takes too much memory
  if (SPIFFS.exists("/music.json")) {
    File file = SPIFFS.open("/music.json", "r");
    if (file) {
      size_t fileSize = file.size();
      file.close();
      Serial.printf("music.json size: %zu bytes\n", fileSize);
      
      // Check if there's enough heap
      if (fileSize > 50000 || ESP.getFreeHeap() < 80000) {
        Serial.println("music.json too large or insufficient memory, deleting...");
        SPIFFS.remove("/music.json");
        return;
      }
    }
    loadNotesFromSPIFFS();
  }
  
  Serial.printf("Free heap after setup: %zu bytes\n", ESP.getFreeHeap());
}


void loop() {
  server.handleClient();
  
  if (isPlaying && !isPaused && hasLoadedFile) {
    playMusic();
  }
  
  updateActiveLeds();
  FastLED.show();
  delay(10);
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getMainPage());
  });
  
  server.on("/setBrightness", HTTP_POST, []() {
    if (server.hasArg("brightness")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      globalBrightness = constrain(server.arg("brightness").toInt(), 0, 255);
      FastLED.setBrightness(globalBrightness);
      FastLED.show();
      if (debugMode) Serial.println("Pressed button Brightness Control : BRIGHTNESS");
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing brightness parameter");
    }
  });
  
  server.on("/setNoteOffDuration", HTTP_POST, []() {
    if (server.hasArg("duration")) {
      noteOffDuration = server.arg("duration").toInt();
      if (debugMode) Serial.printf("Note Off Duration set to: %lu ms\n", noteOffDuration);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing duration parameter");
    }
  });
  
  server.on("/setMicrophone", HTTP_POST, []() {
    if (server.hasArg("enabled")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      microphoneEnabled = server.arg("enabled") == "true";
      if (microphoneEnabled) {
        initMicrophone();
        server.send(200, "text/plain", "Microphone enabled");
      } else {
        server.send(200, "text/plain", "Microphone disabled");
      }
    } else {
      server.send(400, "text/plain", "Missing enabled parameter");
    }
  });

  server.on("/startCalibration", HTTP_POST, []() {
    if (microphoneEnabled) {
      isCalibrating = true;
      calibrationStartTime = millis();
      calibrationPeak = 0.0;
      calibration05s = 0.0;
      calibration1s = 0.0;
      calibration2s = 0.0;
      server.send(200, "text/plain", "Calibration started");
    } else {
      server.send(400, "text/plain", "Microphone not enabled");
    }
  });

  server.on("/getCalibration", HTTP_GET, []() {
    DynamicJsonDocument doc(256);
    doc["peak"] = calibrationPeak;
    doc["s05"] = calibration05s;
    doc["s1"] = calibration1s;
    doc["s2"] = calibration2s;
    doc["isCalibrating"] = isCalibrating;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  server.on("/setThreshold", HTTP_POST, []() {
    if (server.hasArg("threshold")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      volumeThreshold = server.arg("threshold").toFloat();
      server.send(200, "text/plain", "Threshold updated");
    } else {
      server.send(400, "text/plain", "Missing threshold parameter");
    }
  });

  server.on("/setMinimumVolumeThreshold", HTTP_POST, []() {
    if (server.hasArg("minimumThreshold")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      minimumVolumeThreshold = server.arg("minimumThreshold").toFloat();
      server.send(200, "text/plain", "Minimum Volume Threshold updated");
    } else {
      server.send(400, "text/plain", "Missing minimumThreshold parameter");
    }
  });
  
  server.on("/setThresholdTimeout", HTTP_POST, []() {
    if (server.hasArg("timeout")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      thresholdTimeout = server.arg("timeout").toInt();
      if (debugMode) Serial.printf("Threshold timeout set to: %lu ms\n", thresholdTimeout);
      server.send(200, "text/plain", "Threshold timeout updated");
    } else {
      server.send(400, "text/plain", "Missing timeout parameter");
    }
  });
  
  server.on("/setPosition", HTTP_POST, []() {
    if (server.hasArg("position") && hasLoadedFile && notes.size() > 0) {
      float position = server.arg("position").toFloat() / 100.0;
      currentNoteIndex = position * notes.size();
      if (currentNoteIndex >= notes.size()) currentNoteIndex = notes.size() - 1;
      
      // Automatically pause when using the slider
      bool wasPlaying = isPlaying && !isPaused;
      if (wasPlaying) {
        isPaused = true;
        pauseTime = millis();
      }
      
      // Spegni tutti i LED quando si usa lo slider
      clearAllLeds();
      clearActiveLeds();
      
      // Segna che stiamo riprendendo da uno slider
      isResumeFromSlider = true;
      
      server.send(200, "text/plain", "Position updated");
    } else {
      server.send(400, "text/plain", "Missing parameters or no file loaded");
    }
  });
  
  server.on("/testLed", HTTP_POST, []() {
    if (server.hasArg("index")) {
      int ledIndex = server.arg("index").toInt();
      if (debugMode) Serial.printf("Pressed button Individual LED Test : %d\n", ledIndex);
      testSingleLed(ledIndex);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing index parameter");
    }
  });
  
  server.on("/play", HTTP_POST, []() {
    if (hasLoadedFile) {
      if (debugMode) Serial.println("Pressed button Playback Controls : PLAY");
      if (!isPlaying) {
        playStartTime = millis();
        totalPauseTime = 0;
        awaitingSound = false;
        currentNoteIndex = 0;
        lastThresholdDetectionTime = 0;
        microphoneFirstNoteShown = false;
        volumeBelowMinimumThreshold = false; // Reset minimum volume threshold flag for new playback
        
        // Skip initial silence: find first note and subtract its startTime from all notes
        // ONLY if initial silence is > 2 seconds (to avoid collapsing truly simultaneous notes)
        if (notes.size() > 0) {
          unsigned long firstNoteTime = 0;
          
          // Find the minimum startTime among all notes (considering both tracks)
          for (const Note& note : notes) {
            if (firstNoteTime == 0 || note.startTime < firstNoteTime) {
              firstNoteTime = note.startTime;
            }
          }
          
          // Only skip if initial silence is >= 2000 ms (2 seconds)
          if (firstNoteTime >= 2000) {
            Serial.println("DEBUG: Note startTimes before skipping first silence:");
            for (int i = 0; i < min((int)notes.size(), 10); i++) {
              Serial.printf("  idx %d start=%lu led=%d\n", i, notes[i].startTime, notes[i].ledIndex);
            }

            for (Note& note : notes) {
              if (note.startTime >= firstNoteTime) {
                note.startTime -= firstNoteTime;
              } else {
                note.startTime = 0;
              }
            }

            Serial.printf("Skipped initial silence: %lu ms\n", firstNoteTime);
            Serial.println("DEBUG: Note startTimes after skipping first silence:");
            for (int i = 0; i < min((int)notes.size(), 10); i++) {
              Serial.printf("  idx %d start=%lu led=%d\n", i, notes[i].startTime, notes[i].ledIndex);
            }
          } else if (firstNoteTime > 0) {
            Serial.printf("DEBUG: Initial silence is only %lu ms (< 2000 ms), not skipping\n", firstNoteTime);
          }
        }
        
        // If resuming from a slider, calculate time correctly
        if (isResumeFromSlider) {
          playStartTime = millis() - (notes[currentNoteIndex].startTime / playbackSpeed);
          isResumeFromSlider = false;
        }
      } else if (isPaused) {
        totalPauseTime += millis() - pauseTime;
      }
      isPlaying = true;
      isPaused = false;
      server.send(200, "text/plain", "Playback started");
    } else {
      server.send(400, "text/plain", "No file loaded");
    }
  });
  
  server.on("/pause", HTTP_POST, []() {
    if (isPlaying) {
      if (debugMode) Serial.println("Pressed button Playback Controls : PAUSE");
      isPaused = !isPaused;
      if (isPaused) {
        pauseTime = millis();
      } else {
        totalPauseTime += millis() - pauseTime;
      }
      server.send(200, "text/plain", isPaused ? "Paused" : "Resumed");
    } else {
      server.send(400, "text/plain", "No playback in progress");
    }
  });
  
  server.on("/stop", HTTP_POST, []() {
    if (debugMode) Serial.println("Pressed button Playback Controls : STOP");
    isPlaying = false;
    isPaused = false;
    currentNoteIndex = 0;
    totalPauseTime = 0;
    awaitingSound = false;
    lastProcessedNoteGroup = ULONG_MAX;  // Reset note group tracker
    clearAllLeds();
    clearActiveLeds();
    // Reset microphone state
    lastThresholdDetectionTime = 0;
    microphoneFirstNoteShown = false;
    volumeBelowMinimumThreshold = false;
    server.send(200, "text/plain", "Stop");
  });
  
  server.on("/restart", HTTP_POST, []() {
    if (debugMode) Serial.println("Pressed button Playback Controls : RESTART");
    currentNoteIndex = 0;
    playStartTime = millis();
    totalPauseTime = 0;
    awaitingSound = false;
    lastThresholdDetectionTime = 0;
    microphoneFirstNoteShown = false;  // Reset microphone flag
    volumeBelowMinimumThreshold = false; // Reset minimum volume threshold flag
    clearAllLeds();
    clearActiveLeds();
    server.send(200, "text/plain", "Restart");
  });
  
  server.on("/setSpeed", HTTP_POST, []() {
    if (server.hasArg("speed")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      playbackSpeed = server.arg("speed").toFloat();
      // Clear LEDs to prevent ghosting when speed changes
      clearAllLeds();
      clearActiveLeds();
      if (debugMode) Serial.println("Pressed button Playback Controls : SPEED");
      server.send(200, "text/plain", "Speed set");
    } else {
      server.send(400, "text/plain", "Missing speed parameter");
    }
  });
  
  server.on("/setDebug", HTTP_POST, []() {
    if (server.hasArg("enabled")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      debugMode = server.arg("enabled") == "true";
      if (debugMode) Serial.println("Debug mode enabled");
      else Serial.println("Debug mode disabled");
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing enabled parameter");
    }
  });
  

  
  server.on("/setColors", HTTP_POST, []() {
    if (server.hasArg("leftHand") && server.hasArg("rightHand")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      String leftHex = server.arg("leftHand");
      String rightHex = server.arg("rightHand");
      
      leftHandColor = CRGB(strtol(leftHex.substring(1).c_str(), NULL, 16));
      rightHandColor = CRGB(strtol(rightHex.substring(1).c_str(), NULL, 16));
      
      if (debugMode) Serial.printf("Pressed button Color Configuration : Track 1 = %s, Track 2 = %s\n", leftHex.c_str(), rightHex.c_str());
      server.send(200, "text/plain", "Colors updated");
    } else {
      server.send(400, "text/plain", "Missing color parameters");
    }
  });
  
  server.on("/upload", HTTP_POST,
    []() { server.send(200, "text/plain", "OK"); },
    handleFileUpload
  );
  
  server.on("/analyzeFile", HTTP_GET, []() {
    // If no saved data but temp file exists, re-analyze
    if (availableTracks.size() == 0 && SPIFFS.exists("/temp_upload")) {
      File file = SPIFFS.open("/temp_upload", "r");
      if (file) {
        String filename = loadedFileName;
        file.close();
        
        if (filename.endsWith(".mid") || filename.endsWith(".midi")) {
          analyzeMIDITracks();
        }
      }
    }
    
    DynamicJsonDocument doc(2048);
    JsonArray tracksArray = doc.createNestedArray("tracks");
    
    // Sempre restituisci le tracce che hai
    for (const TrackInfo& track : availableTracks) {
      JsonObject trackObj = tracksArray.createNestedObject();
      trackObj["index"] = track.trackIndex;
      trackObj["name"] = track.trackName;
      trackObj["noteCount"] = track.noteCount;
      trackObj["minNote"] = track.minNote;
      trackObj["maxNote"] = track.maxNote;
      trackObj["hasNotes"] = track.hasNotes;
    }
    
    doc["hasAnalyzed"] = hasAnalyzedTracks;
    doc["fileName"] = loadedFileName;
    doc["selectedTrack"] = selectedTrackIndex;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });
  
  server.on("/selectTrack", HTTP_POST, []() {
    if (server.hasArg("trackIndex") && server.hasArg("trackSlot") && hasAnalyzedTracks) {
      int trackIndex = server.arg("trackIndex").toInt();
      int trackSlot = server.arg("trackSlot").toInt();
      
      // Attiva il loading prima di iniziare
      isLoading = true;
      loadingProgress = 0;
      
      Serial.printf("Track selection request: trackIndex=%d, trackSlot=%d\n", trackIndex, trackSlot);
      
      // Find the track info to get note count
      int trackNoteCount = 0;
      for (const TrackInfo& track : availableTracks) {
        if (track.trackIndex == trackIndex) {
          trackNoteCount = track.noteCount;
          break;
        }
      }
      
      // Check memory before loading - be EXTREMELY conservative
      size_t freeHeap = ESP.getFreeHeap();
      
      // Need at least 100KB free for safe operation
      if (freeHeap < 100000) {
        String errorMsg = "Insufficient memory: only " + String(freeHeap / 1024) + "KB available";
        if (debugMode) Serial.println(errorMsg);
        server.send(400, "text/plain", errorMsg);
        return;
      }
      
      // Maximum supported notes: (freeHeap - 100KB) / 40 bytes per note, max 1000 notes
      int maxSupportedNotes = (freeHeap - 100000) / 40;
      if (maxSupportedNotes > 1000) maxSupportedNotes = 1000;
      if (maxSupportedNotes < 0) maxSupportedNotes = 0;
      
      // Apply 10% safety margin for parsing overhead
      int safeNotes = (int)(maxSupportedNotes * 0.90);
      
      // Check if total notes would exceed limit of 900
      int totalNotesAfter = trackNoteCount;
      if (trackSlot == 2) {
        totalNotesAfter += notes.size();
      }
      
      if (totalNotesAfter > 900) {
        String errorMsg = "Too many notes : the maximum is limited to 900";
        isLoading = false;
        if (debugMode) Serial.println(errorMsg);
        server.send(400, "text/plain", errorMsg);
        return;
      }
      
      if (trackSlot == 1) {
        selectedTrackIndex = trackIndex;
      } else if (trackSlot == 2) {
        selectedTrackIndex2 = trackIndex;
      }
      
      // Stop playback if currently playing when changing tracks
      if (isPlaying || isPaused) {
        isPlaying = false;
        isPaused = false;
        currentNoteIndex = 0;
        totalPauseTime = 0;
        awaitingSound = false;
        clearAllLeds();
        clearActiveLeds();
        Serial.println("Playback stopped due to track change");
      }
      
      // If microphone is enabled, turn off LEDs when changing track
      if (microphoneEnabled) {
        clearAllLeds();
        clearActiveLeds();
      }
      
      if (SPIFFS.exists("/temp_upload")) {
        File file = SPIFFS.open("/temp_upload", "r");
        if (file) {
          if (trackSlot == 1) {
            notes.clear();
            processSelectedTrack(file, selectedTrackIndex, 1);
            
            if (selectedTrackIndex2 >= 0) {
              file.seek(0);
              processSelectedTrack(file, selectedTrackIndex2, 2);
            }
          } else {
            file.seek(0);
            processSelectedTrack(file, selectedTrackIndex2, 2);
          }
          
          file.close();
          saveNotesToSPIFFS();
          hasLoadedFile = true;
          
          Serial.printf("Track %d (slot %d) loaded successfully\n", trackIndex, trackSlot);
          server.send(200, "text/plain", "Track loaded successfully");
        } else {
          Serial.println("Error opening temp_upload file");
          server.send(500, "text/plain", "Error opening file");
        }
      } else {
        Serial.println("temp_upload file not found");
        server.send(404, "text/plain", "Temporary file not found");
      }
    } else {
      Serial.println("Missing parameters for selectTrack");
      server.send(400, "text/plain", "Missing parameters or file not analyzed");
    }
  });
  
  server.on("/setTranspose", HTTP_POST, []() {
    if (server.hasArg("track") && server.hasArg("transpose")) {
      // Auto-pause if playing
      if (isPlaying && !isPaused) {
        isPaused = true;
        pauseTime = millis();
      }
      int track = server.arg("track").toInt();
      int transpose = server.arg("transpose").toInt();
      
      if (track == 1) {
        trackTranspose1 = transpose;
      } else if (track == 2) {
        trackTranspose2 = transpose;
      }
      
      if (debugMode) Serial.printf("Pressed button Track Transposition : Track %d = %d octaves\n", track, transpose);
      
      clearAllLeds();  // Reset LEDs before transposing
      
      if (SPIFFS.exists("/temp_upload")) {
        File file = SPIFFS.open("/temp_upload", "r");
        if (file) {
          notes.clear();
          
          if (selectedTrackIndex >= 0) {
            processSelectedTrack(file, selectedTrackIndex, 1);
          }
          
          if (selectedTrackIndex2 >= 0) {
            file.seek(0);
            processSelectedTrack(file, selectedTrackIndex2, 2);
          }
          
          file.close();
          saveNotesToSPIFFS();
        }
      }
      
      server.send(200, "text/plain", "Transposition updated");
    } else {
      server.send(400, "text/plain", "Missing parameters");
    }
  });
  
  server.on("/deleteFile", HTTP_POST, []() {
    if (debugMode) Serial.println("Pressed button Delete File : DELETE");
    if (SPIFFS.exists("/music.json")) {
      SPIFFS.remove("/music.json");
    }
    if (SPIFFS.exists("/temp_upload")) {
      SPIFFS.remove("/temp_upload");
    }
    
    notes.clear();
    availableTracks.clear();
    hasLoadedFile = false;
    hasAnalyzedTracks = false;
    selectedTrackIndex = -1;
    selectedTrackIndex2 = -1;
    trackTranspose1 = 0;
    trackTranspose2 = 0;
    isPlaying = false;
    isPaused = false;
    currentNoteIndex = 0;
    loadedFileName = "";
    clearAllLeds();
    clearActiveLeds();
    server.send(200, "text/plain", "File deleted");
  });
  
  server.on("/status", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    doc["isPlaying"] = isPlaying;
    doc["isPaused"] = isPaused;
    doc["hasFile"] = hasLoadedFile;
    doc["hasAnalyzed"] = hasAnalyzedTracks;
    doc["brightness"] = globalBrightness;
    doc["speed"] = playbackSpeed;
    doc["currentNote"] = currentNoteIndex;
    doc["totalNotes"] = notes.size();
    doc["fileName"] = loadedFileName;
    doc["selectedTrack"] = selectedTrackIndex;
    doc["availableTracks"] = availableTracks.size();
    doc["microphoneEnabled"] = microphoneEnabled;
    doc["isLoading"] = isLoading;
    doc["loadingProgress"] = loadingProgress;
    doc["thresholdTimeout"] = thresholdTimeout;
    
    // Calculate available notes based on current free memory - very conservative
    size_t freeHeap = ESP.getFreeHeap();
    // Reserve 60KB minimum for operations, each note ~40 bytes, max 1000 notes
    int maxSupportedNotes = (freeHeap - 60000) / 40;
    if (maxSupportedNotes > 1000) maxSupportedNotes = 1000;
    if (maxSupportedNotes < 0) maxSupportedNotes = 0;
    // Apply 5% safety margin
    int safeNotes = (int)(maxSupportedNotes * 0.95);
    doc["availableNotes"] = safeNotes;
    
    if (microphoneEnabled) {
      doc["currentVolume"] = detectVolume();
    }
    
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    doc["storageUsed"] = usedBytes;
    doc["storageTotal"] = totalBytes;
    doc["storageAvailable"] = totalBytes - usedBytes;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });
}

void handleFileUpload() {
  HTTPUpload& upload = server.upload();
  static File uploadFile;
  
  if (upload.status == UPLOAD_FILE_START) {
    if (SPIFFS.exists("/temp_upload")) {
      SPIFFS.remove("/temp_upload");
    }
    if (SPIFFS.exists("/music.json")) {
      SPIFFS.remove("/music.json");
    }
    Serial.printf("Upload started: %s\n", upload.filename.c_str());
    loadedFileName = upload.filename;
    
    notes.clear();
    availableTracks.clear();
    hasLoadedFile = false;
    hasAnalyzedTracks = false;
    selectedTrackIndex = -1;
    selectedTrackIndex2 = -1;
    trackTranspose1 = 0;
    trackTranspose2 = 0;
    
    if (SPIFFS.exists("/temp_upload")) {
      SPIFFS.remove("/temp_upload");
    }
    
    uploadFile = SPIFFS.open("/temp_upload", "w");
    if (!uploadFile) {
      Serial.println("Error opening file for upload");
      return;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
    }
    
    Serial.printf("Upload completed: %zu bytes\n", upload.totalSize);
    
    if (upload.filename.endsWith(".mid") || upload.filename.endsWith(".midi")) {
      analyzeMIDITracks();
    } else if (upload.filename.endsWith(".mxl") || upload.filename.endsWith(".xml")) {
      File file = SPIFFS.open("/temp_upload", "r");
      if (file) {
        processMusicXMLFile(file);
        file.close();
        SPIFFS.remove("/temp_upload");
        hasLoadedFile = true;
      }
    }
  }
}

void analyzeMIDITracks() {
  File file = SPIFFS.open("/temp_upload", "r");
  if (!file) {
    Serial.println("Error opening file for analysis");
    return;
  }
  
  availableTracks.clear();
  
  size_t fileSize = file.size();
  uint8_t* buffer = new uint8_t[fileSize];
  file.readBytes((char*)buffer, fileSize);
  file.close();
  
  const uint8_t* data = buffer;
  int dataSize = fileSize;
  int pos = 0;
  
  if (dataSize < 14 || memcmp(data, "MThd", 4) != 0) {
    Serial.println("Invalid MIDI file");
    delete[] buffer;
    return;
  }
  
  pos += 4;
  uint32_t headerLen = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
  pos += 4;
  
  int format = (data[pos] << 8) | data[pos + 1];
  pos += 2;
  int tracks = (data[pos] << 8) | data[pos + 1];
  pos += 2;
  ticksPerQuarter = (data[pos] << 8) | data[pos + 1];
  pos += 2;
  
  Serial.printf("MIDI Analysis: Format=%d, Tracks=%d, TicksPerQuarter=%d\n", format, tracks, ticksPerQuarter);
  
  for (int track = 0; track < tracks && pos < dataSize - 8; track++) {
    if (pos + 4 > dataSize || memcmp(data + pos, "MTrk", 4) != 0) {
      Serial.println("Invalid track header");
      break;
    }
    
    pos += 4;
    uint32_t trackLength = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    int trackEnd = pos + trackLength;
    
    TrackInfo trackInfo;
    trackInfo.trackIndex = track;
    trackInfo.trackName = "Track " + String(track + 1);
    trackInfo.noteCount = 0;
    trackInfo.minNote = 127;
    trackInfo.maxNote = 0;
    trackInfo.hasNotes = false;
    trackInfo.programNumber = -1;  // -1 indicates no program found
    
    int trackPos = pos;
    uint8_t runningStatus = 0;
    
    while (trackPos < trackEnd) {
      if (trackInfo.noteCount % 1000 == 0 && trackInfo.noteCount > 0) {
        if (ESP.getFreeHeap() < 30000) {
          Serial.printf("Insufficient memory during analysis, stopping\n");
          break;
        }
      }
      
      uint32_t deltaTicks = readVariableLength(data, trackPos, dataSize);
      
      if (trackPos >= dataSize) break;
      
      uint8_t statusByte = data[trackPos];
      
      if (statusByte == 0xFF) {
        trackPos++;
        if (trackPos >= dataSize) break;
        uint8_t metaType = data[trackPos++];
        uint32_t length = readVariableLength(data, trackPos, dataSize);
        
        if (metaType == 0x03 && length > 0 && length < 50) {
          char trackName[51];
          int nameLen = min((int)length, 50);
          memcpy(trackName, data + trackPos, nameLen);
          trackName[nameLen] = '\0';
          trackInfo.trackName = String(trackName);
        }
        
        trackPos += length;
        continue;
      } else if (statusByte == 0xF0 || statusByte == 0xF7) {
        trackPos++;
        uint32_t length = readVariableLength(data, trackPos, dataSize);
        trackPos += length;
        continue;
      }
      
      if (statusByte & 0x80) {
        statusByte = data[trackPos++];
        runningStatus = statusByte;
      } else {
        if (runningStatus == 0) break;
        statusByte = runningStatus;
      }
      
      uint8_t eventType = statusByte & 0xF0;
      
      if (eventType == 0x90 || eventType == 0x80) {
        if (trackPos + 1 >= dataSize) break;
        uint8_t note = data[trackPos++];
        uint8_t velocity = data[trackPos++];
        
        if (eventType == 0x90 && velocity > 0) {
          trackInfo.noteCount++;
          trackInfo.hasNotes = true;
          if (note < trackInfo.minNote) trackInfo.minNote = note;
          if (note > trackInfo.maxNote) trackInfo.maxNote = note;
          
          if (trackInfo.noteCount > MAX_NOTES_PER_TRACK + 100) {
            Serial.printf("Track %d has too many notes, stopping count\n", track);
            break;
          }
        }
      } else if (eventType == 0xC0) {
        // Program Change event
        if (trackPos >= dataSize) break;
        uint8_t programNum = data[trackPos++];
        if (trackInfo.programNumber == -1) {
          trackInfo.programNumber = programNum;
        }
      } else if (eventType == 0xD0) {
        // Channel Pressure - skip 1 byte
        if (trackPos >= dataSize) break;
        trackPos += 1;
      } else {
        if (trackPos + 1 >= dataSize) break;
        trackPos += 2;
      }
    }
    
    Serial.printf("Track %d: %s - %d notes (range: %d-%d) - Instrument: %s\n",
                  track, trackInfo.trackName.c_str(), trackInfo.noteCount, trackInfo.minNote, trackInfo.maxNote,
                  (trackInfo.programNumber >= 0) ? getGMInstrumentName(trackInfo.programNumber).c_str() : "Not detected");
    
    availableTracks.push_back(trackInfo);
    pos = trackEnd;
  }
  
  delete[] buffer;
  hasAnalyzedTracks = true;
  
  std::vector<TrackInfo> validTracks;
  for (const TrackInfo& track : availableTracks) {
    if (track.hasNotes) {
      validTracks.push_back(track);
    }
  }
  
  if (validTracks.size() == 1 && validTracks[0].noteCount <= MAX_NOTES_PER_TRACK) {
    selectedTrackIndex = validTracks[0].trackIndex;
    File file = SPIFFS.open("/temp_upload", "r");
    if (file) {
      processSelectedTrack(file, selectedTrackIndex);
      file.close();
      saveNotesToSPIFFS();
      hasLoadedFile = true;
    }
  } else if (validTracks.size() == 1) {
    Serial.printf("Track has too many notes (%d), manual loading required\n", validTracks[0].noteCount);
  }
  
  Serial.printf("Analysis completed: %d tracks found, %d with notes\n", availableTracks.size(), validTracks.size());
}

void processSelectedTrack(File& file, int targetTrack, int trackSlot) {
  isLoading = true;
  loadingProgress = 0;
  
  if (trackSlot == 1 && selectedTrackIndex2 == -1) {
    notes.clear();
  } else if (trackSlot == 1) {
    notes.erase(std::remove_if(notes.begin(), notes.end(),
        [](const Note& n) { return n.trackSource == 1; }), notes.end());
  } else {
    notes.erase(std::remove_if(notes.begin(), notes.end(),
        [](const Note& n) { return n.trackSource == 2; }), notes.end());
  }
  
  size_t freeHeap = ESP.getFreeHeap();
  size_t fileSize = file.size();
  size_t estimatedNeed = fileSize + (notes.size() * 32) + 20000;
  
  if (estimatedNeed > freeHeap) {
    Serial.printf("Insufficient memory: need %zu, available %zu\n", estimatedNeed, freeHeap);
    isLoading = false;
    return;
  }
  
  uint8_t* buffer = new uint8_t[fileSize];
  if (!buffer) {
    Serial.println("Memory allocation error");
    isLoading = false;
    return;
  }
  file.readBytes((char*)buffer, fileSize);
  
  const uint8_t* data = buffer;
  int dataSize = fileSize;
  int pos = 0;
  
  pos += 4;
  uint32_t headerLen = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
  pos += 4;
  pos += headerLen;
  
  // First pass: find the tempo from any track
  uint32_t microsecondsPerQuarter = 500000;
  int tempPos = pos;
  for (int track = 0; tempPos < dataSize - 8 && track < 100; track++) {
    if (tempPos + 4 > dataSize || memcmp(data + tempPos, "MTrk", 4) != 0) break;
    
    tempPos += 4;
    uint32_t trackLength = (data[tempPos] << 24) | (data[tempPos + 1] << 16) | (data[tempPos + 2] << 8) | data[tempPos + 3];
    tempPos += 4;
    int trackEnd = tempPos + trackLength;
    
    int tempTrackPos = tempPos;
    uint8_t tempRunningStatus = 0;
    
    while (tempTrackPos < trackEnd) {
      uint32_t deltaTicks = readVariableLength(data, tempTrackPos, dataSize);
      
      if (tempTrackPos >= dataSize) break;
      
      uint8_t statusByte = data[tempTrackPos];
      
      if (statusByte == 0xFF) {
        tempTrackPos++;
        if (tempTrackPos >= dataSize) break;
        uint8_t metaType = data[tempTrackPos++];
        uint32_t length = readVariableLength(data, tempTrackPos, dataSize);
        
        if (metaType == 0x51 && length == 3) {
          microsecondsPerQuarter = (data[tempTrackPos] << 16) | (data[tempTrackPos + 1] << 8) | data[tempTrackPos + 2];
          // Found tempo, break both loops
          tempTrackPos = trackEnd;
          track = 100;
          break;
        }
        
        tempTrackPos += length;
        continue;
      } else if (statusByte == 0xF0 || statusByte == 0xF7) {
        tempTrackPos++;
        uint32_t length = readVariableLength(data, tempTrackPos, dataSize);
        tempTrackPos += length;
        continue;
      }
      
      if (statusByte & 0x80) {
        tempRunningStatus = data[tempTrackPos++];
      } else {
        if (tempRunningStatus == 0) break;
      }
      
      uint8_t eventType = statusByte & 0xF0;
      if (eventType == 0xC0 || eventType == 0xD0) {
        if (tempTrackPos >= dataSize) break;
        tempTrackPos += 1;
      } else {
        if (tempTrackPos + 1 >= dataSize) break;
        tempTrackPos += 2;
      }
    }
    
    tempPos = trackEnd;
  }
  
  double msPerTickLocal = (microsecondsPerQuarter / 1000.0) / (double)ticksPerQuarter;
  
  for (int track = 0; pos < dataSize - 8; track++) {
    if (pos + 4 > dataSize || memcmp(data + pos, "MTrk", 4) != 0) break;
    
    pos += 4;
    uint32_t trackLength = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    int trackEnd = pos + trackLength;
    
    if (track == targetTrack) {
      uint32_t currentTick = 0;
      std::vector<struct ActiveNote> activeNotes;
      uint8_t runningStatus = 0;
      int notesProcessed = 0;
      
      struct ActiveNote {
        int midiNote;
        uint32_t startTick;
      };
      
      while (pos < trackEnd) {
        // Safety check: if memory drops below critical threshold, stop parsing
        if (ESP.getFreeHeap() < 50000) {
          Serial.printf("Critical memory threshold reached, stopping track parsing\n");
          break;
        }
        
        // Update progress every 100 notes
        if (notesProcessed % 100 == 0 && notesProcessed > 0) {
          loadingProgress = min(90, (notesProcessed * 90) / 1300);
        }
        
        uint32_t deltaTicks = readVariableLength(data, pos, dataSize);
        currentTick += deltaTicks;
        
        if (pos >= dataSize) break;
        
        uint8_t statusByte = data[pos];
        
        if (statusByte == 0xFF) {
          pos++;
          if (pos >= dataSize) break;
          uint8_t metaType = data[pos++];
          uint32_t length = readVariableLength(data, pos, dataSize);
          
          // Don't update msPerTickLocal in second pass - use the global tempo found
          // if (metaType == 0x51 && length == 3) {
          //   microsecondsPerQuarter = (data[pos] << 16) | (data[pos + 1] << 8) | data[pos + 2];
          //   msPerTickLocal = (microsecondsPerQuarter / 1000.0) / (double)ticksPerQuarter;
          // }
          
          pos += length;
          continue;
        } else if (statusByte == 0xF0 || statusByte == 0xF7) {
          pos++;
          uint32_t length = readVariableLength(data, pos, dataSize);
          pos += length;
          continue;
        }
        
        if (statusByte & 0x80) {
          statusByte = data[pos++];
          runningStatus = statusByte;
        } else {
          if (runningStatus == 0) break;
          statusByte = runningStatus;
        }
        
        uint8_t eventType = statusByte & 0xF0;
        
        if (eventType == 0x90) {
          if (pos + 1 >= dataSize) break;
          uint8_t note = data[pos++];
          uint8_t velocity = data[pos++];
          
          if (velocity > 0) {
            struct ActiveNote an;
            an.midiNote = note;
            an.startTick = currentTick;
            activeNotes.push_back(an);
          } else {
            for (int i = activeNotes.size() - 1; i >= 0; i--) {
              if (activeNotes[i].midiNote == note) {
                Note newNote;
                newNote.midiNote = note;
                int transposeAmount = (trackSlot == 1) ? trackTranspose1 : trackTranspose2;
                int transposedNote = newNote.midiNote + (transposeAmount * 12);
                newNote.trackSource = trackSlot;
                newNote.velocity = 80;
                newNote.ledIndex = mapNoteToLed(transposedNote);
                newNote.isLeftHand = (transposedNote < splitPoint);
                newNote.startTime = (unsigned long)round(activeNotes[i].startTick * msPerTickLocal);
                newNote.duration = max(100UL, (unsigned long)round((currentTick - activeNotes[i].startTick) * msPerTickLocal));
                notes.push_back(newNote);
                notesProcessed++;
                activeNotes.erase(activeNotes.begin() + i);
                break;
              }
            }
          }
        } else if (eventType == 0x80) {
          if (pos + 1 >= dataSize) break;
          uint8_t note = data[pos++];
          uint8_t velocity = data[pos++];
          
          for (int i = activeNotes.size() - 1; i >= 0; i--) {
            if (activeNotes[i].midiNote == note) {
              Note newNote;
              newNote.midiNote = note;
              int transposeAmount = (trackSlot == 1) ? trackTranspose1 : trackTranspose2;
              int transposedNote = newNote.midiNote + (transposeAmount * 12);
              newNote.trackSource = trackSlot;
              newNote.velocity = velocity;
              newNote.ledIndex = mapNoteToLed(transposedNote);
              newNote.isLeftHand = (transposedNote < splitPoint);
              newNote.startTime = (unsigned long)round(activeNotes[i].startTick * msPerTickLocal);
              newNote.duration = max(100UL, (unsigned long)round((currentTick - activeNotes[i].startTick) * msPerTickLocal));
              notes.push_back(newNote);
              notesProcessed++;
              activeNotes.erase(activeNotes.begin() + i);
              break;
            }
          }
        } else {
          if (eventType == 0xC0 || eventType == 0xD0) {
            if (pos >= dataSize) break;
            pos += 1;
          } else {
            if (pos + 1 >= dataSize) break;
            pos += 2;
          }
        }
      }
      
      for (const struct ActiveNote& an : activeNotes) {
        Note newNote;
        newNote.midiNote = an.midiNote;
        int transposeAmount = (trackSlot == 1) ? trackTranspose1 : trackTranspose2;
        int transposedNote = newNote.midiNote + (transposeAmount * 12);
        newNote.trackSource = trackSlot;
        newNote.velocity = 80;
        newNote.ledIndex = mapNoteToLed(transposedNote);
        newNote.isLeftHand = (transposedNote < splitPoint);
        newNote.startTime = (unsigned long)round(an.startTick * msPerTickLocal);
        newNote.duration = 1000;
        notes.push_back(newNote);
      }
    }
    
    pos = trackEnd;
  }
  
  size_t trackNotes = 0;
  for (const Note& note : notes) {
    if (note.trackSource == trackSlot) {
      trackNotes++;
    }
  }
  
  if (trackNotes > MAX_NOTES_PER_TRACK) {
    Serial.printf("Too many notes in track %d (%zu), limiting to %d\n",
                  trackSlot, trackNotes, MAX_NOTES_PER_TRACK);
    
    int removed = 0;
    for (auto it = notes.begin(); it != notes.end() && removed < (trackNotes - MAX_NOTES_PER_TRACK); ) {
      if (it->trackSource == trackSlot) {
        it = notes.erase(it);
        removed++;
      } else {
        ++it;
      }
    }
  }
  
  delete[] buffer;
  
  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    return a.startTime < b.startTime;
  });
  
  loadingProgress = 100;
  delay(500); // Give time for UI to show 100%
  isLoading = false;
  
  Serial.printf("Track %d processed: %d notes loaded (transposition %d octaves)\n",
                targetTrack, notes.size(), (trackSlot == 1) ? trackTranspose1 : trackTranspose2);
  printMemoryInfo();
}

void processMusicXMLFile(File& file) {
  String content = file.readString();
  
  if (loadedFileName.endsWith(".mxl")) {
    Serial.println("Warning: MXL file requires decompression, processing as XML");
  }
  
  int pos = 0;
  unsigned long currentTime = 0;
  float quarterNoteDuration = 60000.0 / beatsPerMinute;
  int divisions = 4;
  
  int divisionsPos = content.indexOf("<divisions>");
  if (divisionsPos != -1) {
    int divisionsEnd = content.indexOf("</divisions>", divisionsPos);
    if (divisionsEnd != -1) {
      divisions = content.substring(divisionsPos + 11, divisionsEnd).toInt();
    }
  }
  
  while (pos < content.length()) {
    int noteStart = content.indexOf("<note", pos);
    if (noteStart == -1) break;
    
    int noteEnd = content.indexOf("</note>", noteStart);
    if (noteEnd == -1) break;
    
    String noteXML = content.substring(noteStart, noteEnd + 7);
    
    if (noteXML.indexOf("<rest") != -1) {
      int durationPos = noteXML.indexOf("<duration>");
      if (durationPos != -1) {
        int duration = noteXML.substring(durationPos + 10, noteXML.indexOf("</duration>")).toInt();
        currentTime += (duration * quarterNoteDuration) / divisions;
      }
    } else {
      int stepPos = noteXML.indexOf("<step>");
      int octavePos = noteXML.indexOf("<octave>");
      int durationPos = noteXML.indexOf("<duration>");
      
      if (stepPos != -1 && octavePos != -1 && durationPos != -1) {
        String step = noteXML.substring(stepPos + 6, stepPos + 7);
        int octave = noteXML.substring(octavePos + 8, octavePos + 9).toInt();
        int duration = noteXML.substring(durationPos + 10, noteXML.indexOf("</duration>")).toInt();
        
        int midiNote = (octave + 1) * 12;
        if (step == "C") midiNote += 0;
        else if (step == "D") midiNote += 2;
        else if (step == "E") midiNote += 4;
        else if (step == "F") midiNote += 5;
        else if (step == "G") midiNote += 7;
        else if (step == "A") midiNote += 9;
        else if (step == "B") midiNote += 11;
        
        if (noteXML.indexOf("<alter>1</alter>") != -1) midiNote += 1;
        if (noteXML.indexOf("<alter>-1</alter>") != -1) midiNote -= 1;
        
        Note newNote;
        newNote.midiNote = midiNote;
        newNote.ledIndex = mapNoteToLed(midiNote);
        newNote.isLeftHand = (midiNote < splitPoint);
        newNote.startTime = currentTime;
        newNote.duration = (duration * quarterNoteDuration) / divisions;
        newNote.velocity = 80;
        newNote.trackSource = 1;
        
        notes.push_back(newNote);
      }
    }
    
    pos = noteEnd + 7;
  }
  
  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    return a.startTime < b.startTime;
  });
  
  if (notes.size() > 0) {
    unsigned long baseTime = notes.front().startTime;
    if (baseTime > 0) {
      for (auto &n : notes) {
        if (n.startTime >= baseTime) n.startTime -= baseTime;
        else n.startTime = 0;
      }
    }
  }
  
  saveNotesToSPIFFS();
  hasLoadedFile = true;
}

void initMicrophone() {
  static bool isInitialized = false;
  
  if (microphoneEnabled && !isInitialized) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    
    i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = I2S_SCK,
        .ws = I2S_WS,
        .dout = I2S_GPIO_UNUSED,
        .din = I2S_SD,
        .invert_flags = {
          .mclk_inv = false,
          .bclk_inv = false,
          .ws_inv = false,
        },
      },
    };
    
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle);
    if (err != ESP_OK) {
      Serial.printf("I2S driver install error: %d\n", err);
      microphoneEnabled = false;
      return;
    }
    
    err = i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
    if (err != ESP_OK) {
      Serial.printf("I2S init error: %d\n", err);
      i2s_del_channel(i2s_rx_handle);
      microphoneEnabled = false;
      return;
    }

    err = i2s_channel_enable(i2s_rx_handle);
    if (err != ESP_OK) {
      Serial.printf("I2S enable error: %d\n", err);
      i2s_del_channel(i2s_rx_handle);
      microphoneEnabled = false;
      return;
    }
    
    isInitialized = true;
    Serial.println("Microphone initialized successfully");
  } else if (!microphoneEnabled && isInitialized) {
    i2s_del_channel(i2s_rx_handle);
    isInitialized = false;
    Serial.println("Microphone uninstalled");
  }
}

float detectVolume() {
  if (!microphoneEnabled) return 0.0f;
  
  const int sampleCount = 64;  // Reduced for faster response
  int32_t samples[sampleCount];
  size_t bytesRead = 0;
  
  esp_err_t result = i2s_channel_read(i2s_rx_handle, samples, sizeof(int32_t) * sampleCount, &bytesRead, 100 / portTICK_PERIOD_MS);
  
  if (result != ESP_OK || bytesRead == 0) {
    Serial.println("I2S read error or no data");
    return 0.0f;
  }

  float maxSample = 0;
  for (int i = 0; i < sampleCount; i++) {
    samples[i] = samples[i] >> 14;  // Convert to 18 bit
    float sample = abs(samples[i]);
    if (sample > maxSample) {
      maxSample = sample;
    }
  }

  // Convert to decibels with reference to 1 (normalized)
  if (maxSample > 0) {
    float db = 20.0 * log10(maxSample / (float)(1 << 19));
    // Map dB to a more manageable value (0-1000) with a more sensitive scale
    float mapped = 0;
    if (db > -70) {  // Ignore background noise
      mapped = map(constrain((int)(db + 70), 0, 70), 0, 70, 0, 1000);
    }
    static unsigned long lastLogTime = 0;
    unsigned long timeDiff = millis() - lastLogTime;
    if (lastLogTime == 0) timeDiff = 0;
    lastLogTime = millis();
    Serial.printf("Max sample: %.0f, dB: %.1f, Mapped: %.1f, Time=%lums\n", maxSample, db, mapped, timeDiff);
    return mapped;
  }
  
  return 0.0f;
}

int pitchToMIDI(float frequency) {
  if (frequency <= 0) return -1;
  
  // A4 = 440 Hz = MIDI 69
  float midi = 12.0 * log2(frequency / 440.0) + 69.0;
  return (int)round(midi);
}

void highlightNoteWithFeedback(int correctNote) {
  clearAllLeds();
  
  // Highlight correct note in green
  int correctLedIndex = mapNoteToLed(correctNote);
  leds[correctLedIndex] = CRGB::Green;
  
  // Highlight semitones before and after in red
  int noteBefore = correctNote - 1;
  int noteAfter = correctNote + 1;
  
  int ledBefore = mapNoteToLed(noteBefore);
  int ledAfter = mapNoteToLed(noteAfter);
  
  if (ledBefore >= 0 && ledBefore < NUM_LEDS) {
    leds[ledBefore] = CRGB::Red;
  }
  if (ledAfter >= 0 && ledAfter < NUM_LEDS) {
    leds[ledAfter] = CRGB::Red;
  }
  
  FastLED.show();
}

uint32_t readVariableLength(const uint8_t* data, int& pos, int dataSize) {
  uint32_t value = 0;
  uint8_t byte;
  
  do {
    if (pos >= dataSize) return 0;
    byte = data[pos++];
    value = (value << 7) | (byte & 0x7F);
  } while ((byte & 0x80) && pos < dataSize);
  
  return value;
}

String getGMInstrumentName(int programNumber) {
  if (programNumber < 0 || programNumber > 127) return "Unknown";
  
  // Array con i nomi degli strumenti GM (semplificati)
  static const char* instruments[] = {
    "Acoustic Piano", "Bright Piano", "Electric Piano", "Honky-tonk Piano",
    "Electric Piano", "Electric Piano", "Harpsichord", "Clavi",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
    "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Bandneon",
    "Nylon Acoustic Guitar", "Steel Acoustic Guitar", "Jazz Electric Guitar", "Clean Electric Guitar",
    "Muted Electric Guitar", "Overdriven Guitar", "Distorted Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Fingered Electric Bass", "Picked Electric Bass", "Fretless Bass",
    "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass",
    "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
    "Choir Aahs", "Choir Oohs", "Synth Choir", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Saxophone", "Alto Saxophone", "Tenor Saxophone", "Baritone Saxophone",
    "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute",
    "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead Square", "Lead Sawtooth", "Lead Calliope", "Lead Chiff",
    "Lead Charang", "Lead Voice", "Lead Fifths", "Lead Bass",
    "Pad New Age", "Pad Warm", "Pad Polysynth", "Pad Choir",
    "Pad Bowed", "Pad Metallic", "Pad Halo", "Pad Sweep",
    "FX Rain", "FX Soundtrack", "FX Crystal", "FX Atmosphere",
    "FX Brightness", "FX Goblins", "FX Echoes", "FX Sci-Fi",
    "Sitar", "Banjo", "Shamisen", "Koto",
    "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
    "Taiko Drum", "Melodic Drum", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot"
  };
  
  return String(instruments[programNumber]);
}

int mapNoteToLed(int midiNote) {
  int minNote = 21;
  int maxNote = 93;
  
  if (midiNote < minNote) midiNote = minNote;
  if (midiNote > maxNote) midiNote = maxNote;
  
  return map(midiNote, minNote, maxNote, 0, NUM_LEDS - 1);
}

void printMemoryInfo() {
  Serial.printf("Free memory: %zu bytes\n", ESP.getFreeHeap());
  Serial.printf("Notes loaded: %d\n", notes.size());
  Serial.printf("Analyzed tracks: %d\n", availableTracks.size());
}

void saveNotesToSPIFFS() {
  File file = SPIFFS.open("/music.json", "w");
  if (!file) {
    Serial.println("Error opening file for writing");
    return;
  }
  
  size_t jsonSize = 8192;
  size_t needed = notes.size() * 100 + availableTracks.size() * 100 + 2048;
  if (needed > jsonSize) jsonSize = needed;
  if (jsonSize > 40960) jsonSize = 40960;
  
  DynamicJsonDocument doc(jsonSize);
  JsonArray notesArray = doc.createNestedArray("notes");
  JsonArray tracksArray = doc.createNestedArray("availableTracks");
  doc["fileName"] = loadedFileName;
  doc["bpm"] = beatsPerMinute;
  doc["totalNotes"] = notes.size();
  doc["selectedTrack"] = selectedTrackIndex;
  doc["selectedTrack2"] = selectedTrackIndex2;
  doc["transpose1"] = trackTranspose1;
  doc["transpose2"] = trackTranspose2;
  doc["hasAnalyzedTracks"] = hasAnalyzedTracks;
  
  // Save available tracks
  for (const TrackInfo& track : availableTracks) {
    JsonObject trackObj = tracksArray.createNestedObject();
    trackObj["index"] = track.trackIndex;
    trackObj["name"] = track.trackName;
    trackObj["noteCount"] = track.noteCount;
    trackObj["minNote"] = track.minNote;
    trackObj["maxNote"] = track.maxNote;
    trackObj["hasNotes"] = track.hasNotes;
    trackObj["programNumber"] = track.programNumber;
  }
  
  for (const Note& note : notes) {
    JsonObject noteObj = notesArray.createNestedObject();
    noteObj["led"] = note.ledIndex;
    noteObj["leftHand"] = note.isLeftHand;
    noteObj["start"] = note.startTime;
    noteObj["duration"] = note.duration;
    noteObj["midi"] = note.midiNote;
    noteObj["velocity"] = note.velocity;
    noteObj["trackSource"] = note.trackSource;
  }
  
  serializeJson(doc, file);
  file.close();
  
  Serial.println("Notes saved to SPIFFS");
}

void loadNotesFromSPIFFS() {
  File file = SPIFFS.open("/music.json", "r");
  if (!file) {
    Serial.println("Music file not found");
    return;
  }
  
  size_t fileSize = file.size();
  if (fileSize > 60000) {
    Serial.printf("JSON file too large: %zu bytes\n", fileSize);
    file.close();
    SPIFFS.remove("/music.json");
    return;
  }
  
  size_t jsonSize = fileSize + 1024;
  DynamicJsonDocument doc(jsonSize);
  DeserializationError error = deserializeJson(doc, file);
  
  if (error) {
    Serial.printf("JSON parsing error: %s\n", error.c_str());
    file.close();
    SPIFFS.remove("/music.json");
    return;
  }
  
  file.close();
  
  notes.clear();
  availableTracks.clear();
  loadedFileName = doc["fileName"] | "unknown_file";
  beatsPerMinute = doc["bpm"] | 120.0;
  selectedTrackIndex = doc["selectedTrack"] | -1;
  selectedTrackIndex2 = doc["selectedTrack2"] | -1;
  trackTranspose1 = doc["transpose1"] | 0;
  trackTranspose2 = doc["transpose2"] | 0;
  hasAnalyzedTracks = doc["hasAnalyzedTracks"] | false;
  
  // Load available tracks
  JsonArray tracksArray = doc["availableTracks"];
  for (JsonObject trackObj : tracksArray) {
    TrackInfo track;
    track.trackIndex = trackObj["index"];
    track.trackName = trackObj["name"] | "Track";
    track.noteCount = trackObj["noteCount"];
    track.minNote = trackObj["minNote"];
    track.maxNote = trackObj["maxNote"];
    track.hasNotes = trackObj["hasNotes"];
    track.programNumber = trackObj["programNumber"] | -1;
    availableTracks.push_back(track);
  }
  if (availableTracks.size() > 0) {
    Serial.printf("Loaded %d tracks from SPIFFS\n", availableTracks.size());
  }
  
  JsonArray notesArray = doc["notes"];
  
  for (JsonObject noteObj : notesArray) {
    Note note;
    note.ledIndex = noteObj["led"];
    note.isLeftHand = noteObj["leftHand"];
    note.startTime = noteObj["start"];
    note.duration = noteObj["duration"];
    note.midiNote = noteObj["midi"] | 60;
    note.velocity = noteObj["velocity"] | 80;
    note.trackSource = noteObj["trackSource"] | 1;
    notes.push_back(note);
  }
  
  if (notes.size() > MAX_NOTES_PER_TRACK * 2) {
    Serial.printf("Too many notes loaded: %d, limiting...\n", notes.size());
    notes.resize(MAX_NOTES_PER_TRACK * 2);
  }
  
  Serial.printf("Free memory after loading notes: %zu bytes\n", ESP.getFreeHeap());
  
  hasLoadedFile = true;
  Serial.printf("Loaded %d notes from file %s (tracks: %d, hasAnalyzed=%d)\n", notes.size(), loadedFileName.c_str(), availableTracks.size(), hasAnalyzedTracks);
}

void playMusic() {
  if (notes.size() == 0) {
    Serial.println("DEBUG: No notes to play");
    return;
  }
  
  unsigned long now = millis();
  // Calculate actual elapsed time, NOT multiplied by playbackSpeed
  unsigned long elapsedTime = (now - playStartTime - totalPauseTime);
  
  // MICROPHONE CONTROL HAS PRIORITY: if enabled, ONLY use microphone logic regardless of track count
  if (microphoneEnabled) {
    static unsigned long lastDetectionAttempt = 0;
    static unsigned long lastVolumeCheckTime = 0;
    
    // Limit detection attempts to avoid overload
    if (millis() - lastDetectionAttempt < 100) {
      return;
    }
    lastDetectionAttempt = millis();
    
    if (isCalibrating) {
      static bool peakDetected = false;
      static unsigned long peakTime = 0;
      unsigned long timeSinceStart = millis() - calibrationStartTime;
      
      float currentVolume = detectVolume();
      if (currentVolume > calibrationPeak) {
        calibrationPeak = currentVolume;
        if (!peakDetected) {
          peakDetected = true;
          peakTime = millis();
        }
      }
      
      if (peakDetected) {
        unsigned long timeSincePeak = millis() - peakTime;
        
        if (timeSincePeak >= 500 && calibration05s == 0) {
          calibration05s = currentVolume;
        }
        if (timeSincePeak >= 1000 && calibration1s == 0) {
          calibration1s = currentVolume;
          isCalibrating = false;
          float suggestedThreshold = (calibrationPeak + calibration05s) / 2;
          Serial.printf("Calibration complete - Peak: %.1f, 0.5s: %.1f, Suggested: %.1f\n",
                       calibrationPeak, calibration05s, suggestedThreshold);
          volumeThreshold = suggestedThreshold;
        }
      }
      return;
    }
    
    // Show first note(s) automatically
    if (!microphoneFirstNoteShown && currentNoteIndex < notes.size()) {
      unsigned long noteTime = notes[currentNoteIndex].startTime;
      unsigned long now = millis();
      
      while (currentNoteIndex < notes.size() && notes[currentNoteIndex].startTime == noteTime) {
        Note& note = notes[currentNoteIndex];
        
        if (note.ledIndex >= 0 && note.ledIndex < NUM_LEDS) {
          CRGB color = (note.trackSource == 1) ? leftHandColor : rightHandColor;
          float intensity = max(0.3f, note.velocity / 127.0f);
          color.r = color.r * intensity;
          color.g = color.g * intensity;
          color.b = color.b * intensity;
          
          leds[note.ledIndex] = color;
          // In microphone mode: all notes have fixed 500ms duration
          ledEndTimes[note.ledIndex] = now + 500;
          ledStartTimes[note.ledIndex] = now;
          ledOffTimes[note.ledIndex] = 0;
          activeLeds[note.ledIndex] = 1;
          
          Serial.printf("First note(s) shown: note %d on LED %d\n", currentNoteIndex, note.ledIndex);
        }
        
        currentNoteIndex++;
      }
      
      FastLED.show();
      microphoneFirstNoteShown = true;
    }
    
    // Check if we can detect a new threshold crossing
    unsigned long nowMs = millis();
    float volume = detectVolume();
    
    // Track if volume has dropped below minimum threshold
    if (volume < minimumVolumeThreshold) {
      volumeBelowMinimumThreshold = true;
    }
    
    // Only advance to next note if:
    // 1. Volume has previously dropped below minimum threshold (volumeBelowMinimumThreshold = true)
    // 2. Volume now exceeds the main threshold
    // 3. Sufficient time has passed since last detection
    if (volumeBelowMinimumThreshold && volume > volumeThreshold) {
      if (nowMs - lastThresholdDetectionTime >= thresholdTimeout) {
        lastThresholdDetectionTime = nowMs;
        volumeBelowMinimumThreshold = false; // Reset flag after advancing
        
        // Turn off previous note(s)
        int prevNoteIndex = currentNoteIndex - 1;
        while (prevNoteIndex >= 0 && notes[prevNoteIndex].startTime == notes[currentNoteIndex - 1].startTime) {
          Note& prevNote = notes[prevNoteIndex];
          if (prevNote.ledIndex >= 0 && prevNote.ledIndex < NUM_LEDS) {
            leds[prevNote.ledIndex] = CRGB::Black;
            activeLeds[prevNote.ledIndex] = -1;
          }
          prevNoteIndex--;
        }
        
        // Show current note(s)
        if (currentNoteIndex < notes.size()) {
          unsigned long noteTime = notes[currentNoteIndex].startTime;
          unsigned long now = millis();
          
          while (currentNoteIndex < notes.size() && notes[currentNoteIndex].startTime == noteTime) {
            Note& note = notes[currentNoteIndex];
            
            if (note.ledIndex >= 0 && note.ledIndex < NUM_LEDS) {
              CRGB color = (note.trackSource == 1) ? leftHandColor : rightHandColor;
              float intensity = max(0.3f, note.velocity / 127.0f);
              color.r = color.r * intensity;
              color.g = color.g * intensity;
              color.b = color.b * intensity;
              
              leds[note.ledIndex] = color;
              // In microphone mode: all notes have fixed 500ms duration
              ledEndTimes[note.ledIndex] = now + 500;
              ledStartTimes[note.ledIndex] = now;
              ledOffTimes[note.ledIndex] = 0;
              activeLeds[note.ledIndex] = 1;
              
              Serial.printf("Volume detected: %.1f (threshold: %.1f, minimum: %.1f) - Showing note %d\n", volume, volumeThreshold, minimumVolumeThreshold, currentNoteIndex);
            }
            
            currentNoteIndex++;
          }
          
          FastLED.show();
        }
      }
    }
    
    // Check if playback is complete
    if (currentNoteIndex >= notes.size()) {
      isPlaying = false;
      currentNoteIndex = 0;
      awaitingSound = false;
      microphoneFirstNoteShown = false;
      clearAllLeds();
      Serial.println("DEBUG: Playback completed");
    }
    
    yield();
    return;
  }
  
  // If microphone is disabled: advance by timing with playbackSpeed (works with 1 or 2 tracks)
  // Optimized LED off: only check active LEDs instead of all
  for (int i = 0; i < NUM_LEDS; i++) {
    if (activeLeds[i] == 1) {
      // Applica playbackSpeed solo al confronto con ledEndTimes
      unsigned long scaledElapsedTime = (unsigned long)(elapsedTime * playbackSpeed);
      if (scaledElapsedTime >= ledEndTimes[i]) {
        leds[i] = CRGB::Black;
        activeLeds[i] = -1;
      }
    }
  }
  
  if (currentNoteIndex < notes.size()) {
    unsigned long noteTime = notes[currentNoteIndex].startTime;
    bool needsUpdate = false;
    
    // Applica playbackSpeed per il confronto con noteTime
    unsigned long scaledElapsedTime = (unsigned long)(elapsedTime * playbackSpeed);
    
    // Only process notes if we've reached their start time AND haven't already processed this note group
    if (scaledElapsedTime >= notes[currentNoteIndex].startTime && lastProcessedNoteGroup != noteTime) {
      // Mark this note group as processed
      Serial.printf("DEBUG: Processing note group time=%lu (scaledElapsed=%lu) at idx=%d\n", noteTime, scaledElapsedTime, currentNoteIndex);
      Serial.print("DEBUG: Notes in this group: ");
      unsigned long dbgNoteTime = noteTime;
      int dbgIdx = currentNoteIndex;
      while (dbgIdx < notes.size() && notes[dbgIdx].startTime == dbgNoteTime) {
        Serial.printf("%d(", notes[dbgIdx].ledIndex);
        Serial.print(") ");
        dbgIdx++;
      }
      Serial.println();
      lastProcessedNoteGroup = noteTime;
      
      // Process all notes that have the same startTime (chords/simultaneous notes)
      while (currentNoteIndex < notes.size() && notes[currentNoteIndex].startTime == noteTime) {
        Note& note = notes[currentNoteIndex];
        
        if (note.ledIndex >= 0 && note.ledIndex < NUM_LEDS) {
          CRGB color = (note.trackSource == 1) ? leftHandColor : rightHandColor;
          float intensity = max(0.3f, note.velocity / 127.0f);
          color.r = color.r * intensity;
          color.g = color.g * intensity;
          color.b = color.b * intensity;
          
          leds[note.ledIndex] = color;
          // ledEndTimes in base al tempo reale scalato
          ledEndTimes[note.ledIndex] = scaledElapsedTime + note.duration;
          ledStartTimes[note.ledIndex] = scaledElapsedTime;  // Track LED start time
          ledOffTimes[note.ledIndex] = 0;  // Reset LED off time
          activeLeds[note.ledIndex] = 1;
          needsUpdate = true;
        }
        
        currentNoteIndex++;
      }
      
      if (needsUpdate) {
        FastLED.show();
      }
    }
  }
  
  // Check if playback is complete
  if (currentNoteIndex >= notes.size()) {
    bool anyLedActive = false;
    for (int i = 0; i < NUM_LEDS; i++) {
      if (activeLeds[i] == 1) {
        anyLedActive = true;
        break;
      }
    }
    
    if (!anyLedActive) {
      isPlaying = false;
      currentNoteIndex = 0;
      awaitingSound = false;
      clearAllLeds();
      Serial.println("DEBUG: Playback completed");
    }
  }
}

void updateActiveLeds() {
  if (!isPlaying || isPaused) return;
  
  unsigned long now = millis();
  unsigned long currentPlayTime = (now - playStartTime - totalPauseTime) * playbackSpeed;
  
  for (int i = 0; i < NUM_LEDS; i++) {
    if (activeLeds[i] == 1) {
      bool shouldTurnOff = false;
      
      // Spegni se il tempo è arrivato a (endTime - noteOffDuration)
      // Questo permette al LED di stare spento per noteOffDuration ms prima che la nota finisca
      unsigned long offTime = ledEndTimes[i] - noteOffDuration;
      if (currentPlayTime >= offTime) {
        shouldTurnOff = true;
      }
      // Spegni se la nota è accesa da più di 4 secondi (4000 ms)
      else if (ledStartTimes[i] > 0 && currentPlayTime >= ledStartTimes[i] + 4000) {
        shouldTurnOff = true;
      }
      
      if (shouldTurnOff) {
        leds[i] = CRGB::Black;
        activeLeds[i] = -1;
        ledOffTimes[i] = now;  // Track when LED was turned off
      }
    }
  }
}

void clearActiveLeds() {
  for (int i = 0; i < NUM_LEDS; i++) {
    activeLeds[i] = -1;
    ledEndTimes[i] = 0;
    ledStartTimes[i] = 0;
    ledOffTimes[i] = 0;
  }
}

void testSingleLed(int ledIndex) {
  clearAllLeds();
  if (ledIndex >= 0 && ledIndex < NUM_LEDS) {
    leds[ledIndex] = CRGB::White;
    FastLED.show();
    delay(500);
    leds[ledIndex] = CRGB::Black;
    FastLED.show();
  }
}

void clearAllLeds() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

void startupAnimation() {
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Green;
    FastLED.show();
    delay(20);
    leds[i] = CRGB::Black;
  }
  FastLED.show();
}

String getMainPage() {
  String html = "";
  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>PIANETHOR - a LED Piano Controller</title>";
  // expose the device boot session id so the client can key ephemeral state to it (resets on device reboot)
  html += "<script>var bootSessionId = " + String(bootSessionId) + ";</script>";
  html += "<style>";
  html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  html += "body { font-family: Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; color: white; padding: 20px; }";
  html += ".container { max-width: 800px; margin: 0 auto; background: rgba(255, 255, 255, 0.1); border-radius: 20px; padding: 30px; }";
  html += "h1 { text-align: center; margin-bottom: 30px; font-size: 2.5em; }";
  html += ".section { background: rgba(255, 255, 255, 0.1); border-radius: 15px; padding: 20px; margin-bottom: 20px; }";
  html += ".section h2 { margin-bottom: 15px; color: #FFD700; font-size: 1.3em; }";
  html += ".controls { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin-bottom: 15px; }";
  html += "button { background: #4ECDC4; border: none; color: white; padding: 12px 20px; border-radius: 10px; cursor: pointer; font-size: 1em; font-weight: bold; transition: all 0.1s; box-shadow: 0 4px 10px rgba(0,0,0,0.3); }";
  html += "button:hover { transform: translateY(-2px); box-shadow: 0 6px 15px rgba(0,0,0,0.4); }";
  html += "button:active { transform: translateY(2px); box-shadow: 0 2px 5px rgba(0,0,0,0.2); }";
  html += "button:disabled { opacity: 0.5; cursor: not-allowed; transform: none; }";
  html += ".play-btn { background: #4CAF50; }";
  html += ".pause-btn { background: #FF9800; }";
  html += ".stop-btn { background: #f44336; }";
  html += ".restart-btn { background: #2196F3; }";
  html += ".play-btn.disabled, .pause-btn.disabled, .stop-btn.disabled, .restart-btn.disabled { background: #999 !important; opacity: 0.5 !important; cursor: not-allowed !important; }";
  html += ".play-btn.active { background: #45a049; }";
  html += ".pause-btn.active { background: #e68900; }";
  html += ".stop-btn.active { background: #da190b; }";
  html += ".restart-btn.active { background: #1976d2; }";
  html += "input[type='range'] { width: 100%; margin: 10px 0; height: 8px; border-radius: 5px; background: #ddd; outline: none; }";
  html += "input[type='color'] { width: 60px; height: 40px; border: none; border-radius: 8px; cursor: pointer; }";
  html += ".file-upload input[type='file'] { display: none; }";
  html += ".file-upload-label { display: block; background: #9C27B0; color: white; padding: 15px 20px; border-radius: 10px; text-align: center; font-weight: bold; cursor: pointer; transition: all 0.3s; }";
  html += ".file-upload-label:hover { background: #7B1FA2; transform: translateY(-2px); }";
  html += ".led-test { display: grid; grid-template-columns: repeat(12, 1fr); gap: 5px; margin-top: 15px; }";
  html += ".led-button { width: 40px; height: 40px; border-radius: 50%; font-size: 0.8em; background: #555; border: none; color: white; cursor: pointer; }";
  html += ".led-button:hover { background: #777; }";
  html += ".status { background: rgba(0, 0, 0, 0.3); padding: 15px; border-radius: 10px; margin-top: 20px; font-family: monospace; }";
  html += ".color-control { display: flex; align-items: center; gap: 10px; margin: 10px 0; }";
  html += ".slider-container { display: flex; align-items: center; gap: 15px; margin: 15px 0; }";
  html += ".slider-value { background: rgba(255, 255, 255, 0.2); padding: 5px 10px; border-radius: 5px; min-width: 60px; text-align: center; }";
  html += ".file-info { background: rgba(0, 255, 0, 0.1); padding: 10px; border-radius: 8px; margin: 10px 0; border-left: 4px solid #4CAF50; }";
  html += ".track-info { background: rgba(255, 165, 0, 0.1); padding: 10px; border-radius: 8px; margin: 10px 0; border-left: 4px solid #FF9800; }";
  html += ".storage-info { background: rgba(255, 165, 0, 0.1); padding: 10px; border-radius: 8px; margin: 10px 0; border-left: 4px solid #FF9800; }";
  html += ".progress-bar { width: 100%; height: 20px; background: rgba(255, 255, 255, 0.2); border-radius: 10px; overflow: hidden; margin: 5px 0; }";
  html += ".progress-fill { height: 100%; background: linear-gradient(90deg, #4CAF50, #8BC34A); transition: width 0.3s; }";
  html += ".track-selector { display: none; }";
  html += ".track-list { display: grid; gap: 10px; margin-top: 15px; }";
  html += ".track-item { background: rgba(255, 255, 255, 0.1); padding: 15px; border-radius: 10px; border: 2px solid transparent; cursor: pointer; transition: all 0.3s; }";
  html += ".track-item:hover { border-color: #4ECDC4; }";
  html += ".track-item.selected { border-color: #4CAF50; background: rgba(76, 175, 80, 0.2); }";
  html += ".track-name { font-weight: bold; margin-bottom: 5px; }";
  html += ".track-stats { font-size: 0.9em; opacity: 0.8; }";
  html += ".toggle-switch { position: relative; display: inline-block; width: 60px; height: 34px; }";
  html += ".toggle-switch input { opacity: 0; width: 0; height: 0; }";
  html += ".slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; transition: 0.4s; border-radius: 34px; }";
  html += ".slider:before { position: absolute; content: ''; height: 26px; width: 26px; left: 4px; bottom: 4px; background-color: white; transition: 0.4s; border-radius: 50%; }";
  html += "input:checked + .slider { background-color: #4CAF50; }";
  html += "input:checked + .slider:before { transform: translateX(26px); }";
  html += ".toggle-switch-large { position: relative; display: inline-block; width: 120px; height: 68px; }";
  html += ".toggle-switch-large input { opacity: 0; width: 0; height: 0; }";
  html += ".toggle-switch-large .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; transition: 0.4s; border-radius: 68px; }";
  html += ".toggle-switch-large .slider:before { position: absolute; content: ''; height: 52px; width: 52px; left: 8px; bottom: 8px; background-color: white; transition: 0.4s; border-radius: 50%; }";
  html += ".toggle-switch-large input:checked + .slider { background-color: #4CAF50; }";
  html += ".toggle-switch-large input:checked + .slider:before { transform: translateX(52px); }";
  html += ".mic-label { display: flex; align-items: center; gap: 15px; margin: 15px 0; font-size: 1.2em; font-weight: bold; }";
  html += ".normal-label { display: flex; align-items: center; gap: 15px; margin: 15px 0; }";
  html += ".loading-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0, 0, 0, 0.8); display: none; z-index: 9999; flex-direction: column; justify-content: center; align-items: center; }";
  html += ".loading-overlay.show { display: flex; }";
  html += ".loading-content { background: rgba(255, 255, 255, 0.1); padding: 40px; border-radius: 20px; text-align: center; max-width: 400px; position: relative; }";
  html += ".loading-close-btn { position: absolute; top: 10px; right: 10px; background: #f44336; border: none; color: white; width: 30px; height: 30px; border-radius: 50%; cursor: pointer; font-size: 18px; display: flex; align-items: center; justify-content: center; }";
  html += ".loading-close-btn:hover { background: #da190b; }";
  html += ".loading-content h2 { margin-bottom: 20px; font-size: 1.5em; }";
  html += ".loading-content .progress-bar { height: 30px; margin: 20px 0; }";
  html += ".loading-content .progress-fill { font-size: 0.9em; line-height: 30px; color: white; font-weight: bold; }";
  html += ".loading-spinner { border: 4px solid rgba(255,255,255,0.3); border-top: 4px solid white; border-radius: 50%; width: 40px; height: 40px; animation: spin 1s linear infinite; margin: 0 auto 20px; }";
  html += "@keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }";
  html += ".controls.disabled { pointer-events: none; opacity: 0.5; }";
  html += ".controls.disabled button { background: #999 !important; cursor: not-allowed !important; }";
  html += ".loading-overlay.show ~ .container .controls button { pointer-events: none; opacity: 0.5; background: #999 !important; }";
  html += ".controls button:disabled { background: #999 !important; opacity: 0.5 !important; cursor: not-allowed !important; }";
  html += ".controls button.disabled { background: #999 !important; opacity: 0.5 !important; cursor: not-allowed !important; }";
  html += ".loading-overlay.show ~ .container .controls button { background: #999 !important; }";
  html += ".error-popup { position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%); background: rgba(244, 67, 54, 0.95); padding: 30px; border-radius: 15px; z-index: 10000; max-width: 400px; box-shadow: 0 10px 40px rgba(0,0,0,0.5); display: none; }";
  html += ".error-popup.show { display: block; }";
  html += ".error-popup h3 { margin-bottom: 15px; font-size: 1.3em; color: white; }";
  html += ".error-popup p { margin-bottom: 20px; color: rgba(255,255,255,0.9); line-height: 1.6; }";
  html += ".error-popup .error-close { background: #FF5722; border: none; color: white; padding: 10px 20px; border-radius: 8px; cursor: pointer; font-weight: bold; }";
  html += ".error-popup .error-close:hover { background: #E64A19; }";
  html += ".epilepsy-warning { display: block; background: rgba(255, 50, 50, 0.2); padding: 10px 0; }";
  html += ".epilepsy-warning.hidden { display: none !important; }";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div id='epilepsyWarning' class='epilepsy-warning'>";
  html += "<div style='background: rgba(255, 50, 50, 0.95); padding: 20px; border-radius: 15px; max-width: 600px; margin: 20px auto;'>";
  html += "<button onclick='closeEpilepsyWarning()' style='float: right; background: #f44336; border: none; color: white; width: 30px; height: 30px; border-radius: 50%; cursor: pointer; font-size: 18px; display: flex; align-items: center; justify-content: center; padding: 0;'>✕</button>";
  html += "<h2 style='color: white; margin-bottom: 10px;'>⚠️ Photosensitive Epilepsy Warning</h2>";
  html += "<p style='color: white; margin-bottom: 10px;'><strong>This application displays rapid LED flashing that may cause seizures in people with photosensitive epilepsy.</strong></p>";
  html += "<ul style='color: white; margin-left: 20px; margin-bottom: 10px;'>";
  html += "<li>Do not use if you have a history of seizures or photosensitivity</li>";
  html += "<li>Stop immediately if you experience any discomfort, dizziness, or visual disturbances</li>";
  html += "<li>Use in a well-lit environment</li>";
  html += "<li>Take regular breaks to prevent eye strain</li>";
  html += "</ul>";
  html += "<p style='color: white;'><strong>By using this application, you acknowledge that you have read and understood this warning.</strong></p>";
  html += "</div>";
  html += "</div>";
  html += "<div class='error-popup' id='errorPopup'>";
  html += "<h3>❌ Error</h3>";
  html += "<p id='errorMessage'></p>";
  html += "<button class='error-close' onclick='closeErrorPopup()'>✕ Close</button>";
  html += "</div>";
  html += "<div class='loading-overlay' id='loadingOverlay'>";
  html += "<div class='loading-content'>";
  html += "<button class='loading-close-btn' onclick='closeLoadingOverlay()'>✕</button>";
  html += "<div class='loading-spinner'></div>";
  html += "<h2>⏳ Please Wait...</h2>";
  html += "<p>Loading tracks...</p>";
  html += "<div class='progress-bar'><div class='progress-fill' id='loadingBar' style='width: 0%;'><span id='loadingPercent'>0%</span></div></div>";
  html += "</div>";
  html += "</div>";
  html += "<div class='container'>";
  html += "<h1>🎹 PIANETHOR - a LED Piano Controller</h1>";
  html += "<div style='text-align: center; margin-top: -20px; margin-bottom: 20px; font-size: 1.8em; opacity: 0.8;'>a free project by battiemme</div>";
  html += "<div style='text-align: center; margin-bottom: 20px;'>";
  html += "</div>";
  
  // Brightness Control
  html += "<div class='section'>";
  html += "<h2>💡 Brightness Control</h2>";
  html += "<div class='slider-container'>";
  html += "<label>Brightness:</label>";
  html += "<input type='range' id='brightness' min='5' max='255' value='128' oninput='setBrightness(this.value)'>";
  html += "<span class='slider-value' id='brightnessValue'>128</span>";
  html += "</div>";
  html += "</div>";
  
  // Note Off Duration Control
  html += "<div class='section'>";
  html += "<h2>⏱️ Note Off Duration</h2>";
  html += "<div class='slider-container'>";
  html += "<label>Note Off Duration (ms):</label>";
  html += "<input type='range' id='noteOffDuration' min='10' max='200' step='10' value='50' oninput='setNoteOffDuration(this.value)'>";
  html += "<span class='slider-value' id='noteOffDurationValue'>50</span>";
  html += "</div>";
  html += "<p style='font-size: 0.9em; opacity: 0.8;'>Time a note must be off before it can be pressed again</p>";
  html += "</div>";
  
  // Microphone Control
  html += "<div class='section'>";
  html += "<h2>🎤 Microphone Control</h2>";
  html += "<p style='font-size: 0.9em; opacity: 0.8;'>Enable microphone to control note progression with volume detection</p>";
  html += "<div class='mic-label'>";
  html += "<label>Enable Microphone:</label>";
  html += "<label class='toggle-switch-large'>";
  html += "<input type='checkbox' id='microphoneToggle' onchange='toggleMicrophone(this.checked)'>";
  html += "<span class='slider'></span>";
  html += "</label>";
  html += "</div>";
  html += "<div class='normal-label'>";
  html += "<label>See Current Volume:</label>";
  html += "<label class='toggle-switch'>";
  html += "<input type='checkbox' id='seeVolumeToggle' onchange='toggleSeeVolume(this.checked)'>";
  html += "<span class='slider'></span>";
  html += "</label>";
  html += "</div>";
  html += "<div id='currentVolume' style='margin-top: 10px; display: none;'>";
  html += "<strong>Current Volume Level:</strong><br>";
  html += "<div style='background: rgba(0,0,0,0.2); padding: 10px; border-radius: 5px; margin: 5px 0;'>";
  html += "<div id='volumeBar' style='background: linear-gradient(90deg, #4CAF50, #f44336); height: 20px; width: 0%; border-radius: 3px;'></div>";
  html += "<span id='currentVolumeValue' style='display: block; text-align: center; margin-top: 5px;'>-</span>";
  html += "</div>";
  html += "</div>";
  html += "<div class='slider-container' style='margin-top: 15px;'>";
  html += "<label>Minimum Volume Threshold (MVT):</label>";
  html += "<input type='number' id='minimumVolumeThreshold' min='0' step='0.1' value='100' style='width: 100px;'>";
  html += "<button onclick='setMinimumVolumeThreshold()'>Set</button>";
  html += "<p style='font-size: 0.85em; opacity: 0.8;'>Volume must drop below this value before rising above Volume Threshold</p>";
  html += "</div>";
  html += "<div class='slider-container' style='margin-top: 15px;'>";
  html += "<label>Volume Threshold (VT):</label>";
  html += "<input type='number' id='volumeThreshold' min='0' step='0.1' value='300' style='width: 100px;'>";
  html += "<button onclick='setThreshold()'>Set</button>";
  html += "<p style='font-size: 0.85em; opacity: 0.8;'>Volume must exceed this threshold to advance to the next note</p>";
  html += "</div>";
  html += "<div class='slider-container' style='margin-top: 15px;'>";
  html += "<label>Threshold Timeout (ms):</label>";
  html += "<input type='number' id='thresholdTimeout' min='50' step='25' value='150' style='width: 100px;'>";
  html += "<button onclick='setThresholdTimeout()'>Set</button>";
  html += "<p style='font-size: 0.85em; opacity: 0.8;'>Minimum time between detection events (prevents false triggers)</p>";
  html += "</div>";
  html += "</div>";
  
  // LED Test
  html += "<div class='section'>";
  html += "<h2>🔧 Individual LED Test</h2>";
  html += "<button onclick='testAllLeds()'>Test All LEDs Sequentially</button>";
  html += "<div class='led-test' id='ledTest'></div>";
  html += "</div>";
  
  // File Management and Track Selection
  html += "<div class='section'>";
  html += "<h2>📁 Music File Management</h2>";
  html += "<div class='file-upload'>";
  html += "<input type='file' id='fileInput' accept='.xml,.mid,.midi,.mxl' onchange='uploadFile()'>";
  html += "<label for='fileInput' class='file-upload-label'>📤 Select MIDI/XML/MXL File</label>";
  html += "</div>";
  html += "<button onclick='deleteFile()' style='margin-top: 10px; background: #f44336;'>🗑️ Delete File</button>";
  html += "<div id='fileInfo' class='file-info' style='display: none;'></div>";
  html += "<div id='trackSelector' class='track-selector'>";
  html += "<div class='track-info'>";
  html += "<h3>🎼 Select Track to Play:</h3>";
  html += "<div id='trackList' class='track-list'></div>";
  html += "</div>";
  html += "</div>";
  html += "<div id='storageInfo' class='storage-info'></div>";
  html += "</div>";
  
  // Color Configuration
  html += "<div class='section'>";
  html += "<h2>🎨 Color Configuration</h2>";
  html += "<div class='color-control'>";
  html += "<label>Track 1:</label>";
  html += "<input type='color' id='leftHandColor' value='#ff0000' onchange='updateColors()'>";
  html += "</div>";
  html += "<div class='color-control'>";
  html += "<label>Track 2:</label>";
  html += "<input type='color' id='rightHandColor' value='#0000ff' onchange='updateColors()'>";
  html += "</div>";
  html += "</div>";
  
  // Track Transposition
  html += "<div class='section'>";
  html += "<h2>🎹 Track Transposition</h2>";
  html += "<div class='slider-container'>";
  html += "<label>Track 1 (octaves):</label>";
  html += "<input type='range' id='transpose1' min='-4' max='4' step='1' value='0' oninput='setTranspose(1, this.value)'>";
  html += "<span class='slider-value' id='transpose1Value'>0</span>";
  html += "</div>";
  html += "<div class='slider-container'>";
  html += "<label>Track 2 (octaves):</label>";
  html += "<input type='range' id='transpose2' min='-4' max='4' step='1' value='0' oninput='setTranspose(2, this.value)'>";
  html += "<span class='slider-value' id='transpose2Value'>0</span>";
  html += "</div>";
  html += "</div>";
  
  // Playback Controls
  html += "<div class='section'>";
  html += "<h2>🎵 Playback Controls</h2>";
  html += "<div class='controls'>";
  html += "<button class='play-btn' onclick='play()' id='playBtn'>▶️ PLAY</button>";
  html += "<button class='pause-btn' onclick='pause()'>⏸️ PAUSE</button>";
  html += "<button class='stop-btn' onclick='stop()'>⏹️ STOP</button>";
  html += "<button class='restart-btn' onclick='restart()'>🔄 RESTART</button>";
  html += "</div>";
  
  html += "<div style='height: 10px;'></div>";
  
  html += "<div style='height: 10px;'></div>";
  
  html += "<div class='slider-container'>";
  html += "<label>Speed:</label>";
  html += "<input type='range' id='speed' min='10' max='150' step='5' value='75' oninput='setSpeed(this.value)'>";
  html += "<span class='slider-value' id='speedValue'>75%</span>";
  html += "<input type='number' id='speedInput' min='10' max='100' value='75' style='width: 60px; margin-left: 10px;' onchange='setSpeedManual(this.value)'>";
  html += "</div>";
  
  html += "<div style='height: 10px;'></div>";

  html += "<div id='playbackProgress' class='progress-bar' style='display: none;'>";
  html += "<div id='progressFill' class='progress-fill' style='width: 0%;'></div>";
  html += "</div>";
  html += "<div class='slider-container' style='margin-top: 15px;'>";
  html += "<label>Track Position:</label>";
  html += "<input type='range' id='position' min='0' max='100' value='0' oninput='setPosition(this.value)' disabled>";
  html += "<span class='slider-value' id='positionValue'>0%</span>";
  html += "</div>";
  html += "</div>";
  
  // Status
  html += "<div class='section'>";
  html += "<h2>📊 System Status</h2>";
  html += "<div class='status' id='status'>Loading status...</div>";
  html += "</div>";
  
  // Reset Controls
  html += "<div class='section'>";
  html += "<h2>🔄 Reset Controls</h2>";
  html += "<button onclick='resetToDefaults()' style='background: #FF5722; width: 100%;'>⚙️ Reset All to Default</button>";
  html += "</div>";
  
  // Debug Mode (moved to bottom)
  html += "<div class='section'>";
  html += "<h2>🐛 Debug Mode</h2>";
  html += "<div class='normal-label'>";
  html += "<label>Enable Debug Mode:</label>";
  html += "<label class='toggle-switch'>";
  html += "<input type='checkbox' id='debugToggle' onchange='toggleDebug(this.checked)'>";
  html += "<span class='slider'></span>";
  html += "</label>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  // JavaScript
  html += "<script>";
  html += "let selectedTrackIndex = -1;";
  html += "let selectedTrackIndex2 = -1;";
  html += "let trackTranspose1 = 0;";
  html += "let trackTranspose2 = 0;";
  html += "let availableTracks = [];";
  html += "let microphoneEnabled = false;";
  html += "let seeVolumeEnabled = false;";
  html += "let savedLeftColor = '#ff0000';";
  html += "let savedRightColor = '#0000ff';";
  html += "";
  html += "function getGMInstrumentName(programNumber) {";
  html += "var instruments = [";
  html += "'Acoustic Piano','Bright Piano','Electric Piano','Honky-tonk Piano',";
  html += "'Electric Piano','Electric Piano','Harpsichord','Clavi',";
  html += "'Celesta','Glockenspiel','Music Box','Vibraphone',";
  html += "'Marimba','Xylophone','Tubular Bells','Dulcimer',";
  html += "'Drawbar Organ','Percussive Organ','Rock Organ','Church Organ',";
  html += "'Reed Organ','Accordion','Harmonica','Bandneon',";
  html += "'Nylon Acoustic Guitar','Steel Acoustic Guitar','Jazz Electric Guitar','Clean Electric Guitar',";
  html += "'Muted Electric Guitar','Overdriven Guitar','Distorted Guitar','Guitar Harmonics',";
  html += "'Acoustic Bass','Fingered Electric Bass','Picked Electric Bass','Fretless Bass',";
  html += "'Slap Bass 1','Slap Bass 2','Synth Bass 1','Synth Bass 2',";
  html += "'Violin','Viola','Cello','Contrabass',";
  html += "'Tremolo Strings','Pizzicato Strings','Orchestral Harp','Timpani',";
  html += "'String Ensemble 1','String Ensemble 2','Synth Strings 1','Synth Strings 2',";
  html += "'Choir Aahs','Choir Oohs','Synth Choir','Orchestra Hit',";
  html += "'Trumpet','Trombone','Tuba','Muted Trumpet',";
  html += "'French Horn','Brass Section','Synth Brass 1','Synth Brass 2',";
  html += "'Soprano Saxophone','Alto Saxophone','Tenor Saxophone','Baritone Saxophone',";
  html += "'Oboe','English Horn','Bassoon','Clarinet',";
  html += "'Piccolo','Flute','Recorder','Pan Flute',";
  html += "'Blown Bottle','Shakuhachi','Whistle','Ocarina',";
  html += "'Lead Square','Lead Sawtooth','Lead Calliope','Lead Chiff',";
  html += "'Lead Charang','Lead Voice','Lead Fifths','Lead Bass',";
  html += "'Pad New Age','Pad Warm','Pad Polysynth','Pad Choir',";
  html += "'Pad Bowed','Pad Metallic','Pad Halo','Pad Sweep',";
  html += "'FX Rain','FX Soundtrack','FX Crystal','FX Atmosphere',";
  html += "'FX Brightness','FX Goblins','FX Echoes','FX Sci-Fi',";
  html += "'Sitar','Banjo','Shamisen','Koto',";
  html += "'Kalimba','Bagpipe','Fiddle','Shanai',";
  html += "'Tinkle Bell','Agogo','Steel Drums','Woodblock',";
  html += "'Taiko Drum','Melodic Drum','Synth Drum','Reverse Cymbal',";
  html += "'Guitar Fret Noise','Breath Noise','Seashore','Bird Tweet',";
  html += "'Telephone Ring','Helicopter','Applause','Gunshot'";
  html += "];";
  html += "if (programNumber < 0 || programNumber > 127) return 'Unknown';";
  html += "return instruments[programNumber];";
  html += "}";
  html += "";
  html += "function loadColorsFromStorage() {";
  html += "var left = localStorage.getItem('leftHandColor');";
  html += "var right = localStorage.getItem('rightHandColor');";
  html += "if (left) { savedLeftColor = left; document.getElementById('leftHandColor').value = left; }";
  html += "if (right) { savedRightColor = right; document.getElementById('rightHandColor').value = right; }";
  html += "updateColors();";
  html += "}";
  html += "";
  html += "function closeEpilepsyWarning() {";
  html += "var warning = document.getElementById('epilepsyWarning');";
  html += "warning.classList.add('hidden');";
  html += "try { localStorage.setItem('epilepsyWarningClosed_' + bootSessionId, 'true'); } catch(e) {}";
  html += "}";
  html += "";
  html += "function checkEpilepsyWarning() {";
  html += "var warning = document.getElementById('epilepsyWarning');";
  html += "if (warning && !warning.classList.contains('hidden')) {";
  html += "var isClosed = null; try { isClosed = localStorage.getItem('epilepsyWarningClosed_' + bootSessionId); } catch(e) { isClosed = null; }";
  html += "if (isClosed === 'true') { warning.classList.add('hidden'); }";
  html += "}";
  html += "}";
  html += "";
  html += "function closeLoadingOverlay() {";
  html += "var loadingOverlay = document.getElementById('loadingOverlay');";
  html += "loadingOverlay.classList.remove('show');";
  html += "}";
  html += "";
  html += "function showErrorPopup(message) {";
  html += "var errorPopup = document.getElementById('errorPopup');";
  html += "document.getElementById('errorMessage').textContent = message;";
  html += "errorPopup.classList.add('show');";
  html += "}";
  html += "";
  html += "function closeErrorPopup() {";
  html += "var errorPopup = document.getElementById('errorPopup');";
  html += "errorPopup.classList.remove('show');";
  html += "}";
  html += "";
  html += "function loadStateFromStorage() {";
  html += "var savedDebug = localStorage.getItem('debugMode');";
  html += "if (savedDebug === 'true') {";
  html += "document.getElementById('debugToggle').checked = true;";
  html += "debugMode = true;";
  html += "}";
  html += "var savedMicrophone = localStorage.getItem('microphoneEnabled');";
  html += "if (savedMicrophone === 'true') {";
  html += "document.getElementById('microphoneToggle').checked = true;";
  html += "microphoneEnabled = true;";
  html += "}";
  html += "var savedSeeVolume = localStorage.getItem('seeVolumeEnabled');";
  html += "if (savedSeeVolume === 'true') {";
  html += "document.getElementById('seeVolumeToggle').checked = true;";
  html += "seeVolumeEnabled = true;";
  html += "}";
  html += "var savedTrack1 = localStorage.getItem('selectedTrackIndex');";
  html += "if (savedTrack1) {";
  html += "selectedTrackIndex = parseInt(savedTrack1);";
  html += "}";
  html += "var savedTrack2 = localStorage.getItem('selectedTrackIndex2');";
  html += "if (savedTrack2) {";
  html += "selectedTrackIndex2 = parseInt(savedTrack2);";
  html += "}";
  html += "var savedThreshold = localStorage.getItem('volumeThreshold');";
  html += "if (savedThreshold) {";
  html += "document.getElementById('volumeThreshold').value = savedThreshold;";
  html += "}";
  html += "var savedMinimumThreshold = localStorage.getItem('minimumVolumeThreshold');";
  html += "if (savedMinimumThreshold) {";
  html += "document.getElementById('minimumVolumeThreshold').value = savedMinimumThreshold;";
  html += "}";
  html += "var savedTimeout = localStorage.getItem('thresholdTimeout');";
  html += "if (savedTimeout) {";
  html += "document.getElementById('thresholdTimeout').value = savedTimeout;";
  html += "}";
  html += "}";
  html += "";
  html += "function saveColorsToStorage() {";
  html += "localStorage.setItem('leftHandColor', savedLeftColor);";
  html += "localStorage.setItem('rightHandColor', savedRightColor);";
  html += "}";
  
  html += "function toggleDebug(enabled) {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setDebug');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('enabled=' + (enabled ? 'true' : 'false'));";
  html += "localStorage.setItem('debugMode', enabled);";
  html += "};";
  
  html += "function toggleMicrophone(enabled) {";
  html += "microphoneEnabled = enabled;";
  html += "document.getElementById('microphoneToggle').checked = enabled;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setMicrophone');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "localStorage.setItem('microphoneEnabled', enabled);";
  html += "console.log('Microphone ' + (enabled ? 'enabled' : 'disabled'));";
  html += "} else {";
  html += "microphoneEnabled = !enabled;";
  html += "document.getElementById('microphoneToggle').checked = !enabled;";
  html += "localStorage.setItem('microphoneEnabled', !enabled);";
  html += "console.log('Error toggling microphone');";
  html += "}";
  html += "updateStatus();";
  html += "};";
  html += "xhr.onerror = function() {";
  html += "microphoneEnabled = !enabled;";
  html += "document.getElementById('microphoneToggle').checked = !enabled;";
  html += "console.log('Network error toggling microphone');";
  html += "};";
  html += "xhr.send('enabled=' + (enabled ? 'true' : 'false'));";
  html += "};";
  
  html += "function toggleSeeVolume(enabled) {";
  html += "seeVolumeEnabled = enabled;";
  html += "var currentVolume = document.getElementById('currentVolume');";
  html += "if (enabled && microphoneEnabled) {";
  html += "currentVolume.style.display = 'block';";
  html += "document.getElementById('seeVolumeToggle').checked = true;";
  html += "console.log('Volume display enabled');";
  html += "} else {";
  html += "currentVolume.style.display = 'none';";
  html += "document.getElementById('seeVolumeToggle').checked = false;";
  html += "if (!microphoneEnabled) console.log('Cannot enable volume display: microphone not enabled');";
  html += "}";
  html += "localStorage.setItem('seeVolumeEnabled', enabled);";
  html += "};";

  html += "function startCalibration() {";
  html += "document.getElementById('calibrateBtn').disabled = true;";
  html += "document.getElementById('calibrationResults').style.display = 'block';";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/startCalibration');";
  html += "xhr.send();";
  html += "checkCalibration();";
  html += "};";
  
  html += "function checkCalibration() {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "var data = JSON.parse(xhr.responseText);";
  html += "document.getElementById('peakVolume').textContent = data.peak.toFixed(1);";
  html += "document.getElementById('vol05s').textContent = data.s05.toFixed(1);";
  html += "document.getElementById('vol1s').textContent = data.s1.toFixed(1);";
  html += "if (data.isCalibrating) {";
  html += "setTimeout(checkCalibration, 50);";
  html += "} else {";
  html += "document.getElementById('calibrateBtn').disabled = false;";
  html += "if (data.peak > 0) {";
  html += "document.getElementById('volumeThreshold').value = Math.round(data.peak * 0.7);";
  html += "setThreshold();";
  html += "}";
  html += "}";
  html += "}";
  html += "};";
  html += "xhr.open('GET', '/getCalibration');";
  html += "xhr.send();";
  html += "};";
  
  html += "function setThreshold() {";
  html += "var threshold = document.getElementById('volumeThreshold').value;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setThreshold');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "localStorage.setItem('volumeThreshold', threshold);";
  html += "alert('Volume Threshold set to ' + threshold);";
  html += "document.getElementById('seeVolumeToggle').checked = false;";
  html += "toggleSeeVolume(false);";
  html += "}";
  html += "};";
  html += "xhr.send('threshold=' + threshold);";
  html += "}";
  html += "";
  html += "function setMinimumVolumeThreshold() {";
  html += "var minimumThreshold = document.getElementById('minimumVolumeThreshold').value;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setMinimumVolumeThreshold');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "localStorage.setItem('minimumVolumeThreshold', minimumThreshold);";
  html += "alert('Minimum Volume Threshold set to ' + minimumThreshold);";
  html += "document.getElementById('seeVolumeToggle').checked = false;";
  html += "toggleSeeVolume(false);";
  html += "}";
  html += "};";
  html += "xhr.send('minimumThreshold=' + minimumThreshold);";
  html += "}";
  html += "";
  html += "function setThresholdTimeout() {";
  html += "var timeout = document.getElementById('thresholdTimeout').value;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setThresholdTimeout');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "localStorage.setItem('thresholdTimeout', timeout);";
  html += "alert('Threshold timeout set to ' + timeout + ' ms');";
  html += "}";
  html += "};";
  html += "xhr.send('timeout=' + timeout);";
  html += "}";
  html += "";
  html += "function setPosition(value) {";
  html += "document.getElementById('positionValue').textContent = value + '%';";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setPosition');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('position=' + value);";
  html += "if (typeof sendDebug !== 'undefined' && sendDebug) fetch('/setDebug?msg=Pressed%20slider%20Track%20Position%20:%20' + value + '%');";
  html += "}";
  
  html += "function setTranspose(track, value) {";
  html += "document.getElementById('transpose' + track + 'Value').textContent = value;";
  html += "if (track == 1) trackTranspose1 = value;";
  html += "else trackTranspose2 = value;";
  html += "var sliderName = (track == 1) ? 'Track 1' : 'Track 2';";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setTranspose');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('track=' + track + '&transpose=' + value);";
  html += "console.log('Pressed slider Track Transposition : ' + sliderName + ' = ' + value + ' octaves');";
  html += "}";
  
  html += "function generateLedButtons() {";
  html += "var ledTest = document.getElementById('ledTest');";
  html += "for (var i = 0; i < 72; i++) {";
  html += "var button = document.createElement('button');";
  html += "button.className = 'led-button';";
  html += "button.textContent = i;";
  html += "button.setAttribute('onclick', 'testLed(' + i + ')');";
  html += "ledTest.appendChild(button);";
  html += "}";
  html += "}";
  
  html += "function setBrightness(value) {";
  html += "document.getElementById('brightnessValue').textContent = value;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setBrightness');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('brightness=' + value);";
  html += "console.log('Pressed slider Brightness Control : ' + value);";
  html += "}";
  html += "";
  html += "function setNoteOffDuration(value) {";
  html += "document.getElementById('noteOffDurationValue').textContent = value;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setNoteOffDuration');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('duration=' + value);";
  html += "console.log('Note Off Duration set to: ' + value + ' ms');";
  html += "}";
  
  html += "function testLed(index) {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/testLed');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('index=' + index);";
  html += "}";
  
  html += "function testAllLeds() {";
  html += "var index = 0;";
  html += "function testNext() {";
  html += "if (index < 72) { testLed(index); index++; setTimeout(testNext, 100); }";
  html += "}";
  html += "testNext();";
  html += "}";
  
  html += "function uploadFile() {";
  html += "var fileInput = document.getElementById('fileInput');";
  html += "var file = fileInput.files[0];";
  html += "if (!file) return;";
  html += "var formData = new FormData();";
  html += "formData.append('file', file);";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) { ";
  html += "alert('File uploaded successfully!'); ";
  html += "selectedTrackIndex = -1;";
  html += "selectedTrackIndex2 = -1;";
  html += "document.getElementById('trackSelector').style.display = 'none';";
  html += "document.getElementById('fileInfo').style.display = 'none';";
  html += "document.getElementById('fileInfo').innerHTML = '';";
  html += "document.getElementById('playBtn').disabled = true;";
  html += "setTimeout(checkForTracks, 1000);";
  html += "setTimeout(updateStatus, 1500);";
  html += "} else { alert('Error uploading file'); }";
  html += "};";
  html += "xhr.open('POST', '/upload');";
  html += "xhr.send(formData);";
  html += "}";
  
  html += "function checkForTracks() {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "var data = JSON.parse(xhr.responseText);";
  html += "console.log('checkForTracks response:', data);";
  html += "console.log('Number of tracks received:', data.tracks ? data.tracks.length : 0);";
  html += "if (data.tracks && data.tracks.length > 0) {";
  html += "if (data.hasAnalyzed && data.tracks.length > 1) {";
  html += "console.log('Showing track selector...');";
  html += "showTrackSelector(data.tracks);";
  html += "} else if (data.tracks.length === 1) {";
  html += "console.log('Single track, auto-loading');";
  html += "document.getElementById('fileInfo').innerHTML = '<strong>📄 File Loaded:</strong> ' + data.fileName + '<br><strong>🎼 Track loaded automatically</strong>';";
  html += "document.getElementById('fileInfo').style.display = 'block';";
  html += "} else if (data.tracks.length > 1 && !data.hasAnalyzed) {";
  html += "console.log('Multiple tracks available, showing selector');";
  html += "showTrackSelector(data.tracks);";
  html += "}";
  html += "}";
  html += "}";
  html += "};";
  html += "xhr.open('GET', '/analyzeFile');";
  html += "xhr.send();";
  html += "}";
  
  html += "function showTrackSelector(tracks) {";
  html += "availableTracks = tracks;";
  html += "var trackList = document.getElementById('trackList');";
  html += "trackList.innerHTML = '';";
  html += "var fileInfo = document.getElementById('fileInfo');";
  html += "if (fileInfo && fileInfo.innerHTML) {";
  html += "trackList.innerHTML = '<div class=\"file-info\" style=\"margin-bottom:20px\">' + fileInfo.innerHTML + '</div>';";
  html += "}";
  html += "trackList.innerHTML += '<h4>Track 1 (' + tracks.length + ' available):</h4>';";
  html += "for (var i = 0; i < tracks.length; i++) {";
  html += "var track = tracks[i];";
  html += "if (track.hasNotes) {";
  html += "var trackItem = document.createElement('div');";
  html += "trackItem.className = 'track-item';";
  html += "trackItem.setAttribute('data-track-index', track.index);";
  html += "trackItem.setAttribute('data-track-slot', '1');";
  html += "trackItem.setAttribute('onclick', 'selectTrack(' + track.index + ', 1)');";
  html += "var instrumentName = track.programNumber >= 0 ? getGMInstrumentName(track.programNumber) : 'Not detected';";
  html += "trackItem.innerHTML = '<div class=\"track-name\">🎼 ' + track.name + '</div>';";
  html += "trackItem.innerHTML += '<div class=\"track-stats\">Notes: ' + track.noteCount + ' | Range: MIDI ' + track.minNote + '-' + track.maxNote + '</div>';";
  html += "trackItem.innerHTML += '<div class=\"track-stats\">Instrument: ' + instrumentName + '</div>';";
  html += "trackList.appendChild(trackItem);";
  html += "}";
  html += "}";
  html += "if (tracks.length > 1) {";
  html += "trackList.innerHTML += '<br><h4>Track 2 (optional):</h4>';";
  html += "var noneItem = document.createElement('div');";
  html += "noneItem.className = 'track-item';";
  html += "noneItem.setAttribute('data-track-index', '-1');";
  html += "noneItem.setAttribute('data-track-slot', '2');";
  html += "noneItem.setAttribute('onclick', 'selectTrack(-1, 2)');";
  html += "noneItem.innerHTML = '<div class=\"track-name\">❌ None</div>';";
  html += "trackList.appendChild(noneItem);";
  html += "for (var j = 0; j < tracks.length; j++) {";
  html += "var track2 = tracks[j];";
  html += "if (track2.hasNotes) {";
  html += "var trackItem2 = document.createElement('div');";
  html += "trackItem2.className = 'track-item';";
  html += "trackItem2.setAttribute('data-track-index', track2.index);";
  html += "trackItem2.setAttribute('data-track-slot', '2');";
  html += "trackItem2.setAttribute('onclick', 'selectTrack(' + track2.index + ', 2)');";
  html += "var instrumentName2 = track2.programNumber >= 0 ? getGMInstrumentName(track2.programNumber) : 'Not detected';";
  html += "trackItem2.innerHTML = '<div class=\"track-name\">🎼 ' + track2.name + '</div>';";
  html += "trackItem2.innerHTML += '<div class=\"track-stats\">Notes: ' + track2.noteCount + ' | Range: MIDI ' + track2.minNote + '-' + track2.maxNote + '</div>';";
  html += "trackItem2.innerHTML += '<div class=\"track-stats\">Instrument: ' + instrumentName2 + '</div>';";
  html += "trackList.appendChild(trackItem2);";
  html += "}";
  html += "}";
  html += "}";
  html += "document.getElementById('trackSelector').style.display = 'block';";
  html += "document.getElementById('playBtn').disabled = true;";
  html += "updateTrackSelectionUI();";
  html += "}";
  
  html += "function selectTrack(trackIndex, trackSlot) {";
  html += "if (trackSlot == 1) { selectedTrackIndex = trackIndex; localStorage.setItem('selectedTrackIndex', trackIndex); }";
  html += "else { selectedTrackIndex2 = trackIndex; localStorage.setItem('selectedTrackIndex2', trackIndex); }";
  html += "updateTrackSelection(trackIndex, trackSlot);";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "alert('Track ' + trackSlot + ' selected!');";
  html += "document.getElementById('playBtn').disabled = false;";
  html += "updateStatus();";
  html += "} else {";
  html += "showErrorPopup(xhr.responseText);";
  html += "}";
  html += "};";
  html += "xhr.open('POST', '/selectTrack');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('trackIndex=' + trackIndex + '&trackSlot=' + trackSlot);";
  html += "}";
  
  html += "function updateTrackSelection(trackIndex, trackSlot) {";
  html += "var trackItems = document.querySelectorAll('.track-item');";
  html += "trackItems.forEach(function(item) {";
  html += "var itemTrackIndex = parseInt(item.getAttribute('data-track-index'));";
  html += "var itemTrackSlot = parseInt(item.getAttribute('data-track-slot'));";
  html += "if (itemTrackSlot === trackSlot && itemTrackIndex === trackIndex) {";
  html += "item.classList.add('selected');";
  html += "} else if (itemTrackSlot === trackSlot) {";
  html += "item.classList.remove('selected');";
  html += "}";
  html += "});";
  html += "updateTrackSelectionUI();";
  html += "}";
  html += "";
  html += "function updateTrackSelectionUI() {";
  html += "var trackItems = document.querySelectorAll('[data-track-slot=\"2\"]');";
  html += "var track1SelectedItem = document.querySelector('[data-track-slot=\"1\"].selected');";
  html += "var selectedTrackIndexFromUI = track1SelectedItem ? parseInt(track1SelectedItem.getAttribute('data-track-index')) : -2;";
  html += "trackItems.forEach(function(item) {";
  html += "var itemTrackIndex = parseInt(item.getAttribute('data-track-index'));";
  html += "if (selectedTrackIndexFromUI >= 0 && itemTrackIndex === selectedTrackIndexFromUI) {";
  html += "item.style.opacity = '0.5';";
  html += "item.style.cursor = 'not-allowed';";
  html += "item.style.pointerEvents = 'none';";
  html += "item.classList.add('disabled');";
  html += "} else {";
  html += "item.style.opacity = '1';";
  html += "item.style.cursor = 'pointer';";
  html += "item.style.pointerEvents = 'auto';";
  html += "item.classList.remove('disabled');";
  html += "}";
  html += "});";
  html += "}";
  
  html += "function deleteFile() {";
  html += "console.log('Pressed button Delete File : DELETE');";
  html += "if (confirm('Are you sure you want to delete the file?')) {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() { ";
  html += "alert(xhr.responseText); ";
  html += "document.getElementById('trackSelector').style.display = 'none';";
  html += "document.getElementById('fileInfo').style.display = 'none';";
  html += "document.getElementById('playBtn').disabled = false;";
  html += "updateStatus(); ";
  html += "};";
  html += "xhr.open('POST', '/deleteFile');";
  html += "xhr.send();";
  html += "}";
  html += "}";
  
  html += "function updateColors() {";
  html += "var leftColor = document.getElementById('leftHandColor').value;";
  html += "var rightColor = document.getElementById('rightHandColor').value;";
  html += "savedLeftColor = leftColor;";
  html += "savedRightColor = rightColor;";
  html += "saveColorsToStorage();";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setColors');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('leftHand=' + leftColor + '&rightHand=' + rightColor);";
  html += "var debugMsg = 'Updated Colors - Track 1: ' + leftColor + ', Track 2: ' + rightColor;";
  html += "console.log(debugMsg);";
  html += "}";
  
  html += "function play() {";
  html += "console.log('Pressed button Playback Controls : PLAY');";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() { ";
  html += "if (xhr.status === 200) {";
  html += "updateStatus();";
  html += "document.getElementById('playbackProgress').style.display = 'block';";
  html += "} else { alert(xhr.responseText); }";
  html += "};";
  html += "xhr.open('POST', '/play');";
  html += "xhr.send();";
  html += "}";
  
  html += "function pause() {";
  html += "console.log('Pressed button Playback Controls : PAUSE');";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() { updateStatus(); };";
  html += "xhr.open('POST', '/pause');";
  html += "xhr.send();";
  html += "}";
  
  html += "function stop() {";
  html += "console.log('Pressed button Playback Controls : STOP');";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() { ";
  html += "updateStatus();";
  html += "document.getElementById('playbackProgress').style.display = 'none';";
  html += "};";
  html += "xhr.open('POST', '/stop');";
  html += "xhr.send();";
  html += "}";
  
  html += "function restart() {";
  html += "console.log('Pressed button Playback Controls : RESTART');";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() { updateStatus(); };";
  html += "xhr.open('POST', '/restart');";
  html += "xhr.send();";
  html += "}";
  
  html += "function setSpeed(value) {";
  html += "document.getElementById('speedValue').textContent = value + '%';";
  html += "document.getElementById('speedInput').value = value;";
  html += "var speedFloat = value / 100.0;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() { updateStatus(); };";
  html += "xhr.open('POST', '/setSpeed');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('speed=' + speedFloat);";
  html += "console.log('Pressed slider Playback Controls : Speed = ' + value + '%');";
  html += "}";
  
  html += "function setSpeedManual(value) {";
  html += "if (value < 10) value = 10;";
  html += "if (value > 150) value = 150;";
  html += "document.getElementById('speed').value = value;";
  html += "document.getElementById('speedValue').textContent = value + '%';";
  html += "var speedFloat = value / 100.0;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setSpeed');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('speed=' + speedFloat);";
  html += "}";
  
  html += "function setNoteDuration(value) {";
  html += "document.getElementById('noteDurationValue').textContent = value + '%';";
  html += "document.getElementById('noteDurationInput').value = value;";
  html += "var durationFloat = value / 100.0;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setNoteDuration');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('duration=' + durationFloat);";
  html += "}";
  
  html += "function setNoteDurationManual(value) {";
  html += "if (value < 25) value = 25;";
  html += "if (value > 200) value = 200;";
  html += "document.getElementById('noteDuration').value = value;";
  html += "document.getElementById('noteDurationValue').textContent = value + '%';";
  html += "var durationFloat = value / 100.0;";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.open('POST', '/setNoteDuration');";
  html += "xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
  html += "xhr.send('duration=' + durationFloat);";
  html += "}";
  
  html += "function updateStatus() {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "var data = JSON.parse(xhr.responseText);";
  html += "var currentVolume = document.getElementById('currentVolume');";
  html += "var loadingOverlay = document.getElementById('loadingOverlay');";
  html += "var allButtons = document.querySelectorAll('.controls button');";
  html += "var playBtn = document.getElementById('playBtn');";
  html += "var pauseBtn = document.querySelectorAll('.pause-btn')[0];";
  html += "var stopBtn = document.querySelectorAll('.stop-btn')[0];";
  html += "var restartBtn = document.querySelectorAll('.restart-btn')[0];";
  html += "if (data.isLoading) {";
  html += "loadingOverlay.classList.add('show');";
  html += "allButtons.forEach(function(btn) { btn.disabled = true; btn.classList.add('disabled'); });";
  html += "var progress = data.loadingProgress || 0;";
  html += "document.getElementById('loadingBar').style.width = progress + '%';";
  html += "document.getElementById('loadingPercent').textContent = progress + '%';";
  html += "} else {";
  html += "loadingOverlay.classList.remove('show');";
  html += "allButtons.forEach(function(btn) { btn.classList.remove('disabled'); });";
  html += "if (data.hasFile) {";
  html += "allButtons.forEach(function(btn) { btn.disabled = false; });";
  html += "} else {";
  html += "allButtons.forEach(function(btn) { btn.disabled = true; });";
  html += "}";
  html += "}";
  html += "if (data.speed) {";
  html += "var speedPercent = Math.round(data.speed * 100);";
  html += "document.getElementById('speed').value = speedPercent;";
  html += "document.getElementById('speedValue').textContent = speedPercent + '%';";
  html += "document.getElementById('speedInput').value = speedPercent;";
  html += "}";
  html += "if (data.brightness) {";
  html += "document.getElementById('brightness').value = data.brightness;";
  html += "document.getElementById('brightnessValue').textContent = data.brightness;";
  html += "}";
  html += "if (microphoneEnabled && seeVolumeEnabled) {";
  html += "currentVolume.style.display = 'block';";
  html += "var volumeBar = document.getElementById('volumeBar');";
  html += "var volumeValue = document.getElementById('currentVolumeValue');";
  html += "var volume = data.currentVolume || 0;";
  html += "var maxVolume = 1000;";
  html += "var percentage = Math.min((volume / maxVolume) * 100, 100);";
  html += "volumeBar.style.width = percentage + '%';";
  html += "volumeValue.textContent = volume.toFixed(1);";
  html += "} else {";
  html += "currentVolume.style.display = 'none';";
  html += "}";
  html += "var status = document.getElementById('status');";
  html += "var playStatus = data.isPlaying ? (data.isPaused ? '⏸️ Paused' : '▶️ Playing') : '⏹️ Stopped';";
  html += "status.innerHTML = '<strong>Status:</strong> ' + playStatus + '<br>';";
  html += "if (data.isPlaying && !data.isPaused) {";
  html += "playBtn.classList.add('disabled');";
  html += "} else {";
  html += "playBtn.classList.remove('disabled');";
  html += "}";
  html += "if (data.isPaused) {";
  html += "pauseBtn.classList.add('disabled');";
  html += "} else {";
  html += "pauseBtn.classList.remove('disabled');";
  html += "}";
  html += "status.innerHTML += '<strong>Brightness:</strong> ' + data.brightness + '/255<br>';";
  html += "var speedPercent = Math.round(data.speed * 100);";
  html += "status.innerHTML += '<strong>Speed:</strong> ' + speedPercent + '%<br>';";
  html += "if (data.totalNotes > 0) {";
  html += "status.innerHTML += '<strong>Progress:</strong> ' + data.currentNote + '/' + data.totalNotes + ' notes<br>';";
  html += "var progress = (data.currentNote / data.totalNotes) * 100;";
  html += "document.getElementById('progressFill').style.width = progress + '%';";
  html += "}";
  html += "if (data.selectedTrack >= 0) {";
  html += "status.innerHTML += '<strong>Selected Track:</strong> ' + data.selectedTrack + '<br>';";
  html += "}";
  html += "status.innerHTML += '<strong>Microphone:</strong> ' + (data.microphoneEnabled ? 'Enabled' : 'Disabled') + '<br>';";
  html += "status.innerHTML += '<strong>Available Tracks:</strong> ' + data.availableTracks + '<br>';";
  html += "status.innerHTML += '<strong>Available Notes to play:</strong> ' + data.availableNotes + '<br>';";
  html += "var positionSlider = document.getElementById('position');";
  html += "if (data.hasFile && data.totalNotes > 0) {";
  html += "positionSlider.disabled = false;";
  html += "var currentPos = Math.round((data.currentNote / data.totalNotes) * 100);";
  html += "if (positionSlider.value != currentPos) {";
  html += "positionSlider.value = currentPos;";
  html += "document.getElementById('positionValue').textContent = currentPos + '%';";
  html += "}";
  html += "} else {";
  html += "positionSlider.disabled = true;";
  html += "}";
  html += "var fileInfo = document.getElementById('fileInfo');";
  html += "if (data.hasFile && data.fileName) {";
  html += "fileInfo.style.display = 'block';";
  html += "fileInfo.innerHTML = '<strong>📄 File Loaded:</strong> ' + data.fileName + '<br>';";
  html += "fileInfo.innerHTML += '<strong>🎵 Total Notes:</strong> ' + data.totalNotes;";
  html += "if (data.selectedTrack >= 0) {";
  html += "fileInfo.innerHTML += '<br><strong>🎼 Track:</strong> ' + data.selectedTrack;";
  html += "}";
  html += "} else {";
  html += "fileInfo.style.display = 'none';";
  html += "}";
  html += "var storageInfo = document.getElementById('storageInfo');";
  html += "if (data.storageTotal) {";
  html += "var usedKB = Math.round(data.storageUsed / 1024);";
  html += "var totalKB = Math.round(data.storageTotal / 1024);";
  html += "var availableKB = Math.round(data.storageAvailable / 1024);";
  html += "var usagePercent = (data.storageUsed / data.storageTotal) * 100;";
  html += "storageInfo.innerHTML = '<strong>💾 Storage Space:</strong><br>';";
  html += "storageInfo.innerHTML += 'Used: ' + usedKB + ' KB / ' + totalKB + ' KB<br>';";
  html += "storageInfo.innerHTML += 'Available: ' + availableKB + ' KB<br>';";
  html += "storageInfo.innerHTML += '<div class=\"progress-bar\"><div class=\"progress-fill\" style=\"width: ' + usagePercent + '%;\"></div></div>';";
  html += "}";
  html += "}";
  html += "};";
  html += "xhr.open('GET', '/status');";
  html += "xhr.send();";
  html += "}";
  
  html += "let statusInterval;";
  html += "window.onload = function() {";
  html += "loadColorsFromStorage();";
  html += "loadStateFromStorage();";
  html += "generateLedButtons();";
  html += "syncStateWithServer();";
  html += "setStatusInterval();";
  html += "document.getElementById('currentVolume').style.display = 'none';";
  html += "setTimeout(checkAndShowTracks, 500);";
  html += "};";
  html += "";
  html += "function checkAndShowTracks() {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "var data = JSON.parse(xhr.responseText);";
  html += "console.log('checkAndShowTracks:', data);";
  html += "if (data.hasAnalyzed && data.tracks && data.tracks.length > 0) {";
  html += "console.log('Tracks available on reload, showing selector');";
  html += "showTrackSelector(data.tracks);";
  html += "setTimeout(function() {";
  html += "if (selectedTrackIndex >= 0) {";
  html += "updateTrackSelection(selectedTrackIndex, 1);";
  html += "}";
  html += "if (selectedTrackIndex2 >= 0) {";
  html += "updateTrackSelection(selectedTrackIndex2, 2);";
  html += "}";
  html += "}, 100);";
  html += "}";
  html += "}";
  html += "};";
  html += "xhr.open('GET', '/analyzeFile');";
  html += "xhr.send();";
  html += "}";
  
  html += "function setStatusInterval() {";
  html += "if (statusInterval) clearInterval(statusInterval);";
  html += "statusInterval = setInterval(updateStatus, 500);";
  html += "}";
  html += "";
  html += "function syncStateWithServer() {";
  html += "var xhr = new XMLHttpRequest();";
  html += "xhr.onload = function() {";
  html += "if (xhr.status === 200) {";
  html += "var data = JSON.parse(xhr.responseText);";
  html += "microphoneEnabled = data.microphoneEnabled;";
  html += "document.getElementById('microphoneToggle').checked = data.microphoneEnabled;";
  html += "seeVolumeEnabled = false;";
  html += "document.getElementById('seeVolumeToggle').checked = false;";
  html += "document.getElementById('currentVolume').style.display = 'none';";
  html += "if (data.thresholdTimeout) {";
  html += "document.getElementById('thresholdTimeout').value = data.thresholdTimeout;";
  html += "console.log('Loaded threshold timeout from server: ' + data.thresholdTimeout + ' ms');";
  html += "}";
  html += "updateStatus();";
  html += "}";
  html += "};";
  html += "xhr.open('GET', '/status');";
  html += "xhr.send();";
  html += "}";
  html += "";
  html += "function resetToDefaults() {";
  html += "if (!confirm('Reset all controls to default values?')) return;";
  html += "document.getElementById('brightness').value = 128;";
  html += "document.getElementById('brightnessValue').textContent = '128';";
  html += "document.getElementById('speed').value = 75;";
  html += "document.getElementById('speedValue').textContent = '75%';";
  html += "document.getElementById('speedInput').value = 75;";
  html += "document.getElementById('transpose1').value = 0;";
  html += "document.getElementById('transpose1Value').textContent = '0';";
  html += "document.getElementById('transpose2').value = 0;";
  html += "document.getElementById('transpose2Value').textContent = '0';";
  html += "document.getElementById('volumeThreshold').value = 300;";
  html += "document.getElementById('minimumVolumeThreshold').value = 100;";
  html += "document.getElementById('thresholdTimeout').value = 250;";
  html += "document.getElementById('debugToggle').checked = false;";
  html += "document.getElementById('microphoneToggle').checked = false;";
  html += "document.getElementById('seeVolumeToggle').checked = false;";
  html += "document.getElementById('leftHandColor').value = '#ff0000';";
  html += "document.getElementById('rightHandColor').value = '#0000ff';";
  html += "microphoneEnabled = false;";
  html += "seeVolumeEnabled = false;";
  html += "savedLeftColor = '#ff0000';";
  html += "savedRightColor = '#0000ff';";
  html += "localStorage.clear();";
  html += "setBrightness(128);";
  html += "setSpeed(75);";
  html += "toggleDebug(false);";
  html += "toggleMicrophone(false);";
  html += "toggleSeeVolume(false);";
  html += "updateColors();";
  html += "alert('All controls reset to default values');";
  html += "}";
  html += "</script>";
  html += "</body>";
  html += "</html>";
  
  return html;
}

