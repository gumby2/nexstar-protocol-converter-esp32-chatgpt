# Protocol stack

This document describes the current request paths. It records behavior present in the source; it does not define new protocol support.

## 1. SkySafari over Wi-Fi LX200

- Input transport: TCP client accepted by the LX200/SkySafari Wi-Fi server on `LX200_PORT`.
- Parser/handler: `handleLX200Server()` services the client and routes bytes through `processLX200Stream(..., LX_SRC_WIFI)`.
- Protocol module: `lx200_protocol` parses `#`-terminated or line-terminated LX200 commands and calls `handleLX200Command()`.
- Mount transport interaction: position polls use the cached mount-position API. GOTO requests are accepted immediately, then either queued or started asynchronously through the sketch's GOTO machinery, which eventually calls `nexstar_protocol` and `mount_transport`.
- Response path: `lx200Send(LX_SRC_WIFI, ...)` writes the LX200 reply to the connected TCP client.

## 2. SkySafari over Bluetooth LX200

- Input transport: ESP32 Bluetooth SPP through `SerialBT`.
- Parser/handler: `handleBluetoothLX200()` detects client connect/disconnect and routes one complete command per loop pass through `processLX200Stream(..., LX_SRC_BT)`.
- Protocol module: the same `lx200_protocol` command core used by Wi-Fi.
- Mount transport interaction: identical command semantics to Wi-Fi LX200. Bluetooth parsing is not serviced during mount serial waits, preserving the single-command mount rule.
- Response path: `lx200Send(LX_SRC_BT, ...)` writes the LX200 reply to `SerialBT`.

## 3. Alpaca HTTP request

- Input transport: HTTP request on the Alpaca server port.
- Parser/handler: registered Alpaca routes and `handleNotFound()` route telescope API actions to `handleTelescopeGet()` or `handleTelescopePut()`.
- Protocol module: Alpaca handlers live in the sketch today; they use the same cached position APIs and queued GOTO entry points used by other application clients.
- Mount transport interaction: Alpaca coordinate reads are served from cached mount state. slew requests enqueue or start RA/Dec or Alt/Az GOTO work, then the async GOTO path calls `nexstar_protocol` and `mount_transport`.
- Response path: `sendAlpacaValue()`, `sendAlpacaOK()`, or `sendAlpacaError()` writes JSON through the HTTP server.

## 4. Stellarium TCP request

- Input transport: TCP client accepted by the Stellarium server on `STELLARIUM_PORT`.
- Parser/handler: `handleStellariumServer()` buffers binary packets and passes complete packets to `handleStellariumPacket()`.
- Protocol module: Stellarium handling is still in the sketch.
- Mount transport interaction: position packets use cached RA/Dec. GOTO packets enqueue or start RA/Dec GOTO work, then the async GOTO path calls `nexstar_protocol` and `mount_transport`.
- Response path: `sendStellariumPosition()` writes binary position packets back to the Stellarium TCP client.

## 5. Browser web UI request

- Input transport: HTTP request on the web UI server.
- Parser/handler: route handlers such as `/status`, `/mount_test`, `/webgoto_radec`, `/webgoto_altaz`, `/getradecweb`, and `/getaltazweb` live in the sketch.
- Protocol module: web reads use cached position APIs where appropriate; web GOTO actions enqueue or start RA/Dec or Alt/Az GOTO work.
- Mount transport interaction: direct mount tests call `testInit()` through `nexstar_protocol`. GOTO actions flow through the same queue/async path as other clients.
- Response path: route handlers send HTML, text, or JSON through the web server.

## 6. Direct mount test

- Input transport: serial console, Telnet console, or browser route depending on caller.
- Parser/handler: command/page handlers in the sketch call `testInit()`.
- Protocol module: `nexstar_protocol` sends a handshake and safe `E` command to verify the original NexStar command path.
- Mount transport interaction: `mount_transport` enforces the command lock, performs the `?`/`#` handshake, writes `E`, and reads the 4-byte RA/Dec payload.
- Response path: the caller reports success/failure through console, Telnet, or web response text.

## Original NexStar mount rules

- Serial baud rate is 9600.
- The mount accepts only one active command at a time.
- Applicable commands begin with `?`.
- The mount acknowledges the handshake with `#`.
- `E` reads RA/Dec.
- `Z` reads Az/Alt.
- `R` starts RA/Dec GOTO.
- The firmware's existing Alt/Az GOTO command must be preserved exactly as implemented.
- Movement completion is reported by `@`.
- Coordinate payloads are big-endian signed 16-bit values.
- Do not poll `E` or `Z` during active GOTO.
- AbortSlew is not supported by the original mount.
