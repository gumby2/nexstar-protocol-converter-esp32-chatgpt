# Python simulator package scaffold

This directory documents the intended Python simulator layout. It is a scaffold only; do not add implementation files until a task explicitly requests simulator implementation.

Planned package layout:

```text
src/
  nexstar_sim/
    mount.py
    protocol.py
    state.py
    lx200_server.py
    alpaca_server.py
    stellarium_server.py
    faults.py

tests/
  test_handshake.py
  test_single_command.py
  test_goto_completion.py
  test_no_poll_during_goto.py
  test_coordinate_encoding.py
  test_lx200.py
```

Planned responsibilities:

- `mount.py`: original NexStar mount emulator lifecycle and serial/TCP-facing command loop.
- `protocol.py`: `?`/`#`, `E`, `Z`, `R`, existing Alt/Az GOTO behavior, `@`, and big-endian signed 16-bit payload helpers.
- `state.py`: coordinate state, slewing state, cache/estimate model, and timing.
- `lx200_server.py`: TCP LX200 endpoint for SkySafari-style tests.
- `alpaca_server.py`: Alpaca endpoint simulation.
- `stellarium_server.py`: Stellarium endpoint simulation.
- `faults.py`: timeout, delayed response, malformed response, and disconnect injection.

The initial tests should focus on handshake behavior, single-command enforcement, delayed GOTO completion, no polling during active GOTO, coordinate encoding, and LX200 request/response compatibility.
