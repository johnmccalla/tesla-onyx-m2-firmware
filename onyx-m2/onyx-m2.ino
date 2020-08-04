//   ___  _  ___   ____  __  __  __ ___
//  / _ \| \| \ \ / /\ \/ / |  \/  |_  )
// | (_) | .` |\ V /  >  <  | |\/| |/ /
//  \___/|_|\_| |_|  /_/\_\ |_|  |_/___|
//
// Firmware for Macchina M2
// https://github.com/onyx-m2/onyx-m2-firmware

#include <variant.h>
#include <due_can.h>
#include <Arduino_Due_SD_HSMCI.h>
#include <PacketSerial.h>

// Logging is now controlled by the built in "core debug level", and means you
// can control it from the Arduino menu (or vscode board config panel). If you don't
// see anything other than "None", add the relevant entries in boards.txt for superb.
#define PRINT(...)
#define LOG_E(...)
#define LOG_W(...)
#define LOG_I(...)
#define LOG_D(...)
#ifndef CORE_DEBUG_LEVEL
  #error "Update your boards.txt file to include CORE_DEBUG_LEVEL as per the README"
#endif
#if CORE_DEBUG_LEVEL > 0
  char __log_buffer[256];
  #define PRINT(...) sprintf(__log_buffer, __VA_ARGS__);SerialUSB.println(__log_buffer);
  #define OUTPUT_LOG(level, ...) sprintf(__log_buffer, __VA_ARGS__);SerialUSB.print("[");SerialUSB.print(level);SerialUSB.print("][m2:");SerialUSB.print(__LINE__);SerialUSB.print("] ");SerialUSB.println(__log_buffer);
  #define LOG_E(...) OUTPUT_LOG("E", __VA_ARGS__)
#endif
#if CORE_DEBUG_LEVEL > 1
  #define LOG_W(...) OUTPUT_LOG("W", __VA_ARGS__)
#endif
#if CORE_DEBUG_LEVEL > 2
  #define LOG_I(...) OUTPUT_LOG("I", __VA_ARGS__)
#endif
#if CORE_DEBUG_LEVEL > 3
  #define LOG_D(...) OUTPUT_LOG("D", __VA_ARGS__)
#endif

// Serial maps to the BLE link xbee
// BLE allows you transfer maximum of 20 bytes, so we'll keep everything under that
// limit to allow atomic transmission of CAN messages (which works out well as CAN
// messages max out at 16 bytes).
// The format of the transmitted message is [timestamp | bus | id | length | data]:
// uint32_t timestamp (little endian milliseconds)
// uint8_t bus (the source bus, vehicle:0, chassis:1)
// uint16_t id (little endian message id)
// uint8_t length (number of bytes of data)
// uint8_t data[1..8] (signal data, up to 8 bytes worth)
#define Xbee Serial
PacketSerial SuperB;

// Tesla Model 3 Can buses
// Can0 is connected to the vehicle bus, under the center console at the back.
// Can1 is connected to the chassis bus, under the passenger seat.
#define VehCan Can0
#define VEH_BUS 0
#define ChCan Can1
#define CH_BUS 1

// Mock is a mock device that is enabled when something is connected to the usb port.
// This can be used to inject can message directly into the M2 for testing. It is
// designed to be used with the 'bin/onyx-serial-replay' script.
PacketSerial Mock;

// FS allows access to the SD card for data logging.
#define DATALOGGING_DIRECTORY "logs"
#define DATALOGGING_FLUSH_INTERVAL 1000
FileStore FS;

// Mode constants
// The firmware has 2 completely separate modes of operation
// In RUN mode, the firmware runs normally
// In SUPERB mode, the firmware provides a direct link to the SuperB, allowing it to
// flashed and debugged
#define MODE_RUN 1
#define MODE_SUPERB 2

// State constants
// The firmware can be in one of 3 states: idle (no can messages), passive (can messages
// received but not transmitted), and active.
#define STATE_IDLE 0
#define STATE_PASSIVE 1
#define STATE_ACTIVE 2

// Message size related constants
#define CAN_MSG_SIZE 8        // Size of fixed part of can message
#define CAN_MSG_MAX_LENGTH 8  // Maximum frame length
#define CAN_BUS_COUNT 2       // Number of can buses
#define CAN_MSG_COUNT 2048    // Number of message ids
#define CAN_MSG_MAX_SIZE 15   // Maximum size of a can message
#define CAN_MSG_TS_OFFSET 0   // Offset of timestamp
#define CAN_MSG_ID_OFFSET 4   // Offset of message id
#define CAN_MSG_LEN_OFFSET 7  // Offset of message payload length

