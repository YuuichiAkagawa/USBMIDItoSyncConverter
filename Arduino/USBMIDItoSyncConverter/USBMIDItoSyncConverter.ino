/*
 * USB MIDI to Sync Converter for XIAO RP2040
 * Copyright (C) 2024 Yuuichi Akagawa
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of 
 * the GNU General Public License as published by the Free Software Foundation, either version 3 
 * of the License, or any later version.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. 
 * If not, see <https://www.gnu.org/licenses/>. 
 *
*/

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <EEPROM.h>

#define DEBUG
/* On-board LED definitions */
#define USER_LED_R 17  //RX_LED
#define USER_LED_G 16  //TX_LED
#define USER_LED_B 25

/* Pin definitions */
#define SYNCPIN D0
#define MODEPIN D1
#define DINSYNCRUNPIN D2
#define MODE0PIN D5
#define MODE1PIN D4
#define MODE2PIN D3
#define MODE0BIT 2
#define MODE1BIT 1
#define MODE2BIT 0

/* PPQ (default: 2PPQ)*/
#define CLOCK_BASE (24)
#define DEFAULT_PPQN (2)

/* Sync Pulse Width (SPW) */
#define SPW_5MS (1)
#define SPW_15MS (2)
#define SPW_RESERVED (3)
#define DEFAULT_SPW SPW_5MS

/* EEPROM address*/
#define EEPROM_ADDRESS_PPQN 0x00
#define EEPROM_ADDRESS_SPW 0x01

/* MIDI filter */
#define MIDI_FILTER_NONE (3)
#define MIDI_FILTER_TRANSPORT (2)
#define MIDI_FILTER_CLOCKONLY (1)

/* MIDI device ID for SysEx */
#define SYSEX_DEVICE_ID (0x49)

//Global valiables
uint32_t g_ppqn = DEFAULT_PPQN;  // current PPQN
uint32_t g_clock_period;         // Period of the MIDI Clock that fires the Sync signal
int32_t g_spw = -5;              // current Sync pluse width
uint32_t g_count_clock = 0;      // Number of MIDI Clocks received
bool g_is_start = false;         // Is MIDI Start received
uint32_t g_midi_filter = MIDI_FILTER_NONE;
uint32_t g_dipsw_mode_value = 0;

//timer
static repeating_timer_t timer;

// USB MIDI object
Adafruit_USBD_MIDI usb_midi;

// Create a new instance of the Arduino MIDI Library, and attach usb_midi as the transport.
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, USBMIDI);
// Create a new instance of the Arduino MIDI Library, and attach Serial1 as the transport.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

bool timer_callback(repeating_timer_t *rt);
void startSync(void);
void handleClock(void);
void handleStart(void);
void handleStop(void);
void handleContinue(void);
void handleSysEx(byte *array, unsigned size);
uint8_t setPPQ(uint8_t c, uint8_t ppqn);
void setSPW(uint8_t c);
uint32_t getModeValue(void);
void applyModeValue(uint32_t sw);
bool isFilterd(uint32_t sw, byte type);

// Timer handler (one shot)
bool __not_in_flash_func(timer_callback)(repeating_timer_t *rt) {
  digitalWrite(SYNCPIN, LOW);
#ifdef DEBUG
  digitalWrite(LED_BUILTIN, HIGH);
#endif
  return false;
}

/**
 * Start Sync signal
 */
void __not_in_flash_func(startSync)() {
  g_count_clock = 0;
  digitalWrite(SYNCPIN, HIGH);  // Rise SYNC pin
#ifdef DEBUG
  digitalWrite(LED_BUILTIN, LOW);
#endif
  add_repeating_timer_ms(g_spw, &timer_callback, NULL, &timer);  // Start Timer
}

/**
 * MIDI message handlers
 */

// MIDI Clock
void __not_in_flash_func(handleClock)() {
  if (digitalRead(MODEPIN) == LOW) {
    if (g_is_start == false) {
      return;
    }
  }
  /* Count the number of timing clocks */
  g_count_clock++;
  if (g_count_clock >= g_clock_period) {
    startSync();
  }
}

// MIDI Start
void __not_in_flash_func(handleStart)() {
  g_is_start = true;
  g_count_clock = g_clock_period - 1;
  //for DIN SYNC
  digitalWrite(DINSYNCRUNPIN, HIGH);
}

// MIDI Stop
void __not_in_flash_func(handleStop)() {
  g_is_start = false;
  //for DIN SYNC
  digitalWrite(DINSYNCRUNPIN, LOW);
}

// MIDI Continue
void __not_in_flash_func(handleContinue)() {
  g_is_start = true;
  //for DIN SYNC
  digitalWrite(DINSYNCRUNPIN, HIGH);
}

