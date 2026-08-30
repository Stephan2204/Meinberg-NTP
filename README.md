# Meinberg UA537TGP Ethernet NTP Server

![Meinberg UA537TGP with added Ethernet
interface](docs/images/meinberg-ua537tgp-ethernet.jpg)

The photograph above shows the completed reference installation with the
WT32-ETH01 Ethernet connector added to the original 19-inch plug-in
front panel.

Turn a classic **Meinberg UA537TGP DCF77 radio clock** into a compact
Ethernet NTP server using a **WT32-ETH01 (ESP32 + LAN8720)**.

The important idea is that the Meinberg clock provides two different
pieces of information:

-   a serial telegram containing the absolute date/time and receiver
    status
-   `P_SEK`, a short pulse once per second that provides the precise
    second marker

The ESP32 combines the absolute time from the telegram with the precise
`P_SEK` edge and serves the resulting time over Ethernet using NTP.

## Architecture

``` text
                         DCF77 antenna
                              |
                              v
                    +-------------------+
                    | Meinberg UA537TGP |
                    |                   |
                    | serial time ------+---- TXD
                    | second pulse -----+---- P_SEK
                    +-------------------+
                         |          |
                    level adaptation
                         |          |
                         v          v
                    +-------------------+
                    |   WT32-ETH01      |
                    | ESP32 + LAN8720   |
                    |                   |
                    | GPIO35 <- time    |
                    | GPIO33 <- P_SEK   |
                    |                   |
                    | NTP server        |
                    | status web UI     |
                    | Web OTA           |
                    | DHCP / Syslog     |
                    +---------+---------+
                              |
                           Ethernet
                              |
                              v
                             LAN
```

## Why use the Meinberg clock?

The UA537TGP is much more useful than a simple serial clock source. It
separates **absolute time information** from the **precise second
event**.

The serial telegram tells the ESP32 which UTC second it is. `P_SEK`
tells it precisely when that second begins. This avoids using the
comparatively slow serial telegram itself as the timing reference.

The receiver also exposes status information. The firmware uses this to
distinguish a synchronized receiver from quartz free-run or a receiver
that has not synchronized since reset.

## Meinberg serial telegram

The tested configuration outputs one telegram per second at **9600 baud,
8N1**.

Example:

``` text
D:31.12.89;T:7;U:23.07.07;#*
```

The telegram contains date, weekday, UTC time and receiver/status
characters.

Important status characters used by the firmware include:

  Character   Meaning
  ----------- -------------------------------------------
  `#`         Receiver has not synchronized since reset
  `*`         Quartz free-run
  `S`         Summer-time status
  `!`         Time-change announcement

The exact available formats and configuration options depend on the
UA537TGP configuration.

## P_SEK

`P_SEK` is the precise one-second timing signal used by this project.

On the tested unit it is normally LOW and produces a short HIGH pulse
once per second. The rising edge is captured by an ESP32 interrupt and
is used as the precise second boundary.

The firmware checks the pulse interval over multiple seconds before
considering `P_SEK` stable.

Because reset logic in the Meinberg hardware may also affect this
signal, a single edge is not blindly accepted as a valid timing source.
Stability is established from repeated approximately one-second
intervals.

## Hardware

### ESP32 Ethernet board

The reference hardware is a **WT32-ETH01**, which combines:

-   ESP32
-   LAN8720 Ethernet PHY
-   RJ45 Ethernet interface

UART0 remains reserved for flashing and debugging.

The Meinberg signals are connected as follows:

  Signal               WT32-ETH01
  -------------------- ------------------
  Meinberg time data   GPIO35 / UART RX
  Meinberg `P_SEK`     GPIO33
  GND                  GND

### Important: voltage levels

Do **not** connect unknown Meinberg or RS-232 signal levels directly to
an ESP32 GPIO.

The ESP32 inputs are 3.3 V logic.

Two connection methods are practical.

### Option A: internal TTL signals

The tested installation uses the internal Meinberg TXD and `P_SEK` logic
signals.

Both signals are reduced to ESP32-compatible levels using a resistor
divider:

``` text
Meinberg signal ---- 1 kOhm ----+---- ESP32 input
                                |
                              2.2 kOhm
                                |
                               GND
```

This is appropriate only after verifying that the selected internal
signals are TTL-level signals and have the expected voltage.

### Option B: rear COM0/COM1 RS-232 port

The rear `COM0` or `COM1` connector can alternatively be used for the
serial telegram.

Because this is **RS-232**, a proper RS-232 transceiver such as a
**MAX3232** must be placed between the Meinberg COM port and the ESP32
UART.

``` text
Meinberg COM0 TX
       |
     RS-232
       |
   +---------+
   | MAX3232 |
   +---------+
       |
   3.3 V UART
       |
 ESP32 GPIO35
```

