# GL-S20 OpenThread Border Router

[GL's S20](https://www.gl-inet.com/products/gl-s20) is a nice and inexpensive device but firmware support is lacking behind as GL's attention is on more recent hardware.
Luckily enough, they shared an [OpenSDK](https://github.com/gl-inet/s20_thread_br_opensdk) that I forked to upgrade things a bit.

This is a minimalist, performance oriented firmare that supports:

- [esp-idf](https://github.com/espressif/esp-idf) -> v6.0.1
    - Thread v1.4
    - TREL support
    - BBR support
    - Home Assistant OTBR integration
- [esp-thread-br](https://github.com/espressif/esp-thread-br) -> main @ 66762f2
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

If you're using WSL, check [Connect USB devices to WSL](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)

To flash the firmware:
```
$ idf.py -p /dev/ttyUSB0 flash
```

To start monitor:
```
$ idf.py -p /dev/ttyUSB0 monitor
```

To exit monitor use `Ctrl + ]` or `CTRL + T, CTRL + X`

More [flashing procedures](docs/flash.md).

## Work with Home Assistant

### Installing the Open Thread Border Router and Thread Intergration

The Open Thread Border Router integration allows Home Assistant to acess Open Thread Border Router.

To install this integration, navigate to **Home Assistant > Settings > Devices & Services > Add Intergration** and search for **Open Thread Border Router**, submit the url like below.

`http://<YOUR_GL-S20_IP_ADDRESS>`

Replace "`<YOUR_GL-S20_IP_ADDRESS>`" with GL-S20's IP address and *make sure it is static*

Then click **Add Intergration** again, search for **Thread**, select it and click **FINISH**, enter **Thread** Intergrations, click **CONFIGURE** and make sure you have S20‘s Thread network under **Preferred network** line, and it contains an icon with **key+phone**. If not, do the following:

Click **three dots** on the right to OpenThread Border Router, choose **Add to preferred network**.

Under the **preferred network** now, click again **three dots** on the right to OpenThread Border Router, and choose **Use router for Android + iOS credentials**.

### Installing the Matter Server Add-On and Matter Intergration

To set up Home Assistant to manage Matter devices, we need the **Matter Server** add-on.

For this, navigate to **Home Assistant > Settings > Add-Ons > Add-On Store** and search for **Matter server**, select it and follow the instructions to complete.

After the Matter server is correctly installed, navigate to **Home Assistant >** **Settings > Devices & Services > Add Integration** and search for **Matter**.

A prompt will show up asking for a connection method. If you are working with custom containers running the Matter server, uncheck the box.

In our case, we leave it checked as the Matter server is running in Home Assistant. You will receive a **Success** message if everything is installed.

**Sync Thread Credentials**

Update Thread network credentials from Home Assistant to your phone provide to matter commissioning process.

Open Android **Companion APP > Settings > Companion app > Troubleshooting > Sync Thread credentials**.

If you get a "Added network from Home Assistant to this device", then you are good to go. Otherwise, you will need to clear all the data of Google Play Services then try again.

### Commission End Devices

Before you start commission end devices in Home Assistant app, you need to install Google Home app in Play Store, it is recommended by Home Assistant developers and you'll not able to pair Matter over Thread devices without installed Google Home app.

To commission your device, open the Home Assistant app on your smartphone, go to **Home Assistant > Settings > Devices & Services**, in the lower tabs bar, go to **Devices** and tap on **Add Device**, tap on **Add Matter device**.

After you turn the End Devices in Commission mode, scan the Matter QR code on the End Devices.
