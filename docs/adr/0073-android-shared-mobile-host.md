# ADR 0073: Android Host for the Shared Mobile Application

- **Status:** Proposed
- **Date:** 2026-11-05
- **Amendment:** 2026-11-10 — local shell and bundled Python scope
- **Supersedes:** None

## Context

The owner requested an Android port with functional and visual parity to the
iOS application. ADR 0031 requires reuse of the Rust terminal and renderer,
native secret storage, and no desktop updater. The implemented iOS shell now
also includes profiles, tabs, SSH/SFTP, Dev Tunnels, documents, custom
keyboards and app lock.

The Android port originally excluded local shells and command-line tools.
The owner has now explicitly expanded the requested Android scope to include
an interactive app-private local shell and a bundled Python runtime. This is
an Android capability extension, not a request to add a parallel terminal UI
or a general-purpose plugin/scripting framework.

## Decision

Reuse `app/festerm-mobile`, including its theme, widgets and application
policy. Keep the Android `NativeActivity` host thin and keep JNI in
`festerm-android-platform`; Android-native services remain behind that
boundary. Neither the core nor renderer gains platform dependencies.

Android owns measured keyboard insets, IME integration, clipboard and browser
intents, device authentication, app-private storage, document selection and
on-device speech. Native events wake the existing renderer; queues and payloads
remain bounded. The shared application remains the sole terminal writer.
Phone/tablet layout uses the existing measured breakpoints and design tokens,
not a second Android UI.

The Android host selects GLES and enables a default-off `external-ime` feature
in the vendored egui-winit integration. Both platform-output and viewport IME
commands yield to the field-aware native connection; other platforms retain
their existing backend selection and IME behavior. The shared keyboard modes
map onto one text-aware input view. The system keyboard serves the native
keyboard and form fields. For the app-owned keyboards (`Terminal` and
`FieldCustom`) the same client stays focused, so hardware and field-targeted
input still work, but the system keyboard is hidden rather than layered over
the app's own keys.

The shared terminal keyboards split around a vertical fold or hinge. The
activity reads `FoldingFeature` bounds through the optional platform
`androidx.window.extensions` library by reflection, adding no AndroidX
dependency, and reports them to Rust as window-width fractions. A missing
library, failed reflection or invalid span means no hinge; the width rule
still applies. Horizontal folds are ignored.

Secrets remain behind `festerm-secret-store`. Android uses a non-exportable
Android Keystore AES-GCM key, per-reference authenticated encryption and atomic
private ciphertext files excluded from backup. Missing, locked or invalidated
keys fail explicitly, never falling back to plaintext.

Android's Storage Access Framework does not provide compare-and-swap writes.
Picked documents are explicitly imported as editable app-private copies;
fesTerm does not overwrite the original provider document. Private copies use
the shared revision/conflict model and writer admission. No broad filesystem
permission or display-path authority is introduced.
The native saved-copy chooser reopens these private sources after relaunch,
without requiring an SSH connection or granting write access to a provider.

App lock reuses the existing policy and native device-credential/biometric
authentication through the request-bound `Locker` contract. Every native
prompt carries the lock's reason and a token; replies and cancellations name
their token, so a late reply cannot complete, and a stale cancellation cannot
dismiss, a newer prompt. Unknown results fail closed. "Use device passcode"
requests the device credential alone on Android 11 or newer; Android 10 only
offers the combined prompt. Native privacy cover and secure-window flags
protect background snapshots. On-device speech is capability-gated on Android
12 or newer; an unavailable recognizer fails without falling back to a network
service. Its microphone meter is the recognizer's own level estimate, not
measured samples, and a failed capture preserves reviewed text for Resume.

### Android local shell

The existing isolated Android PTY helper remains a dedicated PIE process
packaged in the ABI-specific native-library directory and launched by the
Android host. Rust connects to its private Unix socket through the existing
`festerm-android-platform` boundary. The shared mobile application remains
the only terminal writer. Session input, output, resize, cancellation and
process retirement must retain bounded queues and explicit lifecycle handling.
The shell starts in the app-private home directory and does not grant general
filesystem access, root access, or authority over arbitrary Android app data.
No background service or persistent shell after app/process termination is
introduced by this decision.

### Bundled Python runtime

Provide an ABI-matched CPython Android runtime as an optional built-in
command in the local shell. The Android build must fetch a pinned upstream
runtime, verify its published SHA-256, retain required license notices, and
package the PIE launcher and required shared libraries in the APK's native
library directory. The standard library and runtime support files are
installed or extracted atomically into versioned app-private storage on first
use; no user-managed ZIP extraction or manual PATH edits are required.

The shell's `python` command must resolve to the packaged runtime, pass
arguments without shell-string interpolation, preserve the app-private working
directory, and report a clear error if provisioning or startup fails. Python
must not silently become a network service, background worker, or privileged
execution environment. Third-party Python packages and `pip` installation
are outside the initial acceptance scope; they require separate dependency,
native-extension, licensing and security review.

The exact runtime layout, loader search path, first-run extraction strategy,
and process lifecycle must be verified on each supported ABI before release.
The previously downloaded Python probe failing when executed directly from
ordinary app files storage is not evidence that the packaged native-library
launch path works; native-device execution evidence is required.

Build tools produce a development-signed APK. Store publication, release keys
and background services remain outside this port. Store publication needs
separate release signing, licensing/distribution review, target-SDK review and
native acceptance; the generated development keystore must never be committed
or used as a release key.

## Alternatives considered

- A Kotlin/Compose rewrite duplicates the existing application and visual
  system and was rejected.
- Using only NativeActivity's key events misses software IME editing;
  a narrow native input connection preserves the shared input model instead.
- Direct provider overwrites cannot preserve the document publication
  invariant; explicit imports preserve authority and avoid silent data loss.
- Running Python from a ZIP or app files directory was rejected as the
  execution strategy because Android may deny execution from ordinary app
  storage. The runtime must use the verified packaged native-library launch
  path or an equally constrained Android-supported mechanism.
- Embedding a general-purpose scripting/plugin system was rejected; the
  initial scope is the explicit local-shell Python command only.

## Consequences

iOS and desktop retain their current entry points and behavior. Android-only
dependencies are target-gated. The Android host now owns local PTY lifecycle
and Python runtime provisioning in addition to its existing platform services.
The shared renderer and terminal writer remain unchanged. Python increases APK
size and first-run storage use; both must be measured and bounded. Native
device qualification remains distinct from compilation, host tests and
emulator evidence.

## Validation impact

- **Invariants introduced or changed:** Android local shell and bundled
  Python runtime added; single terminal writer, bounded work, app-private
  storage, secrets and renderer boundaries preserved.
- **GUI/action edges affected:** existing Android terminal/local-session
  controls and local shell lifecycle; update traceability to identify the
  exact affected edges before implementation.
- **Automated tests required:** shared mobile regression suite, Android PTY
  launch/socket ownership, input/output bounds, resize, cancellation and
  process retirement; Python launcher ABI/ELF/dependency checks, upstream
  checksum verification, safe atomic provisioning, argument passing, working
  directory, `python --version`, script execution and clear failure states.
- **Native/manual evidence required:** install and launch on physical ARM64
  Android; run `python --version`, execute a local `.py` script, verify
  keyboard/input/output, app restart and interrupted first-run provisioning.
  Emulator evidence is useful but does not replace physical-device evidence.
- **Packaging and licensing:** pinned upstream artifacts and all applicable
  Python/runtime license notices must be included and audited; test APK size
  and 16-KiB native alignment for every shipped ABI.
- **Coverage superseded:** None; iOS evidence is not Android evidence.
