# v7.6.1 – NTP timing test + system/flash diagnostics

This is a focused test release based on v7.6.

## NTP timing
- Replaces Arduino `WiFiUDP::parsePacket()` polling for NTP with a raw lwIP
  dual-stack UDP callback bound to UDP/123.
- IPv4 and IPv6 use one dual-stack lwIP PCB.
- NTP receive timestamp T2 is sampled when lwIP delivers the packet to the
  UDP callback, instead of when the Arduino `loop()` eventually polls it.
- NTP transmit timestamp T3 is sampled immediately before the reply is handed
  to lwIP.
- Adds last/max NTP callback processing time in microseconds to the status page.
- Keeps separate IPv4 and IPv6 request counters.

The goal is to determine whether the previously observed IPv4 jitter and the
very stable ~30–36 ms IPv6 offset were caused by Arduino loop/polling latency.

## Firmware update page
Adds a read-only System / Memory block showing:
- firmware version
- flash size
- current sketch/app size
- free OTA/app space
- heap total/free
- minimum free heap
- largest free allocation block
- PSRAM total/free, if present
- CPU frequency / core count
- chip revision

IPv6 SLAAC, DHCPv4 Option 7, Syslog and existing web security remain unchanged.
