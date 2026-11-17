# NEXUS Home Lab — Python OOP dashboard

A local dashboard for up to three ESP32 / ESP32-S3 boards. Monitor SmartGuard security and a DHT11 + IR + motor automation node, select one device, then deploy its application binary over Wi-Fi.

## Start on Windows

1. Extract the whole `Nexus_OTA_Hub` folder. Keep `server.py` and `static` together.
2. Install Python 3.10 or later if missing. Run `START_NEXUS.bat`.
3. Chrome/browser opens at **http://localhost:8765**. If it does not open, enter that URL manually.
4. For USB port listing and identification, run `INSTALL_USB_SUPPORT.bat` once while internet is available. Otherwise use IP registration; the dashboard has no mandatory third-party dependencies.
5. Keep the terminal window open. Ctrl+C stops the server. Device registrations are saved to `devices.json` next to `server.py`.

The Python server runs on YOUR laptop. It does not run on the ESP32 and is not a cloud website. It binds only to the laptop loopback address. The included UI has no CDN dependencies.

## Network and three USB cables

Connect the laptop and all boards to ONE 2.4 GHz router/hotspot, with client isolation disabled. Each board receives a different IP. The three USB cables provide power and serial identification. OTA bytes travel over Wi-Fi, not USB.

Your previous firmware creates `SmartGuard-Node` at 192.168.4.1. Three independent access points all using that address cannot be selected reliably through one Wi-Fi connection. The included LAN firmware fixes that by joining the shared network. It falls back to a unique `Nexus-<id>` recovery AP if connection fails.

A single board's recovery AP remains usable for its browser OTA page, but a shared LAN is needed for simultaneous access to all three boards. A router is preferable if a phone hotspot blocks communication between clients. A Windows hotspot may also work if it allows the laptop host to reach connected devices.

## Your ESP32-S3: preserve the working three-version logic

Open `firmware/SmartGuard_Nexus/SmartGuard_Nexus.ino`.

- Set `LAN_SSID` and `LAN_PASS` to your shared Wi-Fi credentials.
- Set `NODE_NAME` to a simple label, e.g. `Security node` (avoid quotes/backslashes in the name).
- Set `FW_VERSION` to your desired 1, 2 or 3.
- Preserve your working `PASSIVE_BUZZER` and `BUZZER_ON` settings, board settings and partition scheme. The supplied copy defaults to an active, active-HIGH buzzer, as the original did.
- Existing pins: ultrasonic 1/2, vibration 11, buzzer 10, LEDs 16/15/7/6/5/4, pot 8.
- Sensor/control logic is retained from the last working sketch. The additions are Wi-Fi station mode, stable device identity, build fingerprint, richer `/info`, and serial identification. No LCD code.

### Bootstrap without another USB flash

1. Compile/export the Nexus S3 application for **ESP32S3 Dev Module**, using your existing OTA-capable partition layout.
2. Join the OLD `SmartGuard-Node` network and open http://192.168.4.1.
3. Upload **SmartGuard_Nexus.ino.bin** using that old OTA page.
4. The board now joins the configured shared Wi-Fi. Reconnect the laptop to that same Wi-Fi.
5. Read the board's IP from Serial Monitor (115200), or use the dashboard USB Identify action.
6. On the dashboard choose **Add a board**, enter its IP and a name, then **Connect board**.

You may instead install Nexus firmware once by Arduino USB upload. Do not change the partition table via normal application OTA.

### Later updates

Change functionality/version, export the application binary, select the target card, choose the binary, and press **Deploy update**. Keep the network credentials and OTA/identity functions in every later firmware release.

The dashboard validates image type, checks the board's unique ID and chip ID, transfers only to the chosen IP, and checks its application ELF fingerprint after reboot. If verification cannot complete, it reports **unverified** instead of pretending success. Changing only the version number does not add functionality by itself; in SmartGuard, the version selects existing compile-time branches.

## Friend's ESP32: DHT11 + IR + DC motor

`firmware/Climate_Nexus/Climate_Nexus.ino` is a ready-to-configure EXAMPLE, not a merge of your friend's unknown working sketch. Check/adapt these assumptions BEFORE uploading. It supports a standard ESP32 Dev Module and an L298N-controlled DC motor. Do not use this example unchanged for a different motor driver, motor-control method, or mains fan.

