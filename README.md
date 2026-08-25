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
meinberg-ntp syslog configured via DHCP option 7; server=192.168.1.8; firmware=v7.4
meinberg-ntp startup state; ip=192.168.1.40; dcf_status=[#*  ]; psek=unstable; ntp=NO TIME; stratum=16
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

``` bash
pio run
```

For a completely clean rebuild:

``` bash
rm -rf .pio sdkconfig
pio run
```

The managed ESP-IDF/Arduino components may remain cached in
`managed_components/`.

The OTA firmware image is generated as:

``` text
.pio/build/wt32-eth01/firmware.bin
```

## Firmware updates

After the initial wired/serial flash, later firmware versions can be
installed using the built-in Web OTA page.

This is especially useful when the WT32-ETH01 is permanently installed
inside the Meinberg chassis.

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

## Documentation images

The hardware photograph and DIL-switch table image in `docs/images/`
document the actual reference unit used to develop and test this
project. The DIL table is included as a practical reference only; the
original equipment documentation for the exact receiver revision remains
authoritative.


## License

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

## Credit
The intitial idea for the code was taken from this project:
https://github.com/G-3-3-R-T/gps-ntp-wt32-eth01

