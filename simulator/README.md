# Simulator scaffold

This directory is a scaffold for a future protocol simulator. It is not implemented yet.

## Goals

The simulator should provide repeatable protocol regression coverage for the firmware without requiring a physical original NexStar mount for every test run. It should model the mount-side behavior closely enough to validate command ordering, timing, coordinate encoding, and application-facing protocol responses.

## Planned architecture

- Original NexStar mount emulator.
- Strict single-command enforcement.
- `?`/`#` handshake support.
- `E` RA/Dec reads.
- `Z` Az/Alt reads.
- `R` RA/Dec GOTO and the firmware's existing Alt/Az GOTO behavior.
- Realistic delayed GOTO completion.
- `@` completion marker.
- Cached or estimated coordinates while GOTO is active.
- Configurable timeout and fault injection.
- TCP LX200 endpoint for SkySafari-style request testing.
- Optional Bluetooth simulation, limited by host platform support and CI availability.
- Alpaca endpoint simulation.
- Stellarium endpoint simulation.
- Web UI integration plan for driving firmware-visible scenarios.
- CI use for protocol regression tests.
- Protocol regression test vectors for handshake, single-command locking, GOTO completion, no-poll-during-GOTO behavior, coordinate encoding, LX200 responses, Alpaca requests, and Stellarium packets.

## Intended use

Future tests should be able to run a simulated mount, drive firmware or protocol-layer clients against it, and assert that:

- no second mount command is accepted while one is active
- no `E` or `Z` polling occurs during active GOTO
- app-facing clients receive cached or estimated coordinates during GOTO
- `@` completion unlocks post-GOTO verification behavior
- original NexStar big-endian signed 16-bit payloads are preserved

Implementation files are intentionally not present yet.