| Signal | Example ESP32 GPIO / supply |
|---|---|
| DHT11 DATA | GPIO27; bare sensor needs a pull-up to 3.3 V |
| IR module DO | GPIO26; ensure output is 3.3 V compatible |
| L298N IN1 | GPIO25 |
| L298N IN2 | GPIO33 |
| L298N ENA | GPIO32; remove the ENA jumper |
| Motor | Driver OUT1/OUT2 |
| Motor supply | Separate supply appropriate for the motor/driver |
| Grounds | ESP32, sensors, and motor driver share GND |

Install **DHT sensor library by Adafruit** and its **Adafruit Unified Sensor** dependency in Arduino IDE.

Default automation: temperature demand turns ON at 30 °C and OFF at 28 °C. IR is active LOW; a detection permits motor operation for 15 seconds after the most recent detection. The motor runs only when both temperature demand and this presence condition are true. Set `REQUIRE_IR=false` for temperature-only control with IR telemetry. Set `IR_ACTIVE` to match your module. DHT reads every 2 seconds; invalid readings turn the motor off. Motor stops during OTA and startup.

Set shared network credentials. Compile this sketch for **ESP32 Dev Module**, not ESP32-S3. Initially flash via USB unless its current firmware already supports compatible HTTP OTA. Add its IP to the dashboard. The third slot can use another compatible security or climate node with a different name; its unique hardware ID prevents confusion even when versions match.

If your friend wants the exact existing behaviour preserved, merge the network/OTA functions into that existing sketch rather than replacing it with this example. Required interface: `GET /info` with device_id, chip_id, kind, version, elf_sha256 and telemetry; `POST /update` accepts multipart firmware and returns OK only after successful finalization. The supplied sketches show both endpoints.

## USB identification

1. Close Arduino Serial Monitor for that port.
2. Add a board → Find a USB-connected board → Refresh ports.
3. Choose COM5, COM6, etc. → Identify. The board must already run the Nexus firmware.
4. Its name/IP are filled in. Click Connect board to verify its Wi-Fi identity.

This does NOT flash through USB. Opening a serial port can restart some boards. Port listing alone does not open/reset them. Serial identification is requested explicitly. If the board has two USB interfaces, select the interface used by its Arduino `Serial` configuration. If no reply arrives, use the IP shown by its Serial Monitor instead. Do not open a serial port while that board is updating.

## Files and OOP

- `server.py`: Python OOP backend, persistence, API, firmware checks and OTA orchestration.
- `static/`: local HTML/CSS/JavaScript interface. Chrome renders it; Python performs device access and update coordination.
- `OOP_VIVA.md`: class map, explanations and viva points.
- `firmware/`: two independent single-file sketches. Only upload the one for the correct board and circuit.
- `tests/test_hub.py`: meaningful backend tests using simulated boards; no real device is flashed by the tests.

## Troubleshooting

- **Legacy firmware:** bootstrap the Nexus sketch using your old OTA page first.
- **Wrong chip:** ESP32 and ESP32-S3 .bin files are not interchangeable.
- **Choose application binary:** do not select bootloader, partitions, or merged binaries.
- **Device identity changed:** DHCP may have reassigned an IP; identify the right board and register it again.
- **Offline:** confirm same network, current IP and no AP/client isolation.
- **COM busy:** close Serial Monitor or another serial program.
- **Update failed / not enough space:** verify an OTA-capable partition scheme and binary fits its application slot.
- **Port 8765 in use:** close the earlier hub terminal before starting another.
- **No buzzer after updating:** copy your confirmed buzzer type/polarity settings from the working sketch.
- **Browser progress:** stages show transfer/reboot/verification, not a fabricated byte percentage.

Use this demo on a trusted lab network. The boards' HTTP OTA endpoints retain the original unauthenticated local-network design. This is a lab demonstration, not production-secured OTA.

## Validation

Python syntax, JavaScript syntax and 9 backend tests were checked. Tests cover image validation, chip/identity mismatch, polymorphic device behaviour, registration persistence/limit, successful fingerprint verification, old-build rejection, and HTTP/token handling. Firmware was source-reviewed but not compiled against the ESP32 toolchain or flashed to physical hardware in this environment. Real USB/network/peripheral testing must be completed on your boards.
