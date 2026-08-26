/*
  Meinberg UA537TGP -> NTP server for WT32-ETH01
  ------------------------------------------------
  Hardware:
    WT32-ETH01 (ESP32 + LAN8720)
    Meinberg UA537TGP configured for:
      9600 baud, 8N1, output every second, UTC

  Wiring:
    Meinberg TXD (TTL, level shifted to 3.3 V) -> GPIO35 (UART2 RX)
    Meinberg P_SEK (TTL, level shifted to 3.3 V) -> GPIO33
    Meinberg GND -> WT32-ETH01 GND

    UART0 remains free for flashing/debugging:
      GPIO1 = TX0
      GPIO3 = RX0

  Notes:
    - P_SEK is idle LOW and pulses HIGH for ~100 us once per second.
    - The rising edge is used as the precise second marker.
    - The serial telegram supplies the absolute UTC date/time and status.
    - If '#' is present, the Meinberg has not synchronized since reset.
    - '*' indicates quartz free-run / holdover.
    - While unsynchronized, this server answers as NTP stratum 16 (LI=3),
      so clients should NOT synchronize to it. This is intentional and
      useful for bench testing without a DCF77 antenna.

  Based conceptually on:
    https://github.com/G-3-3-R-T/gps-ntp-wt32-eth01
  but rewritten for the Meinberg telegram and a 64-bit ESP timer.
*/

#include <Arduino.h>
#include <ETH.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "lwip/dhcp.h"
#include "lwip/pbuf.h"
#include "esp_timer.h"
#include "esp_arduino_version.h"

// ------------------------- Configuration -------------------------

static constexpr int MEINBERG_RX_PIN = 35;  // input-only, excellent UART RX pin
static constexpr int PSEK_PIN        = 33;  // safe GPIO, interrupt capable
static constexpr uint32_t MEINBERG_BAUD = 9600;

static constexpr uint16_t NTP_PORT = 123;
static constexpr size_t NTP_PACKET_SIZE = 48;

// Accept P_SEK as plausible when consecutive pulses are within this range.
static constexpr int64_t PSEK_MIN_INTERVAL_US = 950000;
static constexpr int64_t PSEK_MAX_INTERVAL_US = 1050000;

// Telegram should follow the P_SEK marker quickly.
// 250 ms is deliberately generous for the first version.
static constexpr int64_t MAX_TELEGRAM_AFTER_PSEK_US = 250000;

// Require this many plausible one-second intervals before calling P_SEK stable.
static constexpr uint8_t REQUIRED_STABLE_PULSES = 3;

// If the receiver was synchronized before but enters '*' free-run,
// keep serving synchronized time for this long.
// Datasheet quartz free-run accuracy: 1e-5 (10 ppm).
// During holdover we increase NTP root dispersion by this worst-case rate.
// The 24 h trust limit below is OUR conservative policy, not a Meinberg spec.
static constexpr double HOLDOVER_DRIFT_RATE = 1e-5;  // seconds error per second
static constexpr uint32_t MAX_HOLDOVER_SECONDS = 86400; // 24 h policy limit
static constexpr double LOCKED_BASE_DISPERSION_S = 0.005; // 5 ms

// Debug output over UART0.
static constexpr bool DEBUG_OUTPUT = true;
static const char *FIRMWARE_VERSION = "v7.5.2";

// Status helpers are implemented below and also used by Syslog.
static String rawStatusString();
static String meinbergStateText();
static String currentNtpStateText();
static String htmlEscape(const String &s);

// ------------------------- Ethernet / UDP -------------------------

WiFiUDP udp;
WiFiUDP syslogUdp;
WebServer web(80);

// ------------------------- Web security -------------------------
// Credentials are stored in ESP32 NVS and survive reboot/OTA updates.
// HTTP Basic Authentication protects administrative actions.
// Note: Basic Auth over plain HTTP does not encrypt credentials on the wire.
Preferences securityPrefs;
static String webAdminUser;
static String webAdminPassword;
static bool webAdminConfigured = false;
static bool otaUploadAuthorized = false;

static void loadWebSecurity() {
  if (!securityPrefs.begin("webadmin", true)) {
    webAdminConfigured = false;
    return;
  }
  webAdminUser = securityPrefs.getString("user", "");
  webAdminPassword = securityPrefs.getString("pass", "");
  securityPrefs.end();
  webAdminConfigured = !webAdminUser.isEmpty() && !webAdminPassword.isEmpty();
}

static bool requireWebAdmin() {
  if (!webAdminConfigured) {
    web.send(403, "text/plain; charset=utf-8",
             "Web administrator credentials are not configured yet.");
    return false;
  }
  if (web.authenticate(webAdminUser.c_str(), webAdminPassword.c_str())) {
    return true;
  }
  web.requestAuthentication(BASIC_AUTH, "Meinberg NTP Admin");
  return false;
}


// ------------------------- Meinberg UART parser -------------------------

HardwareSerial MeinbergSerial(2);

static char telegram[31];       // 30 chars + terminating NUL
static uint8_t telegramPos = 0;
static bool inTelegram = false;

struct MeinbergTime {
  int year = 0;
  int month = 0;
  int day = 0;
  int weekday = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  bool neverSynced = true;  // '#'
  bool freeRun = true;      // '*'
  bool summerTime = false;  // 'S'
  bool announce = false;    // '!'
  bool valid = false;
};

MeinbergTime mb;

// ------------------------- P_SEK interrupt state -------------------------

portMUX_TYPE psekMux = portMUX_INITIALIZER_UNLOCKED;

volatile int64_t isrLastPsekUs = 0;
volatile uint32_t isrPsekCounter = 0;

void IRAM_ATTR psekISR() {
  const int64_t now = esp_timer_get_time();

  portENTER_CRITICAL_ISR(&psekMux);
  isrLastPsekUs = now;
  const uint32_t nextCounter = isrPsekCounter + 1U;
  isrPsekCounter = nextCounter;
  portEXIT_CRITICAL_ISR(&psekMux);
}

// ------------------------- Time base -------------------------

struct TimeBase {
  bool haveTime = false;

  // NTP whole seconds corresponding exactly to the accepted P_SEK rising edge.
  uint32_t ntpSecondsAtPsek = 0;
  int64_t psekUs = 0;

  // Most recent accepted reference timestamp (for NTP reference timestamp field).
  uint32_t refNtpSeconds = 0;

