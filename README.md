# PIANETHOR
Ever wanted to play classical piano without knowing anything about music? Now you can

[See the example video on YouTube](https://youtu.be/YS5QBqhOpKc)

[See the first explanation video on YouTube](https://youtu.be/Q1QKWW2SMzw)

Soon will upload more info on how to make everything!

**Careful! You need a soldering iron and some basic soldering skills in order to proceed**
**Before proceeding check if the piano tiles correspond (more or less) at the distance of each led** 
**I am not responsible for any damage!**


## Hardware
I've built this project using:
1 x ESP32 WR-32 CH340C with type-C connector(trust me it's better than mini and micro usb!)
1 x strip of 5V LEDs WS2812B 74 leds/m
1 x I2S microphone INMP441 (optional)
1 x powerbank or a 5V power supply with at least 1.5A output
Lots of Arduino cables to connect everything together(or you can just simply solder them)

## How to prepare the leds
You need to cut 2 of the 74 leds in order to make 6 groups of 12 leds each(cut them at the end, at the opposite side of the connector)
Solder 3 wires between each group to adjust the LEDs so that each LED correspond to a tile of a piano key.
Isolate the excessive 2 wires that aren't in the connector(those are for an external power supply, we don't nned one)

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
