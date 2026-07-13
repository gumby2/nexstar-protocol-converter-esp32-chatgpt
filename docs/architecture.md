# Firmware architecture

This firmware is in the middle of a modularization pass. The Arduino sketch still owns the main application runtime, but the protocol and support layers now have clearer ownership boundaries.

## Current module ownership

`Nexstar_Protocol_Converter.ino`

- Owns `setup()` and `loop()`.
- Owns Wi-Fi, Bluetooth, Telnet, HTTP web UI, Alpaca, Stellarium, and TCP server wiring.
- Owns the background polling scheduler, coordinate cache state, GOTO queue, non-blocking GOTO completion watcher, and most observer/time/location logic.
- Owns user-facing web pages, JSON status endpoints, console commands, and route registration.

`logging.h` / `logging.cpp`

- Own log levels, subsystem categories, the in-RAM log buffer, alert state, and filtered log formatting.
- May be used by all modules as a shared support layer.

`settings.h` / `settings.cpp`

- Own runtime settings globals, default values, ESP32 Preferences/NVS persistence, non-ESP32 LittleFS settings persistence, Wi-Fi settings persistence, bridge mode, Bluetooth-lite settings, and saved polling/GOTO timeout values.
- May be used by application and protocol modules as a shared support layer.

`mount_transport.h` / `mount_transport.cpp`

- Owns the physical mount UART/serial object, pin selection, 9600 baud mount transport, byte-level read/write helpers, byte logging, mount fault state, `mountBusy`, command begin/end locking, the `?`/`#` handshake, and waiting for movement completion `@`.
- This is the only module that should directly access `MountSerial`.

`nexstar_protocol.h` / `nexstar_protocol.cpp`

- Owns original NexStar command verbs and coordinate encoding/decoding.
- Implements `E` RA/Dec reads, `Z` Az/Alt reads, `R` RA/Dec GOTO, the existing Alt/Az GOTO command as implemented, angle normalization, signed big-endian 16-bit payload conversion, and direct mount test initialization.
- This is the only module that should duplicate or change NexStar wire encoding.

`lx200_protocol.h` / `lx200_protocol.cpp`

- Owns the shared LX200 parser/router used by both Wi-Fi SkySafari and Bluetooth SkySafari paths.
- Converts LX200 polling, staging, GOTO, location/time, and nudge commands into cached position API calls, settings updates, queued GOTO requests, or NexStar protocol operations through application callbacks.
- Does not own TCP or Bluetooth transport setup; those remain in the sketch.

Bright Star Catalog data header

- `bsc5_catalog_data.h` owns static catalog data exposed by web/UI code.
- Catalog logic beyond static data remains in the sketch.

HTTPS credentials header

- `https_credentials.h` owns static HTTPS setup certificate/key material.
- Existing certificate behavior must remain unchanged unless a task explicitly targets it.

## Dependency direction

```text
Web / Bluetooth / TCP clients
             |
       LX200 protocol
             |
      NexStar protocol
             |
      Mount transport
             |
      UART / RS-232 mount

Logging and Settings are shared support modules.
```

Dependencies should point downward through this stack. Circular dependencies are prohibited. Shared state that still lives in the sketch is exposed to extracted modules through narrow function declarations or globals during the transition, but new code should move toward explicit APIs.

## Runtime model

The firmware uses the Arduino single-threaded `setup()` and `loop()` model. Long work is split into state machines where possible so network clients remain responsive:

- `setup()` initializes serial, filesystems/settings, Wi-Fi or Bluetooth mode, mount transport, web routes, Alpaca routes, TCP listeners, UDP discovery, and Telnet.
- `loop()` services mount polling, queued GOTO work, non-blocking GOTO completion, async read/slew requests, HTTP clients, Alpaca discovery, LX200 TCP, Stellarium TCP, Bluetooth LX200, Telnet, NTP, staged Alpaca time/location updates, and console input.
- Mount waits call `serviceNetworkDuringMountWait()` so selected network work can continue while the mount serial path waits for bytes.
- Bluetooth LX200 parsing is intentionally not serviced from mount wait loops, because accepting a new SkySafari command while a mount command is active can violate the original mount's single-command rule.

## Command ownership and invariants

- The original NexStar mount is strictly single-command. A new mount transaction must not start while `mountBusy` is true.
- Commands that require the original NexStar handshake must send `?` and receive `#` before the command byte/payload.
- Movement completion is marked by `@`.
- Do not poll `E` or `Z` while a GOTO is active or while completion is unknown.
- During active GOTO, app-facing clients should report slewing and answer position requests from cached or estimated coordinates.
- `AbortSlew` is unsupported by the original mount. UI stop requests may clear local UI/queue state only when the mount command has not started; they must not invent a mount abort.
- Big-endian signed 16-bit coordinate encoding belongs in `nexstar_protocol`.
- Direct access to `MountSerial` outside `mount_transport` is prohibited.
- Duplicating NexStar command encoding outside `nexstar_protocol` is prohibited.

## Remaining modularization

- observer/time/location
- background polling and coordinate cache
- Wi-Fi/network servers
- Bluetooth transport
- HTTP/web interface
- Alpaca
- Stellarium
- GOTO queue and slew state
- catalog logic beyond static data
