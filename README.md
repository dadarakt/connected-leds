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

## Networking states

+ 0 Undefined
  + Send broadcast message to join network
  + On receive of other broadcast: If magic number higher, take on peer and
    -> state 2
+ 1 Follower
  + Periodically receive clock syncs -> state 1
  + Answer clock syncs with heartbeat -> state 1
  + Receive led commands -> state 1
  + when too long ago, assume leader dead -> state 0
+ 2 - leader
  + Receive join request: Add to peer list -> state 2
  + Receive heartbeat:
    + Peer in list: Update last heartbeat
    + Peer not in list: Add to peer list
  + When peers don't send heartbeats, remove from peer list -> state 2
  + Listen for commands from outside for led commands -> state 1
  + Keep track of peers

## Simpler case

Distinct roles which cannot dynamically change

+ (1) Leader
  + Listens for peer broadcasts
  + keeps track of accepted peers
  + Listens for peer heartbeats (removes them if not heard of for too long)
  + Sends out sync commands
  + Fans out any LED commands from outside
+ (N) Followers
  + Initially broadcast presence, so that leader can register them
  + Listen for sync requests (call-response?)
  + Listen for LED commands
