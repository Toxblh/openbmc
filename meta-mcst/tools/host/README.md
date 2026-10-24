# The Elbrus host's side of REIMU

Tools that run on the host (1Э8СВ-uATX, E8C2, ALT-based Linux), not on the BMC:

| file | what |
|---|---|
| `reimu-host.sh` | KVM: the X screen into the ASPEED VGA, H.264 from the GPU (`reimu-h264.py`), EDID, USB hub |
| `reimu-hostd.py` + `.service` + `.conf` | **the host agent**: inventory, health, GPU fan policy for the BMC (below) |
| `install.sh` | the installer the BMC serves (`https://BMC/agent/install.sh`) and puts on its USB disk: fetches/checks (SHA-256) the agent's files, installs, pairs, starts; `--uninstall [--purge]` |
| `mkbundle.sh`, `agent-README.txt` | the build of that bundle and of the USB disk image (recipe `reimu-agent-bundle`) |
| `tests/test_install_sh.py` | install.sh against a fake BMC (pin, hashes, curl/wget/python3, idempotent, uninstall, USB, self-contained) |
| `tests/test_reimu_hostd.py` | its unit tests (67; fake /sys, /proc; `python3 tests/test_reimu_hostd.py`) |

# Host agent (reimu-hostd) and host link (reimu-hostlink)

Like the server vendors' host agents (Dell iDRAC Service Module, HPE AMS,
Lenovo XClarity, IBM): the host tells its BMC what it is and how it is doing;
the BMC may ask it for a few fixed things.

```
 Elbrus host (ALT Linux, e2k)                           BMC (AST2500, OpenBMC)
 ┌──────────────────────────────┐   board LAN, no cable   ┌────────────────────────────────────────┐
 │ reimu-hostd.py (root,python3)│   needed (IPv6 link-     │ reimu-hostlink (C, sd-bus, OpenSSL)    │
 │  /sys /proc: CPU NUMA PCIe   │   local fe80::9aa7:b0ff: │  TLS [::]:5564 bound to eth0           │
 │  EDAC disks NICs amdgpu      │   fe06:f7e%enp1s1f1,     │  UDP [::]:5564 discovery (link-local)  │
 │  GpuFan: policy, floor 30 %, │   IPv4 fallback)         │  hl-proto: auth, limits, state, events │
 │   re-apply after GPU reset,  │ ── TLS 1.2+/1.3 ───────► │  /var/lib/reimu-hostlink: key cert.pem │
 │   auto when link lost/exit   │    cert pinned (SHA-256) │     policy edac          (rwfs, 0700)  │
 │  Controller: allow-list only │    HMAC-SHA256 challenge │  D-Bus xyz.openbmc_project.Reimu.HostLink
 │   gpu_fan gpu_turbo          │ ◄── policy/control ───── │   Json, Pair/Unpair, SetGpuFan,        │
 │   os_power edac_load         │                          │   GpuTurbo, OsPower, LoadEdac          │
 └──────────────────────────────┘                          │  Sensor.Value Host_GPU_* Host_Disk_Max │
                                                           │   → mapper → Redfish, IPMI SDR         │
                                                           │  events → journal, phosphor-logging    │
                                                           │  /run/reimu-hostlink/gpu-temp → fand   │
                                                           │  reimu-fand TurboLeft → GPU turbo      │
                                                           └──────────┬─────────────────────────────┘
                                                                      │ bmcweb D-Bus REST
                                                           web UI «Хост» (js/pages/host.js)
```

## Install on the host (as root)

Nothing needs to be copied to the host first: the BMC hands out the installer.
On the BMC's web page **Хост** (needs python3 on the host; the one-time
pairing string is valid 15 min, «Новый ключ» makes another):

* **«Подключить агент хоста»** shows commands for the host's terminal, over
  the board's LAN (IPv6 link-local, every interface tried: no cable, no DHCP)
  or over IPv4:

  ```
  s=$(curl -fsS --pinnedpubkey 'sha256//<bmcweb key>' -k https://192.168.10.79/agent/install.sh) && printf '%s\n' "$s" | sudo sh -s -- --pair 'reimu1:…'
  ```

  curl pins bmcweb's public key (its pin comes with `Pair()`: reimu-hostlink
  reads `/etc/ssl/certs/https/server.pem`, `HTTPS_CERT`), so nobody on the
  network can hand the host another install.sh: no trust on first use.
  install.sh carries the SHA-256 of the agent's files (written in at the
  BMC's build) and refuses any other file; the agent pins reimu-hostlink's own
  certificate (`fp` in the pairing string). A wrong pin: curl stops before
  the request, nothing runs.
