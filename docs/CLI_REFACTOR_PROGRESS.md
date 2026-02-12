# CLI Mode Refactoring Progress

## Implementation Steps

- [x] Step 1: Create `src/climode.h` - CliMode class header
- [x] Step 2: Implement `src/climode.cpp` - Full CLI mode implementation
  - [x] MessageHandler for async protocol responses
  - [x] WaitForConnection with timeout
  - [x] CmdListProfiles
  - [x] CmdListChats
  - [x] CmdListContacts
  - [x] CmdReadMessages
  - [x] CmdSendMessage (with stdin support)
  - [x] CmdSearch
  - [x] CmdMarkRead
  - [x] CmdWatch (SIGINT-interruptible)
  - [x] JSON output helpers (hand-rolled escaping)
  - [x] Human-readable text output
  - [x] Contact name resolution
- [x] Step 3: Modify `src/main.cpp` - Extended argument parsing and routing
  - [x] Parse --cmd, --chat, --text, --json, --limit, --query, --msg, --file, --profile, --timeout
  - [x] Route to CliMode when --cmd is present
  - [x] Reuse existing profile loading code
  - [x] Set up MessageCache handler for CliMode
  - [x] Blocking login for CLI mode
- [x] Step 4: Update `CMakeLists.txt` - Add climode sources
- [x] Step 5: Update help text - Document CLI commands and options

## Verification

- [x] Build compiles without errors ✓
- [x] `nchat --help` shows new CLI commands ✓
- [x] `nchat --cmd list-profiles` validates no profiles gracefully ✓
- [ ] `nchat --cmd list-chats --json` returns JSON (needs configured profile)
- [ ] `nchat --cmd read --chat <id> --limit 5` shows messages (needs configured profile)
- [ ] `nchat --cmd send --chat <id> --text "test"` sends message (needs configured profile)
- [ ] `nchat --cmd search --chat <id> --query "keyword"` finds messages (needs configured profile)
- [ ] `nchat --cmd mark-read --chat <id> --msg <id>` marks read (needs configured profile)
- [ ] `nchat --cmd watch --json` streams messages (needs configured profile)
- [x] `echo "hello" | nchat --cmd send --chat <id>` accepts stdin ✓
- [x] Missing required args produce clear errors ✓

## Build Status

✅ **Successfully built on macOS with:**
- CMake configuration completed
- All source files compiled
- nchat executable linked
- CLI mode integration working

## Next Steps

To fully test the CLI mode functionality, you need to:
1. Set up a profile: `nchat --setup`
2. Choose a protocol (WhatsApp, Telegram, Signal)
3. Complete authentication
4. Test all CLI commands with actual chat data

## Known Limitations

- Requires at least one configured profile to test most commands
- `--file` attachments not fully tested
- Search functionality depends on MessageCache implementation
- Watch mode requires real-time message flow to demonstrate
