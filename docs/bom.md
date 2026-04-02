# Bill of Materials — IoT Environmental Monitoring Gateway

**Date:** April 2026

---

## Already Owned

| Item                          | Qty | Notes                                    |
|-------------------------------|-----|------------------------------------------|
| STM32F469 Discovery           | 1   | Field device (Node 1)                    |
| B-L475E-IOT01A                | 1   | Gateway (Node 2)                         |

---

## Purchased

### RS-485 Bus

| Item                                  | Qty | Source    | Notes                                                        |
|---------------------------------------|-----|-----------|--------------------------------------------------------------|
| TTL-to-RS-485 module (3.3 V / 5 V)   | 3   | Amazon.de | One per board, one spare. 3.3 V compatible (verified). Has onboard 120 Ω termination resistor (enable by bridging R16). ESD protection ±15 kV. Max 500 kbps. |
| USB-to-RS-485 converter (CH340-based) | 1   | Amazon.de | JZK brand. For sniffing Modbus traffic from PC. Compatible with Windows, Linux, Mac OS. |

### Wiring

| Item                                  | Qty    | Source | Notes                                                       |
|---------------------------------------|--------|--------|-------------------------------------------------------------|
| Dupont jumper wires (FF/MF/MM assorted) | 1 set | eBay  | NetElectroShop. For connecting board headers to RS-485 modules and general wiring. |

### Debugging

| Item                                  | Qty | Source    | Notes                                                        |
|---------------------------------------|-----|-----------|--------------------------------------------------------------|
| KeeYees USB Logic Analyser (8-ch, 24 MHz) | 1 | Amazon.de | Includes 12-piece test hook clip set. For debugging UART timing, RS-485 DE/RE control, Modbus frames. Use with PulseView (free, open source). |

---

## Still Needed

| Item                                  | Qty    | Est. Price | Notes                                                       |
|---------------------------------------|--------|------------|-------------------------------------------------------------|
| Twisted pair cable or Cat5 segment    | ~0.5 m | €1–3       | RS-485 bus between the two transceiver modules.              |
| Small breadboard                      | 1      | €3–5       | For connecting RS-485 modules and termination resistors.     |

---

## Check You Have

| Item                                  | Qty | Notes                                                         |
|---------------------------------------|-----|---------------------------------------------------------------|
| Micro-USB cables                      | 2–3 | Both boards use micro-USB for ST-Link + power.                |
| USB hub (powered preferred)           | 1   | 4–5 USB devices connected at once (2 ST-Links, dongle, analyser). |
| Multimeter                            | 1   | Checking 3.3 V lines, continuity, RS-485 bias voltages.      |

---

*UART pin assignments will be added during the LLD phase.*
