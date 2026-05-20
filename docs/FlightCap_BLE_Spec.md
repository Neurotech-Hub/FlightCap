# FlightCap BLE Spec (Phase 1)

This document is the device-side contract for the `flightcap-prod` firmware
running on the FlightCap board (nRF52840). It is written for an iOS developer
implementing a Core Bluetooth central. Phase 1 covers motion-event counting
and time-of-flight distance telemetry only — no writable control,
configuration, or OTA characteristics yet.

The firmware source lives in `applications/flightcap-prod/`; the contract
below is what the radio actually emits and accepts on hardware.

## At a glance

| Item | Value |
| --- | --- |
| Local Name | `FlightCap` |
| Role | Connectable Peripheral (`BT_LE_ADV_OPT_CONN`) |
| Bonding / pairing | None — connect unencrypted |
| Max concurrent centrals | 1 |
| Advertising interval (not connected) | 500–600 ms (`0x0320`–`0x03C0` in 0.625 ms units) |
| Advertising during forced sleep (magnet present) | Stopped |
| TX power | +8 dBm (firmware default; SoftDevice Controller) |
| Address | Random static — changes on firmware reflash, **not** between boots of the same build |
| Connection parameters | Whatever iOS proposes; firmware does not request alternates |
| Endianness | All multi-byte fields are little-endian |
| Notification cadence | 1 Hz, aligned to the device's internal 1 s window |

## Discovery payload

The 31-byte advertising payload contains:

- `Flags`: `0x06` (`BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR`).
- `Complete List of 128-bit Service UUIDs`: the single primary service UUID.

The scan response carries:

- `Complete Local Name`: `FlightCap`.

iOS will see both the name and the service UUID without performing an active
scan in most cases. You can also discover by UUID directly with
`CBCentralManager.scanForPeripherals(withServices: [serviceUUID])`.

## Service and characteristics

All UUIDs share the base `ad2a98a4-2148-4b58-9e14-7e2cbb6c7bXX`. They are
distinct from the test app `flightcap-ble-test` (which uses `…7a0X`).

| Role | UUID | Properties | Length |
| --- | --- | --- | --- |
| Primary service | `ad2a98a4-2148-4b58-9e14-7e2cbb6c7b01` | — | — |
| Motion characteristic | `ad2a98a4-2148-4b58-9e14-7e2cbb6c7b02` | Read, Notify | 4 bytes |
| Distance characteristic | `ad2a98a4-2148-4b58-9e14-7e2cbb6c7b03` | Read, Notify | 4 bytes |

Each characteristic has a standard Client Characteristic Configuration
Descriptor (CCCD, UUID `0x2902`). Writing `0x0001` (Notify) to the CCCD
subscribes that characteristic; writing `0x0000` unsubscribes. Subscription
is per-characteristic — you can subscribe to either one or both
independently.

### Motion characteristic (`…7b02`)

Reports the number of motion events recorded by the on-board LIS2DH12
accelerometer's INT1 line over the last 1 s window.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| 0 | `motion_events` | `uint32` LE | Count for the most-recently-closed window. Resets every window. |

- Reading the characteristic at any time returns the latest closed window's
  value, even without subscribing.
- Notifications fire **every** 1 s window when the CCCD is set to Notify,
  including when `motion_events == 0`. Use this as a 1 Hz heartbeat for the
  connection.

### Distance characteristic (`…7b03`)

Reports a trimmed-mean snapshot of the VL53L4CD time-of-flight sensor's
rolling buffer (16 entries deep, ~20 Hz raw rate, 50 ms timing budget).

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| 0..1 | `dist_mm` | `uint16` LE | Filtered distance in millimeters. **`0` when invalid.** |
| 2..3 | `sample_count` | `uint16` LE | Number of raw samples in the rolling buffer at snapshot time (`0..16`). |

**Validity rule:** treat the reading as invalid when `sample_count < 4`. In
that case the firmware emits `dist_mm = 0` as a sentinel. This happens:

- transiently, right after boot (filter not yet primed) and immediately
  after wake-from-magnet-sleep (filter is cleared on wake);