  // State
  bool psekStable = false;
  bool synchronized = false;
  bool holdover = false;
};

TimeBase timeBase;

static int64_t lastProcessedPsekUs = 0;
static uint32_t lastProcessedPsekCounter = 0;
static uint8_t stablePulseIntervals = 0;
static int64_t lastLockedUs = 0;

// Diagnostics shown on the status page.
static int64_t lastPsekIntervalUs = 0;
static int64_t lastTelegramDelayUs = 0;
static uint32_t telegramCount = 0;
static uint32_t ntpRequestCount = 0;
static uint32_t lastNtpRequestMs = 0;
static IPAddress lastNtpClientIP;
static bool previousEthLink = false;
static IPAddress previousEthIP(0, 0, 0, 0);

static portMUX_TYPE syslogMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t dhcpLogServerBytes[4] = {0,0,0,0};
static volatile bool dhcpLogServerPending = false;
static IPAddress syslogServer(0,0,0,0);
static bool syslogConfigured = false;
static uint32_t syslogMessageCount = 0;
static uint32_t lastSyslogMs = 0;

static bool statusMonitorInitialized = false;
static bool prevNeverSynced = true;
static bool prevFreeRun = true;
static bool prevSummerTime = false;
static bool prevAnnounce = false;
static bool prevPsekStable = false;
static bool prevNtpSync = false;
static bool prevHoldover = false;
static int prevStratum = 16;

// ------------------------- Date conversion -------------------------

// Howard Hinnant's civil-date algorithm, adapted for embedded use.
// Returns days relative to 1970-01-01.
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

static bool validDateTime(const MeinbergTime &t) {
  if (t.year < 1970 || t.year > 2099) return false;
  if (t.month < 1 || t.month > 12) return false;
  if (t.day < 1 || t.day > 31) return false;
  if (t.hour < 0 || t.hour > 23) return false;
  if (t.minute < 0 || t.minute > 59) return false;
  if (t.second < 0 || t.second > 60) return false; // allow leap-second representation
  return true;
}

static uint32_t toNtpSeconds(const MeinbergTime &t) {
  const int64_t unixSeconds =
      daysFromCivil(t.year, t.month, t.day) * 86400LL +
      t.hour * 3600LL +
      t.minute * 60LL +
      t.second;

  // NTP epoch 1900-01-01 vs Unix epoch 1970-01-01.
  return static_cast<uint32_t>(unixSeconds + 2208988800ULL);
}

// ------------------------- Meinberg telegram decoding -------------------------

static bool parseMeinbergTelegram(const char *s, MeinbergTime &out) {
  // Expected payload length is 30 bytes, not counting STX/ETX:
  // D:31.12.89;T:7;U:23.07.06;#*__
  if (strlen(s) != 30) return false;

  int dd, mo, yy, wd, hh, mm, ss;
  char status[5] = {0};

  const int fields = sscanf(
      s,
      "D:%2d.%2d.%2d;T:%1d;U:%2d.%2d.%2d;%4c",
      &dd, &mo, &yy, &wd, &hh, &mm, &ss, status);

  if (fields != 8) return false;

  MeinbergTime t;
  t.day = dd;
  t.month = mo;

  // Preserve old dates such as 1989 when bench-testing without antenna,
  // while mapping current DCF years to the 2000s.
  t.year = (yy >= 80) ? (1900 + yy) : (2000 + yy);

  t.weekday = wd;
  t.hour = hh;
  t.minute = mm;
  t.second = ss;

  t.neverSynced = (status[0] == '#');
  t.freeRun     = (status[1] == '*');
  t.summerTime  = (status[2] == 'S');
  t.announce    = (status[3] == '!');

  t.valid = validDateTime(t);
  if (!t.valid) return false;

  out = t;
  return true;
}

static void snapshotPsek(int64_t &psekUs, uint32_t &counter) {
  portENTER_CRITICAL(&psekMux);
  psekUs = isrLastPsekUs;
  counter = isrPsekCounter;
  portEXIT_CRITICAL(&psekMux);
}

static void processDecodedTelegram(const MeinbergTime &t) {
  int64_t psekUs;
  uint32_t psekCounter;
  snapshotPsek(psekUs, psekCounter);

  const int64_t nowUs = esp_timer_get_time();
  const int64_t telegramDelayUs = nowUs - psekUs;
  lastTelegramDelayUs = telegramDelayUs;
  ++telegramCount;

  // A serial telegram must be associated with a recent P_SEK marker.
  if (psekUs == 0 ||
      telegramDelayUs < 0 ||
      telegramDelayUs > MAX_TELEGRAM_AFTER_PSEK_US) {
    if (DEBUG_OUTPUT) {
      Serial.printf("Telegram valid, but no recent P_SEK (delay=%lld us)\n",
                    (long long)telegramDelayUs);
    }
    return;
  }

  // Only evaluate a P_SEK pulse once.
  if (psekCounter != lastProcessedPsekCounter) {
    if (lastProcessedPsekUs != 0) {
      const int64_t interval = psekUs - lastProcessedPsekUs;
      lastPsekIntervalUs = interval;

      if (interval >= PSEK_MIN_INTERVAL_US &&
          interval <= PSEK_MAX_INTERVAL_US) {
        if (stablePulseIntervals < 255) ++stablePulseIntervals;
      } else {
        stablePulseIntervals = 0;
        if (DEBUG_OUTPUT) {
          Serial.printf("P_SEK interval rejected: %lld us\n", (long long)interval);
        }
      }
    }

    lastProcessedPsekUs = psekUs;
    lastProcessedPsekCounter = psekCounter;
  }

  const bool psekStable = (stablePulseIntervals >= REQUIRED_STABLE_PULSES);

  // Pair the telegram's absolute UTC second with the P_SEK rising edge.
  timeBase.ntpSecondsAtPsek = toNtpSeconds(t);
  timeBase.refNtpSeconds = timeBase.ntpSecondsAtPsek;
  timeBase.psekUs = psekUs;
  timeBase.haveTime = true;
  timeBase.psekStable = psekStable;

  // Receiver state:
  // '#' -> definitely not trustworthy since reset.
  // no '*' -> currently locked; remember when lock was last seen.
  // '*' -> free-run/holdover; allow only for a bounded interval after real lock.
  if (!t.neverSynced && !t.freeRun) {
    lastLockedUs = nowUs;
    timeBase.synchronized = psekStable;
    timeBase.holdover = false;
  } else if (!t.neverSynced && t.freeRun && lastLockedUs != 0) {
    const uint32_t holdoverAge =
        static_cast<uint32_t>((nowUs - lastLockedUs) / 1000000LL);

    timeBase.holdover = (holdoverAge <= MAX_HOLDOVER_SECONDS);
    timeBase.synchronized = psekStable && timeBase.holdover;
  } else {
    timeBase.synchronized = false;
    timeBase.holdover = false;
  }

  if (DEBUG_OUTPUT) {
    Serial.printf(
      "MB %04d-%02d-%02d %02d:%02d:%02d UTC  "
      "status=[%c%c%c%c]  P_SEK delay=%lld us  stable=%u  NTP=%s%s\n",
      t.year, t.month, t.day,
      t.hour, t.minute, t.second,
      t.neverSynced ? '#' : ' ',
      t.freeRun ? '*' : ' ',
      t.summerTime ? 'S' : ' ',
      t.announce ? '!' : ' ',
      (long long)telegramDelayUs,
      stablePulseIntervals,
      timeBase.synchronized ? "SYNC" : "UNSYNC",
      timeBase.holdover ? "/HOLDOVER" : ""
    );
  }
}

