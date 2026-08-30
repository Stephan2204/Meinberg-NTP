# v7.6 – IPv6 / SLAAC

- Enables IPv6 on the WT32-ETH01 Ethernet interface.
- Creates a link-local IPv6 address.
- Uses SLAAC / Router Advertisements for global or ULA addressing.
- Adds a dedicated IPv6 NTP listener on UDP/123 alongside IPv4.
- Shows link-local and SLAAC/global IPv6 addresses on the status page.
- Shows separate IPv4 and IPv6 NTP request counters.
- Adds IPv6 addresses and counters to `/status.json`.
- Existing DHCPv4, DHCP Option 7 and IPv4 Syslog remain unchanged.
- DHCPv6 is deliberately not enabled in this first IPv6 release.
