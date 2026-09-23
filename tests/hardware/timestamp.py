#!/usr/bin/env python3

import time
import SoapySDR
from SoapySDR import *  # SOAPY_SDR_ constants

def main():
    card = 0
    args = {"driver": "sidekiq", "card": f"{card}"}  # All values must be strings
    sdr = SoapySDR.Device(args)

    handle = 0  # RX channel index

    if card == 2:
        # Set RX sample rate to 10 MHz
        sample_rate = int(10e6)
    else:
        sample_rate = int(50e6)

    sdr.setSampleRate(SOAPY_SDR_RX, handle, sample_rate)
    print(f"Sample rate set to {sample_rate} Hz")

    sdr.setHardwareTime(0, "now")

    last_time = 0;
    # Print both timestamps every second
    for i in range(10):
        rf_time = sdr.getHardwareTime("rx_rf_timestamp")
        print(f"At time {i} timestamp: {rf_time}, \
                delta seconds {(rf_time - last_time) * 0.000000001:,}")
        last_time = rf_time
        time.sleep(1)

if __name__ == "__main__":
    main()
