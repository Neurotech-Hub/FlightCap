# FlightCap nRF52833 Firmware

Firmware workspace for the FlightCap coin-cell wearable. Bring-up apps validate GPIO, accelerometer, and BLE before production firmware.

## Hardware Pin Map

| Signal    | nRF Pin        | Notes                                                                             |
| --------- | -------------- | --------------------------------------------------------------------------------- |
| XL1       | `P0.00`        | 32.768 kHz LFXO                                                                   |
| XL2       | `P0.01`        | 32.768 kHz LFXO                                                                   |
| LED1      | `P0.02`        | Status LED (mirrored with LED0 in bring-up apps)                                  |
| LED0      | `P0.03`        | Status LED                                                                        |
| TOF_EN    | `P0.09`        | TPS63900 enable — **HIGH** powers VL53L0X rail (`flightcap-demo` only)            |
| SCL       | `P0.04`        | I2C (`&i2c0` / TWIM0), 100 kHz standard mode                                      |
| SDA       | `P0.05`        | I2C (`&i2c0` / TWIM0)                                                             |
| ~AXY_INT2 | `P0.15`        | LIS2DH12 interrupt 2 (`GPIO_ACTIVE_LOW`)                                          |
| ~RESET    | `P0.18`        | Hardware reset (not driven by firmware)                                            |
| ~AXY_INT1 | `P0.17`        | LIS2DH12 interrupt 1 — motion IRQ target (`GPIO_ACTIVE_LOW`)                      |
| ~CS_MEM   | `P0.28`        | MX25R8035F SPI NOR chip select                                                    |
| SCK       | `P0.29`        | SPI (`&spi1` / SPIM1)                                                             |
| MISO      | `P0.30`        | SPI                                                                               |
| MOSI      | `P0.31`        | SPI                                                                               |
| MAG_INT   | `P1.09`        | **Electrical LOW when magnet present** (`GPIO_ACTIVE_LOW`)                        |
| ANT       | Fixed RF       | BLE antenna (not assignable)                                                      |

## Hardware Notes

- **MCU**: Nordic **nRF52833-QDAA** (512 KB flash, 128 KB RAM). Main crystal **32 MHz**; 32.768 kHz LFXO on `P0.00` / `P0.01`.
- **Power**: Coin cell tied directly to **VDD**. Battery voltage is read via **SAADC internal VDD sense** (`NRF_SAADC_VDD` on `&adc` channel 0) — no external divider pin required on this hardware.
- **Magnet `MAG_INT` (`P1.09`)**: `GPIO_ACTIVE_LOW` — magnet present asserts electrical LOW; `gpio_pin_get_dt(magnet_sensor)` is non-zero when active.
- **Accelerometer**: LIS2DH12 over **I2C** at address **0x19** (SA0 high) on `&i2c0`. INT1 on `P0.17` for motion interrupts.
- **Time-of-flight**: VL53L0X at I2C **0x29** on the same bus. Rail switched by **TPS63900** via **`TOF_EN` (`P0.09`, active high)** — asserted before sensor init at boot; cut if probe fails. Magnet forced-sleep pauses ToF polling but keeps the rail up (NCS 3.0.2 VL53L0X driver cannot re-init after a power cycle).
- **External flash**: MX25R8035F (8 Mb SPI NOR) on `&spi1` (SPIM1) — instance 0 is shared with TWIM0, so I2C uses `&i2c0` and SPI uses `&spi1`. Validated by `flightcap-mem` (JEDEC ID + last-sector R/W).
- **HFXO / INT1 note**: Nordic defaults map the 32 MHz HFXO to **P0.16 (XC1)** and **P0.17 (XC2)**. Confirm your schematic: if the 32 MHz crystal uses those pins, `~AXY_INT1` on P0.17 cannot also serve as a GPIO interrupt without a board revision.

## Board Target

All active apps build against **`FlightCap_nRF52833`**:

```bash
west build -b FlightCap_nRF52833 applications/<app>
```

Board files: [`boards/NeurotechHub/FlightCap_nRF52833/`](boards/NeurotechHub/FlightCap_nRF52833/).

## Application Structure

Bring-up validation order:

### 1. `applications/flightcap-blink`

- **Purpose**: GPIO sanity check — dual LEDs and magnet path.
- **Behavior**: Magnet absent ⇒ LED0+LED1 blink ~200 ms; magnet present ⇒ both LEDs solid ON. RTT logs `vdd_mv` every 5 s.
- **Build**: `west build -b FlightCap_nRF52833 applications/flightcap-blink`

### 2. `applications/flightcap-axy`

- **Purpose**: LIS2DH12 I2C bring-up and INT1 motion counting.
- **Behavior**: 1 Hz RTT telemetry (`x_mg`, `y_mg`, `z_mg`, `temp_c`, magnet, `INT1_SRC`, `vdd_mv`). 50 ms LED pulse on both LEDs when motion events occurred in the window.
- **Driver**: [`lib/lis2dh12/lis2dh12_zephyr.c`](lib/lis2dh12/lis2dh12_zephyr.c) over I2C `0x19`; register helpers in [`lib/lis2dh12/`](lib/lis2dh12/). Boot-time `flightcap_hw_check()` logs VDD + accelerometer pass/fail before init.
- **Build**: `west build -b FlightCap_nRF52833 applications/flightcap-axy`

