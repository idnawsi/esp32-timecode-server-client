# esp32-timecode-server-client
A LTC timecode for esp32, 1 server many client, with autosync, configurable framerate.
markdown
# ESP32 Timecode Master-Client System

A professional timecode synchronization system using ESP32 microcontrollers for film production, broadcasting, and audio-video synchronization.

## Features

- **LTC SMPTE Audio Output**: Generate professional Linear Timecode on GPIO25 (DAC1)
- **Wireless Synchronization**: WiFi-based timecode distribution
- **Multiple Sync Modes**: 
  - **HARD**: Immediate synchronization
  - **SOFT**: Gradual speed adjustment
  - **AUTO**: Smart sync based on drift threshold
- **Web Interface**: Real-time control and monitoring
- **Multi-Client Support**: Sync unlimited cameras/devices
- **Persistent Settings**: Automatic resume after power cycle

## Hardware Requirements

- ESP32 development boards (for Master and Clients)
- Audio cables (3.5mm TRS)
- Power supplies
- (Optional) Level shifting circuitry for professional audio gear

## Quick Start

1. **Master Device**:
   - Upload `firmware/master/master.ino` to your ESP32
   - Device creates WiFi network: `TimecodeMaster` (password: `timecode123`)
   - Access web interface at `192.168.4.1`

2. **Client Devices**:
   - Upload `firmware/client/client.ino` to your ESP32 clients
   - Clients automatically connect to Master network
   - LTC audio output on GPIO25

## Sync Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| HARD | Immediate frame-accurate sync | Critical synchronization |
| SOFT | Gradual speed adjustment | Live production |
| AUTO | Smart sync (<1s=SOFT, ≥1s=HARD) | Balanced performance |

## Web Interface

Control everything via the web interface:
- Start/Stop timecode
- Set frame rate (24, 25, 30 fps)
- Configure drop frame
- Monitor client connections
- Control sync modes per client
- Enable/disable LTC output

## Pinout

| GPIO | Function | Notes |
|------|----------|-------|
| 25   | LTC Audio Output | DAC1, connect to camera audio input |
| 2    | Sync Status LED | Client sync status |
| 4    | Connection LED | WiFi/Network status |
| 0    | Sync Button | Master manual sync trigger |

## Documentation

- [Hardware Setup](docs/HARDWARE.md)
- [Software Setup](docs/SETUP.md)  
- [Troubleshooting](docs/TROUBLESHOOTING.md)
