# Networking between ESP32s

This is an [espdif](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
and [PlatformIO](https://platformio.org/) project which allows for two different
deploy targets:

+ *root* which assumes the role of a server and sends out commands to other nodes
+ *node* which receives messages from the mesh and acts on them

ESP-Mesh:

+ Assumes a gateway server to connect to the internet
+ Hard to setup without external router

ESP-Now

+ Ad-hoc network between nodes, no external internet connectivity assumed

## ESP-Now general steps

Mesh setup
-
