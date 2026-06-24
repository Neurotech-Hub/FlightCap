# flightcap-prod

Production FlightCap firmware for **nRF52833**. It broadcasts sensor telemetry in **BLE manufacturer-specific data** — no pairing, no connection, no GATT.

This document is written for an **Arduino / ESP32 receiver agent** that needs to decode advertisements from the wearable.

## Operational modes

The firmware runs an explicit state machine (`prod_state.c`):

| Mode | Trigger | Behavior |
| --- | --- | --- |
| **Run** (default) | Face-down orientation | ~20 s duty cycle: ToF sample → 10 s advertise → 10 s sleep (+ per-device offset). Motion on INT1. |
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
| Advertise | **10 s** | Non-connectable BLE beacon @ **~100 ms** interval (per-device dither); interaction count updates live |
| Sleep | **10 s + 0–500 ms** | Radio off; deterministic per-device offset from BLE identity; accelerometer still counts motion on INT1 |
| Repeat | — | ~20 s total cycle (10 s adv + 10 s sleep; ToF adds ~0.1–0.5 s) |

Each cap applies a **one-time phase offset** (0–19 999 ms, derived from `device_addr`) at boot and after shelf→run so nearby caps do not share the same adv/sleep boundary.

**Interactions** = LIS2DH12 motion events on INT1 since the last **60 s accrual reset** (counts during Run advertise, Run sleep, and Pair mode; INT1 off in shelf).

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

- **Passive scan** is sufficient for telemetry — manufacturer data is in the **primary advertisement** (`device_addr`, `distance_mm`, `interactions`, `vbatt_mv` on v0x03+).
- Enable **active scan** if you also want the **`FCap`** device name from scan response (debugging / UI).
- Filter on **manufacturer data** with company ID **`0x4E48`**, magic **`0xA5`**, and **`version >= 0x02`**. Current firmware sends **`0x03`**. Match assigned devices by **`device_addr[6]`** inside the payload — **not** the scanner-reported BLE MAC.

Advertisement type: legacy **non-connectable, scannable** (`ADV_SCAN_IND`); telemetry in primary AD, name in scan response.

During Run mode **sleep** (~10 s + device offset) you will see **no packets**. Shelf mode also has no BLE traffic.

### 10 s scanner duty cycle (Arduino / ESP32)

If your receiver listens for **~10 s every 10 s** (50% scan duty matching the cap’s ~50% advertise duty):

- A full **10 s listen** typically overlaps **multiple seconds** of advertising and yields many packets (~100 ms spacing during on-windows).
- Occasional **misses are still possible** if your window aligns entirely with the cap’s sleep phase — mitigate with **±1–2 s jitter** on the scanner timer (do not fire scans on a rigid 10.000 s wall-clock grid).
- **Multi-cap lab:** per-device phase offset spreads caps in time; expect occasional same-channel overlap anyway — identify caps by **`device_addr`** in the payload, not arrival order or scanner MAC.

## Manufacturer data layout

Source of truth: [`src/telemetry_adv.h`](src/telemetry_adv.h).

Total payload: **19 bytes** (v0x03), little-endian multi-byte fields, immediately after the 2-byte company ID in the BLE AD manufacturer field. v0x02 frames were 17 bytes (no battery field).

On the wire, the full manufacturer-specific value is:

```
[0x48, 0x4E,  magic, version, addr0..addr5, seq_lo, seq_hi, dist_lo, dist_hi, int_lo, int_hi, flags, vbatt_lo, vbatt_hi]
 ^^^^^^^^^^^
 company_id = 0x4E48 (LE)
              ^^^^^^^^^^^^^^^^
              device_addr[6] — save this as the device ID
                                                      ^^^^^^^^^^^^^
                                                      vbatt_mv (v0x03+)
```

| Byte | Field | Type | Description |
| --- | --- | --- | --- |
| 0–1 | `company_id` | `uint16` LE | **`0x4E48`** — provisional Neurotech Hub ID (`'N' \| 'H'<<8`) |
| 2 | `magic` | `uint8` | **`0xA5`** — frame marker; reject if wrong |
| 3 | `version` | `uint8` | **`0x03`** — current schema (`0x02` = no `vbatt_mv`) |
| 4–9 | `device_addr` | `uint8[6]` | **Per-device ID** — BLE identity, MSB-first byte order |
| 10–11 | `seq` | `uint16` LE | Increments when telemetry fields change; wraps naturally |
| 12–13 | `distance_mm` | `int16` LE | Average ToF distance in millimeters |
| 14–15 | `interactions` | `uint16` LE | Motion events since last 60 s accrual reset |
| 16 | `flags` | `uint8` | Status bits (see below) |
| 17–18 | `vbatt_mv` | `uint16` LE | Supply voltage in millivolts (v0x03+); valid when bit 5 set |

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
| 5 | `VBATT_VALID` | `vbatt_mv` is a valid SAADC sample |
| 6–7 | — | Reserved; ignore |

Typical healthy Run packet: `flags & 0x03 == 0x03`; battery present when `(flags & 0x20) != 0`.

Pair mode packet: `(flags & 0x10) != 0` — use for pair-menu filtering; **`device_addr` is still present** and is what you save on assign.

