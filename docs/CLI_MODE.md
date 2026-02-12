# nchat CLI Mode

## Overview

nchat now supports a non-interactive CLI mode that enables programmatic access to chat functionality without launching the TUI. This is ideal for:
- AI agents (like Claude Code) to send/receive messages
- Shell scripts and automation
- CI/CD pipelines
- Remote/headless environments

## Quick Start

### View Help
```bash
nchat --help
```

### List Configured Profiles
```bash
nchat --cmd list-profiles
nchat --cmd list-profiles --json
```

### List Chats
```bash
# Human-readable format
nchat --cmd list-chats

# JSON format
nchat --cmd list-chats --json

# Limit results
nchat --cmd list-chats --limit 10

# Specific profile
nchat --cmd list-chats --profile WhatsAppMd_0
```

### Read Messages
```bash
# Last 20 messages (default)
nchat --cmd read --chat <chat_id>

# Last 50 messages
nchat --cmd read --chat <chat_id> --limit 50

# JSON output
nchat --cmd read --chat <chat_id> --json
```

### Send Messages
```bash
# With --text argument
nchat --cmd send --chat <chat_id> --text "Hello!"

# From stdin (pipe)
echo "Hello from script" | nchat --cmd send --chat <chat_id>

# With file attachment
nchat --cmd send --chat <chat_id> --text "Here's a file" --file /path/to/file.pdf
```

### Search Messages
```bash
nchat --cmd search --chat <chat_id> --query "keyword"
nchat --cmd search --chat <chat_id> --query "error message" --json
```

### Mark as Read
```bash
nchat --cmd mark-read --chat <chat_id> --msg <message_id>
```

### Watch Mode (Stream Incoming Messages)
```bash
# Watch all chats
nchat --cmd watch --json

# Watch specific chat
nchat --cmd watch --chat <chat_id> --json

# Press Ctrl-C to stop
```

## Global Options

- `--profile <id>` - Select profile (e.g., `Telegram_0`, `WhatsAppMd_0`)
  - Auto-selected if only one profile configured
  - Required if multiple profiles exist
- `--json` - Output in JSON format instead of human-readable text
- `--timeout <sec>` - Connection timeout in seconds (default: 30)
- `-d, --confdir <dir>` - Use custom config directory (default: `~/.config/nchat`)
- `-e, --verbose` - Enable verbose logging
- `-ee, --extra-verbose` - Enable extra verbose logging

## Output Formats

### Human-Readable Text
```
=== Chats ===
[1] John Doe (WhatsAppMd_0:1234567890@s.whatsapp.net) - unread
[2] Family Group (WhatsAppMd_0:120363...) - pinned
```

### JSON
```json
{
  "chats": [
    {
      "id": "1234@s.whatsapp.net",
      "profile": "WhatsAppMd_0",
      "name": "John Doe",
      "unread": true,
      "muted": false,
      "pinned": false,
      "last_message_time": 1705312200
    }
  ]
}
```

## Requirements

Before using CLI mode, you must set up at least one profile:

```bash
nchat --setup
```

Follow the prompts to configure Telegram, WhatsApp, or Signal.

## Exit Codes

- `0` - Success
- `1` - Error (no profiles, connection timeout, invalid arguments, command failed)

## Architecture

### Key Components

1. **CliMode class** (`src/climode.{h,cpp}`)
   - Handles all CLI commands
   - Manages protocol connections and message handling
   - Formats output (text/JSON)

2. **Protocol Integration**
   - Reuses existing protocol libraries (`tgchat`, `wmchat`, `sgchat`)
   - Synchronous login (unlike TUI's async approach)
   - Message handler routes async responses via condition variables

3. **MessageCache**
   - Fetches chats, contacts, and messages from local cache
   - Synchronous operations for CLI mode

### Design Decisions

- **Hand-rolled JSON**: No external JSON library dependency
- **Synchronous execution**: CLI commands run to completion, then exit
- **Stdin support**: Enables piping for `send` command
- **Watch mode**: Keeps process alive, prints messages as they arrive
- **Contact name resolution**: Uses cached contact info to display names instead of IDs

## Examples

### Script to Send Daily Report
```bash
#!/bin/bash
CHAT_ID="1234567890@s.whatsapp.net"
REPORT=$(generate_daily_report.sh)

echo "$REPORT" | nchat --cmd send --chat "$CHAT_ID" --profile WhatsAppMd_0
```

### Monitor for Keywords
```bash
#!/bin/bash
CHAT_ID="group@s.whatsapp.net"

nchat --cmd watch --chat "$CHAT_ID" --json | \
  jq -r 'select(.text | contains("urgent")) | .text'
```

### Export Chat History
```bash
#!/bin/bash
CHAT_ID="1234567890@s.whatsapp.net"

nchat --cmd read --chat "$CHAT_ID" --limit 1000 --json > chat_history.json
```

## Limitations

- Requires at least one configured profile
- Cannot create new chats programmatically (use existing chat IDs)
- File downloads not implemented via CLI (use TUI mode)
- Reactions not accessible via CLI
- Group management not available

## Troubleshooting

### "No profiles setup, exiting"
Run `nchat --setup` to configure a profile first.

### "Connection timed out"
- Check network connectivity
- Increase timeout: `--timeout 60`
- Check protocol login status in `~/.config/nchat/log.txt`

### "Unknown profile"
List available profiles: `nchat --cmd list-profiles`

### "error: --chat is required"
Some commands need a chat ID. Get it from: `nchat --cmd list-chats`

## Future Enhancements

Potential improvements for future versions:
- Create new chats/groups
- File download support
- Reaction management
- Group admin operations (add/remove members, change settings)
- Message editing/deletion via CLI
- Typing indicators control
- Read receipts management
