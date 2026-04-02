# Hardware Setup Guide — IoT Environmental Monitoring Gateway

**Date:** April 2026

---

## RS-485 Module Specifications

The purchased TTL-to-RS-485 modules have the following relevant characteristics:

- **Voltage:** 3.3 V and 5 V compatible (both STM32 boards run at 3.3 V — confirmed safe).
- **Max data rate:** 500 kbps (Modbus RTU at 9600–115200 baud is well within this).
- **Bus capacity:** Up to 128 devices (1/4 unit load receiver impedance).
- **ESD protection:** ±15 kV with dual transient suppression diodes.
- **Termination:** Onboard 120 Ω resistor — enable by bridging (shorting) the R16 pads on the module PCB.
- **Indicators:** Power, RXD, and TXD LEDs for monitoring bus activity.
- **Connectors:** Screw terminals and solder pads available.

---

## RS-485 Bus Wiring

```
STM32F469          TTL-RS485 Module #1       TTL-RS485 Module #2       B-L475E-IOT01A
-----------        ----------------          ----------------          ----------------
UART TX  --------> DI                                                  
UART RX  <-------- RO                                                  
GPIO     --------> DE/RE                                               
3.3V     --------> VCC                                                 
GND      --------> GND                                                 

                   A  <----twisted pair----->  A
                   B  <----twisted pair----->  B
                   GND <--------GND--------->  GND

                                              RO  --------> UART RX
                                              DI  <-------- UART TX
                                              DE/RE <------ GPIO
                                              VCC  <------- 3.3V
                                              GND  <------- GND

Termination: Bridge R16 on both Module #1 and Module #2 to enable
             the onboard 120 Ω termination resistors.
```

### Wiring Notes

- Use a twisted pair (one pair from a Cat5 ethernet cable works well) for the A/B differential lines between the two RS-485 modules.
- Connect GND between the two modules — RS-485 requires a common ground reference.
- The DE (Driver Enable) and RE (Receiver Enable) pins are active-high and active-low respectively. On most breakout modules they are tied together, so a single GPIO controls both: HIGH = transmit, LOW = receive.
- Use the Dupont jumper wires for all connections between board headers and RS-485 module pins.
- UART pin assignments for each board are determined during the LLD phase.

### Bus Termination

RS-485 requires 120 Ω termination at each end of the bus to prevent signal reflections. These modules have the termination resistor built in — bridge (short) the R16 pads on the PCB of both end-of-bus modules to enable it. No external resistors are needed.

For the short cable lengths in this project (~0.5 m), termination is not strictly necessary at these baud rates, but enabling it is correct practice and demonstrates knowledge of the RS-485 specification.

---

## Debugging Setup

### Logic Analyser (KeeYees 8-ch, 24 MHz)

The logic analyser is used for debugging UART timing, RS-485 DE/RE pin control, and Modbus frame boundaries.

**Software:** PulseView (free, open source) — download from [sigrok.org](https://sigrok.org/wiki/PulseView). PulseView includes protocol decoders for UART and Modbus RTU.

**Typical probe connections for Modbus debugging:**
- CH0: UART TX (from STM32 to RS-485 module DI pin)
- CH1: UART RX (from RS-485 module RO pin to STM32)
- CH2: DE/RE control GPIO
- CH3: RS-485 A line (differential, for timing reference)
- GND: Common ground

Use the included test hook clips for connecting to module pins without soldering.

### USB-to-RS-485 Dongle (JZK, CH340-based)

The dongle connects to the RS-485 bus from a PC, allowing you to sniff live Modbus traffic or act as a Modbus master/slave for testing.

**Driver:** CH340 — may require manual driver installation on some systems. Download from the manufacturer if not auto-detected.

**Usage:** Connect the dongle's A/B terminals to the RS-485 bus alongside the two STM32 modules. Use a Modbus diagnostic tool (e.g., QModMaster on Windows, mbpoll on Linux) to send test frames and verify communication.

---

## Supplier Recommendations (Ireland)

For the remaining items (twisted pair cable, breadboard):

| Supplier           | URL                        | Shipping to Limerick | Best For                              |
|--------------------|----------------------------|----------------------|---------------------------------------|
| Mouser Electronics | mouser.ie                  | Free over €50        | Professional parts, datasheets, fast  |
| Farnell / element14| ie.farnell.com             | Free over €30        | Professional parts, good stock        |
| RS Components      | ie.rs-online.com           | Next-day available   | Industrial, fast delivery             |
| Amazon.de          | amazon.de                  | 3–5 days to Ireland  | Cheap modules, jumper wire kits       |

These are commodity items — any supplier or local electronics shop will have them. A short piece of Cat5 ethernet cable can often be found lying around.

---

*This guide will be updated with specific UART pin assignments and photographs of the physical setup during the LLD and implementation phases.*
