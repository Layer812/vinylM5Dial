# VinylM5 Dial Edition

**VinylM5 Dial Edition** is a vinyl-record-style MP3 player for the **M5Stack M5Dial** and **M5Stack Unit AudioPlayer (N9301)**. The circular display shows an animated record; the Unit AudioPlayer plays music from its own microSD card over UART.

日本語 / English

![Status](https://img.shields.io/badge/status-development%20build-lightgrey)

## Hardware / 対応機器

- [M5Stack M5Dial — official product documentation](https://docs.m5stack.com/ja/core/Dial)
- [M5Stack Unit AudioPlayer — official product documentation](https://docs.m5stack.com/ja/unit/Unit_AudioPlayer)
- Connect the AudioPlayer to **M5Dial PORT.B**: GPIO1 = UART RX, GPIO2 = UART TX, 9600 baud.
- Insert a FAT32 microSD card with MP3 files into the **Unit AudioPlayer**, not the Dial.

## Operation / 操作方法

| M5Dial control / 操作 | Function / 動作 |
| --- | --- |
| Rotate dial clockwise / 右回転 | Next track / 次の曲 |
| Rotate dial counterclockwise / 左回転 | Previous track / 前の曲 |
| Press encoder / ダイヤル押し込み | Play / Stop |
| Tap center / 中央タップ | Pause / Resume |
| Tap upper / 上タップ | Volume + |
| Tap lower / 下タップ | Volume − |
| Tap right / 右タップ | Next folder (N9301 command) |
| Tap left / 左タップ | Previous folder (N9301 command) |
| Swipe left or right / 左右スワイプ | Next or previous track |

The UI displays a rotating label and track/total count without a playback-time readout. It uses `LOADING` during startup and initially sets volume to **10/30** unless a saved preference exists.

**Development status (R1F27):** Device playback and screen operation have been demonstrated in earlier revisions. R1F27 was successfully built and written APP-only to COM21 on Windows, but on-device review reports that the initial screen can remain at `LOADING` with no audio and that the dial takes too many turns to change one track. These problems are **not resolved**. Folder A/B/C operation and continuous playback also remain under review. This is **not a stable release**.

## Build / ビルド

Windows CMD + PlatformIO / Arduino (ESP32-S3):

```bat
pio run -e m5dial
```

Project settings target `m5stack-stamps3`, `espressif32@55.3.311`, and `m5stack/M5Dial@1.0.3`.

Please do not use generic PlatformIO upload as a substitute for the supplied APP-only procedure; it may rewrite bootloader or partition metadata. The development flash script checks the actual partition table and boot selector and refuses an unknown layout. Never erase the full flash just to update this app.

## Relationship to VinylM5 / オリジナル作品

This project is the **M5Dial + Unit AudioPlayer** edition of [VinylM5 for the M5Stack StopWatch](https://github.com/Layer812/VinylM5). It is a separate device target, with hardware decoding via N9301 rather than the StopWatch application's audio pipeline.

- [VinylM5 repository / オリジナルのGitHub](https://github.com/Layer812/VinylM5)
- [Original VinylM5 WebBurner](https://burner.m5stack.com/LRW3MJ) (for **StopWatch**, **not** M5Dial)

## License / ライセンス

Source code is distributed under the **GNU General Public License v3.0 or later** (GPL-3.0-or-later), consistent with the original [VinylM5 license](https://github.com/Layer812/VinylM5/blob/main/LICENSE).

M5Stack / Espressif libraries are separately licensed by their respective authors; see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Album/label artwork, when supplied, is not automatically licensed under the source-code license. Do not assume artwork may be redistributed separately.

Project is not affiliated with M5Stack. Product names and trademarks remain with their owners.
