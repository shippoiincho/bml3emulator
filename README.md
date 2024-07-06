# Hitachi Basic Master Level-3 Emulator for Raspberry Pi Pico

# 概要

![screenshot](/screenshot00.jpg)

Raspberry Pi Pico で動作する、
日立ベーシックマスターレベル３のエミュレータになります。
PIO が必要だったので pico-sdk を使っています。

以下の機能を実装しています。

- メインメモリ 64KB
- カラーRAM
- CRTC (一部機能のみ)
- ACIA (一部機能のみ)
- IG
- Beep 出力
- 漢字 ROM (オプション)

一応 IG の機能もあるので、マーク５用のソフトもそれなりに動くかと思います。
なお、ROM エリアのバンク切り替えはテストしていないので、バグがあるかもしれません。

# 注意

いつものようにレベル３実機の ROM が必要です。
`bml3rom_dummy.h` を `bml3rom.h` にリネームの上、BASIC ROM (24KB)とフォントROM (8KB) のデータを入れてください。

漢字ROM が必要な場合は、同様に漢字ROM の内容(128KB) を `bml3kanji.h` の中に入れてください。

# 接続

VGA コネクタとの接続は以下の通りです

- GPIO0: HSYNC
- GPIO1: VSYNC
- GPIO2: Blue
- GPIO3: Red
- GPIO4: Green
- GND: GND

RGB の出力と VGA コネクタの間には 330 Ωの抵抗を直列に入れてください。

Beep 出力は GPIO5 を使っています。

![pico connection](/pico.png)

# 画面出力

PIO を使った VGA 画面出力を行っています。
元のライブラリが、640x480 出力だったものを、メモリ節約のため 640x400 出力に変更しています。
よって、長残光ディスプレイでなくてもひらがなの表示ができます。

全画面の書き換えが 1/60 秒で終わらないために、高速で複数画面を切り替えるようなソフトでは激しくちらついたり、
正しく表示されないことがあります。
(I/O 1983 年 3月号 SPACE FALCON など)

# キーボード

USB ホスト機能を使っています。Pico の USB ポートに OTG アダプターをつけて USB キーボードを接続します。
通常の USB キーボード上にに存在しないキーは以下のようになっています。

- GRAPH -> ALT
- かな -> カタカナ/ひらがな
- Break -> Pause/Break
- テンキーの`?` -> NumLock

F12 キーを押すとメニュー画面になります。
ここで、セーブ＆ロード用のファイル選択、およびリセットを行うことができます。

![menu](/screenshot01.jpg)

# カセット

カセットのセーブ＆ロードは、UART 経由の出力と LittleFS を使ってフラッシュへ保存ができます。
通常 UART 経由で入出力されるようになっていますが、
メニューでファイルを選択すると LittleFS 経由になります。

UART経由の場合、単にデータを 16進数にしたものが使用されます。
流し込む場合は、適当なディレイ(1ms くらい？)を入れてください。
ボーレートの切り替えは無視されます。
常に 115200 bps で通信します。

なお、LittleFS の容量は 0x80000 からの 512KiB を確保していますが、これは 1M フラッシュの RP2040-A で使用することを想定しているためです。
2M フラッシュの純正 Pico を使う場合はもっと容量を増やすことができます。

なお littleFS のバックアップは SWD を接続して OpenOCD を使ってください。

# ライセンスなど

このプロフラムは Pico SDK 以外に、以下のライブラリを利用しています。

- [MC6809 エミュレータ(一部改変)](https://github.com/spc476/mc6809)
- [VGA ライブラリ(一部改変)](https://github.com/vha3/Hunter-Adams-RP2040-Demos/tree/master/VGA_Graphics)
- [LittleFS](https://github.com/littlefs-project/littlefs)

# Gallary

![graph](/pic00.jpg)
![Game1](/pic01.png)
![Game2](/pic02.png)