# PIANETHOR
Ever wanted to play classical piano without knowing anything about music? Now you can

![DSC_0096 (2)](https://github.com/user-attachments/assets/520c0c13-39a0-4eca-ae8f-23cba2274d75)

[See the example video on YouTube](https://youtu.be/YS5QBqhOpKc)

[See the first explanation video on YouTube](https://youtu.be/Q1QKWW2SMzw)


### **By following this guide, you acknowledge that you are responsible for your own safety and actions. I am not liable for any injuries, accidents, or damages that may occur while assembling the project, whether to yourself, others, or any objects. This guide involves the use of tools such as scissors and a soldering iron, which can pose risks if not handled properly. Always take necessary precautions, work in a safe environment, and ensure you have the proper skills and equipment. Proceed at your own risk.**

[YOU CAN FIND THE DETAILED GUIDE HERE <----------------------------------------------------------------------------------------------------------](https://hackaday.io/project/205563-pianethor)

## Hardware
I've built this project using:
- 1 x ESP32 WR-32 CH340C with type-C connector(trust me it's better than mini and micro usb!)
- 1 x strip of 5V LEDs WS2812B 74 leds/m
- 1 x I2S microphone INMP441 (optional)
- 1 x powerbank or a 5V power supply with at least 1.5A output

Lots of Arduino cables to connect everything together  F-F and M-F (if you don't have them you can just simply solder them)

## How to prepare the leds
### 1. You need to cut 2 of the 74 leds in order to make 6 groups of 12 leds each: a strip of led has only one end with the connector and other 2 extra cables, you don't need to cut that but the opposite end (they would be the last 2 leds)
Isolate the excessive 2 wires that aren't in the connector(those are for an external power supply, we don't need one)

![20251231_162234](https://github.com/user-attachments/assets/a80b8f44-906b-40e1-a6ac-1739f6f006c6)

### 2. After cutting the last 2 leds you can proceed cutting 6 groups of 12 leds

![20251231_163002](https://github.com/user-attachments/assets/d87da080-e738-427e-be4e-09b1870e3c3f)

### 3. Solder 3 wires of the correct length between each group to adjust the LEDs so that each LED correspond to a tile of a piano key.

![20251231_163959](https://github.com/user-attachments/assets/bf42e8e5-bf01-4562-9ef6-6875d42618d3)

### 4. Eventually you can put some hot glue in the wires you just have soldered for more stability

# How to connect everything together
##  Connections ESP32 → LED Strip Connector

| ESP32 Pin | LED Strip Pin |
|-----------|----------------|
| VIN       | VIN (RED)      |
| GND       | GND (WHITE)    |
| D2        | DIN (GREEN)    |

## Connections ESP32 → Microphone

| ESP32 Pin | Microfono Pin |
|-----------|----------------|
| 3V3       | VDD            |
| GND       | GND            |
| D25       | WS             |
| D33       | SD             |
| D32       | SCK            |

Also connect GND to L/R

## Last thing
1. Program the ESP32 via Arduino via the sketch .ino
2. Connect to the WiFi Pianethor (default pass:12345678)
3. Go to the website 192.168.4.1
4. Upload a MIDI file
5. ENJOY!
