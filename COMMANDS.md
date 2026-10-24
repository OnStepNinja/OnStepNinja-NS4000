# LUNA OnStepNinja NS-4000 - custom commands and Web API

All standard OnStep 4.24j LX200 commands work as in OnStep. These are the additions.

## LX200 commands (TCP 9999 / 9998, serial, or `/api/command?cmd=`)

| Command | Reply | Meaning |
|---|---|---|
| `:=V#` (`:*V#`) | `LUNA OnStepNinja NS-4000 v1.1.0#` | firmware name and version. `:GVP#` = `On-Step` and `:GVN#` = `4.24j` are unchanged for compatibility |
| `:=SD#` | | sound the buzzer |
| `:=SD1#` / `:=SD0#` | | beep every 3 seconds during GOTO on / off |
| `:=SH1#` / `:=SH0#` | | camera shutter output on / off (pin Shutter3000) |
| `:SX93,n#` | `1` | GOTO speed: `5` 50%, `4` 75%, `3` 100%, `2` 150%, `1` 200% of the base rate; `a`..`z` = 50%..about 260% in finer steps |
| `:SX94,d#` | `1` | set the base slew rate (100%) in deg/s, 0.001..20. Saved in NV. Refused while slewing. Limited so that 200% does not exceed `NS3000_MAX_STEP_PPS` |
| `:GX9G#` | `0.800#` | base slew rate in deg/s |
| `:GXWP#` | `45000,252#` | step rate limit in pps, and how many times sidereal that is |
| `:GXW0#` .. `:GXW3#` | | STA IP, AP name, AP IP, STA status (3 = connected) |
| `:GXW4#` / `:GXW5#` | | TCP 9999 / 9998 statistics: accepted,peer closed,idle closed,late replies (over 1 s),commands,longest ms |
| `:SXJ0,1#` / `:SXJ0,0#` | `1` | tracking jitter measurement start (clears the record) / stop |
| `:GXJ0#` | | count,average us,max us,measuring(1/0) |
| `:GXJ1#` / `:GXJ2#` | | counts of RA interrupt lateness <2us,<10us,<50us / <200us,<1000us,1000us or more |
| `:$BR n#` / `:$BD n#`, `:%BR#` / `:%BD#` | | backlash in arcsec, 0..3600 (standard OnStep; set in NS-4000 Control) |

The jitter measurement records how many microseconds the RA motor interrupt starts after its scheduled time. It always includes a few microseconds of ESP32 interrupt overhead; compare before and after a change. See also `:GXFA#` (worst loop load %) and `:GXFC#` (average loop time).

## Web API (port 80)

Same endpoints and reply format as LUNA AiBridge (`{"status":"success","message":"...","timestamp":n,"data":...}`):

| Endpoint | |
|---|---|
| `/api/command?cmd=Xx` | send one LX200 command (`:` and `#` are added) |
| `/api/status`, `/api/goto?ra=&dec=`, `/api/stop`, `/api/buzzer` | status, GOTO, stop, buzzer |
| `/api/system/info` | version, network, free memory, SPIFFS, handbox state |
| `/api/handbox` | BLE handbox: state, address, RSSI, battery voltage (mV), statistics |
| `/api/files/list`, `/api/files/upload`, `/api/files/delete`, `/api/ui/upload` | files on SPIFFS |
| `/api/wifi/config` | Wi-Fi / PIN settings (only for 30 minutes after power-on) |
| `/api/external/alpaca`, `/api/external/discover`, `/api/external/status`, `/goto`, `/stop` | Alpaca relay and discovery (private LAN addresses only) |
| `/api/v1/telescope/0/*`, `/management/*` | ASCOM Alpaca Telescope |
| `/mcp` | MCP tools (JSON-RPC, same form as LUNA): `get_system_info`, `get_mount_status` (read only: RA, Dec, tracking, handbox), `buzzer` (once every 2 s), `get_license_info`, `fs_list`, `fs_read`, `fs_write`, `fs_append`, `fs_delete`. No tools that move the mount |

Guard: pages of this device, non-browser tools and the pages of AiBridge Host / LUNA Host Lab need no PIN. Pages of other web sites need the PIN (`?pin=` or the `X-PIN` header).

## BLE handbox

The handbox advertises a 128-bit service UUID `af926cb9-6e68-4e48-ae4f-7db856cea953`; its characteristic `8ec6a12d-99f3-4c2c-9e16-c6d188d5e9d0` notifies 4 bytes:
`[0] 0x4E`, `[1]` buttons (bit0 N, bit1 S, bit2 E, bit3 W, bit4 Stop, bit5 Speed), `[2]` battery voltage in 20mV units (`0xFF` = not measured), `[3]` sequence number.
The NS-4000 pairs (bonds) with the first handbox found within 30 minutes after power-on and then accepts only that one. While the handbox is moving the mount, the mount stops if the reports stop for 1 second or the link is lost.
