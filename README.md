# NX-MemTest

NX-MemTest is an NRO-only Nintendo Switch homebrew app for full-memory RAM and
memory-controller stress testing.

Run hbmenu through title override/full-memory mode before launching the NRO.
Applet mode has a much smaller heap and is only suitable for Quick Check.

## Host Tests

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

## Switch Build

Install devkitPro with devkitA64 and libnx, then run:

```powershell
make
```

The NRO is produced at `NX-MemTest.nro`.