This is a convenient option because it avoids taking the serial data
signal directly from inside the Meinberg unit.

However, `P_SEK` is still a separate hardware signal and is not provided
by the normal RS-232 serial telegram. It therefore still needs its own
connection and appropriate level adaptation to 3.3 V.

## Meinberg DIL switch configuration

The Meinberg receiver must be configured so that its serial output
matches what this firmware expects. The following information is taken
from the documentation of the **specific receiver/version used for this
project**.

> **Important:** Meinberg produced different models and
> hardware/firmware revisions. The DIL switch assignments may be
> different on another receiver. **Do not copy these switch positions
> blindly. Check the manual for your exact Meinberg model and revision
> before changing any DIL switches.**

![DIL switch table from the documentation of the reference
unit](docs/images/meinberg-dil-switch-settings.png)

On the documented reference unit, each serial interface has its own
8-position DIL bank:

-   `DIL0 (1-3)` --- baud rate, 600 to 19200 baud
-   `DIL0 (4)` --- data format, 7E2 or 8N1
-   `DIL0 (5-6)` --- telegram output cycle: seconds, minutes, or on
    request
-   `DIL0 (7-8)` --- time reference: MEZ/MESZ, UTC, or MEZ
-   `DIL1` provides the corresponding settings for the second serial
    interface, with its own documented baud-rate range.

For this firmware, the serial interface that feeds the ESP32 must be
configured for:

  Parameter         Required setting
  ----------------- ---------------------
  Baud rate         **9600 baud**
  Data format       **8N1**
  Telegram output   **once per second**
  Time reference    **UTC**

The table printed in the reference manual also states that `0` means
**OFF** and `1` means **ON**. Because the physical switch
numbering/orientation can easily be read from the wrong side, it is
safer to configure the four functions above from the manual than to rely
on a copied eight-bit switch string.

If `COM0` is used through a MAX3232, configure the DIL bank belonging to
`COM0`. If an internal TTL telegram signal is used instead, verify which
serial channel that signal belongs to and configure the corresponding
DIL bank.

The `P_SEK` pulse is independent of the UART framing. It still needs to
be connected separately and level-adapted for the 3.3 V ESP32 input.

## NTP operation

The firmware does not treat receipt of a serial telegram alone as
sufficient for a high-quality time source.

It tracks:

-   validity of the Meinberg telegram
-   receiver status
-   `P_SEK` stability
-   time since the last valid timing event
-   holdover state
-   NTP synchronization state

The web interface displays the current Meinberg/DCF state, NTP state,
`P_SEK` state and Ethernet state.

When the receiver is not trustworthy, the NTP server deliberately
reports an unsynchronized state rather than pretending to have valid
reference time.

## Ethernet and DHCP

The WT32-ETH01 operates as a DHCP client.

In addition to receiving its own IPv4 configuration, this firmware
explicitly requests:

## IPv6 support
SLAAC is activated and a link-local und global address will be retrieved (if possible!)
ICMP, NTP and Webserver are available via IPv4 and IPv6.


**DHCP Option 7 --- Log Server**

This required rebuilding lwIP because the normal Arduino-ESP32 framework
ships with a precompiled lwIP library whose DHCP Parameter Request List
cannot simply be changed from the sketch.

For that reason this project uses PlatformIO with ESP-IDF and
Arduino-ESP32 as an ESP-IDF component.

A successful DHCP exchange can therefore contain:

``` text
Option 55: Parameter Request List
    ...
    7  Log Server
```

The address returned in DHCP Option 7 is shown on the web interface and
is used automatically as the Syslog destination.

No Syslog server IP address needs to be compiled into the firmware.

## Syslog

When DHCP Option 7 supplies a Log Server, Syslog becomes active
automatically.

The web interface shows:

``` text
Syslog
Status   ACTIVE (from DHCP)
Server   192.168.1.x
```

Messages are sent using UDP port 514 and facility `local0`.

Logging is event-based rather than periodic. Examples include:

-   startup and current state
-   `P_SEK` becoming stable or unstable
-   DCF receiver status changes
-   NTP synchronization/holdover changes
-   Ethernet address changes
-   OTA update events
-   manual reboot from the web interface

Example:

``` text
meinberg-ntp syslog configured via DHCP option 7; server=192.168.222.4; firmware=v7.6.2
meinberg-ntp startup state; ip=192.168.222.99; dcf_status=[#*  ]; psek=unstable; ntp=NO TIME; stratum=16
meinberg-ntp P_SEK stable=YES; interval_us=1000011
```

## Web interface

The built-in status page provides a compact overview of:

1.  Meinberg DCF receiver
2.  NTP status
3.  `P_SEK`
4.  Ethernet
5.  Syslog
6.  System

