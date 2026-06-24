# flightcap-prod

Production FlightCap firmware for **nRF52833**. It broadcasts sensor telemetry in **BLE manufacturer-specific data** — no pairing, no connection, no GATT.

This document is written for an **Arduino / ESP32 receiver agent** that needs to decode advertisements from the wearable.

## Operational modes

The firmware runs an explicit state machine (`prod_state.c`):

| Mode | Trigger | Behavior |
| --- | --- | --- |
| **Run** (default) | Face-down orientation | ~21 s duty cycle: ToF sample → 10 s advertise → 10–11 s sleep (random jitter). Motion on INT1. |
| **Shelf** | Face-up orientation | BLE off, ToF off, LIS2DH12 low-power with INT2 6D on P0.15. Wake via orientation change. |
| **Pair** | Magnet 3 s hold + release (toggle) | Adv-only loop (no ToF, no sleep). `TELEM_FLAG_PAIR_MODE` (bit 4) set in every packet. |

**Orientation:** Bench calibration (raw LSB, ±2g): **face-up shelf** Z ≈ **+17000**; **face-down run** Z ≈ **−16000**. Enter shelf when `z > 12000` (debounced). Exit shelf when 6D sector changes or `z < 12000` (~45° tilt from face-up). Magnet hold preempts all modes for pair toggling.

**Magnet hold (P1.09, active when magnet present):** Hold magnet for **3 s** (both LEDs on), release after a **500 ms** gap toggles pair mode. A second completed hold exits pair mode (returns to Run or Shelf based on orientation).

**LEDs (both mirrored):**

| Event | LEDs |
| --- | --- |
| Boot / exit shelf → run | 3× blink (100 ms on/off) |
| Advertise window start (Run or Pair) | Single 50 ms pulse |
| Magnet hold in progress | ON up to 3 s |
| Idle / sleep / shelf | OFF |

## Run mode duty cycle

| Phase | Duration | Behavior |
| --- | --- | --- |
| ToF sample | ~0.1–0.5 s | Powers VL53L0X rail, takes **10** distance readings, averages to `distance_mm` |
| Advertise | **10 s** | Non-connectable BLE beacon @ **500 ms** interval; interaction count updates live |
| Sleep | **10–11 s** | Radio off; 10 s + uniform random 0–1000 ms jitter; accelerometer still counts motion on INT1 |
| Repeat | — | ~21 s total cycle (advertise fixed 10 s) |

**Interactions** = LIS2DH12 motion events on INT1 since the last counter reset (after each 10 s advertise window).

## Pair mode (device assignment menu)

- Enter/exit via magnet hold toggle (see above).
- **No ToF sampling** and **no sleep** — continuous 10 s advertise windows back-to-back.
- Filter on **`flags & 0x10`** (`TELEM_FLAG_PAIR_MODE`, bit 4) to show devices in a pair/assign UI.
- **`device_addr[6]`** in manufacturer data is the **stable per-device ID** — save these 6 bytes when the user assigns a cap. Do not use the scanner’s BLE MAC field or `seq` as the device ID.

## Device identity (`device_addr`)

Every advertisement (Run and Pair) includes a **6-byte BLE identity address** in the manufacturer payload (`version >= 0x02`):

- Same byte order as a normal MAC string (`C9:21:FE:5D:EF:F3` → bytes `C9 21 FE 5D EF F3`).
- Sourced from the nRF52833 **static random identity** (programmed in hardware; stable for the life of the chip).
- **Use this as your saved “device ID”** on the receiver — filter future packets with `memcmp(device_addr, saved, 6) == 0`.
- Reject `version < 0x02` frames if you require `device_addr` (older firmware).

## Shelf mode