static void feedMeinbergByte(uint8_t c) {
  if (c == 0x02) {              // STX
    telegramPos = 0;
    inTelegram = true;
    return;
  }

  if (!inTelegram) return;

  if (c == 0x03) {              // ETX
    if (telegramPos < sizeof(telegram)) {
      telegram[telegramPos] = '\0';

      MeinbergTime decoded;
      if (parseMeinbergTelegram(telegram, decoded)) {
        mb = decoded;
        processDecodedTelegram(mb);
      } else if (DEBUG_OUTPUT) {
        Serial.print("Bad Meinberg telegram: [");
        Serial.print(telegram);
        Serial.println("]");
      }
    }

    inTelegram = false;
    telegramPos = 0;
    return;
  }

  if (telegramPos < sizeof(telegram) - 1) {
    telegram[telegramPos++] = static_cast<char>(c);
  } else {
    // Framing/length error: wait for next STX.
    inTelegram = false;
    telegramPos = 0;
  }
}

// ------------------------- NTP helpers -------------------------

static uint64_t currentNtpTimestamp() {
  if (!timeBase.haveTime) return 0;

  int64_t elapsedUs = esp_timer_get_time() - timeBase.psekUs;
  if (elapsedUs < 0) elapsedUs = 0;

  uint64_t wholeSeconds = static_cast<uint64_t>(timeBase.ntpSecondsAtPsek);
  wholeSeconds += static_cast<uint64_t>(elapsedUs / 1000000LL);

  const uint32_t fracUs = static_cast<uint32_t>(elapsedUs % 1000000LL);
  const uint32_t fraction =
      static_cast<uint32_t>((static_cast<uint64_t>(fracUs) << 32) / 1000000ULL);

  return (wholeSeconds << 32) | fraction;
}

static void putNtpTimestamp(uint8_t *dst, uint64_t ts) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<uint8_t>(ts >> (56 - 8 * i));
  }
}

static void put32(uint8_t *dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v >> 24);
  dst[1] = static_cast<uint8_t>(v >> 16);
  dst[2] = static_cast<uint8_t>(v >> 8);
  dst[3] = static_cast<uint8_t>(v);
}

static uint32_t ntpDispersion16_16(double seconds) {
  if (seconds <= 0.0) return 0;
  const double scaled = seconds * 65536.0;
  if (scaled >= 4294967295.0) return 0xFFFFFFFFUL;
  return static_cast<uint32_t>(scaled + 0.5);
}

static uint32_t currentHoldoverAgeSeconds() {
  if (!timeBase.holdover || lastLockedUs == 0) return 0;
  const int64_t ageUs = esp_timer_get_time() - lastLockedUs;
  if (ageUs <= 0) return 0;
  return static_cast<uint32_t>(ageUs / 1000000LL);
}

static double currentRootDispersionSeconds() {
  if (!timeBase.synchronized) return 1.0;
  double d = LOCKED_BASE_DISPERSION_S;
  if (timeBase.holdover) {
    d += static_cast<double>(currentHoldoverAgeSeconds()) * HOLDOVER_DRIFT_RATE;
  }
  return d;
}

static void sendNtpReply(const IPAddress &remoteIP, uint16_t remotePort,
                         const uint8_t *request, uint64_t receiveTimestamp) {
  uint8_t reply[NTP_PACKET_SIZE] = {0};

  const bool synced = timeBase.synchronized;

  // LI=0 when synchronized, LI=3 when unsynchronized.
  // VN=4, Mode=4 (server).
  reply[0] = synced ? 0x24 : 0xE4;
  reply[1] = synced ? 1 : 16;   // Stratum 1, or 16 = unsynchronized
  reply[2] = request[2];        // copy client's poll interval
  reply[3] = static_cast<uint8_t>(-10);  // ~0.98 ms precision, conservative

  // Root delay = 0.
  put32(&reply[4], 0);

  // Root dispersion, 16.16 fixed point.
  // Locked: 5 ms base value.
  // Holdover: base value + worst-case 10 ppm quartz drift from datasheet.
  // Unsynchronized: 1 s placeholder; clients reject us anyway (LI=3, stratum 16).
  put32(&reply[8], ntpDispersion16_16(currentRootDispersionSeconds()));

  // Reference ID.
  if (synced && timeBase.holdover) {
    memcpy(&reply[12], "HOLD", 4);
  } else if (synced) {
    memcpy(&reply[12], "DCF ", 4);
  } else {
    memcpy(&reply[12], "INIT", 4);
  }

  // Reference timestamp: most recently accepted P_SEK.
  const uint64_t referenceTimestamp =
      static_cast<uint64_t>(timeBase.refNtpSeconds) << 32;
  putNtpTimestamp(&reply[16], referenceTimestamp);

  // Originate timestamp = client's transmit timestamp.
  memcpy(&reply[24], &request[40], 8);

  // Receive timestamp captured as soon as the packet was noticed.
  putNtpTimestamp(&reply[32], receiveTimestamp);

  // Transmit timestamp immediately before packet transmission.
  const uint64_t transmitTimestamp = currentNtpTimestamp();
  putNtpTimestamp(&reply[40], transmitTimestamp);

  udp.beginPacket(remoteIP, remotePort);
  udp.write(reply, sizeof(reply));
  udp.endPacket();
}

