| Supported Targets | ESP32-C3 |
| ----------------- | -------- |

# AWS Quick Connect for ESP32-C3

The ESP32-C3 has been configured to work with the AWS Quick Connect demo. This demo uses AWS on the backend, no account required, to connect your device to the cloud. Once connected, messages are sent from the device, allowing you to simulate AWS IoT applications.

## How to use

To clone using HTTPS:
```
git clone https://github.com/espressif/aws-quickconnect.git --recurse-submodules
```
Using SSH:
```
git clone git@github.com:espressif/aws-quickconnect.git --recurse-submodules
```

If you have downloaded the repo without using the `--recurse-submodules` argument, you need to run:
```
git submodule update --init --recursive
```

Follow the steps on [Establish a Serial Connection with ESP32-C3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/establish-serial-connection.html) to ensure a connection can be established with the ESP32-C3.

Inside the bin folder, there are zipped executable packages. Extract the package that corresponds with your operating system and run the Start_Quick_Connect executable. This is necessary to register your device with Espressif's Quick Connect account on AWS, as well as provision your device with the credentials necessary to connect to AWS and publish messages.

After the Start_Quick_Connect utility has run and succeeded, the source code of this repository can be changed then built and flashed using ESP-IDF v4.4 or higher following the instructions below.

### Build, flash, and monitor

This repository is structured as an ESP-IDF project. See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/index.html) for ESP-IDF for full steps to configure and use ESP-IDF to build projects. NOTE: This repository requires ESP-IDF v4.4 or higher.

Before project configuration and build, be sure to set the correct chip target by running `idf.py set-target esp32c3` in the root directory of this source code.

Then run `idf.py -p < PORT > flash` to build and flash the project.

< PORT > can be determined by following the Check Port instructions found in [Establish a Serial Connection with ESP32-C3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/establish-serial-connection.html). On Windows, < PORT > will have the format of COM* (e.g. COM3). On Linux, < PORT > will have the format of /dev/tty* (e.g. /dev/ttyUSB0). On Mac, < PORT > will have the format of /dev/cu.* (e.g. /dev/cu.SLAB_USBtoUART).

The serial output of the device can also be monitored by running `idf.py -p < PORT > monitor` after building.
