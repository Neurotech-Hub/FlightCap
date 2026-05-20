# FlightCap nRF52840 Firmware

This repository is the active FlightCap firmware workspace. The `Reference/` directory is retained as historical source material and is not part of active implementation.

The legacy `applications/juxta5-8-prod` directory is **parked** — its source code (and the companion NOR-logging spec in `docs/`) is kept only as a reference for the upcoming `flightcap-prod` app. It is not built against the FlightCap board.

## Hardware Pin Map

| Signal    | nRF Pin        | Notes                                                                             |
| --------- | -------------- | --------------------------------------------------------------------------------- |
| XL1       | `P0.00`        | External oscillator                                                               |
| XL2       | `P0.01`        | External oscillator                                                               |
| SPI MISO  | `P0.08`        | Shared SPI bus                                                                    |
| LED0      | `P0.09`        | Status LED                                                                        |
| SPI SCK   | `P0.12`        | Shared SPI bus                                                                    |
| ~RESET    | `P0.18`        | Reset pin                                                                         |
| MAG_INT   | `P0.25`        | **Electrical LOW when magnet present** (active-low; devicetree `GPIO_ACTIVE_LOW`) |
| ~AXY_INT2 | `P0.28` / AIN4 | Accelerometer interrupt 2 (input)                                                 |
| SDA       | `P0.30`        | I2C SDA — **Nordic Low-Frequency I/O pin** (see I2C notes below)                  |
| ~AXY_CS   | `P1.06`        | LIS2DH12 chip select                                                              |
| SPI MOSI  | `P1.09`        | Shared SPI bus                                                                    |
| ~AXY_INT1 | `P1.11`        | Accelerometer interrupt 1                                                         |
| SCL       | `P1.13`        | I2C SCL                                                                           |
| ANT       | Fixed RF       | BLE antenna (not assignable)                                                      |

## Hardware Notes