static void serviceNtp() {
  const int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  // Capture receive time as early as possible after parsePacket().
  const uint64_t receiveTimestamp = currentNtpTimestamp();

  uint8_t request[NTP_PACKET_SIZE] = {0};
  const int toRead = min(packetSize, static_cast<int>(NTP_PACKET_SIZE));
  udp.read(request, toRead);

  // Drain any oversized packet.
  while (udp.available()) udp.read();

  if (packetSize < static_cast<int>(NTP_PACKET_SIZE)) return;
  if (!timeBase.haveTime) return;

  const uint8_t mode = request[0] & 0x07;
  if (mode != 3) return; // NTP client request

  lastNtpClientIP = udp.remoteIP();
  lastNtpRequestMs = millis();
  ++ntpRequestCount;

  sendNtpReply(udp.remoteIP(), udp.remotePort(), request, receiveTimestamp);
}




// ------------------------- DHCP Option 7 test -------------------------
static portMUX_TYPE dhcpOptMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t dhcpLogServerRaw[4] = {0, 0, 0, 0};
static IPAddress dhcpLogServer(0, 0, 0, 0);
static bool dhcpOption7Received = false;

// Syslog (RFC3164-style over UDP/514, facility local0)
static bool syslogReady = false;

// Change tracking: only transitions/events are sent.
static bool syslogStateInitialized = false;
static bool prevEthLinkForSyslog = false;
static IPAddress prevEthIpForSyslog(0, 0, 0, 0);

extern "C" void lwip_dhcp_on_extra_option(struct dhcp *dhcp,
                                           uint8_t state,
                                           uint8_t option,
                                           uint8_t len,
                                           struct pbuf *p,
                                           uint16_t offset) {
  (void)dhcp;
  (void)state;
  if (option != 7 || len < 4 || p == nullptr) return;

  uint8_t b[4] = {0, 0, 0, 0};
  if (pbuf_copy_partial(p, b, 4, offset) != 4) return;

  portENTER_CRITICAL(&dhcpOptMux);
  for (int i = 0; i < 4; ++i) dhcpLogServerRaw[i] = b[i];
  dhcpLogServerPending = true;
  portEXIT_CRITICAL(&dhcpOptMux);
}

static void processDhcpOption7() {
  bool pending = false;
  uint8_t b[4] = {0, 0, 0, 0};

  portENTER_CRITICAL(&dhcpOptMux);
  if (dhcpLogServerPending) {
    for (int i = 0; i < 4; ++i) b[i] = dhcpLogServerRaw[i];
    dhcpLogServerPending = false;
    pending = true;
  }
  portEXIT_CRITICAL(&dhcpOptMux);

  if (!pending) return;

  dhcpLogServer = IPAddress(b[0], b[1], b[2], b[3]);
  dhcpOption7Received = true;

  if (DEBUG_OUTPUT) {
    Serial.print("DHCP Option 7 received: ");
    Serial.println(dhcpLogServer);
  }

  // Syslog becomes active in the normal loop context.
  syslogReady = false;
  syslogStateInitialized = false;
}


