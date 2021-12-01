| Supported Targets | ESP32-C3 |
| ----------------- | -------- |

# AWS Quick Connect

The ESP32-C3 has been configured to work with the AWS Quick Connect demo. This demo uses AWS on the backend, no account required, to connect your device to the cloud. Once connected, messages are sent from the device, allowing you to simulate AWS IoT applications.

## How to use example

Follow the steps on [Establish a Serial Conniection with ESP32-C3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/establish-serial-connection.html) to ensure a connection can be established with the ESP32-C3.

Inside the bin folder, there are zipped executable packages. Extract the package that corresponds with your operating system and run the Start_Quick_Connect executable. This is necessary to register your device with Espressif's Quick Connect account on AWS, as well as provision your device with the credentials necessary to connect to AWS and publish messages.

After the Start_Quick_Connect utility has run and succeeded, the source code of this repository can be used to make changes. The source code is written to work with ESP-IDF v4.4 or higher.

### Hardware Required

* A development board with ESP32-C3 SoC (e.g. ESP32-C3-DevKitC-02, ESP32-C3-DevKitM-1, etc.)
* A USB cable for power supply and programming

### Build and Flash

Before project configuration and build, be sure to set the correct chip target using `idf.py set-target esp32c3`.

Build the project and flash it to the board, then run monitor tool to view serial output:

Run `idf.py -p PORT flash monitor` to build, flash and monitor the project.

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.