// Rate limiting constant
// This limits any one message id to the specified period in milliseconds
#define CAN_MSG_DEFAULT_TRANSMIT_INTERVAL 250

// Message flags
// Each message can have up to 8 flags associated with it
// These flags are set remotely by using the command interface

// Flags this message for realtime OTA transmission
#define CAN_MSG_FLAG_TRANSMIT 0x01

// Flags this message for real time OTA transmission even if its value is unmodified
#define CAN_MSG_FLAG_TRANSMIT_UNMODIFIED 0x02

// SuperB interface
// This allows the SuperB to relay commands from apps and send notifications to the M2
// The format is <u8:id, u8:len, u8[len]:data>.
#define SUPERB_COMMAND 0x01
#define SUPERB_NOTIFICATION 0x02

// Command interface
// This allows apps to send commands on realtime to the M2
// Commands have no specific format, other than the first byte being the command id
// and the second byte being the length of the command
#define CMDID_SET_ALL_MSG_FLAGS 0x01
#define CMDID_SET_MSG_FLAGS 0x02
#define CMDID_GET_MSG_LAST_VALUE 0x03
#define CMDID_GET_ALL_MSG_LAST_VALUE 0x04
#define CMDID_TAKE_SNAPSHOT 0x05

// Notification interface
// This allows the SuperB to send notifications in realtime to the M2
#define NOTIFY_WIFI_UP 0x01
#define NOTIFY_WIFI_DOWN 0x02
#define NOTIFY_WS_UP 0x03
#define NOTIFY_WS_DOWN 0x04
#define NOTIFY_LATENCY 0x05
#define NOTIFY_BLE_CONNECTED 0x06
#define NOTIFY_BLE_DISCONNECTED 0x07

// Commands related constants
#define SUPERB_TYPE_OFFSET 0
#define SUPERB_ID_OFFSET 1
#define SUPERB_LENGTH_OFFSET 2
#define SUPERB_DATA_OFFSET 3

// Message ids
#define MSGID_SYSTEM_TIME 792
#define MSGID_UNIX_TIME 1320
#define MSGID_VIN_INFO 1029

// LED colours
// When powered, the red led is, flashing the version number at startup.
// When CAN traffic is detected, the colour changes to blue.
// When packets are actually sent to the SuperB for transmission to the server,
// the colour is green.
#define LED_IDLE RGB_RED
#define LED_PASSIVE RGB_BLUE
#define LED_ACTIVE RGB_GREEN

// The connection status of the SuperB is indicated using surface mount leds (not
// visible in the enclosure). Yellow means wifi is up, green means ws is up.
#define LED_WIFI_UP DS5
#define LED_WS_UP DS6
#define LED_BLE_CONNECTED DS4

// The button activated SuperB programming mode is indicated with another set of
// surface mount leds
#define LED_SUPERB_YELLOW DS3
#define LED_SUPERB_RED DS2

// How fast can an led change colour
#define LEDCFG_CHANGE_INTERVAL 1000

// Firmware buffers
// The message flags buffer hold the current flag values for all messages.
// The message buffer holds all incoming frames from the Tesla. This ends up being
// very large, but the memory is there, so might as well use it. Having this allows
// us to do away with ring buffers means we get write without blocking ever (mostly).
struct CanMsg {
  uint32_t timestamp;
  uint32_t transmitTimestamp;
  uint16_t id;
  uint8_t bus;
  uint8_t flags;
  uint8_t length;
  uint8_t transmitInterval;
  uint8_t values[CAN_MSG_MAX_LENGTH];
  uint8_t transmitValues[CAN_MSG_MAX_LENGTH];

  CanMsg(uint8_t _bus, uint16_t _id) :
    timestamp(0),
    transmitTimestamp(0),
    id(_id),
    bus(_bus),
    flags(0),
    length(0),
    transmitInterval(CAN_MSG_DEFAULT_TRANSMIT_INTERVAL) {
      memset(values, 0, sizeof(values));
      memset(transmitValues, 0, sizeof(transmitValues));
  }

  bool has(uint8_t flag) {
    return (flags & flag) == flag;
  }

  void update(CAN_FRAME frame, uint32_t now) {
    timestamp = now;
    length = frame.length;
    memcpy(values, frame.data.bytes, length);
  }