static String syslogTimestamp() {
  // RFC3164-style timestamp when Meinberg absolute time is trustworthy.
  if (mb.valid && !mb.neverSynced) {
    static const char *months[] = {
      "Jan","Feb","Mar","Apr","May","Jun",
      "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    const int mi = (mb.month >= 1 && mb.month <= 12) ? mb.month - 1 : 0;
    char b[24];
    snprintf(b, sizeof(b), "%s %2d %02d:%02d:%02d",
             months[mi], mb.day, mb.hour, mb.minute, mb.second);
    return String(b);
  }

  // Before first trustworthy DCF sync, make the missing absolute time explicit.
  return String("-");
}

static void sendSyslog(uint8_t severity, const String &message) {
  if (!syslogReady || !ETH.linkUp()) return;

  // facility local0 = 16; PRI = facility*8 + severity
  const int pri = 16 * 8 + (severity & 0x07);

  String line;
  line.reserve(message.length() + 96);
  line += "<";
  line += String(pri);
  line += ">";
  line += syslogTimestamp();
  line += " meinberg-ntp ";
  line += message;

  if (syslogUdp.beginPacket(dhcpLogServer, 514)) {
    syslogUdp.write(reinterpret_cast<const uint8_t *>(line.c_str()), line.length());
    syslogUdp.endPacket();
    ++syslogMessageCount;
    lastSyslogMs = millis();

    if (DEBUG_OUTPUT) {
      Serial.print("SYSLOG -> ");
      Serial.println(line);
    }
  }
}

static void initializeSyslogAfterDhcpOption() {
  if (!dhcpOption7Received || syslogReady) return;

  syslogReady = (dhcpLogServer != IPAddress(0, 0, 0, 0));
  if (!syslogReady) return;

  prevEthLinkForSyslog = ETH.linkUp();
  prevEthIpForSyslog = ETH.localIP();

  // Establish baseline first, then send concise startup/current-state events.
  prevNeverSynced = mb.neverSynced;
  prevFreeRun = mb.freeRun;
  prevSummerTime = mb.summerTime;
  prevAnnounce = mb.announce;
  prevPsekStable = timeBase.psekStable;
  prevNtpSync = timeBase.synchronized;
  prevHoldover = timeBase.holdover;
  prevStratum = timeBase.synchronized ? 1 : 16;
  syslogStateInitialized = true;

  sendSyslog(5, String("syslog configured via DHCP option 7; server=") +
                   dhcpLogServer.toString() +
                   "; firmware=" + FIRMWARE_VERSION);
  sendSyslog(5, String("startup state; ip=") + ETH.localIP().toString() +
                   "; dcf_status=[" + rawStatusString() + "]" +
                   "; psek=" + (timeBase.psekStable ? "stable" : "unstable") +
                   "; ntp=" + currentNtpStateText() +
                   "; stratum=" + String(timeBase.synchronized ? 1 : 16));
}

static void monitorStateForSyslog() {
  if (!syslogReady || !syslogStateInitialized) return;

  const bool ethLink = ETH.linkUp();
  const IPAddress ethIp = ETH.localIP();
  const int stratum = timeBase.synchronized ? 1 : 16;

  if (ethLink != prevEthLinkForSyslog) {
    // Link-down itself cannot be delivered over the same Ethernet interface.
    // On link-up, report that connectivity returned.
    if (ethLink) {
      sendSyslog(5, "Ethernet link UP");
    }
    prevEthLinkForSyslog = ethLink;
  }

  if (ethIp != prevEthIpForSyslog && ethIp != IPAddress(0,0,0,0)) {
    sendSyslog(5, String("DHCP address changed to ") + ethIp.toString());
    prevEthIpForSyslog = ethIp;
  }

  if (mb.neverSynced != prevNeverSynced) {
    sendSyslog(mb.neverSynced ? 3 : 5,
               String("DCF # not-synchronized-since-reset=") +
               (mb.neverSynced ? "ON" : "OFF") +
               "; status=[" + rawStatusString() + "]");
    prevNeverSynced = mb.neverSynced;
  }

  if (mb.freeRun != prevFreeRun) {
    sendSyslog(mb.freeRun ? 4 : 5,
               String("DCF * quartz-free-run=") +
               (mb.freeRun ? "ON" : "OFF") +
               "; status=[" + rawStatusString() + "]");
    prevFreeRun = mb.freeRun;
  }

  if (mb.summerTime != prevSummerTime) {
    sendSyslog(5, String("DCF S summer-time=") +
                   (mb.summerTime ? "ON" : "OFF"));
    prevSummerTime = mb.summerTime;
  }

  if (mb.announce != prevAnnounce) {
    sendSyslog(5, String("DCF ! time-change-announcement=") +
                   (mb.announce ? "ON" : "OFF"));
    prevAnnounce = mb.announce;
  }

  if (timeBase.psekStable != prevPsekStable) {
    sendSyslog(timeBase.psekStable ? 5 : 3,
               String("P_SEK stable=") +
               (timeBase.psekStable ? "YES" : "NO") +
               "; interval_us=" + String((long long)lastPsekIntervalUs));
    prevPsekStable = timeBase.psekStable;
  }

  if (timeBase.synchronized != prevNtpSync ||
      timeBase.holdover != prevHoldover ||
      stratum != prevStratum) {
    uint8_t severity = 3; // error when unsynchronized
    if (timeBase.synchronized && timeBase.holdover) severity = 4; // warning
    else if (timeBase.synchronized) severity = 5; // notice

    String ref = "INIT";
    if (timeBase.synchronized && timeBase.holdover) ref = "HOLD";
    else if (timeBase.synchronized) ref = "DCF";

    sendSyslog(severity,
               String("NTP state=") + currentNtpStateText() +
               "; stratum=" + String(stratum) +
               "; ref=" + ref);

    prevNtpSync = timeBase.synchronized;
    prevHoldover = timeBase.holdover;
    prevStratum = stratum;
  }
}

// ------------------------- Firmware update (Web OTA) -------------------------

static void handleUpdatePage() {
  if (!requireWebAdmin()) return;
  String page;
  page.reserve(2500);
  page += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware Update</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:700px;margin:2rem auto;padding:0 1rem;background:#f5f5f5;color:#222}"
    ".box{background:white;padding:1.2rem;border-radius:.4rem}"
    "input,button{font-size:1rem;margin:.5rem 0;padding:.5rem}"
    ".warn{color:#8a4b00}"
    "</style></head><body>"
    "<h1>Meinberg NTP Firmware Update</h1>"
    "<div class='box'>"
    "<p>Select a compiled ESP32 <code>.bin</code> file.</p>"
    "<p class='warn'><b>Important:</b> Do not interrupt power or Ethernet during the upload.</p>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin,application/octet-stream' required><br>"
    "<button type='submit'>Upload and reboot</button>"
    "</form>"
    "<p><a href='/'>Back to status</a></p>"
    "</div></body></html>"
  );
  web.send(200, "text/html; charset=utf-8", page);
}

static void handleUpdateFinished() {
  if (!otaUploadAuthorized || !requireWebAdmin()) {
    otaUploadAuthorized = false;
    return;
  }
  otaUploadAuthorized = false;
  const bool error = Update.hasError();

  web.sendHeader("Connection", "close");
  web.send(
      error ? 500 : 200,
      "text/html; charset=utf-8",
      error
        ? "<html><body><h1>Update failed</h1><p>See serial log.</p><p><a href='/update'>Try again</a></p></body></html>"
        : "<html><body><h1>Update successful</h1><p>Device is rebooting...</p></body></html>");

  if (!error) {
    delay(750);
    ESP.restart();
  }
}

static void handleUpdateUpload() {
  HTTPUpload &upload = web.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaUploadAuthorized =
        webAdminConfigured &&
        web.authenticate(webAdminUser.c_str(), webAdminPassword.c_str());
    if (!otaUploadAuthorized) {
      if (DEBUG_OUTPUT) Serial.println("OTA: rejected unauthorized upload");
      return;
    }
    if (DEBUG_OUTPUT) {
      Serial.printf("OTA: start %s\n", upload.filename.c_str());
    }
    sendSyslog(5, String("OTA upload started; file=") + upload.filename);

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      if (DEBUG_OUTPUT) Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaUploadAuthorized) return;
    if (!Update.hasError()) {
      const size_t written = Update.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize && DEBUG_OUTPUT) {
        Update.printError(Serial);
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (!otaUploadAuthorized) return;
    if (!Update.hasError()) {
      if (Update.end(true)) {
        if (DEBUG_OUTPUT) {
          Serial.printf("OTA: success, %u bytes\n", upload.totalSize);
        }
        sendSyslog(5, String("OTA upload successful; bytes=") + String(upload.totalSize));
      } else if (DEBUG_OUTPUT) {
        Update.printError(Serial);
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (!otaUploadAuthorized) return;
    Update.abort();
    if (DEBUG_OUTPUT) Serial.println("OTA: aborted");
    sendSyslog(4, "OTA upload aborted");
  }
}


// ------------------------- Web security pages -------------------------
static String securityPage(const String &message = String()) {
  String page;
  page.reserve(4200);
  page += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Web Security</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:700px;margin:2rem auto;padding:0 1rem;background:#f5f5f5;color:#222}"
    ".box{background:white;padding:1.2rem;border-radius:.4rem}"
    "label{display:block;margin-top:.8rem;font-weight:600}"
    "input,button{font-size:1rem;padding:.55rem;margin-top:.25rem;max-width:100%;box-sizing:border-box}"
    ".ok{color:#17823b;font-weight:700}.warn{color:#8a4b00}"
    "</style></head><body><h1>Meinberg NTP Web Security</h1><div class='box'>"
  );
  if (!message.isEmpty()) {
    page += F("<p class='ok'>");
    page += message;
    page += F("</p>");
  }
  if (!webAdminConfigured) {
    page += F("<p class='warn'><b>Initial setup:</b> create the administrator credentials before firmware update and reboot are enabled.</p>");
  } else {
    page += F("<p>Web administrator: <b>");
    page += htmlEscape(webAdminUser);
    page += F("</b></p><p>Enter new credentials below to change them.</p>");
  }
  page += F(
    "<form method='POST' action='/security'>"
    "<label for='user'>Username</label>"
    "<input id='user' name='user' autocomplete='username' minlength='1' maxlength='32' required>"
    "<label for='pass'>Password</label>"
    "<input id='pass' name='pass' type='password' autocomplete='new-password' minlength='8' maxlength='64' required>"
    "<label for='confirm'>Confirm password</label>"
    "<input id='confirm' name='confirm' type='password' autocomplete='new-password' minlength='8' maxlength='64' required><br>"
    "<button type='submit'>Save credentials</button>"
    "</form><p><a href='/'>Back to status</a></p></div></body></html>"
  );
  return page;
}

