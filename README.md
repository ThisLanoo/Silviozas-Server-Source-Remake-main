# Silviozas Server Source (Remake)

Standalone backend for the Silviozas Premium Proxy control channel — the
"Proxy Server" the client connects to. No relay / no real backend needed:
it answers the protocol itself.

## What it does
- Growtopia-style **ENet** transport (crc32 checksum + range-coder compression,
  old packet header) on `0.0.0.0:33131`.
- Decrypts/encrypts the type-4 **SilviozasPacket** protocol (4-byte XOR).
- Answers the client's requests:
  - `[OnNewLogin]` / `[USER]` / ...  -> `Ping Success.`
  - `[GetServerData]` -> `[ServerData]` (maint / server / port) — unlocks game entry
  - `[GetMeta]` / `[GetMetaSign]` -> value from `meta.txt` / `metasign.txt` (else ack)
  - `[CheckItemVendPrices]` / `[CheckAverageItemPrices]` -> `[ItemPricesList]` from `itemprices.txt`
  - `[OnModDetect]` -> aggregates hidden mods
  - `[OnGiveMods]` -> `[OnDialogRequest]` "Hidden Moderator Checker" dialog
- Server -> client extras:
  - `[PROXYCONSOLE]` -> a line in the proxy's exe console (auto every 60s)
  - `[OVERLAY]` -> big text on the game screen (auto every 10 min)
  - Type a line in the server console + Enter -> instant overlay to all clients
- Live client counter in the window title.

## Build (Windows, MSVC)
Requires Visual Studio (C++ Desktop workload):
```
build.bat
```
Output: `build\silviozas-server.exe`.

## Run
Run the exe, then point the proxy config (data11) at it:
```
PGTserver|YOUR_SERVER_IP
PGTport|33131
```

## Runtime config (no rebuild, read from the exe folder)
- `serverdata.txt` — `maint|growtopia_server_ip|port` for `[ServerData]`
- `send.txt` — exe-console line (GT color codes: `` `2 ``green `` `4 ``red `` `9 ``blue)
- `overlay.txt` — in-game overlay line
- `meta.txt` / `metasign.txt` / `itemprices.txt` — optional response values
- `hdnmods.txt` — hidden-mod store (auto-written)

## Layout
- `silviozas-server.cpp` — the server
- `connectors/` — minimal deps (utils, packet struct, proton variant)
- `enet-master/` — the ENet fork it links against

---

Discord: **.dylanogt**