- Stops BLE (`bt_disable`) and powers off ToF.
- LIS2DH12 runs at 1 Hz with **INT2 6D orientation** routed to **P0.15** (`accel-int2`).
- **Enter:** face-up when `z > 12000` (bench: z ≈ +17000), debounced.
- **Stay:** remains in shelf while face-up; no spurious Z-band polling.
- **Wake / exit:** INT2 6D, **`6D sector changed`** poll, or **`z < 12000`** (left face-up band — ~45° tilt, not full face-down). Magnet hold preempts for pair mode.
- RTT logs: `shelf wake: left face-up Z` or `shelf wake: 6D sector changed` (not `face-down Z confirmed`).

## Receiver requirements

- **Passive scan** is enough for telemetry — manufacturer data is in the **primary advertisement**.
- **Active scan** (or a scanner that reads scan response) shows the device name **`FCap`** (`CONFIG_BT_DEVICE_NAME`) for debugging.
- Filter on **manufacturer data** with company ID **`0x4E48`**, magic **`0xA5`**, and **`version == 0x02`**. Match assigned devices by **`device_addr[6]`** inside the payload (passive scan is enough).

Advertisement type: legacy **non-connectable, scannable** (`ADV_SCAN_IND`); telemetry in primary AD, name in scan response.

During Run mode **sleep** (10–11 s) you will see **no packets**. Shelf mode also has no BLE traffic.

## Manufacturer data layout

Source of truth: [`src/telemetry_adv.h`](src/telemetry_adv.h).

Total payload: **17 bytes**, little-endian multi-byte fields, immediately after the 2-byte company ID in the BLE AD manufacturer field.

On the wire, the full manufacturer-specific value is:

```
[0x48, 0x4E,  magic, version, addr0..addr5, seq_lo, seq_hi, dist_lo, dist_hi, int_lo, int_hi, flags]
 ^^^^^^^^^^^
 company_id = 0x4E48 (LE)
              ^^^^^^^^^^^^^^^^
              device_addr[6] — save this as the device ID
```

| Byte | Field | Type | Description |
| --- | --- | --- | --- |
| 0–1 | `company_id` | `uint16` LE | **`0x4E48`** — provisional Neurotech Hub ID (`'N' \| 'H'<<8`) |
| 2 | `magic` | `uint8` | **`0xA5`** — frame marker; reject if wrong |
| 3 | `version` | `uint8` | **`0x02`** — schema version (requires `device_addr`) |
| 4–9 | `device_addr` | `uint8[6]` | **Per-device ID** — BLE identity, MSB-first byte order |
| 10–11 | `seq` | `uint16` LE | Increments when `distance_mm` or `interactions` changes; wraps naturally |
| 12–13 | `distance_mm` | `int16` LE | Average ToF distance in millimeters |
| 14–15 | `interactions` | `uint16` LE | Motion event count for current window |
| 16 | `flags` | `uint8` | Status bits (see below) |

### `distance_mm`

- Valid range when flag bit 0 is set: typically **20–2000** mm (sensor dependent).
- Invalid / error: **`INT16_MIN` (`0x8000`, bytes `0x00 0x80`)** with bit 0 clear.
- Distance is already integer mm on the transmitter — **do not parse as float on the wire**.

### `flags`

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `DIST_VALID` | `distance_mm` is a valid sample |
| 1 | `INTERACT_VALID` | `interactions` counter is active |
| 2 | `TOF_ERR` | ToF sample or sensor failed this cycle |
| 3 | `STALE` | Reserved for future use (not set in v1) |
| 4 | `PAIR_MODE` | Device is in magnet-toggled pair/advertise-only mode |
| 5–7 | — | Reserved; ignore |

Typical healthy Run packet: `flags & 0x03 == 0x03`.

Pair mode packet: `(flags & 0x10) != 0` — use for pair-menu filtering; **`device_addr` is still present** and is what you save on assign.

### `device_addr`

- **Always present in v0x02+** — copy all 6 bytes to non-volatile storage when the user picks a device.
- Compare with `memcmp(a, b, 6) == 0` — do not format as a string unless displaying to the user.
- All-zero `device_addr` means the transmitter has not yet populated identity (should not happen during normal advertise); ignore those frames.

