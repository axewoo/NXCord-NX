# NXCord-NX

Discord voice chat on the Nintendo Switch that keeps running while you play.
A revival of [Grarak/NXCord](https://github.com/Grarak/NXCord) for 2026 Discord:
gateway v10, voice gateway v8 with `aead_xchacha20_poly1305_rtpsize` transport
encryption, and **DAVE end-to-end encryption** (Discord's
[libdave](https://github.com/discord/libdave) + Cisco
[mlspp](https://github.com/cisco/mlspp) + OpenSSL 3, cross-compiled for Horizon).

> **Beta.** Needs testing on real calls. Please report what `/config/nxcord/log.txt` says.

## Bot account, not your account

NXCord-NX logs in with a **bot token**. Logging in with your own account token
("self-botting") is against Discord's Terms of Service and can get the account
banned, so it isn't supported. You'll appear in voice as your bot (name it
something like "Axel (Switch)"), and only in servers you've invited it to.

1. Go to <https://discord.com/developers/applications>, then **New Application**.
2. Under **Bot**, use **Reset Token** and copy the token. Keep it secret.
3. Under **OAuth2 → URL Generator**, tick the `bot` scope and the **View Channels**,
   **Connect**, **Speak** and **Use Voice Activity** permissions. Open the generated URL to
   invite the bot to your server.

No privileged intents are needed.

## Installing on the Switch

Needs Atmosphère and Ultrahand (or any Tesla overlay loader), plus a **wired USB
headset or USB mic** for talking: the Switch has no built-in mic, and Bluetooth
headsets are output-only.

1. Download `NXCord-NX.zip` from the latest release (or the `NXCord-NX-sd` CI artifact) and
   copy its contents to the root of the SD card.
2. Put your bot token alone on the first line of `/config/nxcord/token.txt`.
3. Reboot. Open Ultrahand (L + D-pad Down + R3), pick **NXCord**, then **Join a channel**.

The sysmodule keeps the call running while you play; the overlay only controls
it. Settings (mic sensitivity, mic boost, call volume, rejoin last channel)
are in the overlay and saved to `/config/nxcord/settings.ini`. Set
`verbose_log=1` there for a detailed log.

## Testing on a PC first (nxcord-cli)

`nxcord-cli` runs the exact same Discord/voice/DAVE code as the Switch build.

```sh
# Arch / CachyOS
sudo pacman -S --needed base-devel cmake git libsodium opus openssl
git clone https://github.com/axewoo/NXCord-NX && cd NXCord-NX
scripts/build-linux.sh        # first run also builds mlspp + libdave (~2 min)

echo 'YOUR_BOT_TOKEN' > ~/.nxcord-token && chmod 600 ~/.nxcord-token
build-linux/platform/linux/nxcord-cli --token-file ~/.nxcord-token --list
```

`--list` prints every server and voice channel as `GUILD_ID/CHANNEL_ID`. Then:

```sh
# Beep test: the bot joins and plays a 440 Hz beep every second for 30 s.
build-linux/platform/linux/nxcord-cli --token-file ~/.nxcord-token --join GUILD_ID/CHANNEL_ID --tone --seconds 30

# Full duplex through PipeWire: your mic goes in, the call comes out.
pw-record --format s16 --rate 48000 --channels 2 - \
  | build-linux/platform/linux/nxcord-cli --token-file ~/.nxcord-token --join GUILD_ID/CHANNEL_ID \
  | pw-play --format s16 --rate 48000 --channels 2 -
```

Add `--verbose` to see every gateway/DAVE step. The status line says
`connected (end-to-end encrypted)` once the DAVE handshake finished.

## Layout

| Path | What |
|------|------|
| `core/` | Portable C++17: TLS (mbedTLS), websocket, gateway, voice + DAVE, Opus mixer |
| `platform/linux/` | `nxcord-cli` test tool |
| `platform/switch/sysmodule/` | Background sysmodule: Discord client, audin/audout, IPC service `nxcord` |
| `platform/switch/overlay/` | Tesla/Ultrahand overlay (libtesla) |
| `scripts/build-switch.sh` | Builds everything for Switch inside `devkitpro/devkita64` |
| `scripts/build-deps.sh` | Builds OpenSSL 3 (Switch), mlspp and libdave into `deps/<platform>` |

## Building for Switch

```sh
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkita64 scripts/build-switch.sh
```

The first run builds OpenSSL 3, mlspp and libdave for Horizon (~3 min). Output
is `build-switch/sd/` (SD card layout) and `build-switch/NXCord-NX.zip`.

## Credits

- Original NXCord by Willi Ye (Grarak), MIT.
- [libdave](https://github.com/discord/libdave) (MIT), [mlspp](https://github.com/cisco/mlspp) (BSD-2),
  OpenSSL (Apache-2.0), libsodium, Opus, mbedTLS, nlohmann/json.
- [libtesla](https://github.com/WerWolv/libtesla) (GPL-2.0) for the overlay, so the overlay
  binary is GPL-2.0.
- Protocol details cross-checked against [D++](https://github.com/brainboxdotcc/DPP).
