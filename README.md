# esphome-fpc1020a

<img width="918" height="891" alt="image" src="https://github.com/user-attachments/assets/67813ebc-d734-4725-ac72-615237222151" />

<img width="596" height="875" alt="image" src="https://github.com/user-attachments/assets/d5d2e48a-cfa0-4bed-ae33-bb7800444f32" />

ESPHome external component for the **M5Stack Finger unit (U102)** — an FPC1020A
capacitive fingerprint reader with an onboard STM32 that speaks the
Waveshare-style 8-byte UART protocol. Likely also works with the Waveshare UART
Fingerprint Reader family (same protocol), though only the M5Stack unit has
been tested.

Built for the classic "fingerprint on the door" setup: the reader scans
continuously, and a recognised finger fires a Home Assistant event carrying the
person's name so an automation can unlock the door.

## Features

- **Continuous identify** — no wake pin needed; press a finger any time
- **Enroll from the GUI** — register fingerprints from Home Assistant or the
  device's built-in web server, no reflash, no config edits
- **10 nameable slots** — names are editable text entities, survive reboots,
  and are included in events and the "Last Fingerprint" sensor
- **HA events** on match / reject / enroll (see below)
- **Guided enrollment** via a status text sensor ("Place finger (1/3)"…)
- **HA actions** (`esphome.<node>_enroll`, `_delete_slot`, `_delete_all`) for
  automation-driven management
- Fingerprints are stored **inside the reader module**, so they survive
  reflashes and power loss

## Wiring (M5Stack Atom Lite, Grove port)

| Atom Lite   | Finger unit |
| ----------- | ----------- |
| GPIO26 (TX) | RX          |
| GPIO32 (RX) | TX          |
| 5V          | 5V          |
| GND         | GND         |

UART runs at 19200 baud, 8N1. Any ESP32 board works — adjust the pins.

## Quick start

```yaml
external_components:
  - source: github://JakeTheRabbit/esphome-fpc1020a@main
    components: [fpc1020a]

api:
  # Both flags are required by this component:
  homeassistant_services: true  # lets the driver fire esphome.fingerprint_* events
  custom_services: true         # exposes the enroll/delete actions in HA

uart:
  id: fp_uart
  tx_pin: GPIO26
  rx_pin: GPIO32
  baud_rate: 19200

fpc1020a:
  id: fpc
  uart_id: fp_uart
```

That gives you the driver and the HA actions. For the full experience —
slot-name text entities, enroll/delete buttons, "Last Fingerprint" /
"Enrollment Status" sensors, and a status LED — copy
[`examples/door-fingerprint.yaml`](examples/door-fingerprint.yaml), which is a
complete, flash-ready config for an Atom Lite.

> **Upgrading an existing build?** If you add `custom_services: true` to a
> device that has compiled before and hit
> `undefined reference to ... to_service_arg_type`, run
> `esphome clean <your>.yaml` once — the IDF build caches the API source list.

## Events

| Event                            | Data                          | When                        |
| -------------------------------- | ----------------------------- | --------------------------- |
| `esphome.fingerprint_matched`    | `node`, `user_id`, `user_name` | Recognised finger            |
| `esphome.fingerprint_unmatched`  | `node`                        | Unknown finger rejected     |
| `esphome.fingerprint_enrolled`   | `node`, `user_id`, `user_name` | Enrollment completed        |

Door unlock automation:

```yaml
automation:
  - alias: Unlock on fingerprint
    trigger:
      - platform: event
        event_type: esphome.fingerprint_matched
        event_data:
          node: door-fingerprint   # your device name
    action:
      - service: lock.unlock
        target:
          entity_id: lock.front_door
```

`user_name` / `user_id` are in `trigger.event.data` if you want per-person
conditions or notifications ("Front door unlocked by Ben").

## Enrolling a fingerprint

1. Type the person's name into **Slot N Name**.
2. Set **Slot** to that number.
3. Press **Enroll Fingerprint** and follow **Enrollment Status** — the module
   captures the finger three times (~8 s per placement).

Works identically from the Home Assistant device page and the device's own web
UI (`http://<device-ip>/`) — handy before the device is adopted into HA.

## C++ API

If you're wiring your own YAML instead of using the example, the component
exposes:

```cpp
id(fpc)->start_enroll(slot);        // 1..10
id(fpc)->delete_slot(slot);
id(fpc)->delete_all();
id(fpc)->query_count();
id(fpc)->set_slot_name(slot, name); // hook to text entities
id(fpc)->ui_state();                // 0 idle, 1 enrolling, 2 recent accept, 3 recent reject
```

Display entities are injected from an `on_boot` lambda (see the example):
`set_match_text`, `set_status_text`, `set_count_sensor`, `set_matched_bin`.

## Protocol notes

Frames are 8 bytes: `0xF5 CMD P1 P2 P3 0x00 CHK 0xF5`, `CHK = XOR(CMD..byte 5)`.
Commands used: `0x01/0x02/0x03` (three-step add), `0x04` (delete user),
`0x05` (delete all), `0x09` (user count), `0x0C` (1:N identify),
`0x2E` (capture timeout). The reader only scans while an identify window is
open, so the component keeps one open whenever it isn't doing something else.

## License

[MIT](LICENSE)