  void pack(uint8_t* buf, uint8_t* size) {
    *buf++ = (uint8_t)(timestamp & 0xff);
    *buf++ = (uint8_t)(timestamp >> 8);
    *buf++ = (uint8_t)(timestamp >> 16);
    *buf++ = (uint8_t)(timestamp >> 24);
    *buf++ = bus;
    *buf++ = (uint8_t)(id & 0xff);
    *buf++ = (uint8_t)(id >> 8);
    *buf++ = length;
    // super annoying bug with unix time being big endian while everything else
    // is little endian; let's not slow down regular processing and just have
    // explicit copy statements for unix time
    if (id == MSGID_UNIX_TIME) {
      buf[0] = values[3];
      buf[1] = values[2];
      buf[2] = values[1];
      buf[3] = values[0];
    }
    else {
      memcpy(buf, values, length);
    }
    *size = CAN_MSG_SIZE + length;
  }

  bool transmit(uint32_t now) {
    // check transmission inteval
    if (now - transmitTimestamp < transmitInterval) {
      return false;
    }
    // check for transmission flags: if "transmit unmodified" is set, msg goes; if
    // "transmit" is set, check that the values have changed
    if (!has(CAN_MSG_FLAG_TRANSMIT_UNMODIFIED)) {
      if (!has(CAN_MSG_FLAG_TRANSMIT)) {
        return false;
      }
      if (memcmp(values, transmitValues, length) == 0) {
        return false;
      }
    }
    // if all the above tests passed, snapshot the data that will be transmitted
    transmitTimestamp = timestamp;
    memcpy(transmitValues, values, length);
    return true;
  }
};

CanMsg* canMsgs[CAN_BUS_COUNT][CAN_MSG_COUNT];

CanMsg* createOrGetMsg(uint8_t bus, uint16_t id) {
  CanMsg* msg = canMsgs[bus][id];
  if (msg == NULL) {
    msg = canMsgs[bus][id] = new CanMsg(bus, id);
  }
  return msg;
}

// Global state variables
uint8_t mode = MODE_RUN;
uint32_t lastActiveMillis = 0;
uint32_t lastPassiveMillis = 0;
uint32_t lastLedChangeMillis = 0;
bool dataLoggingEnabled = false;
bool dataLoggingFileCreated = false;
uint32_t dataLoggingLastFlushMillis = 0;

// Initialize can message array.
void initAllMsgs() {
  memset(canMsgs, 0, sizeof(canMsgs));
}

// Set the flags on the specific message on the specified bus.
void setMsgFlags(uint8_t bus, uint16_t id, uint8_t flags) {
  CanMsg* msg = createOrGetMsg(bus, id);
  msg->flags = flags;
}

// Set the specified flags to all (previously created) messages.
void setAllMsgFlags(uint8_t flags) {
  for (int id = 0; id < CAN_MSG_COUNT; id++) {
    for (int bus = 0; bus < CAN_BUS_COUNT; bus++) {
      CanMsg* msg = canMsgs[bus][id];
      if (msg != NULL) {
        msg->flags = flags;
      }
    }
  }
}

// Extract a uint16 value from a buffer (little endian).
uint32_t getUint16(const uint8_t* buf) {
  uint16_t v = buf[0] | ((uint16_t) buf[1]) << 8;
  return v;
}

// Transmit the message of the specified id, if it has a value (timestamp is not zero).
// Explicitly transmitting a msg doesn't trigger the transmission filters or affect
// future msg processing in any way.
bool transmitMsg(uint8_t bus, uint16_t id) {
  CanMsg* msg = canMsgs[bus][id];
  if (msg != NULL && msg->timestamp != 0) {
    uint8_t buf[CAN_MSG_MAX_SIZE];
    uint8_t size;
    msg->pack(buf, &size);
    SuperB.send(buf, size);
    return true;
  }
  return false;
}

// Transmit all messages that currently hold a value. This will take a long time, and
// is meant as a diagnostic tool only.
void transmitAllMsgs() {
  for (int id = 0; id < CAN_MSG_COUNT; id++) {
    for (int bus = 0; bus < 2; bus++) {
      if (transmitMsg(bus, id)) {
        delay(10);
      }
    }
  }
}