static void handleSecurityPage() {
  // First-time setup is intentionally accessible without credentials because none exist.
  // Once configured, changing credentials requires the current login.
  if (webAdminConfigured && !requireWebAdmin()) return;
  web.send(200, "text/html; charset=utf-8", securityPage());
}

static void handleSecuritySave() {
  if (webAdminConfigured && !requireWebAdmin()) return;

  String user = web.arg("user");
  String pass = web.arg("pass");
  String confirm = web.arg("confirm");
  user.trim();

  if (user.isEmpty() || user.length() > 32 ||
      pass.length() < 8 || pass.length() > 64 ||
      pass != confirm) {
    web.send(400, "text/html; charset=utf-8",
             securityPage("Invalid input: password must be 8-64 characters and both passwords must match."));
    return;
  }

  if (!securityPrefs.begin("webadmin", false)) {
    web.send(500, "text/plain; charset=utf-8", "Unable to open NVS.");
    return;
  }
  const size_t u = securityPrefs.putString("user", user);
  const size_t p = securityPrefs.putString("pass", pass);
  securityPrefs.end();

  if (u == 0 || p == 0) {
    web.send(500, "text/plain; charset=utf-8", "Unable to save credentials.");
    return;
  }

  webAdminUser = user;
  webAdminPassword = pass;
  webAdminConfigured = true;

  sendSyslog(5, "web administrator credentials configured/changed");
  web.send(200, "text/html; charset=utf-8",
           securityPage("Credentials saved. Firmware update and reboot are now protected."));
}

// ------------------------- Status web page -------------------------

static String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      default:  out += s[i]; break;
    }
  }
  return out;
}

static String rawStatusString() {
  String s;
  s.reserve(4);
  s += mb.neverSynced ? '#' : ' ';
  s += mb.freeRun ? '*' : ' ';
  s += mb.summerTime ? 'S' : ' ';
  s += mb.announce ? '!' : ' ';
  return s;
}

static String meinbergStateText() {
  if (!mb.valid) return F("NO TELEGRAM");
  if (mb.neverSynced) return F("NOT SYNCHRONIZED SINCE RESET");
  if (mb.freeRun) return F("QUARTZ HOLDOVER / FREE-RUN");
  return F("DCF77 LOCKED");
}

static String currentNtpStateText() {
  if (!timeBase.haveTime) return F("NO TIME");
  if (!timeBase.synchronized) return F("UNSYNCHRONIZED");
  if (timeBase.holdover) return F("SYNCHRONIZED / HOLDOVER");
  return F("SYNCHRONIZED");
}

static String formatLastTelegramUtc() {
  if (!mb.valid) return F("-");
  char b[32];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d UTC",
           mb.year, mb.month, mb.day, mb.hour, mb.minute, mb.second);
  return String(b);
}


static String uptimeText() {
  const uint32_t total = millis() / 1000U;
  const uint32_t days = total / 86400U;
  const uint32_t hours = (total % 86400U) / 3600U;
  const uint32_t minutes = (total % 3600U) / 60U;
  const uint32_t seconds = total % 60U;

  char b[32];
  if (days > 0) {
    snprintf(b, sizeof(b), "%ud %02u:%02u:%02u",
             days, hours, minutes, seconds);
  } else {
    snprintf(b, sizeof(b), "%02u:%02u:%02u",
             hours, minutes, seconds);
  }
  return String(b);
}

static String ageText(uint32_t whenMs) {
  if (whenMs == 0) return F("never");
  const uint32_t age = (millis() - whenMs) / 1000U;
  if (age < 60) return String(age) + F(" s ago");
  if (age < 3600) return String(age / 60U) + F(" min ago");
  return String(age / 3600U) + F(" h ago");
}

