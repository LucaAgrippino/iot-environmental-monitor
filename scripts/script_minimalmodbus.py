import time
import minimalmodbus

# --- Configuration ---
PORT_NAME = 'COM8'  # Change to 'COM3', 'COM11', etc. for Windows
BAUDRATE = 9600

# --- ANSI Terminal Color Codes ---
GREEN = "\033[1;32m"
RED = "\033[1;31m"
YELLOW = "\033[1;33m"
RESET = "\033[0m"  # Resets terminal back to white text

def run_master_test():
    print(f"{YELLOW}=== Starting Modbus Master Test ==={RESET}\n")

    # Initialize the instrument for Slave ID 1 (Phases 1 & 2)
    slave1 = minimalmodbus.Instrument(PORT_NAME, slaveaddress=1)
    slave1.serial.baudrate = BAUDRATE
    slave1.serial.timeout = 0.5  # Master will wait 0.5 seconds before timeout
    slave1.debug = True          # Shows raw hex frames in yellow/white

    # ==========================================
    # Phase 1 — FC03 read holding register
    # ==========================================
    print(f"{YELLOW}--- Phase 1: FC03 Read Holding Register ---{RESET}")
    try:
        # Read 1 register from address 0x0000
        response_val = slave1.read_register(registeraddress=0, number_of_decimals=0, functioncode=3)
        
        print(f"{GREEN}[SUCCESS] PC master log: received response with value: {hex(response_val)} (Decimal: {response_val}){RESET}")
        if response_val == 0x1234:
            print(f"{GREEN}         -> Value matches expected 0x1234!{RESET}")
    except Exception as e:
        print(f"{RED}[FAIL] Phase 1 failed: {e}{RESET}")

    print("\n" + "="*40 + "\n")
    time.sleep(1)

    # ==========================================
    # Phase 2 — FC06 write single register
    # ==========================================
    print(f"{YELLOW}--- Phase 2: FC06 Write Single Register ---{RESET}")
    try:
        # Write value 0x1234 to register address 0x0000
        slave1.write_register(registeraddress=0, value=0x1234, number_of_decimals=0, functioncode=6)
        
        print(f"{GREEN}[SUCCESS] PC master log: ACK echo response received successfully.{RESET}")
    except Exception as e:
        print(f"{RED}[FAIL] Phase 2 failed: {e}{RESET}")

    print("\n" + "="*40 + "\n")
    time.sleep(1)

    # ==========================================
    # Phase 3 — address mismatch (frame to slave 2)
    # ==========================================
    print(f"{YELLOW}--- Phase 3: Address Mismatch (Slave 2) ---{RESET}")
    
    slave2 = minimalmodbus.Instrument(PORT_NAME, slaveaddress=2)
    slave2.serial.baudrate = BAUDRATE
    slave2.serial.timeout = 0.5  # Waits 0.5 seconds for the silent drop test
    slave2.debug = True

    try:
        print(f"{YELLOW}Sending request to Slave 2. Expecting no response...{RESET}")
        slave2.read_register(registeraddress=0, number_of_decimals=0, functioncode=3)
        
        print(f"{RED}[FAIL] Unexpected behavior: Slave 2 actually answered!{RESET}")
    except minimalmodbus.NoResponseError:
        print(f"{GREEN}[SUCCESS] PC master log: Timeout caught. Slave 1 stayed silent as expected.{RESET}")
    except Exception as e:
        print(f"{YELLOW}[INFO] Caught other exception (expected timeout): {e}{RESET}")

    print(f"\n{YELLOW}=== Test Sequence Finished ==={RESET}")

if __name__ == '__main__':
    run_master_test()