// Command processing. Each command has its own function, so this function only figures
// what the command it and dispatches it.
void onSuperB(const uint8_t* buf, size_t size) {
  LOG_D("SuperB msg, size: %d", size);
  uint8_t type = buf[SUPERB_TYPE_OFFSET];
  uint8_t id = buf[SUPERB_ID_OFFSET];
  uint8_t len = buf[SUPERB_LENGTH_OFFSET];
  if (len != size - SUPERB_DATA_OFFSET) {
    LOG_W("Invalid superb message, length mismatch, type: %d, id: %d, len: %d, size: %d", type, id, len, size);
    return;
  }
  const uint8_t* data = &buf[SUPERB_DATA_OFFSET];
  if (type == SUPERB_COMMAND) {
    onSuperBCommand(id, data);
  }
  else if (type == SUPERB_NOTIFICATION) {
    onSuperBNotification(id, data);
  }
}

void onSuperBCommand(uint8_t id, const uint8_t* data) {
  switch (id) {
    case CMDID_SET_ALL_MSG_FLAGS: {
      uint8_t flags = data[0];
      LOG_D("CMDID_SET_ALL_MSG_FLAGS, flags: %d", flags);
      setAllMsgFlags(flags);
      break;
    }

    case CMDID_SET_MSG_FLAGS: {
      uint16_t bus = data[0];
      uint16_t id = getUint16(&data[1]);
      uint8_t flags = data[3];
      LOG_D("CMDID_SET_MSG_FLAGS, bus: %d, id: %d, flags: %d", bus, id, flags);
      setMsgFlags(bus, id, flags);
      break;
    }

    case CMDID_GET_MSG_LAST_VALUE: {
      uint16_t bus = data[0];
      uint16_t id = getUint16(&data[1]);
      LOG_D("CMDID_GET_MSG_LAST_VALUE, bus: %d, id: %d", bus, id);
      transmitMsg(bus, id);
      break;
    }

    case CMDID_GET_ALL_MSG_LAST_VALUE: {
      LOG_D("CMDID_GET_ALL_MSG_LAST_VALUE");
      transmitAllMsgs();
      break;
    }

    default:
      LOG_W("Unknown superb command, id: %d", id);
  }
}

void onSuperBNotification(uint8_t id, const uint8_t* data) {
  switch (id) {

    case NOTIFY_WIFI_UP:
      LOG_I("WIFI UP");
      digitalWrite(LED_WIFI_UP, LOW);
      break;

    case NOTIFY_WIFI_DOWN:
      LOG_I("WIFI DOWN");
      digitalWrite(LED_WS_UP, HIGH);
      digitalWrite(LED_WIFI_UP, HIGH);
      break;

    case NOTIFY_WS_UP:
      LOG_I("WS UP");
      digitalWrite(LED_WS_UP, LOW);
      break;

    case NOTIFY_WS_DOWN:
      LOG_I("WS DOWN");
      digitalWrite(LED_WS_UP, HIGH);
      break;

    case NOTIFY_LATENCY: {
      uint16_t latency = getUint16(&data[0]);
      int8_t rssi = (int8_t) data[2];
      LOG_D("WS, latency: %d, RSSI: %d", latency, rssi);
      break;
    }

    case NOTIFY_BLE_CONNECTED:
      LOG_I("BLE CONNECTED");
      digitalWrite(LED_BLE_CONNECTED, LOW);
      break;

    case NOTIFY_BLE_DISCONNECTED:
      LOG_I("BLE DISCONNECTED");
      digitalWrite(LED_WS_UP, HIGH);
      digitalWrite(LED_BLE_CONNECTED, HIGH);
      break;

    default:
      LOG_W("Unknown superb notification: %d", id);
  }
}

// Mock interface processing.
void onMock(const uint8_t* buf, size_t size) {
  SuperB.send(buf, size);
}