* **«USB-диск с установщиком»** (no network at all): the BMC shows the host a
  read-only USB disk «REIMU-AGENT» (`reimu-agent-disk`: a FAT image made at
  build time, the pairing string written into its PAIRING.TXT; gadget
  `reimu_agent` on vhub port p3, else p2: never p1, the KVM's HID, nor p4/p5,
  virtual media). On the host `sudo sh /run/media/$USER/REIMU-AGENT/install.sh`
  (without a desktop: `sudo mount -o ro /dev/disk/by-label/REIMU-AGENT /mnt &&
  sudo sh /mnt/install.sh`). The BMC takes the disk away when the agent has
  paired and connected, or after 15 min.
* **«Скачать установщик»**: `reimu-agent-install.sh`, install.sh with the
  agent's files appended and the pairing string set; carry it to the host,
  `sudo sh reimu-agent-install.sh`.

install.sh copies the agent to `/usr/local/lib/reimu/`, the unit to
`/etc/systemd/system/`, `/etc/reimu-hostd.conf` (kept when there), pairs
(`/etc/reimu-hostd/secret`, 0600), enables and starts `reimu-hostd.service`
and waits for «connected to the BMC». Run again: an update (with `--pair`: a
new pairing). From a copy of this directory: `sudo sh install.sh --pair
'reimu1:…'` (or the agent's own `sudo ./reimu-hostd.py install --pair …`).
The page then shows «подключён»; `journalctl -u reimu-hostd` on the host.

Optional: `EDAC_AUTOLOAD=yes` in `/etc/reimu-hostd.conf` loads `e2k_edac` at
start (ECC counters); without it the page has a button «Загрузить e2k_edac».
`smartctl` (smartmontools), when installed, adds SMART health.
Pair again: the same `install --pair` (or `pair --pair`) with a new string.
Remove: `sudo sh install.sh --uninstall [--purge]` (or `sudo ./reimu-hostd.py uninstall`); revoke from the BMC: «Отключить агент».

## Transport

* The BMC listens on `[::]:5564` (TCP: TLS, UDP: discovery), dual stack,
  `SO_BINDTODEVICE eth0` (`IFACE`): only the board's LAN, never eth1 (its
  6.18 driver hangs) nor anything else. `ALLOW_IPV4=no` makes it link-local
  only. The image has `iptables` but no rules; none are added (the binding
  and the authentication do the job, and a rule set would be the only one on
  the BMC to keep).
