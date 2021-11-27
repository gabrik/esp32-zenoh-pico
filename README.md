# Zenoh-pico on ESP32 Arduino framework

Code for Zenoh-pico on ESP32 using Arduino framework


## How to build

```
arduino-cli compile --fqbn esp32:esp32:esp32 play/play.ino --output-dir ./  --build-property build.extra_flags=-DZENOH_ESP32=1
```