// Message processing. If the can bus has a message available, we grab it and figure
// out what to do with it.
uint8_t pollCanBus(CANRaw& can, uint8_t bus, uint32_t now) {
  bool hasMessages = (can.available() > 0);
  if (!hasMessages) {
    return STATE_IDLE;
  }

  CAN_FRAME frame;
  can.read(frame);
  uint16_t id = (uint16_t) frame.id;
  uint8_t length = frame.length;

  // throw away any VIN info messages (privacy reasons)
  if (id == MSGID_VIN_INFO) {
    return STATE_PASSIVE;
  }

  // if we haven't initialized the data logging file and logging is enabled, and
  // this is the time msg, create a file using the timestamp as part of the filename
  if (dataLoggingEnabled && !dataLoggingFileCreated && id == MSGID_SYSTEM_TIME) {
    uint8_t* values = frame.data.bytes;
    uint8_t year = values[0];
    uint8_t month = values[1];
    uint8_t seconds = values[2];
    uint8_t hour = values[3];
    uint8_t day = values[4];
    uint8_t minutes = values[5];
    char filename[64];
    sprintf(filename, "can-%d-%d-%dT%d-%d-%d.dat", year, month, day, hour, minutes, seconds);
    if (FS.CreateNew(DATALOGGING_DIRECTORY, filename)) {
      dataLoggingFileCreated = true;
      dataLoggingLastFlushMillis = now;
    }
    else {
      // if the creation fails, data logging is disabled (which will happen eventually
      // as the card will fill up)
      dataLoggingEnabled = false;
    }
  }

  // verify that the frame is valid from our point of view
  // (we only support standard frames, and length is clamped)
  if (id < CAN_MSG_COUNT && length <= CAN_MSG_MAX_LENGTH) {

    // store the msg internally
    CanMsg* msg = createOrGetMsg(bus, id);
    msg->update(frame, now);

    // pack the message into a buffer
    uint8_t buf[CAN_MSG_MAX_SIZE];
    uint8_t size;
    msg->pack(buf, &size);

    // if the data logging file is ready, write this msg
    if (dataLoggingFileCreated) {
      FS.Write(reinterpret_cast<char *>(buf), size);
    }

    // if this message should be transmitted, send to the superb
    if (msg->transmit(now)) {
      SuperB.send(buf, size);
      return STATE_ACTIVE;
    }
  }

  return STATE_PASSIVE;
}

// Set the status using the led colours.
void setStateLedStatus(uint32_t led) {
  digitalWrite(LED_IDLE, HIGH);
  digitalWrite(LED_PASSIVE, HIGH);
  digitalWrite(LED_ACTIVE, HIGH);
  if (led != 0) {
    digitalWrite(led, LOW);
  }
}

// Main controller setup. Initialize the led we'll be using for activity,
// the BleLink and Tesla interfaces, and our internal buffers.
void setup() {
  // LEDs used for RUN mode
  pinMode(LED_IDLE, OUTPUT);
  pinMode(LED_PASSIVE, OUTPUT);
  pinMode(LED_ACTIVE, OUTPUT);
  pinMode(LED_WIFI_UP, OUTPUT);
  pinMode(LED_WS_UP, OUTPUT);
  pinMode(LED_BLE_CONNECTED, OUTPUT);
  digitalWrite(LED_WIFI_UP, HIGH);
  digitalWrite(LED_WS_UP, HIGH);
  digitalWrite(LED_BLE_CONNECTED, HIGH);

  // Mode switching buttons and leds
  pinMode(LED_SUPERB_RED, OUTPUT);
  pinMode(LED_SUPERB_YELLOW, OUTPUT);
  pinMode(Button1, INPUT);
  pinMode(Button2, INPUT);
  digitalWrite(LED_SUPERB_RED, HIGH);
  digitalWrite(LED_SUPERB_YELLOW, HIGH);

  // The USB port is used either as a mock interface that allows direct injection of
  // CAN message on the M2, or a channel to flash the SuperB.
  SerialUSB.begin(115200);
  Mock.setStream(&SerialUSB);
  Mock.setPacketHandler(&onMock);
  LOG_D("USB setup done");

  PRINT(" ---------------------------------------------");
  PRINT("| O N Y X  M 2                                ");
  PRINT("|                                             ");
  PRINT("| https://github.com/onyx-m2/onyx-m2-firmware ");
  PRINT("| Revision 8095668                            ");
  PRINT("| %s", __DATE__);
  PRINT(" ---------------------------------------------");

  // SuperB is reached through the Xbee serial interface, which uses COBS encoding
  Xbee.begin(115200);
  SuperB.setStream(&Xbee);
  SuperB.setPacketHandler(&onSuperB);
  LOG_D("SuperB setup done");

  // We connect to the Tesla Model 3 chassis and vehicle buses, read-only, and we'll
  // initialize all 7 receive mailboxes to accept any standard frame (TM3 doesn't
  // appear to use extended frames)
  VehCan.begin(CAN_BPS_500K);
  ChCan.begin(CAN_BPS_500K);
  for (int i = 0; i < 7; i++) {
    VehCan.setRXFilter(i, 0, 0, false);
    ChCan.setRXFilter(i, 0, 0, false);
  }
  LOG_D("CAN bus setup done");

  // Initialize our internal buffers
  initAllMsgs();
  LOG_D("Buffer initialization done");

  // Initialize the SD card interface for data logging. If a card is not present, or
  // not formatted correctly, the feature is disabled. We'll only create the file once
  // we receive the first timestamp from the car's can bus.
  if (SD.Init()) {
    dataLoggingEnabled = true;
  }

  // Turn on the red idle light to indicate we're good to go
  setStateLedStatus(LED_IDLE);
}

