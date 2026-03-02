# memo

Minimal communication utility. Filesystem-based message transport.

## Requirements

- gcc (MSYS2 or similar)
- No external dependencies (GDI ships with Windows)

## Building

```powershell
./build.ps1
```

Or manually:

```powershell
cp config.def.h config.h
gcc -std=c99 -O2 -Wall -o memo.exe memo.c gap.c -lgdi32 -luser32 -lshell32 -lshcore
```

## Usage

```powershell
# pipe mode — send message to agent
echo "Import February CSV" | memo ask finance

# pipe mode — agent responds
echo "Done, 47 records imported" | memo tell finance

# popup UI mode — no stdin, opens centered editor
memo ask finance

# popup UI mode — shows latest inbound, lets you reply
memo tell finance

# force popup even with stdin
echo "prefill" | memo ask finance --ui
```

## Message Format

```
from: nate
to: finance
at: 2026-03-01T17:09:58Z

Import February CSV
```

Plain text. Greppable. `cat`-able. No JSON. No YAML.

## File Layout

```
$MEMO_DIR/
  <agent>/
    YYYY-MM-DD/
      HHMMSS-MMM-in.txt     message to agent
      HHMMSS-MMM-out.txt    message from agent
```

Default `MEMO_DIR` is `%USERPROFILE%/.memo`.

## Vim Keybindings

Insert mode (default): type normally, Enter for newline, Ctrl+S to send.

Normal mode (Esc): `hjkl` movement, `0$` line ends, `gg/G` top/bottom,
`x` delete char, `dd` delete line, `o/O` open line, `a/A/I` insert variants,
`:w` send, `:q` cancel.

## Environment

| Variable    | Purpose                        | Default              |
|-------------|--------------------------------|----------------------|
| `MEMO_DIR`  | message store root             | `%USERPROFILE%/.memo`|
| `MEMO_USER` | sender identity                | `%USERNAME%`         |

## Exit Codes

| Code | Meaning         |
|------|-----------------|
| 0    | Success         |
| 1    | Usage error     |
| 2    | Filesystem error|
| 3    | Write failure   |
