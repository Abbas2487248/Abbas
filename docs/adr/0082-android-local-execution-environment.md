# ADR 0082: Android Local Execution Environment

- **Status:** Proposed
- **Date:** 2026-11-09
- **Supersedes:** None

## Context

The Android host currently reuses `app/festerm-mobile` for the shared UI,
SSH/SFTP and mobile lifecycle policy. ADR 0073 explicitly excludes local
shells and background services from the Android port. The Android packager
also rejects `festerm-pty`, `festerm-sessiond` and `portable-pty` in its
normal dependency tree.

The workspace already has a desktop local-session backend in
`crates/festerm-pty`, built on `portable-pty`, and shared lifecycle/event
contracts in `crates/festerm-session`. Reusing the desktop backend by merely
removing the packaging guard would bypass the platform boundary and has not
been shown to compile or behave correctly on Android.

The product goal is a Termux-like, on-device environment without requiring a
VPN: a local interactive shell, runnable language runtimes (initially Python
and Node.js), package installation, and optional Debian/Ubuntu userlands.
Android application sandboxing, ABI compatibility, process lifecycle,
storage permissions, terminal job control, and PRoot/ptrace restrictions are
hard constraints. A VPN is not part of local execution.

## Decision

Keep the existing Android dependency guard until a dedicated Android
execution backend passes compile and device qualification. Do not add
`portable-pty` to the Android dependency graph as an untested shortcut.

Implement the work in independently testable stages:

1. **Feasibility spike:** prove a real interactive PTY on supported Android
   API levels and ABIs, including resize, Ctrl-C, EOF, child exit, signals,
   and correct terminal behavior. Compare a small Android-native PTY/JNI
   boundary with a Rust/Linux-target implementation; choose only after an
   emulator/device test.
2. **Local session backend:** expose the existing bounded
   `festerm-session` contract through an Android-specific backend. Keep
   process creation, I/O, resize and shutdown bounded; never execute a
   user-provided command through an interpolated shell string.
3. **App-private runtime prefix:** install and manage runtime binaries under
   app-private storage, with ABI-specific packages and verified downloads.
   Start with a minimal shell and Python; add Node.js only after package and
   storage lifecycle tests pass.
4. **Linux userlands:** evaluate PRoot on real Android devices before
   promising Debian or Ubuntu. Install rootfs images only from verified
   sources, keep them inside app-private storage, and make distro setup,
   updates and removal explicit user actions.
5. **Lifecycle and UX:** add explicit foreground-session behavior and
   explain that Android may stop background work. Do not claim Termux-style
   persistent background sessions until a separately reviewed foreground
   service design and native qualification exist.

The shared terminal UI remains the sole terminal-state writer. Local sessions
must be distinguishable from SSH sessions and must not silently acquire
network, VPN, root, or broad-storage privileges.

## Alternatives considered

- Remove the `FORBIDDEN` entries and link `portable-pty` immediately:
  rejected until Android compilation and PTY behavior are demonstrated.
- Use ordinary `ProcessBuilder` pipes as if they were a PTY: rejected for the
  interactive terminal baseline because pipes do not provide terminal job
  control, terminal modes or PTY resize semantics.
- Bundle Debian/Ubuntu before proving PTY and PRoot compatibility: rejected
  because it creates a large, difficult-to-debug failure surface.
- Require VPN or a remote runner: rejected because the goal is on-device
  execution without VPN.

## Consequences

The first deliverable is a feasibility prototype, not a feature-complete
Termux replacement. Existing Android SSH/SFTP behavior and the `main`
branch remain unchanged. Python, Node.js and distro packages require
ABI-specific distribution, licensing review, checksum/signature validation,
storage accounting and update/uninstall behavior. Android background
execution remains best-effort unless a separately approved foreground
service is implemented.

## Validation impact

- **Invariants introduced or changed:** Android local execution is opt-in and
  app-private; no VPN/root/broad-storage dependency; bounded session I/O;
  existing Android dependency guard remains until qualification.
- **GUI/action edges affected:** None in the feasibility spike; later local
  session creation and runtime-management actions require stable IDs.
- **Automated tests required:** Android dependency-policy test, ABI cross-build,
  PTY spawn/input/output/resize/exit tests, bounded-queue and shutdown tests,
  runtime archive integrity and path traversal tests, package install/remove
  tests.
- **Native/manual evidence required:** Android emulator and physical-device
  PTY interaction on ARM64; lifecycle interruption/resume; storage cleanup;
  PRoot feasibility before enabling Debian/Ubuntu.
- **Coverage superseded:** None.
