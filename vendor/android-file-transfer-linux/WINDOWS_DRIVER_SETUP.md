# Windows WinUSB Driver Setup

For Windows users, you need to install the WinUSB driver before using this MTP client.

## Automatic Driver Installation

The **Zune WinUSB Driver Setup Tool** is now available as a separate project:

**Repository**: `Z:\zune-winusb-driver-setup`

This standalone tool provides:
- Automatic WinUSB driver installation
- Driver backup and restore functionality
- Simple command-line interface
- No need for Zadig or manual driver installation

## Quick Start

1. Build the driver setup tool (see the tool's README)
2. Run as administrator: `aft-driver-setup.exe install`
3. Reconnect your Zune device
4. Build and use this MTP client normally

## Manual Installation (Alternative)

If you prefer to use Zadig:
1. Download [Zadig](https://zadig.akeo.ie/)
2. Options → List All Devices
3. Select your Zune device
4. Select "WinUSB" as the driver
5. Click "Replace Driver"

## More Information

See the driver setup tool's README for detailed documentation, troubleshooting, and build instructions.
