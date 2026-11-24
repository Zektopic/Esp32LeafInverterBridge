# leaf_can

Hardware-independent Nissan LEAF / e-NV200 EV-CAN library: frame type, message IDs, CRC-8, signal
encoders/decoders and the 110/160 kW inverter-upgrade translation state machine.

- Plain C99. No heap, no RTOS, no CAN-controller dependency. Not thread safe: call it from one task.
- Packaged as an **Arduino** library (`library.properties`), a **PlatformIO** library (`library.json`), an
  **ESP-IDF** component (`CMakeLists.txt` + `Kconfig`) and a plain **CMake** target.
- Tested on the host. See [Tests](#tests).

## Files

| Header | Contents |
|---|---|
| `leaf_can.h` | Umbrella header, version macros |
| `leaf_can_frame.h` | `leaf_can_frame_t` (id, dlc, flags, data[8]) and helpers |
| `leaf_can_ids.h` | `LEAF_ID_*` constants with sender, period and purpose |
| `leaf_can_checksum.h` | CRC-8 (poly 0x85) and the nibble checksum used by Nissan ECUs |
| `leaf_can_signals.h` | Getters and setters for 0x11A, 0x1D4, 0x1DA, 0x1DB, 0x1F2, 0x55B |
| `leaf_inverter_upgrade.h` | The bridge: torque translation and ZE1 message emulation |

## Signals

| Function | Message | Unit / range |
|---|---|---|
| `leaf_can_11a_get_shifter()` | 0x11A | `LEAF_SHIFTER_PARK/REVERSE/NEUTRAL/DRIVE` |
| `leaf_can_11a_get_eco()` | 0x11A | ECO switch flag |
| `leaf_can_1d4_get/set_torque_request()` | 0x1D4 | 0.25 Nm, signed 12 bit (−2048…2047), setter saturates |
| `leaf_can_1da_get/set_torque_response()` | 0x1DA | 0.5 Nm, signed 11 bit (−1024…1023), setter saturates |
| `leaf_can_1db_get_current_half_amps()` | 0x1DB | 0.5 A, signed 11 bit |
| `leaf_can_1db_get_voltage_half_volts()` | 0x1DB | 0.5 V, 10 bit |
| `leaf_can_1f2_get_charge_status()` | 0x1F2 | `LEAF_CHARGE_STATUS_*` |
| `leaf_can_55b_get_soc_permille()` | 0x55B | 0.1 % |

Setters only change the bits of their own signal and never update the checksum. Call
`leaf_can_frame_set_crc8()` once you have finished editing a frame.

## Inverter upgrade

```c
leaf_inverter_config_t cfg;
leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_160KW);
cfg.min_soc_permille_for_boost = 700;          /* optional: no boost below 70 % SOC */

leaf_inverter_t inv;
leaf_inverter_init(&inv, &cfg);

/* for every received frame: */
if (leaf_inverter_process(&inv, from_bus, &frame, emit_cb, ctx)) {
    transmit(leaf_bus_other(from_bus), &frame);  /* forward AFTER the call */
}
```

- `from_bus` is `LEAF_BUS_VCM` or `LEAF_BUS_INVERTER`. The logic depends on direction.
- `emit_cb(ctx, bus, frame)` receives every synthesised frame. Its destination is always the inverter bus.
- `leaf_inverter_set_config()` changes the configuration at runtime and keeps counters and learned state.
- `inv.stats` counts frames in, modified, generated and clamped.

Defaults can be overridden at build time with `-DLEAF_TORQUE_MULTIPLIER_110KW_PERMILLE=…`,
`LEAF_TORQUE_MULTIPLIER_160KW_PERMILLE`, `LEAF_REGEN_MULTIPLIER_PERMILLE`, `LEAF_EMULATE_1F2_GROUP_DEFAULT` and
`LEAF_MIN_SOC_PERMILLE_FOR_BOOST_DEFAULT`. In ESP-IDF they appear in menuconfig under *leaf_can*.

## Using it in another project

**ESP-IDF**: add the directory as an extra component, or let the component manager fetch it:

```yaml
# main/idf_component.yml
dependencies:
  leaf_can:
    git: https://github.com/Zektopic/Esp32LeafInverterBridge.git
    path: lib/leaf_can
```

**PlatformIO**: copy or submodule the folder and add `lib_extra_dirs = path/to/lib`.

**Arduino IDE**: copy the `leaf_can` folder into `Arduino/libraries`, then `#include <leaf_can.h>`.

**CMake** (Linux tools, SocketCAN, STM32CubeIDE…):

```cmake
add_subdirectory(path/to/leaf_can)
target_link_libraries(my_app PRIVATE leaf_can)
```

## Tests

```bash
cmake -S . -B build -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
cmake --build build
ctest --test-dir build --output-on-failure
```

- `test/test_leaf_can.c` covers the checksums, every codec over its full range, the translation rules and
  the emulation timing and sequences.
- `test/diff_original.cpp` compiles the **original v1.3.0 Arduino handler** (`test/reference/`) on the host
  and replays randomised drive traffic through both implementations, comparing every frame they would send.
  Intentional differences (saturation instead of wrap-around, exact regen scaling, the restored AC-charge
  guard) are listed at the top of the file.