static void handleRoot() {
  String page;
  page.reserve(7000);

  const bool link = ETH.linkUp();
  const IPAddress ip = ETH.localIP();
  const String raw = rawStatusString();
  const int stratum = timeBase.synchronized ? 1 : 16;
  const bool receiverGood = mb.valid && !mb.neverSynced && !mb.freeRun;

  page += F(
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='2'>"
    "<title>Meinberg NTP</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:850px;margin:2rem auto;padding:0 1rem;background:#f5f5f5;color:#222}"
    "h1{margin-bottom:.2rem}h2{margin-top:1.8rem}"
    "table{border-collapse:collapse;width:100%;background:white}"
    "td,th{padding:.55rem .7rem;border-bottom:1px solid #ddd;text-align:left}"
    "td:first-child{width:42%;font-weight:600}"
    "code{font-size:1.05rem;background:#eee;padding:.1rem .3rem;border-radius:.2rem}"
    ".ok{font-weight:700;color:#17823b}.bad{font-weight:700;color:#c62828}.note{color:#555}"
    "</style></head><body>"
    "<h1>Meinberg UA537TGP NTP Server</h1>"
    "<div class='note'>WT32-ETH01 &middot; firmware "
  );
  page += FIRMWARE_VERSION;
  page += F(" &middot; auto refresh every 2 s");
  if (webAdminConfigured) {
    page += F(" &middot; <a href='/update'>Firmware Update</a>");
  } else {
    page += F(" &middot; <a href='/security'>Set Web Password</a>");
  }
  page += F("</div>");

  // 1) Meinberg / DCF77
  page += F("<h2>Meinberg / DCF77</h2><table>");
  page += F("<tr><td>Last telegram</td><td>");
  page += formatLastTelegramUtc();

  page += F("</td></tr><tr><td>Receiver state</td><td><span class='");
  page += receiverGood ? F("ok'>") : F("bad'>");
  page += meinbergStateText();
  page += F("</span></td></tr><tr><td>Raw status</td><td><code>[");
  page += htmlEscape(raw);
  page += F("]</code></td></tr>");

  page += F("<tr><td># &mdash; not synchronized since reset</td><td><span class='");
  page += mb.neverSynced ? F("bad'>ON") : F("ok'>OFF");
  page += F("</span></td></tr>");

  page += F("<tr><td>* &mdash; quartz free-run</td><td><span class='");
  page += mb.freeRun ? F("bad'>ON") : F("ok'>OFF");
  page += F("</span></td></tr>");

  // S and ! are informational states, not inherently good/bad.
  page += F("<tr><td>S &mdash; summer time</td><td>");
  page += mb.summerTime ? F("ON") : F("OFF");
  page += F("</td></tr><tr><td>! &mdash; time change announced</td><td>");
  page += mb.announce ? F("ON") : F("OFF");
  page += F("</td></tr><tr><td>Telegrams received</td><td>");
  page += String(telegramCount);
  page += F("</td></tr></table>");

  // 2) NTP
  page += F("<h2>NTP</h2><table>");
  page += F("<tr><td>Status</td><td><span class='");
  page += timeBase.synchronized ? F("ok'>") : F("bad'>");
  page += currentNtpStateText();
  page += F("</span></td></tr>");

  page += F("<tr><td>Stratum</td><td><span class='");
  page += (stratum == 1) ? F("ok'>") : F("bad'>");
  page += String(stratum);
  page += F("</span></td></tr><tr><td>Reference ID</td><td>");
  if (timeBase.synchronized && timeBase.holdover) page += F("HOLD");
  else if (timeBase.synchronized) page += F("DCF");
  else page += F("INIT");

  page += F("</td></tr><tr><td>Root dispersion</td><td>");
  page += String(currentRootDispersionSeconds(), 6);
  page += F(" s</td></tr><tr><td>Holdover age</td><td>");
  if (timeBase.holdover) {
    page += String(currentHoldoverAgeSeconds());
    page += F(" s / ");
    page += String(MAX_HOLDOVER_SECONDS);
    page += F(" s policy");
  } else {
    page += F("-");
  }
  page += F("</td></tr><tr><td>NTP requests</td><td>");
  page += String(ntpRequestCount);
  page += F("</td></tr><tr><td>Last NTP client</td><td>");
  page += (ntpRequestCount > 0) ? lastNtpClientIP.toString() : String("-");
  page += F("</td></tr><tr><td>Last NTP request</td><td>");
  page += ageText(lastNtpRequestMs);
  page += F("</td></tr></table>");

  // 3) P_SEK
  page += F("<h2>P_SEK</h2><table>");
  page += F("<tr><td>Stable</td><td><span class='");
  page += timeBase.psekStable ? F("ok'>YES") : F("bad'>NO");
  page += F("</span></td></tr><tr><td>Stable intervals</td><td>");
  page += String(stablePulseIntervals);
  page += F("</td></tr><tr><td>Last interval</td><td>");
  if (lastPsekIntervalUs != 0) {
    page += String((long long)lastPsekIntervalUs);
    page += F(" us");
  } else {
    page += F("-");
  }
  page += F("</td></tr><tr><td>Telegram after P_SEK</td><td>");
  if (lastTelegramDelayUs != 0) {
    page += String((long long)lastTelegramDelayUs);
    page += F(" us");
  } else {
    page += F("-");
  }
  page += F("</td></tr></table>");

  // 4) Ethernet
  page += F("<h2>Ethernet</h2><table>");
  page += F("<tr><td>Link</td><td><span class='");
  page += link ? F("ok'>UP") : F("bad'>DOWN");
  page += F("</span></td></tr><tr><td>IP address</td><td>");
  page += ip.toString();
  page += F("</td></tr></table>");

  // 5) Syslog
  page += F("<h2>Syslog</h2><table>");
  page += F("<tr><td>Status</td><td><span class='");
  if (syslogReady && dhcpOption7Received) {
    page += F("ok'>ACTIVE (from DHCP)");
  } else {
    page += F("bad'>INACTIVE");
  }
  page += F("</span></td></tr><tr><td>Server</td><td>");
  page += dhcpOption7Received ? dhcpLogServer.toString() : String("-");
  page += F("</td></tr></table>");

  // 6) System
  page += F("<h2>System</h2><table>");
  page += F("<tr><td>Firmware</td><td>");
  page += FIRMWARE_VERSION;
  page += F("</td></tr><tr><td>Uptime</td><td>");
  page += uptimeText();
  page += F("</td></tr><tr><td>Web admin</td><td><span class='");
  page += webAdminConfigured ? F("ok'>ENABLED") : F("bad'>NOT CONFIGURED");
  page += F("</span> &middot; <a href='/security'>");
  page += webAdminConfigured ? F("Change credentials") : F("Set credentials");
  page += F("</a></td></tr><tr><td>Restart</td><td>");
  if (webAdminConfigured) {
    page += F("<form method='POST' action='/reboot' style='margin:0' "
              "onsubmit=\"return confirm('Restart NTP server now?');\">"
              "<button type='submit'>Reboot</button></form>");
  } else {
    page += F("Disabled until web credentials are configured");
  }
  page += F("</td></tr></table>");

  page += F(
    "<p class='note'>While # is set, the server deliberately reports "
    "NTP stratum 16 so clients will reject the old/unsynchronized time.</p>"
    "</body></html>"
  );

  web.send(200, "text/html; charset=utf-8", page);
}

