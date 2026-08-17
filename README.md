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
code/service/              controller, button listener, LED port boundary
boards/                    target/native configuration and devicetree
conf/                      development and production profiles
tests/thread/              generic thread lifecycle/stack contract
tests/service/             portable behavior and fault-injection tests
tests/gpio/                native GPIO/input integration
tests/hardware/            connected-DK tests
scripts/                   CI/static enforcement
```

All application components are compiled in every target, native simulator, and
test build. Required LED aliases produce explicit build assertions. Test-only
dependency hooks require `CONFIG_ZTEST` and cannot be enabled in a production
application accidentally.

See the workspace README for the complete build matrix, release signing flow,
quality gates, hardware limitations, and threaded-service acceptance criteria.
