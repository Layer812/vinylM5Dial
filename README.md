# VinylM5 Dial Edition

M5Stack **M5Dial** と **Unit AudioPlayer** を使った、レコードプレイヤー風MP3プレイヤーです。円形ディスプレイにレコード盤が回転し、Unit AudioPlayerのmicroSDカードに保存した音楽を再生します。

*A vinyl-style MP3 player for M5Stack M5Dial and Unit AudioPlayer, with a spinning record on the round display.*

## インストール / Installation

**M5Burner シェアコード / Share code:**

```text
7mmNQf8WeWAV9BgF
```

[M5Burner](https://docs.m5stack.com/en/quick_start/m5burner/m5burner) からシェアコードを使用してインストールできます。

Use the share code above in M5Burner to install the application.

## 対応製品 / Hardware

- [M5Stack M5Dial（製品情報 / Product）](https://docs.m5stack.com/ja/core/Dial)
- [M5Stack Unit AudioPlayer（製品情報 / Product）](https://docs.m5stack.com/ja/unit/Unit_AudioPlayer)

M5Dialの**PORT.B**とUnit AudioPlayerをUARTで接続します（GPIO1 = RX、GPIO2 = TX、9600bps）。MP3ファイルは**Unit AudioPlayer側**のFAT32形式microSDカードに保存してください。

Connect the Unit AudioPlayer to M5Dial **PORT.B** over UART (GPIO1 RX, GPIO2 TX, 9600bps). Put MP3 files on the FAT32 microSD card in the **Unit AudioPlayer**.

## 操作方法 / Controls

| 操作 / Control | 機能 / Function |
| --- | --- |
| ダイヤル右回転 / Clockwise | 次の曲 / Next track |
| ダイヤル左回転 / Counterclockwise | 前の曲 / Previous track |
| ダイヤルの押し込み / Press dial | 再生・停止 / Play–Stop |
| 中央タップ / Tap center | 一時停止・再開 / Pause–Resume |
| 画面上タップ / Tap upper area | 音量＋ / Volume up |
| 画面下タップ / Tap lower area | 音量－ / Volume down |
| 画面右タップ / Tap right | 次のフォルダ / Next folder |
| 画面左タップ / Tap left | 前のフォルダ / Previous folder |
| 左右スワイプ / Swipe horizontally | 曲送り・曲戻し / Next or previous track |

曲数表示、レコードラベルの回転、全曲ループ、前回曲・音量の記憶に対応しています。音量の初期値は**10/30**です（保存済み設定を優先します）。

The display shows the track number and total count on a rotating record label. The player supports all-track repeat and saved track/volume preferences. Default volume is **10/30** unless a saved setting exists.

## ソースからビルド / Build from source

PlatformIO と Arduino-ESP32 を使用します。

```bat
pio run -e m5dial
```

Windowsのビルド・APP-ONLY書き込みには、同梱の `BUILD_AND_FLASH_CMD.cmd` を使用できます。COMポートは環境に合わせて指定してください。

For Windows, `BUILD_AND_FLASH_CMD.cmd` provides build and APP-only flashing. Specify the COM port for your device.

## VinylM5 シリーズ / Related project

VinylM5 Dial Editionは、M5Stack StopWatch用のレコードプレイヤーアプリ **VinylM5** のM5Dial版です。

VinylM5 Dial Edition is the M5Dial + Unit AudioPlayer companion to the original VinylM5 for M5Stack StopWatch.

- [VinylM5 — GitHub](https://github.com/Layer812/VinylM5)
- [VinylM5 — WebBurner（StopWatch用 / StopWatch version）](https://burner.m5stack.com/LRW3MJ)

## ライセンス / License

ソースコードは [GNU General Public License v3.0 or later](LICENSE)（GPL-3.0-or-later）で公開しています。外部ライブラリのライセンスは [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)、レコードラベル画像については [COPYRIGHT_ARTWORK.md](COPYRIGHT_ARTWORK.md) をご覧ください。

Source code is available under **GPL-3.0-or-later**. Third-party dependencies and artwork remain subject to their own license and rights notices.

M5Stack等の製品名・商標は各権利者に帰属します。