* The agent tries, each time it connects: the BMC's EUI-64 link-local
  address (from `BMC_MAC`, given by the pairing: the BMC's eth0 MAC) on every
  interface that is up (`IFACES` to restrict); link-local addresses that
  answered the multicast probe (`REIMU-HOSTLINK? 1` to `ff02::1` port 5564,
  only answers carrying the pinned certificate's fingerprint prefix count:
  the MAC may change with the FRU's `set_mac`); then `BMC_IPV4`. The last
  link-local address that worked goes first. Without the cable there is no
  DHCP and no IPv4: link-local still works (measured 0.3 ms on the board).
* Reconnect: back-off 1, 2, 4 … 60 s; the BMC rebooting or switching A/B
  slots, the host rebooting, the cable or IPv4 coming and going are all just
  a reconnect. TCP keepalive (30 s + 3×10 s) and a ping every 10 s find
  half-open connections (the BMC's reboot resets the host on this board).

## Protocol (version 1)

One JSON object per line, UTF-8, `{"v": 1, "t": TYPE, …}`. Unknown types and
unknown fields are ignored (newer peers can add them); another major `v` is
refused with `error/version`.

| from | t | fields |
|---|---|---|
| BMC | `hello` | `nonce` (128 random bits, hex), `server`, `ver` — first, on every connection |
| agent | `auth` | `id`, `cnonce`, `mac` = HMAC-SHA256(key, `"reimu-hostlink-auth-v1\n" nonce "\n" cnonce "\n" id`), `ver`, `host`, `transport` |
| agent | `pair` | `token` (from the pairing string), `id`, `host` — instead of `auth`, once |
| BMC | `welcome` | `mac` = HMAC-SHA256(key, `"reimu-hostlink-srv-v1\n" cnonce "\n" nonce`) (the BMC proves the key too), `policy`, `ping` |
| BMC | `paired` | `key` (256 bits, hex), then the BMC closes; the agent connects again with `auth` |
| BMC | `error` | `code`: `auth`, `pair`, `version`, `malformed`, `rate`, …; then closed |
| agent | `inventory` | `data`: os, cpu (model, threads, NUMA), memory (DIMMs from EDAC), pci[], disks[], nics[], gpu — at connect, then when it changes (checked hourly) |
| agent | `metrics` | `data`: uptime, load, mem, temps[] (hwmon), edac {loaded, ce_total, ue_total, mcs[{dimms[], csrows[]}]}, gpu {temp_edge/junction/mem, crit_*, fan_rpm, pwm, pwm_pct, pwm_enable, power_w, sclk/mclk, busy, VRAM, link, policy}, disks[{temp, smart}], agent — every `INTERVAL` (10 s) |
| agent | `event` | `data` {kind, text}: e.g. `gpu_fan_reapplied`, `gpu_fan_auto_link_lost` |
| agent | `result` | `id`, `op`, `ok`, `text`/`err` — the answer to a `control` |
| agent | `pong` | — |
| BMC | `policy` | `gpu_fan` {mode auto/manual, duty}, `turbo_left` (s) — on every change; also in `welcome` |
| BMC | `control` | `id`, `op` (`os_power` + `action` poweroff/reboot, `edac_load`) |
| BMC | `ping`, `inventory?` | — |

Limits on the BMC: 2 KiB a line before authentication, 128 KiB after; a
token bucket of 40 messages refilled 4/s (`error/rate`); 10 s to
authenticate; at most 4 connections not authenticated (more are closed at
once); 10 malformed lines after authentication close the connection; a new
authenticated connection replaces the old one (the host rebooted).
The agent: 64 KiB a line from the BMC.

## Security decisions (a home server, the board's own LAN)

* **TLS with a pinned certificate**, not a bare HMAC per message: OpenSSL is in
  the image anyway (bmcweb), so TLS costs the daemon ~15 KiB; it gives
  confidentiality for free (the key during pairing, the host's inventory).
  The certificate is reimu-hostlink's own (EC P-256, self-signed, made at the
  first start, `/var/lib/reimu-hostlink/cert.pem`, rwfs, kept across A/B
  updates), not bmcweb's: replacing the HTTPS certificate doesn't break the
  agent. The agent pins its SHA-256 from the pairing string: no
  trust-on-first-use. A rwfs wipe (revert to manufacturing) makes a new
  certificate and key: pair again.
* **Pairing**: a one-time token (128 bits) from `Pair()` (the web button),
  valid 15 min, 5 wrong tries void it. It is traded over the pinned TLS for a
  256-bit key; the BMC keeps the key in `/var/lib/reimu-hostlink/key` (0600,
  directory 0700), the host in `/etc/reimu-hostd/secret` (0600, root).
  `Unpair()` deletes the key and drops the connection: the agent is refused
  and retries every 5 min until paired again.
* **Authentication** is a challenge–response (HMAC-SHA256 over the BMC's
  fresh nonce and the agent's), both ways: a recorded `auth` line is useless
  on another connection (tested), the key never travels after pairing.
* **No secrets in logs**: the daemon never logs lines or tokens; failed
  authentications are logged at most once a minute with the peer's address;
  the agent masks any hex string of 32+ characters in its log. The `Json`
  property has no token or key (tested); `Pair()` returns the token only to
  its caller.
* **Allow-list**: the agent executes only `systemctl poweroff|reboot`,
  `modprobe e2k_edac` (or `edac_e2k`; fixed argv, no shell) and writes only the amdgpu
  `pwm1`/`pwm1_enable`; each op can be disabled on the host (`ALLOW`). There
  is no way for the BMC to run anything else, and the host can only report:
  nothing the agent sends changes the BMC's configuration.
* The daemon is small and never blocks: one poll loop, non-blocking TLS,
  bounded buffers, D-Bus calls async; `Nice=5`, `Restart=always`, watchdog.

## GPU fan policy

The BMC keeps it (`/var/lib/reimu-hostlink/policy`) and sends it at every
connection: auto (the driver), manual DUTY (20..100 at the BMC; the host
never goes below `GPU_FAN_FLOOR`, 30 %), turbo (100 % for 1..120 min, then
back). reimu-fand's «Турбо» (`TurboLeft`, read every 5 s) turns the GPU fan to
100 % as well (`FOLLOW_FAND_TURBO`). The agent checks the card every 2 s: the
driver's reset after an amdgpu hang (`pwm1_enable` back to 2, fans stopped:
the IOHUB behind the card reached 90 °C), or the hwmon coming back under
another number, is set again (event `gpu_fan_reapplied`). No word from the BMC
for `LINK_LOST_SEC` (60 s), the agent stopping (and `ExecStopPost` after a
crash): the driver's automatic control again.

## Events (BMC journal; warning and critical also phosphor-logging → Redfish EventLog)

| kind | level |
|---|---|
| `ecc_ce` corrected ECC errors increased (names the DIMMs) | warning |
| `ecc_ue` uncorrectable ECC errors increased | critical |
| `gpu_hot` GPU edge/junction/memory at its warning / critical threshold (85/95, 95/105, 95/105 °C) | warning / critical |
| `gpu_temp_ok` back 5 °C below the warning | notice |
| `agent_lost` no agent for `LOST_SEC` (60 s) / `agent_back` | warning / notice |
| `agent_connected`, `agent_disconnected`, `paired`, `unpaired`, `control`, `os_power`, `host` (the agent's own) | notice / info |

The ECC counts the BMC saw are kept (`edac`): a BMC reboot doesn't report the
same errors again; lower counts (the host rebooted) are the new base.
`Heartbeat` (ok / late / lost / never) is there for a later host watchdog.

## Tests

* `tools/host/tests/test_reimu_hostd.py` — the agent (67 tests: fake sysfs
  incl. EDAC, amdgpu hwmon with `pwm1_enable` flipping back to 2 and hwmon
  renumbered, PCIe, allow-list, floor, link loss, turbo, transport choice).
* `meta-common/recipes-mcst/reimu-hostlink/files`: `make test
  NATIVE_SSL=…/openssl-native/usr` in the build container — the C unit tests
  (JSON, config, protocol: auth fail, replay, oversize, malformed, version,
  rate, pairing, events) and `tests/test_protocol.py`: the daemon built
  without D-Bus over real TLS, and the real agent end to end (pairing,
  control, GPU reset, BMC gone → auto, back → policy again, lost/back).
* `tests/ab64/test_hostlink_qemu.py` — the image in QEMU with the agent on
  the Mac (D-Bus sensors, REST, control, link drop, restart); `HL_E2E=DIR` adds the browser test `tests/e2e/host.qemu.mjs` of the page.

## Not in v1 (later)

* **Host watchdog**: `Heartbeat` is there; a policy (lost for N min → NMI,
  reset, screenshot) belongs to the BMC's power control.
* **Crash screenshot**: the KVM's JPEG at `agent_lost` / a kernel panic seen
  on the serial console (reimu-serial has it).
* **Telegram / mail** notifications of the events (a small BMC-side
  forwarder of the journal entries this daemon writes).
* **Agent under Lintel / Windows**: the boot chain is PNS → native E2K Linux
  (this agent) or PNS → Lintel (x86 binary translator) → Windows/x86 OS.
  Under Lintel the OS sees an emulated x86 machine: no EDAC, no e2k sysfs, the
  GPU through Windows' driver. An agent there would be a Windows service
  (Python or Go) speaking the same protocol: inventory from WMI, GPU from the
  AMD ADL/ADLX API (temperatures, fan), ECC from Windows' WHEA events; the
  network the same (the board LAN, link-local `fe80::…%N`). `os.boot` in the
  inventory says which one reports.
* Per-message HMAC inside TLS is not done: TLS already authenticates and
  orders every record after the challenge–response.
