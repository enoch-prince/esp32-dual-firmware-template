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


