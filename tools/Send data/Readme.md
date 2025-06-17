
Python Client For Sending Data (file_sender.py)

A ZeroMQ DEALER client for sending USD files, images, and structured messages to the ANARI USD Middleware server. Designed for testing and integration with the JUSYNC USD Middleware ecosystem. This is a python script to send data to the receiverUI for testing purposes.

## Key Features

- **File Transfer**: Send USD files (.usd, .usda, .usdc, .usdz) and images with SHA-256 hash verification
- **Message Types**: Support for text messages, JSON data, and binary file transfers
- **Interactive Mode**: Real-time testing environment with command input
- **Connection Management**: Automatic retries and configurable timeouts
- **Cross-Platform**: Works on Windows and Linux environments
- **Verbose Feedback**: Detailed status reporting and error handling

## Prerequisites

- Python 3.6+
- ZeroMQ Python bindings: `pyzmq`
- Access to a running ANARI USD Middleware server (ReceiverUI)

## Usage

### Basic Command Structure
```

python3 file_sender.py [--endpoint <zmq-endpoint>] <command> [arguments]

```

### Commands

| Command         | Description                                  | Example                                  |
|-----------------|----------------------------------------------|------------------------------------------|
| `send-file`     | Send USD/image file                          | `send-file model.usd`                    |
| `send-message`  | Send text message                            | `send-message "Hello World"`             |
| `send-json`     | Send JSON file                               | `send-json config.json`                  |
| `test`          | Run predefined test sequence                 | `test`                                   |
| `interactive`   | Enter interactive command mode               | `interactive`                            |

### Options

| Option          | Description                          | Default                      |
|-----------------|--------------------------------------|------------------------------|
| `--endpoint`    | ZeroMQ server endpoint               | `tcp://localhost:5556`       |

## Example Workflows

### Send USD File to Local Server
```

python3 file_sender.py send-file scene.usda

```

### Send File to Remote Server
```

python3 file_sender.py --endpoint tcp://192.168.1.100:5556 send-file Kitchenset.usda

```

### Send JSON Configuration
```

python3 file_sender.py send-json render_settings.json

```

### Interactive Mode
```

python3 file_sender.py interactive
> file model.usdc
> message "Processing complete"
> json {"action": "shutdown"}
> quit

```

## Message Protocol

### File Transfer Format
```

[DEALER IDENTITY] (auto)
[Filename (string)]
[File Content (binary)]
[SHA-256 Hash (string)]

```

### JSON Message Format
```

{
"type": "command",
"timestamp": 1687029201,
"data": {
"action": "process",
"priority": "high"
}
}

```

## Troubleshooting

### Common Errors

**Connection Refused**
```

❌ Connection failed: Connection refused

```
- Verify middleware server is running
- Check firewall/port settings
- Confirm correct endpoint address

**Timeout Errors**
```

⚠️ No reply received (timeout)

```
- Increase timeout with `socket.setsockopt(zmq.RCVTIMEO, 10000)`
- Check server load and network latency

**File Transfer Issues**
```

❌ File not found: missing_file.usd

```
- Use absolute paths for files
- Check file permissions

**JSON Errors**
```

❌ Error encoding JSON: Expecting property name

```
- Validate JSON with `jsonlint`
- Ensure proper escaping of special characters


