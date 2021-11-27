# QuickConnect - ESP32-C3
## Steps to build
1. Set up ESP-IDF
2. From the root of this repository: execute `idf.py set-target esp32c3`, then `idf.py build`

## Steps to flash
1. `idf.py -p (PORT) flash`

## Steps to monitor serial
1. `idf.py -p (PORT) monitor`

## Steps to flash binary using FMC utility (Necessary to provide WiFi credentials and register with IoT endpoint required for Visualizer)
To be added.