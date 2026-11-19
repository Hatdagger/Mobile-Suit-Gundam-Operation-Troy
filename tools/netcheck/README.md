# optroy_netcheck

A small test program for the online-play client, without the game. It is built from
the same source files the game uses to talk to a Xenia Web Services server (the
Xbox Live replacement used by Xenia Canary Netplay). It contains no game code or assets.

GitHub Actions builds it on every change (Actions, then **Netplay**, then the latest
run, then Artifacts, then `optroy-netcheck-windows-x64`).

## Commands

| Command | What it does |
|---------|--------------|
| `optroy_netcheck check` | Can this PC reach the public Xenia Netplay server? Shows your public address (last part hidden). Read only. |
| `optroy_netcheck check 192.168.1.20:36000` | The same for a server on your own network. |
| `optroy_netcheck sessions [server]` | Lists the Operation Troy matches the server knows about. Read only. |
| `optroy_netcheck selftest <server>` | Registers two test players and runs a whole match life cycle (create, search, join, QoS, modify, leave, host migration, delete), then removes them again. **Only for a test server**: it refuses the public one. |

Add `--verbose` to see every request.

## Testing without any real server

`mock_server.py` imitates a Xenia Web Services server in memory (Python 3, no extra
packages):

```
python3 tools/netcheck/mock_server.py --port 36000
optroy_netcheck selftest http://127.0.0.1:36000
```

## Building

```
git submodule update --init --depth 1 thirdparty/rexglue-sdk/thirdparty/fmt thirdparty/rexglue-sdk/thirdparty/rapidjson
cmake -S tools/netcheck -B out/netcheck -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/netcheck
```

On Linux and macOS it uses the system libcurl, loaded when the program runs.