- **MCU**: Nordic nRF52840
- **Magnet input `MAG_INT` (`P0.25`)**: DTS uses **`GPIO_ACTIVE_LOW`** — a present magnet asserts **electrical LOW** on the pad. Zephyr **`gpio_pin_get_dt(magnet_sensor)`** is **non-zero** when the magnet is present.
- **Accelerometer**: LIS2DH12TR over SPI (reuse existing LIS2DH12 approach from reference applications)
- **P0.28 / AIN4**: `~AXY_INT2` (accelerometer interrupt 2, input only — not currently wired in firmware)
- **I2C bus (`&i2c1` / TWIM1)**:
  - **SDA = `P0.30`**, **SCL = `P1.13`**, default **`clock-frequency = 100 kHz`** (`I2C_BITRATE_STANDARD`).
  - `P0.30` is flagged as a **Low-Frequency I/O** pin in the [nRF52840 product specification](https://docs.nordicsemi.com/bundle/ps_nrf52840/page/pin.html); LF-I/O pins have reduced drive strength / slew, so we constrain TWIM1 to standard mode (100 kHz) for headroom. `P1.13` is a standard I/O pin.
  - Instance choice: nRF52840 instance 0 is shared between **SPIM0** (used for the accelerometer) and **TWIM0**, so the I2C bus runs on **TWIM1** (`&i2c1`).
  - **Fallback plan**: if a slave is unreliable at 100 kHz, swap to a bit-banged software I2C driver on the same pads (Zephyr `gpio-i2c`); the SDA/SCL physical mapping does not need to change.

## Software Constraints

- No external NOR flash on this board — all storage is internal nRF52840 flash (`storage_partition` at `0x000f0000` for NVS).
- No SAADC FUEL channel on the board DTS; an application that needs ADC can add its own `zephyr,user` channel overlay.

## Planned Application Structure

Bring-up and feature work are intentionally split into separate apps so each subsystem can be validated in isolation.

### `applications/flightcap-blink` (Phase 2)
- **Purpose**: Hardware sanity check for LED and magnet path.
- **Behavior**: **Magnet absent** ⇒ pad **high**, GPIO **inactive** ⇒ LED blinks (~200 ms); **magnet present** ⇒ pad driven **LOW**, GPIO **active** ⇒ LED forced on. RTT magnet lines mirror **inactive ("LOW blink mode") vs active ("forced ON")** rather than spelling out raw voltage — see Hardware Notes for polarity.
- **Dependencies**: GPIO, RTT logging.
- **Reference links**:
  - [`Reference/nRF/applications/p0_05_test`](Reference/nRF/applications/p0_05_test)
  - [`Reference/nRF/applications/juxta-axy`](Reference/nRF/applications/juxta-axy)

### `applications/flightcap-axy` (Implemented)
- **Purpose**: Validate LIS2DH12 SPI transactions and interrupt behavior on FlightCap pins.
- **Behavior**:
  - Uses SPI Mode 0 transport for LIS2DH12.
  - Emits periodic RTT telemetry (`x_mg`, `y_mg`, `z_mg`, `temp_c`, magnet state, motion count, `INT1_SRC`).
  - Logs motion events from INT1 with event counters and latest accelerometer snapshot.
- **Dependencies**: SPI, GPIO interrupt handling, LIS2DH12 register layer.
- **Reference links**:
  - [`Reference/nRF/applications/juxta-axy`](Reference/nRF/applications/juxta-axy)
  - [`Reference/nRF/applications/juxta-ble/src/lis2dh12.c`](Reference/nRF/applications/juxta-ble/src/lis2dh12.c)

### `applications/flightcap-ble-test` (Implemented)
- **Purpose**: BLE bring-up plus power-characterization helpers: LED control and magnet-wake deep sleep emulation.
- **Behavior**:
  - Advertises as **`FlightCap-BLE`** (connectable, fast interval). **TX power** is **+8 dBm** at the radio via **`CONFIG_BT_CTLR_TX_PWR_ANTENNA`** in [`applications/flightcap-ble-test/prj.conf`](applications/flightcap-ble-test/prj.conf) (SoftDevice Controller; nearest legal level if HW differs). Net power at the **antenna** depends on matching and layout—this is not a regulatory certification setting.
  - **Re-advertises** after disconnect unless a **deep sleep** sequence has started (advertising restart is queued from the system workqueue when appropriate).
  - Single custom **128-bit service** `ad2a98a4-2148-4b58-9e14-7e2cbb6c7a01`:
    - **LED** `…7a02`: read + write (+ write-no-rsp); **1 byte** — `0` = LED off, non-zero = on (`DT_ALIAS(led0)`).
    - **Deep sleep** `…7a04`: **write only** (+ write-no-rsp). **`0x01`** begins the sleep sequence (**magnet away** ⇒ pad **high**/GPIO **inactive**, same **`gpio_pin_get_dt(magnet_sensor) == 0`** as blink inactive — magnet present ⇒ **abort**). Runs on a work queue: **`bt_le_adv_stop()`**, **`bt_conn_disconnect`** (reason **Remote Power Off**), brief delay, **`bt_disable()`**, `MAG_INT` **`GPIO_INPUT` + `GPIO_INT_LEVEL_LOW`** (wake when the pad goes **electrically LOW**, i.e. **magnet present**), **`sys_poweroff()`**. **`0x00` or other values** write successfully but **do nothing**. Wake causes a **cold boot** — pair/scan BLE again afterward.
  - RTT logs Bluetooth lifecycle, LED writes, and deep-sleep sequencing.
- **Dependencies**: BLE peripheral, GPIO, **`sys_poweroff`** (`CONFIG_POWEROFF=y`), RTT. Assumes **SoftDevice Controller**; **TX power** (+8 dBm) is set per-app in [`applications/flightcap-ble-test/prj.conf`](applications/flightcap-ble-test/prj.conf).
- **Reference links**:
  - [`Reference/nRF/applications/juxta-ble`](Reference/nRF/applications/juxta-ble)
  - [`Reference/nRF/samples/ble/peripheral`](Reference/nRF/samples/ble/peripheral)

### `applications/flightcap-prod` (Phase 1)

- **Purpose**: First production-target application — accelerometer motion counting plus I2C bus bring-up. Subsequent phases will layer on BLE, storage, etc.
- **Behavior**:
  - LIS2DH12 over SPI with INT1 routed to `~AXY_INT1` (`P1.11`). Motion detector configured with **`MOTION_THRESHOLD_MG = 3`** (more sensitive than `flightcap-axy`'s 16) and `DURATION = 0`.
  - ISR increments a `volatile uint32_t` motion counter on every INT1 edge. The main loop accumulates for **`WINDOW_MS = 1000` ms**, then atomically reads + resets the counter under `irq_lock()`, logs `seq=… motion_1s=…` over RTT, and pulses **LED0 for ~50 ms** as a visual heartbeat for each window reset.
  - I2C bus (`&i2c1` on TWIM1 at 100 kHz, SDA `P0.30` / SCL `P1.13`) is initialized at boot via `DEVICE_DT_GET(DT_ALIAS(i2c_bus))` + `device_is_ready()`. No slaves are attached yet — this just confirms the TWIM driver came up so later phases can address peripherals on the bus.
- **Phase 1 additions (VL53L4CD ToF integration)**:
  - VL53L4CD time-of-flight distance sensor at I2C address `0x29` on `&i2c1`, configured for **50 ms timing budget in continuous ranging mode** (`inter_meas = 0`, ~20 Hz raw sample rate). The board has no `XSHUT` or `GPIO1` line wired, so the driver assumes `XSHUT` is tied to AVDD on the board and polls data-ready over I2C — there is no hardware interrupt path for the ToF.
  - **Reader thread** (`K_THREAD_DEFINE`, priority `K_PRIO_PREEMPT(7)`, stack `1024`) runs independently of the 1 s motion loop. It polls `vl53l4cd_data_ready()` every ~5 ms; on each fresh sample it reads the 15-byte result frame, and if the remapped status is `0` (valid) and `range_mm > 0`, pushes the millimeter value into a **16-entry rolling buffer** behind a `k_mutex`.
  - **Trimmed-mean filter** (`vl53l4cd_filter_*`): the main 1 s loop snapshots the buffer non-destructively, sorts it (insertion sort, N ≤ 16), drops the top/bottom **1/5 (≈ 20%)**, and averages the middle. A minimum of **4 valid samples** is required; below that the snapshot returns `-EAGAIN` and we log `dist=err`. On wake from forced sleep the buffer is cleared so the next reading reflects only post-wake data.
  - **Log line**: each 1 s window emits one of `seq=… motion_1s=… dist_mm=… (n=…)` (valid sample), `seq=… motion_1s=… dist=err (n=…)` (filter not primed, `sample_count < 4`), or `seq=… motion_1s=… dist=tof-absent` (sensor failed to bring up at boot — see Fault behavior).
  - **Fault behavior (VL53L4CD)**: if the sensor does not respond at boot (model-ID mismatch, FW boot-status timeout, or any failure in `vl53l4cd_init` / `vl53l4cd_set_range_timing` / `vl53l4cd_start_continuous`), bring-up is **non-fatal** — the firmware logs the error, leaves `tof_present == false`, suspends the reader thread (no I2C polling against the absent device), and proceeds with BLE bring-up. The main loop then **publishes `dist_mm = 0` / `sample_count = 0` every window** (iOS treats this as invalid per the BLE spec) and **substitutes the same ~5 Hz fast-blink for the normal 50 ms heartbeat** so the failure is unmistakable on hardware without halting BLE telemetry or DFU. This lets the rest of the system (motion counting, BLE advertise/connect, MCUmgr DFU) be exercised on the bench without a populated sensor. GPIO / I2C-bus / LIS2DH12 init failures keep their pre-existing `return -EFOO` behavior (they fail before BLE is brought up).
  - **Forced sleep on magnet present**: at each 1 s window boundary the firmware reads `MAG_INT`. If the magnet is present (`gpio_pin_get_dt(magnet_sensor) > 0`), it stops VL53L4CD continuous ranging, suspends the reader thread (no further I2C traffic), pins `LED0` off, and idles in a 1 s polling loop until the magnet is removed. On wake it restarts ranging, drops any stale samples in the filter, zeros the motion counter under `irq_lock()`, and resumes the normal log + heartbeat pattern. **Wake latency is up to ~1 s** by design (no new ISR, avoids EXTI plumbing).
  - **LED policy** across the observable patterns:
    - **Heartbeat**: 50 ms on, ~950 ms off (once per window). Normal operation.
    - **Continuous fast blink**: 100 ms on / 100 ms off (~5 Hz) for as long as the VL53L4CD is absent. Telemetry + BLE + DFU continue to run alongside this pattern; `dist_mm` reports `0` every window. (Same cadence as the BLE-bring-up halt fault loop, but non-blocking.)
    - **Halt fast blink**: 100 ms on / 100 ms off forever, with nothing else running. BLE bring-up failed (`bt_enable` / `bt_le_adv_start`).
    - **Off**: solid off, no pulses. Forced sleep (magnet present), or LIS2DH12 fault path.
- **Phase 1 additions (BLE telemetry)**:
  - Advertises as **`FlightCap`** (set via `CONFIG_BT_DEVICE_NAME`) with the single primary service UUID `ad2a98a4-2148-4b58-9e14-7e2cbb6c7b01` in the AD payload. UUID base is intentionally distinct from `flightcap-ble-test` (which uses `…7a0X`) so a scanner can tell the two builds apart.
  - **Advertising interval**: 500–600 ms (`0x0320..0x03C0` in 0.625 ms units) when no central is connected — a power/discoverability balance. `BT_LE_ADV_OPT_CONN` connectable, auto-stops on connect, auto-restarts on disconnect via a `k_work` posted from the disconnect callback (same pattern as `flightcap-ble-test`).
  - **Service** (`…7b01`) exposes two notify+read characteristics, 4 bytes each, little-endian:
    - **Motion** (`…7b02`): `uint32 motion_events` over the most-recent 1 s window. Resets each window. Notifies on every window when CCC = Notify.
    - **Distance** (`…7b03`): `uint16 dist_mm` at bytes `[0..1]` + `uint16 sample_count` at bytes `[2..3]`. iOS clients treat `sample_count < 4` as invalid (firmware emits `dist_mm = 0` as a sentinel in that case). Notifies on every window when CCC = Notify.
  - **Notify cadence**: motion first, then distance, both pushed at each 1 s window boundary from `publish_window()` in `main.c`. Latest snapshots are stashed in `atomic_t` so a GATT read at any time returns the most-recent closed window's values even without subscribing.
  - **Forced-sleep BLE behavior**: on magnet apply the firmware first disconnects any active central with `BT_HCI_ERR_REMOTE_USER_TERM_CONN`, calls `bt_le_adv_stop()`, and clears the `reconnect_adv_after_disconnect` atomic so the disconnect callback does **not** queue an adv-restart. The device is therefore not discoverable while sleeping. On magnet release advertising is re-armed alongside the existing ToF / reader-thread resume. **Wake-via-BLE is not supported** — the magnet must be removed.
  - **Fault behavior**: any failure from `bt_enable()` or the initial `bt_le_adv_start()` routes to a hard-halt fast-blink loop (same 100 ms / 100 ms pattern). BLE failures are fatal because there is no remote way to signal status without it — unlike the VL53L4CD failure path, which is non-fatal and runs the same blink pattern *alongside* BLE telemetry/DFU.
  - **Stacks / heap**: `CONFIG_MAIN_STACK_SIZE` bumped 2048 → 6144 (matches `flightcap-ble-test`); `CONFIG_HEAP_MEM_POOL_SIZE=4096` added for the BLE host. `CONFIG_BT_MAX_CONN=1`.
  - **iOS-facing spec**: full byte layouts, discovery flow, Core Bluetooth Swift sketch, and Phase 1 non-goals live in [docs/FlightCap_BLE_Spec.md](docs/FlightCap_BLE_Spec.md) — hand this file to the iOS developer.
- **Phase 1 additions (DFU over MCUmgr SMP)**:
  - DFU uses the **standard MCUmgr SMP service** `8d53dc1d-1db7-4cd3-868b-8a527460aa84` (defined by `<zephyr/mgmt/mcumgr/transport/smp_bt.h>`), registered automatically at `bt_enable()` — no gesture gate, no manual `smp_bt_register()` call. iOS clients ignore the SMP UUID; nRF Connect Device Manager / the `mcumgr` CLI use it for firmware uploads.
  - **Coexistence**: the same GATT server hosts both the FCP telemetry service (`…7b01`) and the SMP service. Two 128-bit UUIDs do not fit in a single 31-byte advertising payload, so the AD carries `Flags + FCP UUID128_SOME` (21 B) and the scan response carries `Name + SMP UUID128_SOME` (29 B). `CONFIG_BT_MAX_CONN=1` means only one central is connected at a time — iOS for telemetry **or** nRF Connect for DFU, not both simultaneously.
  - **MCUboot** is enabled via sysbuild (`SB_CONFIG_BOOTLOADER_MCUBOOT=y` in [applications/flightcap-prod/sysbuild.conf](applications/flightcap-prod/sysbuild.conf)). Images are **unsigned** for simple in-house DFU (`CONFIG_BOOT_SIGNATURE_TYPE_NONE=y` in [applications/flightcap-prod/sysbuild/mcuboot.conf](applications/flightcap-prod/sysbuild/mcuboot.conf)); switch to ECDSA-P256 + a custom key when production signing is required.
  - **Partition layout** is taken straight from the FlightCap board DTS (`mcuboot` 48 KB / `image-0` 464 KB / `image-1` 464 KB / `storage` 64 KB). NCS Partition Manager auto-derives `mcuboot_primary` / `mcuboot_secondary` / `settings_storage` from these — no `pm_static.yml` needed.
  - **BLE link tuning** for usable upload throughput: `CONFIG_BT_L2CAP_TX_MTU=247`, `CONFIG_BT_BUF_ACL_RX_SIZE=251`, `CONFIG_BT_BUF_ACL_TX_SIZE=251`. Heap bumped 4096 → 8192 for MCUmgr SMP packet buffers; MCUmgr has its own 4 KB transport workqueue (`CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE=4096`) so the system workqueue stays at 2048.
  - **Forced-sleep interaction**: the existing magnet teardown (`bt_le_adv_stop()` + disconnect of any active central) automatically takes DFU offline alongside FCP. No extra sleep-path code is needed; DFU comes back when advertising resumes.
  - **Build**: `west build -b FlightCap_nRF52840 applications/flightcap-prod --sysbuild` (sysbuild produces the bootloader, the app, and the DFU artifact `build/flightcap-prod/zephyr/zephyr.signed.bin`).
  - **First flash** (SWD): `west flash` writes the merged HEX (MCUboot + app slot 0). Subsequent updates go over BLE — no SWD required.
  - **DFU update**: open **nRF Connect Device Manager** (free, App Store / Play Store) on phone → scan → tap `FlightCap` → Image tab → Upload `zephyr.signed.bin` → mark as **Test** → device reboots and runs the new image → verify behavior → **Confirm** (otherwise MCUboot reverts on the next boot).
- **Dependencies**: SPI (LIS2DH12), I2C (TWIM1, VL53L4CD), GPIO interrupt handling (accel `INT1`), polled GPIO input (`MAG_INT`), BLE peripheral (SoftDevice Controller, +8 dBm), MCUmgr SMP + MCUboot (DFU), RTT logging.
- **Shared code**: reuses the LIS2DH12 SPI driver (`lis2dh12_zephyr.{c,h}`) directly from `applications/flightcap-axy/src/` via the `CMakeLists.txt` include path, so the two apps stay in sync on the SPI register layer. The VL53L4CD Zephyr port (`vl53l4cd_zephyr.{c,h}` + `vl53l4cd_filter.{c,h}`) lives under `applications/flightcap-prod/src/` and is a direct port of [Pololu's VL53L4CD Arduino library](https://github.com/pololu/vl53l4cd-arduino) (itself derived from ST's ULD, STSW-IMG026); the 88-byte default config table is kept byte-identical for easy auditing.
- **Build**: `west build -b FlightCap_nRF52840 applications/flightcap-prod`
- **Note on parked code**: the legacy `applications/juxta5-8-prod` tree (and `docs/JUXTA_NOR_Flash_Logging_Spec_v3.md`) is retained only for code reference — it depends on the removed external NOR flash and the old `Juxta5-8_nRF52840` board, so it is **not** built against the FlightCap board.

## Validation Plan

### `flightcap-blink` (defined now)

Manual checklist:
1. Program `applications/flightcap-blink` for board `FlightCap_nRF52840`.
2. Boot device with **no magnet** (`MAG_INT` **high**/inactive):
   - Expected: `LED0` toggles every 200 ms.
   - Expected RTT log: `MAG_INT is LOW (blink mode)` (means **inactive** / no magnet).

3. Bring **magnet** so `MAG_INT` is pulled **LOW** (active-low **present**):
   - Expected: `LED0` becomes steady ON within one loop cycle.
   - Expected RTT log: `MAG_INT is HIGH (LED forced ON)` (means **active** / magnet present).
4. Remove magnet again:
   - Expected: LED resumes 200 ms blinking.
5. Repeat magnet toggle at least 10 times:
   - Expected: deterministic transitions, no lockup, no GPIO errors in RTT.

Execution status:
- Checklist defined: yes
- Checklist executed on hardware: pending (requires board session)

### `flightcap-ble-test`

Manual checklist:
1. Build and flash `applications/flightcap-ble-test` for `FlightCap_nRF52840`.
2. With a phone or **nRF Connect**, scan: device **`FlightCap-BLE`** appears.
3. Connect; RTT shows `Connected` with a BLE address.
4. Open the custom service UUID `ad2a98a4-...-7a01`; write **`0x00`** / **`0x01`** to characteristic `...-7a02`; LED0 follows (off/on).
5. Disconnect; RTT shows `Disconnected` with a reason code.
6. Scan again: **`FlightCap-BLE`** should reappear (connectable advertising restarted).
7. Deep sleep (**use with care** — link drops and device **resets after wake**): ensure **`gpio_pin_get_dt(magnet)==0`** (magnet absent, pad idle **high**; otherwise firmware aborts with a WARN). Write **`0x01`** to characteristic `…-7a04`. Expect RTT `"Entering System OFF"`; BLE disappears. **Wake** when **`MAG_INT` falls LOW** (magnet present); cold boot then **`FlightCap-BLE`** advertising resumes.

Execution status:
- Checklist defined: yes
- Checklist executed on hardware: pending (re-verify after rename to FlightCap board)

### Later app validation placeholders

- `flightcap-axy`: implemented, runtime checklist pending hardware session
- `flightcap-ble-test`: implemented; BLE bring-up checklist pending re-verification on FlightCap board
- `flightcap-prod`: Phase 1 implemented (motion counter + I2C bring-up); on-hardware checklist pending board session