static void handleStatusJson() {
  String json;
  json.reserve(1000);

  json += F("{");
  json += F("\"ethernet_link\":");
  json += ETH.linkUp() ? F("true") : F("false");
  json += F(",\"ip\":\"");
  json += ETH.localIP().toString();
  json += F("\",\"meinberg_time\":\"");
  json += formatLastTelegramUtc();
  json += F("\",\"raw_status\":\"");
  json += rawStatusString();
  json += F("\",\"never_synced\":");
  json += mb.neverSynced ? F("true") : F("false");
  json += F(",\"free_run\":");
  json += mb.freeRun ? F("true") : F("false");
  json += F(",\"summer_time\":");
  json += mb.summerTime ? F("true") : F("false");
  json += F(",\"time_change_announced\":");
  json += mb.announce ? F("true") : F("false");
  json += F(",\"psek_stable\":");
  json += timeBase.psekStable ? F("true") : F("false");
  json += F(",\"psek_interval_us\":");
  json += String((long long)lastPsekIntervalUs);
  json += F(",\"telegram_delay_us\":");
  json += String((long long)lastTelegramDelayUs);
  json += F(",\"ntp_synchronized\":");
  json += timeBase.synchronized ? F("true") : F("false");
  json += F(",\"ntp_stratum\":");
  json += String(timeBase.synchronized ? 1 : 16);
  json += F(",\"root_dispersion_s\":");
  json += String(currentRootDispersionSeconds(), 6);
  json += F(",\"holdover_age_s\":");
  json += String(currentHoldoverAgeSeconds());
  json += F(",\"firmware_version\":\"");
  json += FIRMWARE_VERSION;
  json += F("\",\"uptime_s\":");
  json += String(millis() / 1000U);
  json += F(",\"ntp_requests\":");
  json += String(ntpRequestCount);
  json += F(",\"last_ntp_client\":\"");
  json += (ntpRequestCount > 0) ? lastNtpClientIP.toString() : String("");
  json += F("}");

  web.send(200, "application/json; charset=utf-8", json);
}


static void handleReboot() {
  if (!requireWebAdmin()) return;
  web.send(
      200,
      "text/html; charset=utf-8",
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='8;url=/'>"
      "<title>Rebooting</title></head><body>"
      "<h1>Rebooting...</h1>"
      "<p>The NTP server is restarting. The status page will reopen shortly.</p>"
      "</body></html>");

  if (DEBUG_OUTPUT) Serial.println("Web: reboot requested");
  sendSyslog(5, "manual reboot requested via web interface");
  delay(750);
  ESP.restart();
}

static void startWebServer() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/status.json", HTTP_GET, handleStatusJson);
  web.on("/security", HTTP_GET, handleSecurityPage);
  web.on("/security", HTTP_POST, handleSecuritySave);
  web.on("/update", HTTP_GET, handleUpdatePage);
  web.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateUpload);
  web.on("/reboot", HTTP_POST, handleReboot);
  web.onNotFound([]() {
    web.send(404, "text/plain", "Not found");
  });
  web.begin();

  if (DEBUG_OUTPUT) {
    Serial.println("HTTP/80 listening");
  }
}

static void monitorEthernet() {
  const bool link = ETH.linkUp();
  const IPAddress ip = ETH.localIP();

  if (link != previousEthLink) {
    previousEthLink = link;
    if (DEBUG_OUTPUT) {
      Serial.printf("Ethernet: link %s\n", link ? "UP" : "DOWN");
    }
  }

  if (ip != previousEthIP) {
    previousEthIP = ip;
    if (DEBUG_OUTPUT) {
      Serial.print("Ethernet: IP ");
      Serial.println(ip);
    }
  }
}

// ------------------------- Setup / loop -------------------------

static void startEthernet() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  const bool ok = ETH.begin(
      ETH_PHY_LAN8720,
      1,                      // PHY address
      23,                     // MDC
      18,                     // MDIO
      16,                     // PHY power
      ETH_CLOCK_GPIO0_IN);
#else
  const bool ok = ETH.begin(
      1,                      // PHY address
      16,                     // PHY power
      23,                     // MDC
      18,                     // MDIO
      ETH_PHY_LAN8720,
      ETH_CLOCK_GPIO0_IN);
#endif

  if (!ok && DEBUG_OUTPUT) {
    Serial.println("ETH.begin() failed");
  }

  // Give DHCP a little time. The loop remains functional even if no cable exists.
  const uint32_t start = millis();
  while (ETH.localIP() == IPAddress(0, 0, 0, 0) &&
         millis() - start < 10000) {
    delay(100);
  }

  if (DEBUG_OUTPUT) {
    Serial.print("Ethernet IP: ");
    Serial.println(ETH.localIP());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  if (DEBUG_OUTPUT) {
    Serial.println();
    Serial.println("Meinberg UA537TGP NTP server v7.5.2 / WT32-ETH01");
    Serial.print("Firmware: ");
    Serial.println(FIRMWARE_VERSION);
    Serial.println("UART0 reserved for flash/debug");
    Serial.println("Meinberg RX: GPIO35, P_SEK: GPIO33");
  }

  // RX only; TX is deliberately unused.
  MeinbergSerial.begin(
      MEINBERG_BAUD,
      SERIAL_8N1,
      MEINBERG_RX_PIN,
      -1);

  pinMode(PSEK_PIN, INPUT);
  attachInterrupt(
      digitalPinToInterrupt(PSEK_PIN),
      psekISR,
      RISING);

  startEthernet();

  if (udp.begin(NTP_PORT)) {
    if (DEBUG_OUTPUT) Serial.println("UDP/123 listening");
  } else {
    if (DEBUG_OUTPUT) Serial.println("Failed to bind UDP/123");
  }

  loadWebSecurity();
  startWebServer();

  previousEthLink = ETH.linkUp();
  previousEthIP = ETH.localIP();
}

void loop() {
  while (MeinbergSerial.available()) {
    feedMeinbergByte(static_cast<uint8_t>(MeinbergSerial.read()));
  }

  serviceNtp();
  web.handleClient();
  monitorEthernet();
  processDhcpOption7();
  initializeSyslogAfterDhcpOption();
  monitorStateForSyslog();

  // If P_SEK vanishes for >5 seconds, do not claim synchronization.
  if (timeBase.synchronized) {
    int64_t psekUs;
    uint32_t dummy;
    snapshotPsek(psekUs, dummy);

    if (psekUs == 0 || (esp_timer_get_time() - psekUs) > 5000000LL) {
      timeBase.synchronized = false;
      timeBase.psekStable = false;
      stablePulseIntervals = 0;

      if (DEBUG_OUTPUT) {
        Serial.println("P_SEK timeout -> NTP unsynchronized");
      }
    }
  }
}
