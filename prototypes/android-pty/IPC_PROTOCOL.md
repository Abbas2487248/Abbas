# Android Local PTY IPC Protocol

Status: design specification; not yet implemented or validated on-device.

## Purpose

Define a narrow, versioned byte protocol between the fesTerm Android UI process and a dedicated local PTY helper process. The helper owns the PTY and child shell. The UI process never calls fork/exec itself.

## Transport

- Unix-domain stream socket in the app-private cache/runtime directory.
- Socket permissions: owner read/write only (0600); reject unexpected peers where peer credentials are available.
- One connection per terminal session.
- The helper accepts only a fixed interactive shell path: `/system/bin/sh -i`.
- Do not expose arbitrary executable paths or shell command strings through the control protocol.
- The socket path and helper launch mechanism must be validated on supported Android versions before integration.

## Frame format

Every frame has a 5-byte header followed by its payload:

| Field | Size | Encoding |
|---|---:|---|
| Type | 1 byte | unsigned |
| Payload length | 4 bytes | unsigned big-endian |
| Payload | length bytes | raw bytes |

Maximum payload length: 65,536 bytes. Reject unknown frame types, oversized lengths, and malformed fixed-size payloads. Implement exact-read/exact-write loops and handle EINTR and partial stream operations.

## UI to helper

| Type | Name | Payload |
|---:|---|---|
| 0x01 | INPUT | Raw terminal input bytes, 0–65,536 bytes |
| 0x02 | RESIZE | Exactly 4 bytes: rows u16 BE, columns u16 BE |
| 0x03 | CLOSE | Empty payload; request orderly shutdown |

For RESIZE, reject zero rows or columns. Apply TIOCSWINSZ to the PTY and notify the foreground process group with SIGWINCH.

## Helper to UI

| Type | Name | Payload |
|---:|---|---|
| 0x81 | OUTPUT | Raw bytes read from the PTY master |
| 0x82 | EXIT | Exactly 4 bytes: signed wait status encoded as i32 big-endian |

The helper sends EXIT once after the child has been reaped, then closes the connection. PTY EIO after slave closure is treated as end-of-output, not as a fatal protocol error.

## Lifecycle and safety

1. Create the PTY and establish the child as session leader with a controlling terminal inside the dedicated helper process.
2. Start only the fixed shell; set a controlled working directory and environment.
3. Relay INPUT to the PTY master and PTY output to OUTPUT frames.
4. On CLOSE or socket loss, send SIGHUP to the child process group, allow a bounded graceful-exit interval, then terminate and reap the child if needed.
5. Always reap the child; never leave zombies.
6. Bound buffers and apply backpressure so a slow UI cannot cause unbounded memory growth.
7. Treat socket data as untrusted input. Validate all frame types and lengths before acting.
8. Keep the helper and socket inside the app's security boundary; do not expose a network listener.

## Android implementation gate

This document is a protocol specification only. It does not establish that an executable packaged in an APK can be launched through ProcessBuilder on every supported Android version, nor that the current app build pipeline packages such an executable correctly. Before integrating this design, build a minimal Android proof that packages, extracts, launches, connects to, and terminates the helper on-device. Do not remove the Android dependency guard or add portable-pty to the UI process until that proof and the process-isolation design are reviewed.

## Compatibility

The protocol is currently version 1 by convention. A future implementation should negotiate or explicitly pin a protocol version before allowing incompatible frame changes.
