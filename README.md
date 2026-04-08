##

## Activate ESP IDF Environment first
```bash
$ source "$HOME/.espressif/tools/activate_idf_v6.0.sh"
```

## Build the project

Build Firmware A
```bash
$ idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.fw_a" -B build_fw_a  build
```

