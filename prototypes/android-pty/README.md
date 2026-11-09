# Android Bionic PTY probe

This is a standalone feasibility test. It is deliberately **not** linked into
fesTerm and does not remove the Android dependency guard for `festerm-pty`.

## What it checks

- Bionic PTY master allocation (`posix_openpt`, `grantpt`, `unlockpt`)
- controlling-terminal setup in a child session (`setsid`, `TIOCSCTTY`)
- execution of Android's `/system/bin/sh`
- output flowing from the PTY master
- terminal dimensions changed at runtime from 40x100 to 50x120\n- interactive input sent from the PTY master and echoed by the shell\n- child process exit and `waitpid` reaping

The probe uses `fork()` only in its own single-purpose process. Do not copy this
fork/exec sequence into the multithreaded Android app process.

## Build on a computer with Android NDK

From the repository root, set `NDK` to your installed NDK directory:

```sh
NDK="$ANDROID_NDK_HOME"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang" \
  -Wall -Wextra -Werror -O2 \
  prototypes/android-pty/pty_probe.c -o pty_probe
```

For macOS, replace `linux-x86_64` with `darwin-x86_64` (or the host
prebuilt directory supplied by the installed NDK).

## Run on an ARM64 Android device

```sh
adb push pty_probe /data/local/tmp/pty_probe
adb shell chmod 700 /data/local/tmp/pty_probe
adb shell /data/local/tmp/pty_probe
```

Expected output includes `FESTERM_PTY_OK`, then `40 100`, and finally a
`PASS` line. A failure is useful evidence: keep the exact error and device
Android version/API level.

## Scope and limitations

A PASS proves only this small native PTY path on that device. It does not prove
that `portable-pty` builds for Android, that PTY behavior is correct under app
lifecycle changes, or that the app can safely fork from its existing runtime.
Next qualification must cover interactive input, resize after spawn, signals,
child reaping, app background/foreground, and shutdown. Keep the current Android
dependency guard until those tests pass.
