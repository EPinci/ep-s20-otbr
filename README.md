# GL-S20 OpenThread Border Router

[GL's S20](https://www.gl-inet.com/products/gl-s20) is a nice and inexpensive device but firmware support is lacking behind as GL's attention is on more recent hardware.
Luckily enough, they shared an [OpenSDK](https://github.com/gl-inet/s20_thread_br_opensdk) that I forked to upgrade things a bit.

This is a minimalist, performance oriented firmare that supports:

- [esp-idf](https://github.com/espressif/esp-idf) -> v6.0.2
    - Thread v1.4
    - TREL support
    - BBR support
    - Home Assistant OTBR integration
- [esp-thread-br](https://github.com/espressif/esp-thread-br) -> main @ 25ab204
    - Used as base framework
    - Implemented only wired connectivity in order to keep the radio for Thread use only
    - Forked basic Web UI to add logs, remote/local OTA and more
- [s20_thread_br_opensdk](https://github.com/gl-inet/s20_thread_br_opensdk) -> main @ 2b610f8
    - Leveraged primarily for LED support, PIN layout and base IDF settings

**Use at your own risk!** (but you can always flash back the original firmware...)

## Getting started

```
git clone --recursive https://github.com/epinci/ep-s20-otbr.git
git submodule update --init --recursive
```

Install SDK environment and build RCP firmware:
```
./utils/install-idf.sh
```

Activate SDK environment:
```
. ./esp-idf/export.sh
```

Build S20 firmware:
```
cd s20-otbr
idf.py set-target esp32s3
idf.py build
```

First deployment must use a USB connection, subsequent updates can be pushed by network.
If you're using WSL, check [Connect USB devices to WSL](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)

To erase the flash with a USB cable (recommended before flashing the firmware):
```
$ idf.py -p /dev/ttyUSB0 erase-flash
```
To flash the firmware with a USB cable:
```
$ idf.py -p /dev/ttyUSB0 flash
```
To start log monitoring with a USB cable:
```
$ idf.py -p /dev/ttyUSB0 monitor
```
To exit monitor use `Ctrl + ]` or `CTRL + T, CTRL + X`

After the first flash you can push a build with:
```
$ ./script/push-update.sh <IP-ADDRESS>
```

More [flashing procedures](docs/moreflash.md).

## Integrating with Home Assistant

See [Home Assistant](docs/homeassistant.md).