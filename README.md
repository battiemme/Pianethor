# 🎹 Pianethor Build Guide

**Pianethor** is an interactive LED piano controller that allows you to play classical piano pieces without musical knowledge! Here there is the guide that provides step-by-step instructions to build the project.

You can see the example video here:

[![Pianethor Example Video](https://img.youtube.com/vi/YS5QBqhOpKc/0.jpg)](https://www.youtube.com/watch?v=YS5QBqhOpKc)

And here a quick explanation video on how it works:

[![Pianethor Explanation Video](https://img.youtube.com/vi/Q1QKWW2SMzw/0.jpg)](https://www.youtube.com/watch?v=Q1QKWW2SMzw)

---

## 📋 Prerequisites

By following this guide, you acknowledge that you are responsible for your own safety and actions. I am not liable for any injuries, accidents, or damages that may occur while assembling the project, whether to yourself, others, or any objects. This guide involves the use of tools such as scissors and a soldering iron, which can pose risks if not handled properly. Always take necessary precautions, work in a safe environment, and ensure you have the proper skills and equipment. Proceed at your own risk.

---

## Part 1: Gather Your Materials

### ⚙️ Electronics

| Component | Quantity | Specifications |
|-----------|----------|---|
| ESP32 | 1x | Mine was CH340C (USB Type-C connector) but any other model I think will work |
| LED Strip | 1x | WS2812B **5V** (74 LEDs/meter) **CAREFUL: IT NEEDS TO BE FOR 5V** |
| Microphone | 1x | INMP441 I2S : Optional but if you want the program to follow your speed (see the attached video for better explanation) is necessary |
| Power Supply with USB cable | 1x | Power Bank or 5V Supply (≥1.5A output) (theoretically even a 1A is enough) |
| Connecting Cables | Multiple | F-F and M-F Arduino cables (or you can solder directly) |

### 🔧 Hardware Tools

- Hot glue gun and glue sticks
- Soldering iron and solder
- Wire strippers
- Electrical tape or heat shrink tubing

---

## Part 2: Prepare the LED Strip

### 📍 Step 1:

1. First, cut **2 individual LEDs** from the end (leaving 72 LEDs total)

![LED Step 1](https://cdn.hackaday.io/images/3890061776956809081.jpg)

2. Cut at different lenght (or apply some electrical tape) the 2 extra red and white wires

### 📍 Step 2: Create LED Groups and measure space between piano tiles

1. Cut the remaining 72 LEDs into **6 groups of 12 LEDs each.** Each group should have 12 consecutive LEDs with the connector side preserved on the first group
2. Place 2 group of notes on the piano in order to align them (more or less) in a tile (every led should be right up one piano key) starting from C (Do) and ending with B(Si). Measure the space between the 2 groups. (in my case around 9 mm)

![LED Step 2](https://cdn.hackaday.io/images/7373561776957428034.jpg)

### 📍 Step 3: Connect LED Groups

1. Solder **3 wires** with the same distance you have found (depending on your soldering skill you'll need to add 1 or 2 mm to the measure you found(in my case I opted for 10mm) between each group
2. Once finished check if every led aligns correctly to a piano key
3. Apply **hot glue** to the soldered connections for stability and insulation

---

## Part 3: Wire the Electronics(with Soldering or FF/FM wires)

### 🔌 Step 1: ESP32 to LED Strip Connection

Connect your first LED group's connector to the ESP32:

| ESP32 Pin | LED Strip Pin | Wire Color |
|-----------|---------------|-----------|
| VIN | VIN | **Red** |
| GND | GND | **White** |
| D2 | DIN | **Green** |

### 🔌 Step 2: Connect the Microphone (Optional)

If you have the INMP441 microphone, connect it to the ESP32:

| ESP32 Pin | Microphone Pin | Function |
|-----------|----------------|----------|
| 3V3 | VDD | Power Supply |
| GND | GND | Ground |
| D25 | WS | Word Select (Clock) |
| D33 | SD | Serial Data |
| D32 | SCK | Bit Clock |
| GND | L/R | Ground (Stereo Select) |

---

## Part 4: Programming the ESP32

### 💻 Step 1: Install Arduino IDE

1. Download and install the **Arduino IDE** from [arduino.cc](https://arduino.cc)
2. Add ESP32 board support:
   - Go to **File** → **Preferences**
   - Add `https://dl.espressif.com/dl/package_esp32_index.json` to Additional Board Manager URLs
   - Go to **Tools** → **Board Manager** → Search for "ESP32" and install

### 💻 Step 2: Install Required Libraries

In Arduino IDE, go to **Sketch** → **Include Library** → **Manage Libraries** and install:

- **FastLED** – for LED control
- **ArduinoJson** – for JSON parsing, usually included with ESP32
- **WebServer** – usually included with ESP32
- **SPIFFS** – usually included with ESP32

### 💻 Step 3: Upload the Firmware

1. Download the `pianethor_v2_1.ino` file from the [GitHub repository](https://github.com/battiemme/Pianethor)
2. Open it in Arduino IDE
3. Connect the ESP32 to the PC via usb cable (when inserting the cable you'll need to press EN button for a couple of seconds)
4. Select the correct board: **Tools** → **Board** → **ESP32** → **ESP32 Dev Module**
5. Select the correct port: **Tools** → **Port** → (Your COM port)
6. Click **Upload** to program the ESP32

---

## Part 5: Initial Setup & Testing

###  Step 1: Power On

1. Connect the power supply to the ESP32
2. Place the device on the left part of the piano, with the first led standing over the lowest C(Do)
3. The device should boot up with a green LED animation
4. The ESP32 will create a WiFi access point

###  Step 2: Connect to WiFi

1. On your smartphone or computer, look for WiFi network **"Pianethor"**
2. Password: **"12345678"**
3. Once connected, open your browser and go to `http://192.168.4.1`

###  Step 3: Test the Interface

1. You should see the web interface with controls for:
   - Brightness adjustment
   - LED testing
   - File upload
   - Playback controls

###  Step 4: Upload a MIDI File(XML and MXL may not work)

1. In the web interface, click **"📤 Select MIDI/XML/MXL File"**
2. Choose a classical piano MIDI file
3. The device will analyze the file and show available tracks
4. Select a track to load

---

## Part 6: Fine-Tuning & Advanced Features

### 🎨 Brightness Adjustment

- Use the brightness slider (0–255) to adjust LED brightness

### ⏱️ Playback Speed Control

- Adjust speed from 10% to 150%

### 🎤 Microphone Mode (Optional)

1. Enable the microphone toggle if INMP441 is connected
2. The system will listen to your playing and advance to the next note when it detects sound(you'll need to set volume thresholds for your environment)

### 🎨 Color Configuration

- **Track 1:** Default Red
- **Track 2:** Default Blue

### 🎵 Track Transposition

- Transpose each track by ±4 octaves
- Helps adapt pieces to your comfort range

Eventually I added some adhesive strap in order to keep the led strip placed, without damaging the piano and to be able to remove it whenever I want.

---

## Part 7: Quick Troubleshooting

| Issue | Solution |
|-------|----------|
| ESP32 won't connect to WiFi | Check USB cable, try different port, restart IDE |
| LEDs don't light up | Verify solder connections, check polarity (Red=+, White=−) |
| Only some LEDs light | Check each group's solder joints for cold connections |
| Microphone not working | Verify I2S pin connections match your ESP32 model |
| Can't upload MIDI files | Ensure file size is under less than 100kb and uses MIDI format (maximum notes to play in an ESP32 is limited to around 900) |

---

If you have suggestions or need to ask for something you can contact me on IG: [@battiemme](https://www.instagram.com/battiemme/)

Also if you like the creation consider to subscribe to my channel on Youtube <3

For other informations visit the [GitHub repository](https://github.com/battiemme/Pianethor).

---

Enjoy playing classical piano with Pianethor! 🎹✨
