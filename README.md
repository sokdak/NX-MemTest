# NX-MemTest

NX-MemTest is an NRO-only Nintendo Switch memory stress and verification app.
It is designed for full-memory homebrew execution through hbmenu title override.

## Modes

- Quick Check: short sanity pass, allowed in small-memory environments.
- Memory Load: maximum arena memory traffic for long-running RAM stability.
- Extreme: memory traffic plus CPU pressure to stress the memory controller,
  thermals, and power stability.

## Progress Labels

- System Stress Pass: reaches 100% when the configured arena completes a pass.
- Verified Arena: bytes directly written, read, and compared.
- Physical Coverage: test arena size divided by Switch total physical memory.

NX-MemTest does not directly verify system-only memory pools. Those regions are
outside the normal NRO addressable arena.

## Host Tests

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

## Switch Build

Install devkitPro with devkitA64 and libnx, then run:

```powershell
pwsh -File scripts/check-toolchain.ps1
make
```

The build produces `NX-MemTest.nro`.

## Switch Run

Copy `NX-MemTest.nro` to:

```text
sdmc:/switch/NX-MemTest.nro
```

Launch hbmenu through title override/full-memory mode, then start NX-MemTest.
Applet mode shows a warning and should only be used for Quick Check.

Logs are written to:

```text
sdmc:/switch/NX-MemTest/logs/latest.txt
```
