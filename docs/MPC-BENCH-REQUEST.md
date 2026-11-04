# Request: KickEQ CPU measurement on the Akai MPC Key 61

For the owner / the hardware session. Nothing here was run on the device by the KickEQ session
(the MPC is used only by the hardware session). The ARMv7 build and its tests run in CI under
qemu-user; QEMU timings are a regression guard only, never an absolute figure.

## What to run on the MPC

1. Download the CI artifact `KickEQ-linux-armv7` (GitHub Actions, workflow `ci`, job `armv7`, latest green run
   on `main`) or, from a release, `KickEQ-linux-armv7.tar.gz`. It is built with `-march=armv7-a
   -mfpu=neon-vfpv4 -mfloat-abi=hard`; the tools link libstdc++/libgcc statically.
2. Unpack, copy `KickEQ-linux-armv7/tools/kickeq_bench`, `tools/clap_testhost` and
   `native/KickEQ-native.clap` to a scratch directory on the MPC (for example `/tmp/kickeq`).
3. Run, with nothing else heavy running (Kick stopped is best), once pinned to each of the two isolated
   cores if the image allows it (`taskset -c <cpu>`), three repetitions:

```sh
cd /tmp/kickeq && chmod +x kickeq_bench clap_testhost
./kickeq_bench                                  # prints the table of section 9 scenarios
./clap_testhost --plugin ./KickEQ-native.clap   # conformance of the real .clap on the device
```

## Expected result (docs/ARCHITECTURE.md section 9)

| Scenario (48 kHz, 128 frames, stereo) | Budget |
|---|---|
| 8 bands incl. 2 cuts at 24 dB/oct, parameters at rest, analyser off | <= 3 % of one core |
| same, all parameters moving | <= 6 % of one core |
| worst case: 8 cuts at 48 dB/oct | reported only (no budget defined yet) |

`clap_testhost` must end with 0 failed checks (172 on x86/arm64 with threads; the multi-threaded
section is included on the device if `std::thread` works there).

## Please report back

- the full `kickeq_bench` output (3 runs), `uname -a`, `cat /proc/cpuinfo | grep -E 'model name|Features'`,
  the `taskset` core used, and the `clap_testhost` summary line;
- if a budget is missed, the scenario and measured %: the planned response is a NEON float path
  (docs/ARCHITECTURE.md section 4.2), not a lower quality setting.