### `device_addr`

- **Always present in v0x02+** — copy all 6 bytes to non-volatile storage when the user picks a device.
- Compare with `memcmp(a, b, 6) == 0` — do not format as a string unless displaying to the user.
- All-zero `device_addr` means the transmitter has not yet populated identity (should not happen during normal advertise); ignore those frames.

### `seq`

Use `seq` to detect new telemetry. Same `seq` repeated across consecutive ~100 ms adverts is normal if values unchanged. Expect `seq` to bump when interactions increase or `vbatt_mv` changes during the current cycle.

### `vbatt_mv`

- **v0x03+ only** — sampled once per Run/Pair cycle via SAADC (same path as boot VDD check).
- Millivolts on the wire (e.g. `3047` = 3.047 V). Typical coin-cell range ~2000–3300 mV.
- Ignore when `VBATT_VALID` (bit 5) is clear; field may be zero.

### `interactions`

- Counts INT1 motion edges; **resets every 60 s** (wall clock), not each advertise window.
- During Run, the counter keeps accruing through the 10 s advertise burst **and** the ~10 s sleep until the 60 s timer fires.
- In shelf mode INT1 is off — no new events, but the 60 s timer still runs.

## Arduino / ESP32 parsing example

Works with any stack that exposes raw AD manufacturer bytes (ESP32 `BLEAdvertisedDevice`, NimBLE scan callback, etc.).

```cpp
#include <stdint.h>
#include <string.h>
#include <Arduino.h>

static constexpr uint16_t FLIGHTCAP_COMPANY_ID = 0x4E48;
static constexpr uint8_t  FLIGHTCAP_MAGIC      = 0xA5;
static constexpr uint8_t  FLIGHTCAP_VERSION    = 0x03;
static constexpr uint8_t  FLIGHTCAP_VERSION_MIN = 0x02;

static constexpr uint8_t FLAG_DIST_VALID     = 1u << 0;
static constexpr uint8_t FLAG_INTERACT_VALID = 1u << 1;
static constexpr uint8_t FLAG_TOF_ERR        = 1u << 2;
static constexpr uint8_t FLAG_PAIR_MODE      = 1u << 4;
static constexpr uint8_t FLAG_VBATT_VALID    = 1u << 5;

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
  uint16_t vbatt_mv;
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
  if (!data || !out || len < 17) {
    return false;
  }

  if (rd_le16(data) != FLIGHTCAP_COMPANY_ID) return false;
  if (data[2] != FLIGHTCAP_MAGIC) return false;
  if (data[3] < FLIGHTCAP_VERSION_MIN || data[3] > FLIGHTCAP_VERSION) return false;
  if (data[3] >= 0x03 && len < sizeof(FlightCapTelemetry)) return false;

  memset(out, 0, sizeof(*out));
  memcpy(out, data, data[3] >= 0x03 ? sizeof(FlightCapTelemetry) : 17);
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
  bool vbatt_ok = (t.flags & FLAG_VBATT_VALID) != 0;

  Serial.printf("seq=%u dist=%d mm interactions=%u vbatt=%u mV flags=0x%02X tof_err=%d\n",
                t.seq,
                dist_ok ? t.distance_mm : -1,
                t.interactions,
                vbatt_ok ? t.vbatt_mv : 0,
                t.flags,
                tof_err);
}
```

### Receiver workflow summary

1. **Pair / assign:** Scan for `company_id == 0x4E48`, `magic == 0xA5`, `version >= 0x02`, `flags & PAIR_MODE`. Show `device_addr` (formatted for display if needed). On user confirm, **save the 6 raw bytes**.
2. **Run / track:** On each advert, parse payload and **`memcmp(device_addr, saved, 6)`** — ignore packets from other caps.
3. **Do not** use BLE scanner MAC, device name (`FCap`), or `seq` as the device ID.

### ESP32 Arduino (Bluedroid) hint

```cpp
if (advertisedDevice.haveManufacturerData()) {
  String mfg = advertisedDevice.getManufacturerData();
  flightcap_parse((const uint8_t *)mfg.c_str(), mfg.length(), &t);
}
```

Use passive scan by default (`setActiveScan(false)`). Enable active scan only if you need the **`FCap`** name from scan response.

## What you will not see

- Connectable advertisements (except during Run/Pair adv windows — still non-connectable)
- GATT services or characteristics
- Packets during Run sleep or shelf mode

## Timing expectations for receiver logic

**Run mode:**

```
|-- ToF --|-- 10 s advertise @ ~100 ms --|-- ~10 s silent (device offset) --|
|  ~0.3s  |  ~100 adverts per window      |  0 adverts                      |
```

**Pair mode:** continuous 10 s advertise bursts with no silent gap (same ~100 ms interval).

- Debounce duplicate handling: same `seq` + same fields for up to ~100 ms is normal.
- After a **~10 s** gap (Run only), expect a new burst; `distance_mm` may change every cycle; `interactions` continues from the current 60 s accrual period (resets only on the 60 s boundary).
- Stagger scanner start times by **±1–2 s** when using a fixed 10 s on / 10 s off schedule to reduce alignment with cap sleep windows.

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
run cycle phase offset 12345 ms
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
