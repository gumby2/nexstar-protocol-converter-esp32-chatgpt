# NexStar Protocol Converter

ESP32 firmware for bridging telescope-control clients to an original Celestron NexStar mount while preserving the mount's single-command protocol behavior.

## Tested Baseline

v5.76 is the tested working baseline.

## Target

- Board: ESP32 Dev Module
- ESP32 Arduino core: 3.3.10
- FQBN: `esp32:esp32:esp32:PartitionScheme=huge_app`
- Mount UART: RX GPIO 16, TX GPIO 17

## Build

Install Arduino CLI and the ESP32 core:

```bash
arduino-cli core install esp32:esp32@3.3.10
```

Compile:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app firmware/Nexstar_Protocol_Converter_v5.76
```

Upload:

```bash
arduino-cli upload -p <PORT> --fqbn esp32:esp32:esp32:PartitionScheme=huge_app firmware/Nexstar_Protocol_Converter_v5.76
```
