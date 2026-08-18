# NetworkPinger

Small Windows console monitor that pings multiple IPs and shows their live connectivity state.

## Features
- Monitors several targets at once
- Shows ONLINE / DROPPING... / OUTAGE! states
- Logs outages to `network_outages.log`
- Supports an individual alert threshold for each IP
- Allows sound to be enabled or disabled per IP directly in the console
- Renders animated Matrix-style packet streams under the table: green for delivered packets and red for missed packets

## Config
`appsettings.txt` format:

```txt
<timeoutMs> <intervalMs> [matrix]
<ip1> <alertSeconds> [alias]
<ip2> <alertSeconds> [alias]
<ip3> <alertSeconds> [alias]
```

Meaning:
- `timeoutMs` — ping timeout in milliseconds
- `intervalMs` — delay between checks in milliseconds
- add `matrix` to the first line to enable the animated Matrix display
- each next line is `IP thresholdSeconds [alias]`; the optional alias may contain spaces

Example:

```txt
1000 1000 matrix
8.8.8.8 3 Google DNS
1.1.1.1 60 Cloudflare DNS
9.9.9.9 30 Quad9 DNS
```

Each IP can have its own outage threshold, alias, and sound state. If the ping fails repeatedly for longer than that value, the app marks it as outage and triggers the alert once. Outage logs use a readable duration such as `1г. 2хв. 3сек.`. Click the green `[ON ]` or red `[OFF]` value in the `Sound` column to toggle sound for that IP.

## Build / Run
Build with W64DevKit on Windows. In PowerShell, run:

```bash
$env:PATH = "C:\w64devkit\bin;" + $env:PATH
C:\w64devkit\bin\windres.exe NetworkPinger.rc -O coff -o NetworkPinger-resources.o
C:\w64devkit\bin\g++.exe main.cpp NetworkPinger-resources.o -o NetworkPinger.exe -liphlpapi -lws2_32
```

The `PATH` update is needed so the compiler can find its assembler and other build tools.

`NetworkPinger.ico` is embedded into `NetworkPinger.exe` during this build. To adjust the green digital-rain icon, edit and run `tools\create-icon.ps1` before rebuilding.

Then run `NetworkPinger.exe` in the same folder as `appsettings.txt`.