### 3. `applications/flightcap-mem`

- **Purpose**: MX25R8035F SPI NOR bring-up — JEDEC ID, size, and destructive last-sector R/W test.
- **Behavior**: On boot runs `flightcap_hw_check()` for VDD + flash presence, then `spi_nor_zephyr_verify()`. 1 Hz RTT heartbeat (`MEM status=PASS/FAIL vdd_mv=...`); 50 ms LED pulse when PASS.
- **Build**: `west build -b FlightCap_nRF52833 applications/flightcap-mem`

### 4. `applications/flightcap-ble-test`

- **Purpose**: BLE peripheral bring-up — GATT LED control and magnet-wake System OFF.
- **Behavior**: Advertises as **`FlightCap-BLE`**. GATT LED write toggles **both** LEDs. Deep-sleep characteristic `…7a04` write `0x01` tears down BLE and enters System OFF; wake on magnet present (pad LOW). RTT logs `vdd_mv` every 30 s.
- **TX power**: `CONFIG_BT_CTLR_TX_PWR_ANTENNA=8` in [`applications/flightcap-ble-test/prj.conf`](applications/flightcap-ble-test/prj.conf).
- **Build**: `west build -b FlightCap_nRF52833 applications/flightcap-ble-test`

### 5. `applications/flightcap-demo`

- **Purpose**: Demo firmware — 1 Hz BLE telemetry (motion + distance), magnet forced-sleep, MCUmgr DFU.
- **Sensors**: LIS2DH12 motion on INT1 (`P0.17`); VL53L0X distance via Zephyr `CONFIG_VL53L0X` on I2C `0x29` (rail enabled on `P0.09` / TPS63900).
- **Behavior**: Advertises as **`FlightCap`**. FCP GATT service (`…7b01/02/03`) per [`docs/FlightCap_BLE_Spec.md`](docs/FlightCap_BLE_Spec.md). SMP DFU UUID in scan response. Dual LED heartbeat (50 ms) when ToF healthy; fast-blink when ToF absent. Magnet present stops BLE/ToF polling until released.
- **Build** (MCUboot sysbuild):

```bash
west build -b FlightCap_nRF52833 applications/flightcap-demo --sysbuild
```

## Shared Libraries

| Path | Purpose |
| --- | --- |
| [`lib/flightcap_hw/`](lib/flightcap_hw/) | Boot-time orchestrator — `flightcap_hw_check(mask, &status)` runs per-component `*_check()` probes |
| [`lib/vbatt/`](lib/vbatt/) | SAADC VDD read (`vbatt_init`, `vbatt_read_mv`, `vbatt_check`) via `zephyr,user` → `&adc` channel 0 |
| [`lib/lis2dh12/`](lib/lis2dh12/) | LIS2DH12 register defs + Zephyr I2C wrapper (`lis2dh12_zephyr_check`, `lis2dh12_zephyr_init`, reads) |
| [`lib/vl53l4cd/`](lib/vl53l4cd/) | VL53L0X power/check/read (`vl53l0x_zephyr_*`) and rolling trimmed-mean distance filter |
| [`lib/spi_nor/`](lib/spi_nor/) | MX25R8035 SPI NOR check (`spi_nor_zephyr_check`) and R/W verify (`spi_nor_zephyr_verify`) |

## Validation Checklist

### `flightcap-blink`

1. Flash for `FlightCap_nRF52833`.
2. No magnet: LED0+LED1 blink 200 ms; RTT `vdd_mv` ~2700–3300 mV.
3. Magnet present: both LEDs solid ON.
4. Toggle magnet ≥10×: deterministic, no lockup.

### `flightcap-axy`

1. RTT shows `VDD check PASS`, `LIS2DH12 check PASS`, and `LIS2DH12 ready WHO_AM_I=0x33`.
2. 1 Hz log lines with plausible accel values; `motion_1s` increments on tap/shake.
3. `vdd_mv` present in each log line.

### `flightcap-mem`

1. RTT shows JEDEC ID `c2 28 14`, flash size `1048576`, and `MEM verification: PASS`.
2. Periodic `MEM status=PASS vdd_mv=...` every 1 s with LED pulse on PASS.

### `flightcap-ble-test`

1. Scanner finds **`FlightCap-BLE`**; connect and toggle LED GATT — both LEDs follow.
2. Deep sleep: magnet away, write `0x01` to `…7a04`; device enters System OFF; wake with magnet.
3. Periodic RTT `vdd_mv` every ~30 s.

### `flightcap-demo`

1. Scanner finds **`FlightCap`** with FCP service UUID `…7b01`.
2. Subscribe to motion (`…7b02`) and distance (`…7b03`) — 1 Hz notifications.
3. Distance `sample_count` reaches ≥4 within a few seconds when ToF is populated.
4. Magnet present: advertising stops; release magnet — telemetry resumes, filter cleared.
5. DFU: nRF Connect Device Manager can upload via SMP service in scan response.