- persistently (`sample_count` stays at `0` indefinitely) when the
  VL53L4CD sensor failed to bring up at boot. In that case the device
  still advertises, connects, and notifies normally — only the distance
  reading is permanently invalid. There is no separate "sensor missing"
  flag on the wire; iOS apps should rely on the same `sample_count < 4`
  invalidity check.

Notifications fire **every** 1 s window when the CCCD is set to Notify,
regardless of validity — you'll see the buffer fill up over the first
several seconds after connect.

### Notify ordering

Within a single 1 s window, the firmware emits the motion notification
first, then the distance notification, back-to-back. Both arrive on the
same iOS `peripheral(_:didUpdateValueFor:error:)` queue and are observably
in that order.

## Connection lifecycle

```text
   not connected
        |
        v
   advertising at 500-600 ms
        |
        | (iOS connects)
        v
   connected (1)
        |
        | (CCCD writes enable notifications)
        v
   notifications at 1 Hz on motion + dist
        |
        | (disconnect from either side)
        v
   advertising at 500-600 ms (auto-restarted)
```

`(1)` Only one central can be connected at a time. If a second central
attempts to connect while one is active, the connection request is rejected
by the controller.

### Forced sleep (magnet present)

When the on-board magnet sensor is asserted (i.e. a magnet is placed on the
device — shelf / storage mode), the firmware:

1. Disconnects any active central immediately (HCI reason
   `0x13` `Remote User Terminated Connection`).
2. Stops advertising entirely. The device is not discoverable in this state.
3. Polls the magnet input every 1 s. When the magnet is removed it restarts
   advertising at the same 500–600 ms interval. **Wake latency: up to ~1 s.**

There is no BLE-based wake path — you cannot reach the device until the
magnet is removed.

## Worked examples

Three example notification payloads, exactly as they appear on the wire:

| Characteristic | Hex bytes (LE) | Decode |
| --- | --- | --- |
| Motion | `05 00 00 00` | `motion_events = 5` |
| Motion | `00 00 00 00` | `motion_events = 0` (idle window) |
| Distance | `96 00 0B 00` | `dist_mm = 150`, `sample_count = 11` → valid, 15 cm |
| Distance | `00 00 02 00` | `dist_mm = 0`, `sample_count = 2` → **invalid**, filter not primed |
| Distance | `E8 03 10 00` | `dist_mm = 1000`, `sample_count = 16` → valid, 1.0 m |

## Swift / Core Bluetooth sketch

This is illustrative, not a drop-in implementation. Replace `serviceUUID`,
`motionUUID`, and `distanceUUID` with your `CBUUID` declarations.

```swift
import CoreBluetooth

let serviceUUID  = CBUUID(string: "AD2A98A4-2148-4B58-9E14-7E2CBB6C7B01")
let motionUUID   = CBUUID(string: "AD2A98A4-2148-4B58-9E14-7E2CBB6C7B02")
let distanceUUID = CBUUID(string: "AD2A98A4-2148-4B58-9E14-7E2CBB6C7B03")

func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                    advertisementData: [String : Any], rssi RSSI: NSNumber) {
    // Match by name "FlightCap" or by service UUID (preferred).
    central.connect(peripheral, options: nil)
}

func peripheral(_ peripheral: CBPeripheral,
                didDiscoverCharacteristicsFor service: CBService, error: Error?) {
    for chr in service.characteristics ?? [] {
        if chr.uuid == motionUUID || chr.uuid == distanceUUID {
            peripheral.setNotifyValue(true, for: chr)  // writes CCCD = 0x0001
        }
    }
}

func peripheral(_ peripheral: CBPeripheral,
                didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
    guard let data = characteristic.value else { return }

    switch characteristic.uuid {
    case motionUUID:
        guard data.count >= 4 else { return }
        let motion = data.withUnsafeBytes { $0.load(as: UInt32.self) }.littleEndian
        // motion is the count over the most-recent 1 s window
        handleMotion(motion)

    case distanceUUID:
        guard data.count >= 4 else { return }
        let mm    = UInt16(data[0]) | (UInt16(data[1]) << 8)
        let count = UInt16(data[2]) | (UInt16(data[3]) << 8)
        if count >= 4 {
            handleDistance(mm: mm, sampleCount: count)
        } else {
            handleDistanceInvalid(sampleCount: count)
        }

    default: break
    }
}
```

