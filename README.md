##

## Activate ESP IDF Environment first
```bash
$ source "$HOME/.espressif/tools/activate_idf_v6.0.sh"
```

## Build the project

Build Firmware A
```bash
$ idf.py -DFIRMWARE_VARIANT=firmware_a -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.fw_a" -B build_fw_a  build
```

Build Firmware B
```bash
$ idf.py -DFIRMWARE_VARIANT=firmware_b -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.fw_b" -B build_fw_b  build
```

## Configure Partition Table

Readjust the Partition table using [theLastOutpostWorkshop](https://thelastoutpostworkshop.github.io/ESP32PartitionBuilder/) tool


## Connecting the ESP32 Device

### Using WSL2 on Windows

1. Install usbipd-win: Install the latest version on your Windows machine.

2. Attach Device (PowerShell/Admin):
    - List devices: ```usbipd wsl list```
    - Bind device (required once): ```usbipd bind --busid <busid>```
    - Attach device: ``` usbipd attach --wsl --busid <busid>. ```
3. Verify in WSL: Run ```lsusb``` in your Linux terminal to confirm the USB serial device is detected (e.g., CP210x).
4. Permissions: Run ```sudo chmod 666 /dev/ttyUSB0``` (or similar) inside WSL to allow user-level access to the serial port.
5. Confirm by running: ```dmesg | tail```. You should see `usb <busid>: cp210x converter now attached to ttyUSB0 or ttyACM*`. 
If you see `usb <busid>: SetAddress Request (4) to port 0`, Troubleshoot!

### Troubleshoot

1. Disconnect the ESP32 device
2. Force Load Serial Drivers in WSL2. Sometimes the WSL2 kernel doesn't auto-load the specific serial driver module.
Run this in WSL2:
```bash
sudo modprobe cp210x
sudo modprobe ftdi_sio
sudo modprobe usbserial
```
3. Then unplug/replug (re-attach) the USBIP device and check `dmesg | tail` again.

!!! note
    Anytime you close the WSL2 terminal, you'll have to re-attach the esp32 device in **Powershell(Admin) on Windows** using the command:
    ```bash
    usbipd attach --wsl --busid <busid>
    ```

4. To permanently fix the permissions issues when reconnecting, do the following:
    - Open your Bash configuration file: ```nano ~/.bashrc```
    - Add the following function to the bottom. Replace <WINDOWS_IP> and <BUSID> with your actual values (e.g., 192.168.1.50 and 1-1):
    ```
    # Function to attach ESP32 via USBIP and fix permissions
    attach-esp32() {
        echo "Attaching USB device..."
        sudo usbip attach -r <WINDOWS_IP> -b <BUSID>
        
        echo "Fixing permissions..."
        # Wait a moment for the device node to appear
        sleep 1
        
        # Fix permissions for ttyUSB0 (or ttyACM0 depending on your board)
        if [ -e /dev/ttyUSB0 ]; then
            sudo chgrp dialout /dev/ttyUSB0
            sudo chmod 660 /dev/ttyUSB0
            echo "Success: /dev/ttyUSB0 permissions fixed."
        elif [ -e /dev/ttyACM0 ]; then
            sudo chgrp dialout /dev/ttyACM0
            sudo chmod 660 /dev/ttyACM0
            echo "Success: /dev/ttyACM0 permissions fixed."
        else
            echo "Warning: Device node not found. Check dmesg."
        fi
    }
    ```
    - Save and Exit: Press **Ctrl+O**, Enter to save. Press **Ctrl+X** to exit.
    - Apply changes: ```source ~/.bashrc```
    - Usage: Run in the terminal: ```attach-esp32```

## Flashing Firmware unto the Device

Flash Firmware A
```bash
./build_and_flash.sh --flash-fw-a
```

Flash Firmware B
```bash
./build_and_flash.sh --flash-fw-b
```

Help
```bash
./build_and_flash.sh --help
```