### `seq`

Use `seq` to detect new telemetry. Same `seq` repeated across consecutive 500 ms adverts is normal if values unchanged. Expect `seq` to bump when interactions increase during the 10 s window.

## Arduino / ESP32 parsing example

Works with any stack that exposes raw AD manufacturer bytes (ESP32 `BLEAdvertisedDevice`, NimBLE scan callback, etc.).

```cpp
#include <stdint.h>
#include <string.h>
#include <Arduino.h>

static constexpr uint16_t FLIGHTCAP_COMPANY_ID = 0x4E48;
static constexpr uint8_t  FLIGHTCAP_MAGIC      = 0xA5;
static constexpr uint8_t  FLIGHTCAP_VERSION    = 0x02;

static constexpr uint8_t FLAG_DIST_VALID     = 1u << 0;
static constexpr uint8_t FLAG_INTERACT_VALID = 1u << 1;
static constexpr uint8_t FLAG_TOF_ERR        = 1u << 2;
static constexpr uint8_t FLAG_PAIR_MODE      = 1u << 4;

#pragma pack(push, 1)
struct FlightCapTelemetry {
  uint16_t company_id;
  uint8_t  magic;
  uint8_t  version;
  uint8_t  device_addr[6];
  uint16_t seq;
  int16_t  distance_mm;
  uint16_t interactions;
  uint8_t  flags;
};
#pragma pack(pop)

static inline uint16_t rd_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline int16_t rd_le16s(const uint8_t *p) {
  return (int16_t)rd_le16(p);
}

// manufacturerData = payload from BLE AD type 0xFF (includes company ID)
bool flightcap_parse(const uint8_t *data, size_t len, FlightCapTelemetry *out) {
  if (!data || !out || len < sizeof(FlightCapTelemetry)) {
    return false;
  }

  if (rd_le16(data) != FLIGHTCAP_COMPANY_ID) return false;
  if (data[2] != FLIGHTCAP_MAGIC) return false;
  if (data[3] != FLIGHTCAP_VERSION) return false;

  memcpy(out, data, sizeof(FlightCapTelemetry));
  return true;
}

bool flightcap_addr_valid(const FlightCapTelemetry *t) {
  if (!t) return false;
  for (int i = 0; i < 6; i++) {
    if (t->device_addr[i] != 0) return true;
  }
  return false;
}

bool flightcap_addr_match(const FlightCapTelemetry *t, const uint8_t saved[6]) {
  return t && saved && memcmp(t->device_addr, saved, 6) == 0;
}

void flightcap_addr_format(const uint8_t addr[6], char *buf, size_t buflen) {
  // Optional UI helper only — store raw bytes, not strings.
  snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// Saved IDs (example — use NVS / EEPROM / flash in production)
static uint8_t assigned_addrs[8][6];
static size_t  assigned_count = 0;

bool flightcap_is_assigned(const uint8_t addr[6]) {
  for (size_t i = 0; i < assigned_count; i++) {
    if (memcmp(assigned_addrs[i], addr, 6) == 0) return true;
  }
  return false;
}

void flightcap_assign(const uint8_t addr[6]) {
  if (assigned_count >= 8 || flightcap_is_assigned(addr)) return;
  memcpy(assigned_addrs[assigned_count++], addr, 6);
}

// Example handler (pseudo — adapt to your BLE library):
void on_ble_advert(const uint8_t *mfg, size_t mfg_len) {
  FlightCapTelemetry t;

  if (!flightcap_parse(mfg, mfg_len, &t) || !flightcap_addr_valid(&t)) {
    return;
  }

  if (t.flags & FLAG_PAIR_MODE) {
    // Pair menu: list unassigned caps; on user tap call flightcap_assign(t.device_addr)
    return;
  }

  // Run mode: only process caps the user has assigned
  if (!flightcap_is_assigned(t.device_addr)) {
    return;
  }

  bool dist_ok = (t.flags & FLAG_DIST_VALID) != 0;
  bool tof_err = (t.flags & FLAG_TOF_ERR) != 0;

  Serial.printf("seq=%u dist=%d mm interactions=%u flags=0x%02X tof_err=%d\n",
                t.seq,
                dist_ok ? t.distance_mm : -1,
                t.interactions,
                t.flags,
                tof_err);
}
```