## Reconnect / state recovery guidance

- Subscriptions are **not persisted across disconnects**. On every new
  connection, re-issue `setNotifyValue(true, for:)` on each characteristic
  you care about. (The firmware clears its internal CCC enable flags on
  disconnect.)
- A direct read of either characteristic right after connect returns the
  most-recent closed window's values, even before you re-subscribe. Useful
  for getting an immediate "current state" on app launch.
- No bonding is performed; there is no key material to clear if the user
  reinstalls the iOS app.

## What is intentionally **not** in Phase 1

- No write characteristics (LED control, sleep, calibration, etc.).
- No Device Information Service (firmware version, manufacturer, etc.).
- No Battery Service.
- No fixed / printable Bluetooth address — the random static address changes
  after a firmware reflash.
- No directed advertising or allow-listing; the device is publicly
  discoverable to anything in range.
- No connection-parameter negotiation; iOS-default conn interval is used.
- No MTU exchange required for telemetry — characteristic payloads are 4 bytes
  each so the default 23-byte ATT MTU is plenty. (The firmware advertises a
  larger MTU of 247 for DFU; iOS may opt to use it, but it has no effect on
  the FCP characteristics.)

Any of those can be added later without breaking the Motion / Distance
characteristic contract above.

## Firmware update (DFU) — what the iOS app should know

The device also hosts a **second** GATT service for over-the-air firmware
updates, using the standard MCUmgr SMP protocol. **iOS apps should ignore
it** — it is the firmware-update transport used by Nordic's *nRF Connect
Device Manager* mobile app (iOS / Android) and the `mcumgr` CLI, not the
production app.

| Item | Value |
| --- | --- |
| Transport | MCUmgr SMP over BLE |
| Service UUID | `8D53DC1D-1DB7-4CD3-868B-8A527460AA84` (standard, defined by Zephyr) |
| Advertised in | Scan response, alongside the device name |
| Bootloader | MCUboot (two-slot swap, unsigned in Phase 1) |
| Availability | Whenever the device is awake and advertising — i.e. **not** during forced-sleep (magnet present) |
| Concurrency | Only one central at a time (`CONFIG_BT_MAX_CONN=1`). iOS must disconnect before nRF Connect can connect to perform DFU, and vice versa. |

### Why iOS should ignore the SMP UUID

- nRF Connect Device Manager looks for this UUID; iOS production code does
  not.
- The SMP service intentionally has no notify cadence and no human-readable
  data — it is a binary management transport.
- Connecting from iOS will work (the GATT server is the same), but the
  characteristics inside SMP are not part of the production data contract.
- If the iOS app uses `CBCentralManager.scanForPeripherals(withServices:)`
  filtered on the FCP UUID, the SMP UUID is irrelevant. If the app scans
  with `nil` (all peripherals), it will also see the SMP UUID in the scan
  response — just filter it out by name or by FCP UUID match.

### DFU workflow (out-of-band, not for the iOS app)

The dev team distributes the firmware binary (`zephyr.signed.bin`). End-user
or QA workflow with **nRF Connect Device Manager** (free, App Store):

1. Open the app and scan for `FlightCap`.
2. Connect.
3. **Image** tab → **Upload** the `zephyr.signed.bin` to slot 1.
4. Mark as **Test** → device reboots and runs the new image.
5. Verify the new build behaves correctly (iOS app reconnects and sees
   telemetry).
6. **Confirm** → the new image becomes permanent. If you skip this step,
   MCUboot reverts to the previous image on the next reboot.

### Forward compatibility

The FCP UUIDs and characteristic byte layouts in this spec are stable
across DFU updates **unless** this document is revised and the version
re-issued. A firmware change that alters those would be announced
explicitly.