// Main controller loop. The buttons are checked (any press causes the mode to switch
// to MODE_SUPERB), and the appropriate mode handler is dispatched.
void loop() {
  if (mode == MODE_RUN) {
    runModeLoop();
    if (digitalRead(Button1) == LOW || digitalRead(Button2) == LOW) {
      mode = MODE_SUPERB;
      PRINT("Entering SuperB mode");
      PRINT("Hold BTN2 while pressing then releasing BTN1 to enter programming mode");
      pinMode(XBEE_RST, OUTPUT);
      pinMode(XBEE_MULT4, OUTPUT);
      digitalWrite(LED_IDLE, HIGH);
      digitalWrite(LED_BLE_CONNECTED, HIGH);
      digitalWrite(LED_WS_UP, HIGH);
      digitalWrite(LED_WIFI_UP, HIGH);
    }
  }
  else {
    superbModeLoop();
  }
}

// Main MODE_RUN loop. SuperB is updated to check for commands and notifications,
// and the leds are adjusted based on the state.
void runModeLoop() {
  SuperB.update();
  uint32_t now = millis();
  uint8_t vehState = pollCanBus(VehCan, VEH_BUS, now);
  uint8_t chState = pollCanBus(ChCan, CH_BUS, now);

  // update mock interface if usb is connected and no real can messages were received
  if (vehState == STATE_IDLE && chState == STATE_IDLE) {
    Mock.update();
  }

  // store the timing of the last active and passive states
  if (vehState == STATE_ACTIVE || chState == STATE_ACTIVE) {
    lastActiveMillis = now;
  }
  if (vehState == STATE_PASSIVE || chState == STATE_PASSIVE) {
    lastPassiveMillis = now;
  }

  // if the time interval since the last led change has expired, update the led
  // with the current state; if there were any active states during the interval,
  // the active led is show, if not passive states are checked, and if neither
  // of those occurred, the idle led is shown
  if (now - lastLedChangeMillis > LEDCFG_CHANGE_INTERVAL) {
    if (now - lastActiveMillis < LEDCFG_CHANGE_INTERVAL) {
      setStateLedStatus(LED_ACTIVE);
    }
    else if (now - lastPassiveMillis < LEDCFG_CHANGE_INTERVAL) {
      setStateLedStatus(LED_PASSIVE);
    }
    else {
      setStateLedStatus(LED_IDLE);
    }
    lastLedChangeMillis = now;
  }

  // the data logging file needs to be flushed periodically to make sure it is
  // in a coherent state when the power is turned off (not sure this will work as
  // is though, what if power is cut during a flush? maybe the can bus will stop just
  // prior to the power being turned off?)
  if (dataLoggingFileCreated &&
      (now - dataLoggingLastFlushMillis > DATALOGGING_FLUSH_INTERVAL)) {
    FS.Flush();
    dataLoggingLastFlushMillis = now;
  }
}

// Main MODE_SUPERB loop. In this mode, the USB port is wired to the SuperB directly so
// that the latter can be programmed directly if needed. This allows the SuperB to be
// debugged by allowing its logging messages to get to the USB serial monitor.
bool idleLedState = HIGH;
void superbModeLoop() {
  if (SerialUSB.available() > 0) {
    Serial.write(SerialUSB.read());
  }
  if (Serial.available() > 0) {
    SerialUSB.write(Serial.read());
  }

  int btn1 = digitalRead(Button1);
  if (btn1 == LOW) {
    digitalWrite(LED_SUPERB_RED, LOW);
    digitalWrite(XBEE_RST, LOW);
  } else {
    digitalWrite(LED_SUPERB_RED, HIGH);
    digitalWrite(XBEE_RST, HIGH);
  }

  int bnt2 = digitalRead(Button2);
  if (bnt2 == LOW) {
    digitalWrite(LED_SUPERB_YELLOW, LOW);
    digitalWrite(XBEE_MULT4, LOW);
  } else {
    digitalWrite(LED_SUPERB_YELLOW, HIGH);
    digitalWrite(XBEE_MULT4, HIGH);
  }

  uint32_t now = millis();
  if (now - lastLedChangeMillis > 1000) {
    lastLedChangeMillis = now;
    idleLedState = (idleLedState == HIGH) ? LOW : HIGH;
    digitalWrite(LED_IDLE, idleLedState);
  }
}
