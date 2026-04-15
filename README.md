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

## Flashing Firmware unto the Device

Flash Firmware A
```bash

```