# NetworkPinger

Small Windows console monitor that pings multiple IPs and shows their live connectivity state.

## Features
- Monitors several targets at once
- Shows ONLINE / DROPPING... / OUTAGE! states
- Logs outages to `network_outages.log`
- Supports an individual alert threshold for each IP

## Config
`appsettings.txt` format:

```txt
<timeoutMs> <intervalMs> <soundEnabled>
<ip1> <alertSeconds>
<ip2> <alertSeconds>
<ip3> <alertSeconds>
```

Meaning:
- `timeoutMs` — ping timeout in milliseconds
- `intervalMs` — delay between checks in milliseconds
- `soundEnabled` — `1` to enable sound, `0` to disable it
- each next line is `IP thresholdSeconds`

Example:

```txt
1000 1000 1
8.8.8.8 3
1.1.1.1 60
9.9.9.9 30
```

Each IP can have its own outage threshold. If the ping fails repeatedly for longer than that value, the app marks it as outage and triggers the alert. Set the third value to `0` to mute all sound alerts.

## Build / Run
Build with MinGW / GCC on Windows:

```bash
g++ main.cpp -o NetworkPinger.exe -liphlpapi -lws2_32
```

Then run `NetworkPinger.exe` in the same folder as `appsettings.txt`.
