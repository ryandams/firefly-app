# firefly-app

Zephyr/NCS asset-tracker foundation for
`nrf54lm20dk/nrf54lm20a/cpuapp` and `native_sim`.

The application uses a C entry point, C-compatible message definitions, C++
services, a copied ordered zbus stream, injectable hardware ports, and ztest
suites. `west.yml` temporarily pins sdk-nrf
`e8b176a4900a560d180112302fc5b48dde19bf70` and its selected sdk-zephyr
`0e25e66118d903037be2350cccc382134b7e62ad` for nRF93M1 support. The v3.4.0
toolchain remains in use until Nordic publishes an aligned stable release.

## Direct west commands

```bash
west build -p -b native_sim firefly-app -- -DEXTRA_CONF_FILE=conf/development.conf
west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp firefly-app -- -DEXTRA_CONF_FILE=conf/development.conf
west twister -T firefly-app/tests -p native_sim --tag firefly --warnings-as-errors
west twister -T firefly-app/tests -p nrf54lm20dk/nrf54lm20a/cpuapp --tag portable --build-only
```

## Layout

```text
code/event_bus.*           validated, ordered message envelope
code/thread.hpp            reusable lifecycle-safe Zephyr thread wrapper
code/service/              controller, button listener, LED port boundary,
                            srvNetwork (real modem port) and the raw-AT
                            srvModemTest_* services
boards/                    target/native configuration and devicetree, plus
                            one conf/modem_*.conf + boards/*.overlay pair per
                            externally-wired modem (eg800z, le270,
                            nrf93m1_ext) - see "Modem targets" below
conf/                      development/production profiles and per-modem
                            fragments
tests/thread/              generic thread lifecycle/stack contract
tests/service/             portable behavior and fault-injection tests
tests/gpio/                native GPIO/input integration
tests/hardware/            connected-DK tests
tests/nrf93m1_power/       nRF93M1 raw-AT/PSM hardware test
zephyr/patches/            vendor modem_cellular driver patches (west patch
                            apply), one per externally-wired modem
scripts/                   CI/static enforcement
```

## Modem targets

Three modems are supported, selected at build time rather than by git
branch - see the outer firefly repo's `31_build.sh`/`32_build.sh`/
`33_build.sh`:

- **nRF93M1** (on-board, `TARGET=nrf93m1dk`): uses
  `boards/nrf93m1dk_nrf54l15_cpuapp.conf` directly, no fragment needed.
- **Quectel EG800Z** (external, uart21): `conf/modem_eg800z.conf` +
  `boards/eg800z.overlay`. No AT+CMUX - drives the modem directly via
  `modemEg800zPpp.cpp` (`FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP`).
- **Fibocom LE270** (external, uart21 - same physical pins as the EG800Z):
  `conf/modem_le270.conf` + `boards/le270.overlay`. Supports AT+CMUX, so it
  uses the plain `modem_cellular` transport
  (`FIREFLY_NETWORK_TRANSPORT_MODEM_CELLULAR`), same as the nRF93M1.

An nRF93M1 module wired externally to this board (rather than the on-board
nrf93m1dk target) is also available as `conf/modem_nrf93m1_ext.conf` +
`boards/nrf93m1_ext.overlay` (uart30), with no dedicated build script yet.

All application components are compiled in every target, native simulator, and
test build. Required LED aliases produce explicit build assertions. Test-only
dependency hooks require `CONFIG_ZTEST` and cannot be enabled in a production
application accidentally.

See the workspace README for the complete build matrix, release signing flow,
quality gates, hardware limitations, and threaded-service acceptance criteria.
