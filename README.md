# NetworkPinger

Small Windows console monitor that pings multiple IPs and shows their live connectivity state.

## Features
- Monitors several targets at once
- Shows ONLINE / DROPPING... / OUTAGE! states
- Logs outages to `network_outages.log`
- Measures internet speed (Download/Upload/Ping via Ookla `speedtest.exe`) every four hours at 00:00, 04:00, 08:00, 12:00, 16:00, and 20:00 when enabled
- Uses Ookla automatic server selection
- Color-coded speed indicators use Download: green (>= 100 Mbps), yellow (50..100 Mbps), red (< 50 Mbps)
- Adaptive Matrix rain animation speed tied to internet speed: fast rain (40ms) for green, moderate (250ms) for yellow, slow (500ms) for red; uses the configured `rainStepMs` value until a successful speedtest result is available or when speedtest is disabled
- Logs speed measurements to `speedtest.log`
- Supports an individual alert threshold for each IP
- Allows sound to be enabled or disabled per IP directly in the console
- Renders animated Matrix-style packet streams under the table: green for delivered packets and red for missed packets

## Config
`appsettings.txt` format:

```txt
<timeoutMs> <intervalMs> [matrix] [rainStepMs] [speedtest]
<ip1> <alertSeconds> [alias]
<ip2> <alertSeconds> [alias]
<ip3> <alertSeconds> [alias]
```

Meaning:
- `timeoutMs` — ping timeout in milliseconds
- `intervalMs` — delay between checks in milliseconds
- add `matrix` to the first line to enable the animated Matrix display
- `rainStepMs` — optional Matrix rain step duration in milliseconds (`100` by default, minimum `25`); higher values slow the rain, lower values speed it up
- add `speedtest` to enable scheduled speed measurements; without it, speedtest is disabled
- each next line is `IP thresholdSeconds [alias]`; the optional alias may contain spaces

The Matrix rain alphabet is defined in `kDefaultMatrixAlphabet` in `main.cpp`. Edit that constant and rebuild the program to change it.

Example:

```txt
1000 1000 matrix 100 speedtest
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

## If Windows blocks the application

If Windows shows a message that Smart App Control or Microsoft Defender blocked `NetworkPinger.exe`, open:

```text
Windows Security
	-> App & browser control
	-> Smart App Control settings
	-> Off
```

On Ukrainian Windows:

```text
Безпека Windows
	-> Керування програмами та браузерами
	-> Параметри інтелектуального керування програмами
	-> Вимкнуто
```

The `Check apps and files` switch under reputation-based protection may be disabled because Smart App Control controls it. Disable Smart App Control itself instead of that switch.

This setting reduces Windows protection and may not be possible to turn on again with a simple switch without resetting or reinstalling Windows. For local development, prefer signing the executable and changing this setting only when necessary.