### Receiver workflow summary

1. **Pair / assign:** Scan for `company_id == 0x4E48`, `magic == 0xA5`, `version == 0x02`, `flags & PAIR_MODE`. Show `device_addr` (formatted for display if needed). On user confirm, **save the 6 raw bytes**.
2. **Run / track:** On each advert, parse payload and **`memcmp(device_addr, saved, 6)`** — ignore packets from other caps.
3. **Do not** use BLE scanner MAC, device name (`FCap`), or `seq` as the device ID.

### ESP32 Arduino (Bluedroid) hint

```cpp
if (advertisedDevice.haveManufacturerData()) {
  String mfg = advertisedDevice.getManufacturerData();
  flightcap_parse((const uint8_t *)mfg.c_str(), mfg.length(), &t);
}
```

Use **passive scan** (`setActiveScan(false)`). Active scan is not required.

## What you will not see

- Connectable advertisements (except during Run/Pair adv windows — still non-connectable)
- GATT services or characteristics
- Packets during Run sleep or shelf mode

## Timing expectations for receiver logic

**Run mode:**

```
|-- ToF --|-- 10 s advertise @ 500 ms --|-- 10–11 s silent (jittered) --|
|  ~0.3s  |  ~20 adverts per window      |  0 adverts                   |
```

**Pair mode:** continuous 10 s advertise bursts with no silent gap.

- Debounce duplicate handling: same `seq` + same fields for up to 500 ms is normal.
- After a **10–11 s** gap (Run only), expect a new burst; `distance_mm` may change every cycle, `interactions` resets to 0 at the start of each new advertise window (then grows during the window).

## Firmware build (reference)

Board: **`FlightCap_nRF52833`**

```bash
west build -b FlightCap_nRF52833 applications/flightcap-prod --sysbuild
west flash
```

Debug log via **Segger RTT** (not UART). Example RTT lines:

```
state RUN->SHELF
INT2_SRC=0x.. orientation_irq
orientation z_mg=.. face_up=0/1
state SHELF->RUN
state ->PAIR
magnet_hold phase=Holding
cycle=N dist_mm=... interactions=... seq=... flags=0x..
```

## Related source files

| File | Role |
| --- | --- |
| [`src/telemetry_adv.h`](src/telemetry_adv.h) | Packet struct, company ID, flag definitions |
| [`src/telemetry_adv.c`](src/telemetry_adv.c) | BLE advertising start/stop/update |
| [`src/prod_state.c`](src/prod_state.c) | Run / shelf / pair state machine, magnet hold, LEDs |
| [`src/main.c`](src/main.c) | GPIO/BT/HW init, INT1/INT2 callbacks |
| [`../../lib/lis2dh12/lis2dh12_zephyr.c`](../../lib/lis2dh12/lis2dh12_zephyr.c) | Motion (INT1) + orientation (INT2 6D) + shelf mode |
| [`../../boards/NeurotechHub/FlightCap_nRF52833/`](../../boards/NeurotechHub/FlightCap_nRF52833/) | Pin map, TOF_EN on P0.09 (NFC1 as GPIO), INT2 on P0.15 |

For connectable GATT telemetry (different product mode), see [`flightcap-demo`](../flightcap-demo/) and [`docs/FlightCap_BLE_Spec.md`](../../docs/FlightCap_BLE_Spec.md).
