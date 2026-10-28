# firefly-app

Zephyr/NCS asset-tracker foundation for the nRF93M1 DK
(`nrf93m1dk/nrf54l15/cpuapp`), the nRF54LM20 DK
(`nrf54lm20dk/nrf54lm20a/cpuapp`) and `native_sim`.

The application uses a C entry point, C-compatible message definitions, C++
services, a copied ordered zbus stream, injectable hardware ports, and ztest
suites. `west.yml` temporarily pins sdk-nrf main at an exact commit for
nRF93M1 support (see the comment there for the sdk-zephyr it selects). The
v3.4.0 toolchain remains in use until Nordic publishes an aligned stable
release.

## Direct west commands

The board passed to `-b` must match the one selected in `prj.conf` (see
"Modem targets" below); the default selection is the nRF93M1 DK.

```bash
west build -p -b native_sim firefly-app -- -DEXTRA_CONF_FILE=conf/development.conf
west build -p -b nrf93m1dk/nrf54l15/cpuapp firefly-app -- -DEXTRA_CONF_FILE=conf/development.conf
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
                            one overlay per externally-wired modem (eg800z,
                            le270, nrf93m1_ext) - see "Modem targets" below
conf/                      development/production profiles, one fragment per
                            modem, and echo_server.conf (UDP echo server)
tests/thread/              generic thread lifecycle/stack contract
tests/service/             portable behavior and fault-injection tests
tests/gpio/                native GPIO/input integration
tests/hardware/            connected-DK tests
tests/nrf93m1_power/       nRF93M1 power/raw-AT/PPP hardware tests
tests/eg800z_power/        EG800Z power-key hardware test
zephyr/patches/            zephyr patches (west patch apply): LE270 and
                            EG800Z drivers, nRF93M1 and modem_cellular fixes
scripts/                   CI/static enforcement
```

## Modem targets

The board and modem are selected in `prj.conf`
(`CONFIG_FIREFLY_TARGET_BOARD_*` and `CONFIG_FIREFLY_TARGET_MODEM_*`, see
`Kconfig.firefly`). `CMakeLists.txt` adds the selected modem's conf/overlay
pair and fails the build if the selection doesn't match the board being built.
native_sim builds ignore the selection and build no modem.

- **nRF93M1** (on-board, `NRF93M1DK` + `NRF93M1`): `conf/modem_nrf93m1.conf`,
  no overlay (the board's devicetree already has the modem).
- **Quectel EG800Z** (external, uart21): `conf/modem_eg800z.conf` +
  `boards/eg800z.overlay`. No AT+CMUX - drives the modem directly via
  `modemEg800zPpp.cpp` (`FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP`).
- **Fibocom LE270** (external, uart21 - same physical pins as the EG800Z):
  `conf/modem_le270.conf` + `boards/le270.overlay`. Supports AT+CMUX, so it
  uses the plain `modem_cellular` transport
  (`FIREFLY_NETWORK_TRANSPORT_MODEM_CELLULAR`), same as the nRF93M1.

An nRF93M1 module wired externally to the nRF54LM20 DK (`NRF93M1_EXT`) uses
`conf/modem_nrf93m1_ext.conf` + `boards/nrf93m1_ext.overlay` (uart30).

Every modem target also gets `conf/echo_server.conf`, the GSFleet UDP echo
server srvNetwork sends to. The server gets a new port each day; change it
there.

All application components are compiled in every target, native simulator, and
test build. Required LED aliases produce explicit build assertions. Test-only
dependency hooks require `CONFIG_ZTEST` and cannot be enabled in a production
application accidentally.

See the workspace README for the complete build matrix, release signing flow,
quality gates, hardware limitations, and threaded-service acceptance criteria.
