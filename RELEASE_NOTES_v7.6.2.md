# v7.6.2 – IPv6 dual stack, improved NTP timing and diagnostics

## IPv6
- IPv4 + IPv6 dual stack.
- IPv6 SLAAC enabled.
- Link-local and global/SLAAC IPv6 addresses shown on the status page.
- NTP serves both IPv4 and IPv6.
- Separate IPv4 and IPv6 NTP request counters.
- DHCPv6 remains intentionally disabled; IPv6 addressing uses SLAAC.

## NTP timing
- NTP packet handling uses a raw lwIP dual-stack UDP callback on UDP/123.
- T2 is sampled when lwIP delivers the request to the NTP callback.
- T3 is sampled immediately before the reply is handed back to lwIP.
- This removes Arduino loop()/WiFiUDP::parsePacket() polling latency from the
  NTP timestamp path.
- Status page includes last/max NTP callback processing time.

## System / Memory diagnostics
The authenticated Firmware Update page shows:
- firmware version
- flash size
- current sketch/app size
- free OTA/app space
- heap total/free
- minimum free heap
- largest free block
- PSRAM total/free if present
- CPU clock / core count
- chip revision

## Web status page
- Automatic refresh reduced from 2 seconds to 30 seconds to avoid unnecessary
  HTTP traffic and processing load.

## Existing features retained
- DHCPv4 Option 7 log-server discovery
- Syslog
- web OTA
- HTTP Basic Auth for administrative pages
- DCF77/P_SEK status and holdover handling
