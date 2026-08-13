# NetworkPinger

Small Windows console app that pings multiple IPs and shows live connectivity status in a dashboard.

## Features
- Monitors multiple targets at once
- Shows online / dropping / outage states
- Logs outages to `network_outages.log`
- Uses `appsettings.txt` for timeout, interval, and target IPs

## Config
`appsettings.txt` format:

```txt
<timeoutMs> <intervalMs> <minFailCount>
<ip1>
<ip2>
...
```

Example:

```txt
1000 1000 3
8.8.8.8
1.1.1.1
```

## Build / Run
Build with MinGW/ GCC on Windows:

```bash
g++ main.cpp -o NetworkPinger.exe -liphlpapi -lws2_32
```

Then run `NetworkPinger.exe` in the same folder as `appsettings.txt`.
