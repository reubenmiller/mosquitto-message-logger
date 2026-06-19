# Mosquitto Message Logger Plugin

A standalone Mosquitto broker plugin that logs all MQTT messages with comprehensive metadata to file and/or stderr.

## Features

- **File logging**: Append-only JSON log files with daily rotation
- **Stderr logging**: Optional mosquitto_sub compatible format with `MQTT_LOG:` prefix
- **Smart payload handling**: 
  - Plain text/JSON payloads are stored as escaped JSON strings
  - Binary payloads are automatically detected and Base64 encoded
- **Rich metadata**: ISO 8601 timestamps, topic, QoS, retain flag, payload length, client ID
- **Configurable**: Via environment variables
- **Auto-creating directories**: Log directories are created automatically if they don't exist
- **Cross-compilation support**: Build for multiple architectures (ARM64, ARMv7, x86_64, etc.)

## Requirements

- Mosquitto 2.0 or later (for plugin API v5)
- GCC or Clang
- Mosquitto development headers (`mosquitto-dev` or `libmosquitto-dev` package)
- Optional: `just` command runner (`brew install just` or `cargo install just`)

## Building

### Quick Start

```bash
# Build the plugin
make

# Or with just
just build
```

### Cross-Compilation

The Makefile supports cross-compilation for different architectures.

#### On Linux (Native Cross-Compilation)

```bash
# Install cross-compilation toolchains
sudo apt-get install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf

# For ARM64 (aarch64)
make CROSS_COMPILE=aarch64-linux-gnu-

# For ARMv7 (32-bit ARM)
make CROSS_COMPILE=arm-linux-gnueabihf-

# With just
just build-arm64
just build-armv7
```

#### On macOS (Docker-based Cross-Compilation)

Cross-compiling on macOS requires Docker. This method works reliably and produces Linux binaries:

```bash
# Build all Linux architectures at once
just build-linux-all

# Or build specific architectures
just build-linux-x86_64   # x86_64 Linux
just build-linux-arm64    # ARM64/aarch64 Linux
just build-linux-armv7    # ARMv7 32-bit Linux
```

Binaries will be saved to the `dist/` directory with architecture-specific names.

**Requirements:**
- Docker Desktop for Mac
- `just` command runner: `brew install just`

