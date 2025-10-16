# NetCast

HTTP audio streaming add-on for Haiku.

## Features

- Stream audio from any media source over HTTP
- PCM and MP3 (via LAME)
- Built-in web player with modern UI
- Configurable sample rate, channels, bitrate
- Custom stream names
- Per-client buffering for stable streaming
- Multiple simultaneous clients (up to 10)
- ICY metadata support

## Requirements

- LAME library (optional, for MP3)
```bash
pkgman install lame_devel
```

## Build

```bash
make
```

## Installation

```bash
make install INSTALL_DIR=`finddir B_USER_NONPACKAGED_MEDIA_NODES_DIRECTORY`
```

Restart Media Services or reboot.

## Usage

### Set as default output

**Media preferences → Audio settings → Audio output:**
- Select **NetCast** as default audio output device

### Manual routing with Cortex

**Demos → Cortex:**
- Connect any audio source to NetCast node manually

### Configure streaming

Open NetCast parameters in Media preferences:
- Set port and codec
- Configure stream name
- Enable server

### Play stream

**Web player:**
```
http://[IP]:8000/
```

**Direct stream:**
```
http://[IP]:8000/stream
```

## License

MIT License - Copyright (c) 2025 Gerasim Troeglazov (3dEyes**)