// MIDI System exclusive
void __not_in_flash_func(handleSysEx)(byte *array, unsigned size) {
  /* Processing SysEx                                     */
  /*   Acceptable Messages                                */
  /*     device ID:49                                     */
  /*      F0 7E 49 0B 02 nn F7   : Change PPQN/SPW        */
  /*         PPQN nn = 0x01,0x02,0x03,0x04,0x08,0x0c,0x18 */
  /*         SPW  nn = 0x3F:5ms, 0x5F:15ms, 0x7F:Reserved */
  if (array[0] != 0xF0) return;             // SysEx start
  if (array[1] != 0x7e) return;             // non realtime universal system exclusive message
  if (array[2] != SYSEX_DEVICE_ID) return;  // device ID
  if (array[3] != 0x0B) return;             // File Reference Message
  if (array[4] != 0x02) return;             // change
  if (array[6] != 0xf7) return;             // SysEx end
  byte c = array[5];
  //Change PPQN
  g_ppqn = setPPQ(c, g_ppqn);
  g_clock_period = (CLOCK_BASE / g_ppqn);
  //Change SPW
  setSPW(c);
}

/**
 * Set and Save PPQN
 * c: PPQN : 1 to 24
 *   bit|7|6|5|4|3|2|1|0|
 *      |0|0|0|PPQN 0-24|
 */
uint8_t setPPQ(uint8_t c, uint8_t ppqn) {
  uint8_t newPpqn = ppqn;

  // bit 6,5
  // 00 : PPQN mode, 01-11: SPW mode
  if ((c & 0x60) != 0) {
    return newPpqn;
  }

  //Extracting PPQN value
  c = c & 0x1f;
  if (0 < c && c <= 24) {  // Valid range is 1 to 24
    newPpqn = c;
    EEPROM.write(EEPROM_ADDRESS_PPQN, newPpqn);
    EEPROM.commit();
  }
  return newPpqn;
}

/**
 * Set and Save clock pluse width
 * c: SPW | bit 6,5 = 00: PPQN mode, 01: SPW 5ms, 10: SPW 15ms, 11: Reserved
 *   bit|7|6|5|4|3|2|1|0|
 *      |0|X|X|1|1|1|1|1|
 */
void setSPW(uint8_t c) {
  // Bits 0 through 4 must all be 1
  if ((c & 0x1f) != 0x1f) {
    return;
  }

  //Extracting SPW value
  uint8_t new_spw = (c >> 5) & 3;
  if (new_spw == 0) {
    return;
  }
  //Save SPW value
  EEPROM.write(EEPROM_ADDRESS_SPW, new_spw);
  EEPROM.commit();
  //set SPW
  switch (new_spw) {
    case SPW_5MS:
      g_spw = -5;
      break;
    case SPW_15MS:
      g_spw = -15;
      break;
    default:
      g_spw = -5;
      break;
  }
}

/**
   * Read mode value from DIP Switch
   *     _____
   *  - | o   |- MODE0
   *  - | o   |- MODE1
   *  - | o   |- MODE2
   *  - | o   |- DIN S
   *     -----
   *    off = high, on = low
   *
   * [MODE0] 
   *   0  : PPQ values ​​are changed using DIP Switch. The value stored in the EEPROM is ignored.
   *    PPQ table
   *    [MODE1] [MODE2]
   *      1       1     : 1 PPQ    (3)
   *      1       0     : 2 PPQ    (2)
   *      0       1     : 4 PPQ    (1)
   *      0       0     : 24 PPQ   (0)
   *   1 : PPQ values ​​are changed using SysEx messages. Settings are saved in EEPROM.
   *          MIDI message filter function is enabled.
   *    [MODE1] [MODE2]
   *      1       1     : All messages are forwarded from USB to serial.
   *      1       0     : Only Clock, Start, Stop, Continue are fowarded.
   *      0       1     : Foward only clocks
   *      0       0     : N/A (Reserved)
   *
   */
uint32_t __not_in_flash_func(getModeValue)() {
  uint32_t mode0 = (uint32_t)digitalRead(MODE0PIN);
  uint32_t mode1 = (uint32_t)digitalRead(MODE1PIN);
  uint32_t mode2 = (uint32_t)digitalRead(MODE2PIN);
  uint32_t sw = (mode0 << MODE0BIT) + (mode1 << MODE1BIT) + (mode2 << MODE2BIT);
  return sw;
}

/**
 * Apply mode switch value
 */
void __not_in_flash_func(applyModeValue)(uint32_t sw) {
  if ((sw & (1 << MODE0BIT)) == 0) {  // MODE0 On
    uint32_t temp_ppqn;
    switch (sw & 0x03) {
      case 0:
        temp_ppqn = 24;
        break;
      case 1:
        temp_ppqn = 4;
        break;
      case 2:
        temp_ppqn = 2;
        break;
      case 3:
        temp_ppqn = 1;
        break;
      default:
        temp_ppqn = g_ppqn;
        break;
    }
    g_ppqn = temp_ppqn;
    g_clock_period = (CLOCK_BASE / g_ppqn);
  } else {  // MODE0 Off
    g_midi_filter = sw & 0x03;
  }
}