The Docker approach uses pre-built cross-compilation containers from [cross-rs](https://github.com/cross-rs/cross) with mosquitto development headers included.

#### Using Zig (Recommended - Works on All Platforms)

Zig provides the easiest cross-compilation experience and works on macOS, Linux, and Windows. Mosquitto headers are automatically downloaded as a dependency:

```bash
# Build all Linux architectures at once
just build-zig-all

# Or build specific architectures
just build-zig-x86_64   # x86_64 Linux
just build-zig-arm64    # ARM64/aarch64 Linux
just build-zig-armv7    # ARMv7 32-bit Linux

# Or use zig directly
zig build all -Doptimize=ReleaseSafe
```

Binaries will be saved to the `zig-out/dist/` directory.

**Requirements:**
- Zig 0.16.0 or later: [Download](https://ziglang.org/download/) or `brew install zig`
- `just` command runner (optional): `brew install just`

**Advantages of Zig:**
- No Docker needed
- No manual header installation required (mosquitto headers downloaded automatically)
- Native cross-compilation for all targets
- Fast compilation
- Works identically on macOS, Linux, and Windows

### Custom Mosquitto Headers

If you have mosquitto headers in a non-standard location:

```bash
make MOSQUITTO_INCLUDE=/path/to/mosquitto/include

# Or with just
just build-custom /path/to/mosquitto
```

## Installation

### System-wide Installation

```bash
sudo make install

# Or with just
just install
```

Default installation path: `/usr/local/lib/mosquitto_message_logger.so`

### Custom Installation Path

```bash
sudo make install PREFIX=/opt/mosquitto
sudo make install DESTDIR=/tmp/staging LIBDIR=/usr/lib/mosquitto
```

## Configuration

### mosquitto.conf

Add the plugin to your Mosquitto configuration:

```conf
# If installed system-wide
plugin /usr/local/lib/mosquitto_message_logger.so

# Or use absolute path to the .so file
plugin /path/to/mosquitto_message_logger.so
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `MQTT_LOG_DIR` | `/var/log/mosquitto` | Directory for log files |
| `MQTT_LOG_STDERR` | unset | Set to `1` to enable stderr logging |

### Example

```bash
# Set environment variables before starting mosquitto
export MQTT_LOG_DIR=/var/log/mqtt
export MQTT_LOG_STDERR=1
mosquitto -c /etc/mosquitto/mosquitto.conf
```

Or in a systemd service file:

```ini
[Service]
Environment="MQTT_LOG_DIR=/var/log/mqtt"
Environment="MQTT_LOG_STDERR=1"
ExecStart=/usr/sbin/mosquitto -c /etc/mosquitto/mosquitto.conf
```

## Log Output Formats

### File Output (JSON Lines)

Daily rotated files: `mqtt-messages-YYYYMMDD.log`

**Text/JSON payload:**
```json
{"timestamp":"2026-02-13T06:40:07.822347+0000","topic":"home/temperature","qos":0,"retain":0,"payloadlen":4,"client_id":"sensor01","payload":"22.5"}
```

**Binary payload:**
```json
{"timestamp":"2026-02-13T06:40:08.123456+0000","topic":"binary/data","qos":1,"retain":1,"payloadlen":256,"client_id":"device01","payload_base64":"AQIDBAU="}
```

### Stderr Output (mosquitto_sub format)

Messages are prefixed with `MQTT_LOG:` for easy filtering:

```json
MQTT_LOG: {"timestamp":1770964807.822328645,"message":{"tst":"2026-02-13T06:40:07.822347+0000","topic":"home/temperature","qos":0,"retain":0,"payloadlen":4,"payload":"22.5"},"payload_hex":"32322e35"}
```

**Filtering stderr output:**
```bash
mosquitto -v 2>&1 | grep "MQTT_LOG:"
```

## Testing

### Local Test Run

Use the `just` command to run a test instance:

```bash
just test-local
```

This starts a local mosquitto instance on port 1883 with the plugin loaded, logging to the current directory.

### Manual Testing

```bash
# Terminal 1: Start mosquitto
export MQTT_LOG_DIR=/tmp/mqtt-logs
export MQTT_LOG_STDERR=1
mosquitto -c mosquitto.conf

# Terminal 2: Publish a message
mosquitto_pub -t "test/topic" -m "Hello World"

# Check the logs
cat /tmp/mqtt-logs/mqtt-messages-$(date +%Y%m%d).log
```

## Binary Detection

The plugin automatically detects binary payloads using the following heuristics:
- More than 10% null bytes
- More than 10% control characters (excluding tab, newline, carriage return)
- Only checks the first 1024 bytes of large payloads

Binary payloads are Base64 encoded in file logs and hex encoded in stderr logs.

## Log Encoding

### File Logs
- **Text payloads**: JSON-escaped strings in the `payload` field
- **Binary payloads**: Base64 encoded in the `payload_base64` field

### Stderr Logs
- **All payloads**: JSON-escaped in `payload` field
- **All payloads**: Hex encoded in `payload_hex` field

This dual approach provides:
- **Efficiency** in file storage (Base64 is ~33% smaller than hex)
- **Debugging** convenience in stderr output (hex is human-readable)

## Performance Considerations

- File I/O is buffered and only happens on message receipt
- Log files are opened, written, and closed for each message (ensures durability)
- Binary detection is limited to first 1KB of payload
- Minimal memory allocation with cleanup after each message

For high-throughput scenarios, consider:
- Using a dedicated disk/partition for log storage
- Setting `MQTT_LOG_STDERR=0` to disable stderr logging
- Implementing log rotation/cleanup scripts

## Development

### Project Structure

```
mosquitto-message-logger/
├── mosquitto_message_logger.c   # Plugin source code
├── Makefile                       # Build system with cross-compilation support
├── justfile                       # Convenience commands (requires just)
├── README.md                      # This file
├── LICENSE                        # EPL-2.0 OR BSD-3-Clause
└── .gitignore                     # Git ignore rules
```

### Building with Debug Symbols

```bash
make CFLAGS="-Wall -Werror -g -O0 -fPIC"
```

### Code Formatting

```bash
just format  # Requires clang-format
```

## License

EPL-2.0 OR BSD-3-Clause

This project includes code originally from Eclipse Mosquitto, licensed under EPL-2.0 and EDL-v1.0.

See LICENSE file for details.

## Contributing

Contributions are welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Make your changes with clear commit messages
4. Test your changes
5. Submit a pull request

## Acknowledgments

- Eclipse Mosquitto project for the plugin API and original example code
- thin-edge.io community for message logging requirements

## Support

For issues, questions, or contributions, please use the GitHub issue tracker.
