print("--- INITIALIZING SATELIOT AUTOMATION (System Mode Check) ---")

import sys
import time
import os
import serial
import serial.tools.list_ports
import requests
import warnings
from datetime import datetime, timedelta, timezone
from skyfield.api import Topos, load, EarthSatellite

# Suppress SSL warnings
from urllib3.exceptions import InsecureRequestWarning
warnings.simplefilter('ignore', InsecureRequestWarning)

# --- CONFIGURATION ---
BAUD_RATE = 115200
TARGET_PLMN = "90197" 
TARGET_BAND = "256"
MIN_ELEVATION = 40.0 
CHAIN_THRESHOLD_MINS = 15.0 
TARGET_VID = "1366" 

# UDP CONFIG (SLM Firmware)
UDP_IP = "23.88.115.159"
UDP_PORT = "41234"
UDP_MSG = "JJM_LEO_test!"

# Real NORAD IDs
SATELLITES_IDS = {
    'SATELIOT_1': 60550,
    'SATELIOT_2': 60534,
    'SATELIOT_3': 60552,
    'SATELIOT_4': 60537
}

OBSERVER = Topos(latitude_degrees=63.421628, longitude_degrees=10.437495) # Kangasala 61.3987, 24.1792# Tyholt 63.421628, 10.437495

# HARDCODED BACKUP
BACKUP_TLES = {
    'SATELIOT_1': ('1 60550U 24149AB  25342.12345678  .00001234  00000-0  12345-3 0  9991', '2 60550  97.5000 100.1234 0005000  50.1234 300.1234 15.10000000  1001'),
    'SATELIOT_2': ('1 60534U 24149K   25342.23456789  .00002345  00000-0  23456-3 0  9992', '2 60534  97.6000 150.2345 0004000 100.2345 250.2345 15.05000000  2002'),
    'SATELIOT_3': ('1 60552U 24149AD  25342.34567890  .00003456  00000-0  34567-3 0  9993', '2 60552  97.7000 200.3456 0003000 150.3456 200.3456 15.00000000  3003'),
    'SATELIOT_4': ('1 60537U 24149BX  25342.64289398  .00003833  00000-0  34912-3 0  9995', '2 60537  97.6950  55.6667 0001401   3.3048 356.8182 14.95733229 17486')
}

# --- 1. DATA LOADING ---
def get_tles(ts):
    print("\n--- REFRESHING ORBITAL DATA ---")
    constellation = {}
    '''
    NORAD is a joint U.S.-Canadian organization that tracks artificial objects in Earth's orbit
    Each satellite is assigned a unique NORAD ID (also called a catalog number)
    '''
    for name, norad_id in SATELLITES_IDS.items():
        try:
            url = f"https://celestrak.org/NORAD/elements/gp.php?CATNR={norad_id}&FORMAT=TLE"
            response = requests.get(url, timeout=5, verify=False)
            if response.status_code == 200:
                lines = response.text.strip().splitlines()
                if len(lines) >= 3:
                    sat = EarthSatellite(lines[1].strip(), lines[2].strip(), name, ts)
                    constellation[name] = sat
                    print(f" -> {name}: Updated.")
        except: pass

    if not constellation:
        print("\n[!] USING SIMULATED BACKUP DATA.")
        for name, (l1, l2) in BACKUP_TLES.items():
            constellation[name] = EarthSatellite(l1, l2, name, ts)
    
    return constellation

# --- 2. HARDWARE DETECTION ---
def find_modem():
    print("\n--- HARDWARE DISCOVERY ---")
    ports = serial.tools.list_ports.comports()
    candidates = [p.device for p in ports if TARGET_VID in p.hwid]
    
    if not candidates:
        print("[!] No Nordic devices found.")
        return None

    for port in candidates:
        try:
            print(f" -> Testing {port}...", end="")
            with serial.Serial(port, BAUD_RATE, timeout=1) as ser:
                ser.reset_input_buffer()
                ser.write(b"AT\r\n")
                time.sleep(0.2)
                if "OK" in ser.read_all().decode(errors='ignore'):
                    print(" SUCCESS!")
                    return port
        except: pass
    return None