/**
 * MIDI send filter
 */
bool __not_in_flash_func(isFilterd)(uint32_t sw, byte type) {
  if ((sw & (1 << MODE0BIT)) == 0) {  // MODE0 On
    return false;
  }
  if (g_midi_filter == MIDI_FILTER_NONE) {
    return false;
  }
  if (g_midi_filter == MIDI_FILTER_TRANSPORT) {
    switch (type) {
      case 0xf8:  //clock
      case 0xfa:  //start
      case 0xfb:  //continue
      case 0xfc:  //stop
      case 0xfe:  //active sensing
      case 0xff:  //reset
        return false;
      default:
        return true;
    }
  }
  if (g_midi_filter == MIDI_FILTER_CLOCKONLY) {
    switch (type) {
      case 0xf8:  //clock
      case 0xfe:  //active sensing
      case 0xff:  //reset
        return false;
      default:
        return true;
    }
  }
  return false;
}
/**
 * setup for core0
 */
void __not_in_flash_func(setup)() {
  pinMode(SYNCPIN, OUTPUT);
  pinMode(MODEPIN, INPUT_PULLUP);
  pinMode(DINSYNCRUNPIN, OUTPUT);

  // DIP Switch
  pinMode(MODE0PIN, INPUT_PULLUP);
  pinMode(MODE1PIN, INPUT_PULLUP);
  pinMode(MODE2PIN, INPUT_PULLUP);

  // LED OFF
  pinMode(USER_LED_R, OUTPUT);
  pinMode(USER_LED_G, OUTPUT);
  pinMode(USER_LED_B, OUTPUT);
  digitalWrite(USER_LED_R, HIGH);
  digitalWrite(USER_LED_G, HIGH);
  digitalWrite(USER_LED_B, HIGH);

  /* EEPROM init & PPQN setting */
  uint8_t temp_ppqn = EEPROM.read(EEPROM_ADDRESS_PPQN);
  if (temp_ppqn == 0xff || temp_ppqn == 0) {  //initial value is 0
    g_ppqn = DEFAULT_PPQN;
    EEPROM.write(EEPROM_ADDRESS_PPQN, g_ppqn);
    EEPROM.commit();
  } else {
    g_ppqn = temp_ppqn;
  }
  g_clock_period = (CLOCK_BASE / g_ppqn);

  /* EEPROM init & SPW setting */
  uint8_t temp_spw = EEPROM.read(EEPROM_ADDRESS_SPW);
  if (temp_spw == 0xff || temp_spw == 0) {  //initial value is 0
    temp_spw = DEFAULT_SPW;
    EEPROM.write(EEPROM_ADDRESS_SPW, DEFAULT_SPW);
    EEPROM.commit();
  }

  // Read DIP Switch value
  g_dipsw_mode_value = getModeValue();
  applyModeValue(g_dipsw_mode_value);

  TinyUSB_Device_Init(0);
  TinyUSBDevice.clearConfiguration();

  // This VID and PID were assigned from pid.codes.
  TinyUSBDevice.setID(0x1209, 0x3249);
  TinyUSBDevice.setManufacturerDescriptor("ammlab.org");
  TinyUSBDevice.setProductDescriptor("USBMIDI2Sync converter");

  // Initialize USB MIDI
  USBMIDI.begin(MIDI_CHANNEL_OMNI);
  USBMIDI.turnThruOff();

  // Initialize Serial MIDI
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  // Attach the handler function for messages received via USB MIDI
  USBMIDI.setHandleClock(handleClock);
  USBMIDI.setHandleStart(handleStart);
  USBMIDI.setHandleStop(handleStop);
  USBMIDI.setHandleContinue(handleContinue);
  USBMIDI.setHandleSystemExclusive(handleSysEx);

  // wait until device mounted
  while (!TinyUSBDevice.mounted()) delay(1);
}

/**
 * loop for core0
 */
void __not_in_flash_func(loop)() {
  // Note: SysEx message is not forward
  // read any new MIDI messages from USB
  if (USBMIDI.read()) {
    //workaround for SEQTRAK
    if (USBMIDI.getType() != 0xff && USBMIDI.getData1() != 0xff) {
      //forward to Serial
      if (isFilterd(g_dipsw_mode_value, USBMIDI.getType()) == false) {
        MIDI.send(USBMIDI.getType(),
                  USBMIDI.getData1(),
                  USBMIDI.getData2(),
                  USBMIDI.getChannel());
      }
    }
  }

  // read any new MIDI messages from Serial
  if (MIDI.read()) {
    //forward to USB
    USBMIDI.send(MIDI.getType(),
                 MIDI.getData1(),
                 MIDI.getData2(),
                 MIDI.getChannel());
  }

  //read dip switch
  uint32_t mode_sw = getModeValue();
  if (g_dipsw_mode_value != mode_sw) {
    // apply DIP switch value when changed
    g_dipsw_mode_value = mode_sw;
    applyModeValue(g_dipsw_mode_value);
  }
}