# firefly-app

Asset tracking firmware for the Nordic **nRF54LM20A**. West T2 manifest
repository — `west.yml` imports the nRF Connect SDK at a pinned revision, the
same way `vestfrost-level3-imx-app` imports Zephyr.

`self: path: firefly-app` in `west.yml` matches the git repository name, so the
clone URL and the workspace directory always agree.

## Layout

Folder and file naming follows `vestfrost-level3-imx-app/apps/emd/code`:
`srv*` in `service/`, `my*` in `helpers/`, `drv*` in `drivers/`, plus
`common/`, `utils/` and `zephyr/`. Includes are bare filenames.

```
code/                     application-specific code
  main.c  firefly.h       entry point + app-wide defines (cf. emd.h)
  common/                 common_types.hpp — shared value types
  core/                   NO Zephyr headers. Host-testable in seconds.
    ports.hpp             IClock, IGnss, IBle, ISession, IReportStore, IPowerGate
    helpers/              myMission, mySoc, myReportFactory
  service/                srvController — zbus + SMF glue, extern "C" boundary
  utils/                  CRC, flash locking, log backends
  zephyr/                 shell commands, soc.c, log wrappers
port/                     the HARDWARE boundary (not an RTOS boundary)
  bundle.hpp              the single seam
  mcu/mcuPorts.cpp        nRF54LM20 silicon
  host/hostPorts.cpp      same interfaces, no silicon
  drivers/                drv* out-of-tree Zephyr drivers
  boards/                 custom firefly board definition
  dts/bindings/
tests/
  core/                   ztest — mission policy; runs on native_sim AND the DK
  port/                   ztest — host ports under the real kernel
```

**`port/` is a hardware boundary, not an RTOS boundary.** Zephyr is linked on
both sides — `native_sim` runs the real kernel, scheduler, zbus and logging —
so `code/core` may use Zephyr freely. What it must *not* do is read the clock
or touch I/O ambiently: `myMission::on(event, now_ms)` takes the timestamp so a
test can drive a 6-hour batching window instantly. Helpers that drive silicon
(myLwm2m over the modem, myCredentialStore over CRACEN/KMU) belong in
`port/mcu`.

## Two build targets, one codebase

```
west build -b nrf54lm20dk/nrf54lm20a/cpuapp firefly-app   # port/mcu
west build -b native_sim                    firefly-app   # port/host
```

`CONFIG_FIREFLY_PORT_HOST` picks the set (defaults to `y` on `native_sim`).
`code/` is byte-identical between the two.

## Naming

One prefix token everywhere: folder `app`, repo `firefly-app`, namespace
`firefly::`, CMake `firefly_core` / `firefly_port` / `firefly_service`,
Kconfig `CONFIG_FIREFLY_*`, C symbols `srvController_*` / `firefly_msg`, log
modules `firefly_*`.

Kconfig and CMake target names live in namespaces shared with all of Zephyr and
NCS, so the prefix is load-bearing, not decorative.

## Not yet a Zephyr module

`port/drivers`, `port/boards` and `port/dts` are empty placeholders. When the
first out-of-tree driver or the custom board lands, add `firefly-app/zephyr/module.yml`:

```yaml
name: firefly-port
build:    { cmake: port, kconfig: port/Kconfig }
settings: { board_root: port, dts_root: port }
```

Until then the app builds against the DK board definition shipped with Zephyr,
and nothing here is registered as a module.

## Status

`code/core` and `tests/core` are complete — 32 ztest cases, all verified
passing. `port/mcu` is a skeleton: every method logs and returns a safe
default, with a TODO naming the driver or subsystem from the IBD that fills it
in. `port/host` is functional: scripted GNSS track with periodic failures, fake
temperature tags, and a session that fails twice before succeeding so the
backoff path is reachable in a native_sim run.