Good/healthy states are displayed in green and problematic states in
red.

The System section also provides:

-   firmware version
-   uptime
-   manual reboot
-   Web OTA firmware upload

## Building with PlatformIO

This project intentionally uses ESP-IDF as the PlatformIO framework
while pulling Arduino-ESP32 in as an ESP-IDF component.

Build with:

```bash
pio run
```

For a completely clean rebuild:

```bash
rm -rf .pio sdkconfig
pio run
```

The managed ESP-IDF/Arduino components may remain cached in
`managed_components/`.

Starting with **v7.6.2**, a normal `pio run` also creates two release
images in the `release/` directory:

```text
release/
├── meinberg-ntp-v7.6.2-ota.bin
├── meinberg-ntp-v7.6.2-factory.bin
└── manifest.json
```

`*-ota.bin` is the application image for an already running Meinberg
NTP server. `*-factory.bin` is a merged ESP32 image containing all flash
parts required for an initial installation.

The release files are intentionally ignored by Git. They are suitable
as binary assets for a GitHub Release.

## First installation

A new or erased WT32-ETH01 cannot be initialized with the OTA
`firmware.bin` alone. The ESP32 also needs its bootloader, partition
table and OTA metadata at their correct flash offsets.

There are three practical installation methods.

### Method 1: PlatformIO upload

Connect a 3.3 V USB-to-UART adapter to UART0 of the WT32-ETH01 and put
the ESP32 into its serial bootloader mode. Then run:

```bash
pio run -t upload
```

PlatformIO/esptool writes all required parts to their proper addresses.

### Method 2: merged factory image with esptool

After `pio run`, use the generated merged image:

```bash
python -m esptool --chip esp32 -p /dev/ttyUSB0 write_flash 0x0 \
  release/meinberg-ntp-v7.6.2-factory.bin
```

Replace `/dev/ttyUSB0` with the serial port used on your system.

The merged factory image is intended for a **first installation or full
recovery**. It may initialize/replace the bootloader, partition table
and OTA metadata, so do not use it as the normal Web OTA image.

### Method 3: browser installation with ESP Web Tools

ESP Web Tools can install ESP32 firmware through Web Serial. For ESP32
firmware built with modern ESP-IDF it requires a **merged firmware
image**, not the application-only OTA binary.

The generated `release/manifest.json` references the generated
`*-factory.bin` at flash offset `0x0`. Host the manifest and factory
binary together on an HTTPS website (for example GitHub Pages), then
point an ESP Web Tools install button at that manifest.

The browser used for installation must support Web Serial, and the page
must be served over HTTPS.

## Firmware updates

After the initial installation, use the built-in Web OTA page for
normal upgrades.

Upload only:

```text
meinberg-ntp-vX.Y.Z-ota.bin
```

Do **not** upload the merged `*-factory.bin` through the device's Web OTA
page.

From v7.5 onward, firmware update and reboot require the Web
administrator credentials configured under `/security`.

## Tested setup

Reference setup:

-   Meinberg UA537TGP
-   DCF77 antenna input
-   9600 baud, 8N1
-   serial telegram once per second
-   `P_SEK` once per second
-   WT32-ETH01
-   Meinberg time RX on GPIO35
-   `P_SEK` on GPIO33
-   Ethernet via LAN8720
-   DHCP
-   NTP
-   DHCP Option 7 based Syslog
-   Web status / Web OTA

## Safety and hardware disclaimer

The UA537TGP is vintage professional equipment and different revisions
or installations may expose different signal levels.

Verify voltages and signal polarity before connecting anything to an
ESP32.

In particular:

-   RS-232 must use an RS-232 transceiver such as MAX3232.
-   ESP32 GPIOs are not 5 V tolerant.
-   Internal signals should only be used after measuring and
    understanding their levels.
-   Work on mains-powered equipment only when you are qualified to do
    so.

## License

Licensed under the Apache License 2.0.

## Credit

The initial idea for the code was taken from this project:
https://github.com/G-3-3-R-T/gps-ntp-wt32-eth01


## Release packaging note (v7.6.2)

The release packaging script uses PlatformIO's own `FLASH_EXTRA_IMAGES` and
`ESP32_APP_OFFSET` variables to create the merged factory image. This is
intentional: depending on the PlatformIO/ESP-IDF integration,
`flasher_args.json` may not be produced during a normal `pio run`.

A successful build should end with lines similar to:

```text
[release] OTA: .../release/meinberg-ntp-v7.6.2-ota.bin
[release] creating merged factory image from PlatformIO FLASH_EXTRA_IMAGES
[release] Factory: .../release/meinberg-ntp-v7.6.2-factory.bin
[release] Manifest: .../release/manifest.json
```
