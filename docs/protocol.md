# NexStar protocol constraints

- The mount accepts only one active command at a time.
- The command path must remain blocked until the current command completes.
- `?` initialization/handshake returns `#`.
- Completed movement commands return `@`.
- Do not issue E/Z position queries during an active GOTO.
- AbortSlew is not supported by the original mount.
- During GOTO, report slewing and use cached or estimated position only.