# --- MAIN LOOP ---
def main():
    active_port = find_modem()
    if not active_port:
        input("\nHardware check failed. Press Enter to exit...")
        sys.exit()

    ts = load.timescale()
    
    while True:
        '''
        TLE: Two line element set
        TLE is a data format used to encode information about a satellite's orbit around Earth
        It consists of two lines of 69 characters each that contain:
            - Satellite identification number
            - Orbital inclination
            - Orbital eccentricity
            - Time of orbital pass
            - Other orbital parameters
        '''
        constellation = get_tles(ts)
        
        print("\n" + "="*60)
        print(f"   CALCULATING CHAINED SCHEDULE (> {MIN_ELEVATION}°)")
        print("="*60)

        t0 = ts.now()
        t1 = ts.from_datetime(t0.utc_datetime() + timedelta(hours=48))
        raw_passes = []

        # FIND RISE & SET
        '''
        This code block calculates when each satellite becomes visible and disappears from the observer's location.
        
        '''
        for name, sat in constellation.items():
            times, events = sat.find_events(OBSERVER, t0, t1, altitude_degrees=MIN_ELEVATION)
            current_rise = None
            for ti, event in zip(times, events):
                if event == 0: current_rise = ti
                elif event == 2 and current_rise is not None:
                    raw_passes.append({'name': name, 'rise': current_rise.utc_datetime(), 'set': ti.utc_datetime()})
                    current_rise = None

        if not raw_passes:
            print(f"No passes > {MIN_ELEVATION}° found. Sleeping 1 hour...")
            time.sleep(3600)
            continue

        raw_passes.sort(key=lambda x: x['rise'])
        
        # CHAIN PASSES
        chain = [raw_passes[0]]
        for next_p in raw_passes[1:]:
            prev_p = chain[-1]
            delta_mins = (next_p['rise'] - prev_p['rise']).total_seconds() / 60.0
            if delta_mins < CHAIN_THRESHOLD_MINS: chain.append(next_p)
            else: break

        first_sat = chain[0]
        last_sat = chain[-1]
        start_time = first_sat['rise']
        end_time = last_sat['set']
        target_names = " + ".join([p['name'] for p in chain])
        
        print(f"\nUPCOMING SCHEDULE:")
        for p in raw_passes[:4]:
            print(f"{p['name']:<15} | {p['rise'].strftime('%H:%M:%S')}   | {p['set'].strftime('%H:%M:%S')}")

        print(f"\n*** TARGET CHAIN: {target_names} ***")
        print(f"Start: {start_time.strftime('%Y-%m-%d %H:%M:%S')} UTC")

        # WAIT LOOP
        wake_time = start_time - timedelta(seconds=30)
        while True:
            now = datetime.now(timezone.utc)
            if now >= wake_time: break
            remaining = str(wake_time - now).split('.')[0]
            print(f"Status: Sleeping... T-Minus {remaining}   ", end="\r")
            time.sleep(1)

        # EXECUTION
        print(f"\n\n--- CHAIN STARTED! EXECUTING SEQUENCE ---")
        
        try:
            with serial.Serial(active_port, BAUD_RATE, timeout=3) as ser:
                
                def send(cmd, delay=0.5):
                    print(f"[PC] -> {cmd}")
                    ser.write((cmd + "\r\n").encode())
                    time.sleep(delay)
                    return_lines = []
                    while ser.in_waiting:
                        line = ser.readline().decode(errors='ignore').strip()
                        if line: 
                            print(f"[nRF] <- {line}")
                            return_lines.append(line)
                    return return_lines

                # 1. BASIC CHECKS
                send("AT")
                send("AT+CGMR")
                
                # --- SYSTEM MODE CHECK ---
                print("\n[PC] -> CHECKING SYSTEM MODE...")
                ser.write(b"AT%XSYSTEMMODE?\r\n")
                time.sleep(0.5)
                
                mode_response = ""
                while ser.in_waiting:
                    line = ser.readline().decode(errors='ignore').strip()
                    if line: 
                        print(f"[nRF] <- {line}")
                        mode_response += line

                # Target: NTN enabled (5th parameter is 1)
                # Response format: %XSYSTEMMODE: <LTE_M>,<NB_IOT>,<GPS>,<LTE_M_PREF>,<NTN>
                if "0,0,0,0,1" in mode_response:
                    print("[OK] System Mode is correct (NTN Only).")
                else:
                    print("[!] System Mode Mismatch. Reconfiguring...")
                    send("AT+CFUN=0", 1) # Radio must be OFF to change mode
                    send("AT%XSYSTEMMODE=0,0,0,0,1")
                    print("[OK] NTN Mode Applied.")

                # 2. CONFIGURATION
                send("AT+CFUN=0", 1) # Ensure radio off for config
                send('AT%CHSELECT=2,14,229236')
                send('AT%PERIODICSEARCHCONF=0,1,0,0,"1,2"')
                send(f'AT%XBANDLOCK=2,,"{TARGET_BAND}"')
                send('AT+CEREG=5') 
                send('AT+CSCON=3')
                send('AT%CESQ=1')
                send('AT%MDMEV=2')
                send('AT+CNEC=24')
                send('AT+CGEREP=1')
                send('AT+CIND=1,1,1')
                send('AT%LOCATION=2,"61.39877357436579","24.17924285556194","140",0,0')
                
                # 3. ACTIVATE
                print("\n--- RADIO ON ---")
                send(f'AT+COPS=1,2,"{TARGET_PLMN}"', 0.5)
                send('AT+CFUN=1', 1)
                
                # 4. MONITOR & SEND DATA
                data_sent = False
                print(f"\n--- MONITORING UNTIL {end_time.strftime('%H:%M:%S')} ---")
                
                while datetime.now(timezone.utc) < end_time:
                    if ser.in_waiting:
                        try:
                            line = ser.readline().decode(errors='ignore').strip()
                            if line:
                                print(f"[LOG] {line}")
                                
                                # CHECK FOR REGISTRATION
                                if not data_sent and ("+CEREG: 1" in line or "+CEREG: 5" in line):
                                    print("\n[!!!] CONNECTION DETECTED! UPLOADING DATA [!!!]")
                                    
                                    send('AT#XSOCKET=1,2,0', 1)
                                    send(f'AT#XCONNECT=0,"{UDP_IP}",{UDP_PORT}', 1)
                                    send(f'AT#XSEND=0,0,512,"{UDP_MSG}"', 2)
                                    
                                    print("[!!!] UPLOAD COMPLETE.\n")
                                    data_sent = True 
                        except Exception: pass
                    time.sleep(0.05)

                # 5. CLEANUP
                print("\n--- CHAIN COMPLETE. RADIO OFF. ---")
                ser.write(b"AT+CFUN=4\r\n")
                print("[PC] -> AT+CFUN=4 (Radio OFF)")
                time.sleep(0.5)
                
        except serial.SerialException as e:
            print(f"Serial Error: {e}")
        
        print("Cooling down for 5 seconds...")
        time.sleep(5)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")