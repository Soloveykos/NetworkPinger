# NetworkPinger

Small Windows console monitor that pings multiple IPs and shows their live connectivity state.

## Features
- Monitors several targets at once
- Shows ONLINE / DROPPING... / OUTAGE! states
- Logs outages to `network_outages.log`
- Supports an individual alert threshold for each IP
- Allows sound to be enabled or disabled per IP directly in the console

## Config
`appsettings.txt` format:

```txt
<timeoutMs> <intervalMs>
<ip1> <alertSeconds>
<ip2> <alertSeconds>
<ip3> <alertSeconds>
```

Meaning:
- `timeoutMs` — ping timeout in milliseconds
- `intervalMs` — delay between checks in milliseconds
- each next line is `IP thresholdSeconds`

Example:

```txt
1000 1000
8.8.8.8 3
1.1.1.1 60
9.9.9.9 30
```

Each IP can have its own outage threshold and sound state. If the ping fails repeatedly for longer than that value, the app marks it as outage and triggers the alert once. Click the green `[ON ]` or red `[OFF]` value in the `Sound` column to toggle sound for that IP.

## Build / Run
Build with W64DevKit on Windows. In PowerShell, run:

```bash
$env:PATH = "C:\w64devkit\bin;" + $env:PATH
C:\w64devkit\bin\g++.exe main.cpp -o NetworkPinger.exe -liphlpapi -lws2_32
```

The `PATH` update is needed so the compiler can find its assembler and other build tools.

Then run `NetworkPinger.exe` in the same folder as `appsettings.txt`.
