# Wizardry 6: Bane of the Cosmic Forge PC-9801版 External AutoMap v1.0

## 概要

このリポジトリは、**Wizardry 6: Bane of the Cosmic Forge PC-9801版**をWindows上のPC-98エミュレーターで遊ぶための、非公式External AutoMapです。

`Wizardry6Automap.exe`は、エミュレーター上で動作しているWizardry 6のメモリを外部から読み取り、ゲーム画面とは別のウィンドウにマップを表示します。

標準設定ではAnex86、Neko Project II系、Wizardry Legacy、蘇るPC-9801伝説 第2弾など複数の実行ファイル名を接続候補として登録しています。
`Config\Wizardry6Automap.conf`の`target1`～`target16`を変更することで、別の実行ファイル名も指定できます。

本ツールは、**PC-9801版 Wizardry 6**向けです。
海外MS-DOS版、DOS/V版、Wizardry 7、その他の機種版向けAutoMap機能は含まれていません。

## できること

このツールでは、主に以下のことができます。

* Wizardry 6 PC-9801版のプレイ中に、別ウィンドウでAutoMapを表示
* AutoMap側で記録した踏破情報に合わせたマップ表示
* 現在位置と向きの表示
* 壁、通路、扉、階段、泉、水面、鉄格子など、解析済み固定要素の表示
* 暗闇エリアでのマップ表示制御
* AutoMapウィンドウのマウスによるサイズ変更
* マウスドラッグによるマップ移動
* キャラクターの移動、方向転換、階層移動時に現在位置へ表示を戻す
* マスをダブルクリックしてノートを追加・編集・削除
* ノート上へマウスを重ねると内容を即時表示
* ノートの右クリックでの色変更
* ノート内の座標リンクによるマップ移動
* 対象プロセスやWROOT/WMAZEの自動検出・再検索

## 含まれていないもの

このリポジトリおよびRelease ZIPには、以下のものは含まれていません。

* Wizardry 6のゲームデータ
* PC-98エミュレーター本体
* Wizardry 6本体に由来するゲームプログラムやデータファイル

ゲームを遊ぶには、利用者自身が正規に所有している **Wizardry 6: Bane of the Cosmic Forge PC-9801版** が必要です。

## 対応対象

対象は以下です。

```text
Wizardry 6: Bane of the Cosmic Forge
PC-9801版
```

動作対象は64ビット版Windowsです。

## 導入方法

### 1. Wizardry 6 PC-9801版を遊べる状態にする

最初に、Anex86やNeko Project II系などのWindows上のPC-98エミュレーターで、Wizardry 6 PC-9801版を通常どおり起動できる状態にしてください。

このAutoMapにはゲーム本体やエミュレーターは含まれていません。

### 2. External AutoMapを導入する

このリポジトリの**Releases**からRelease ZIPをダウンロードして、任意のフォルダへ展開してください。

主な構成は以下です。

```text
Wizardry6Automap.exe
README.md
LICENSE.txt

Config\
  Wizardry6Automap.conf

Source\
  ...
```

### 3. Wizardry6Automap.exeを起動する

`Wizardry6Automap.exe`を実行します。

対象エミュレーターのメモリを読み取るため、Windowsのユーザーアカウント制御（UAC）で管理者権限を要求します。表示された場合は内容を確認して許可してください。

AutoMapとエミュレーターは、どちらを先に起動しても構いません。

標準設定では、AutoMapは`Config\Wizardry6Automap.conf`に登録された複数の対象プロセスを順番に検索します。

対象プロセスを検出すると、Wizardry 6のWROOT/WMAZEデータをメモリ上から検索します。

正常に検出できれば、ゲーム内の移動に合わせてAutoMapウィンドウの表示が更新されます。

## AutoMap設定

`Config\Wizardry6Automap.conf`の`[automap]`セクションで設定できます。

標準設定は以下です。

```ini
[automap]
target1="anex86.exe"
target2="np21.exe"
target3="np21nt.exe"
target4="np2sx.exe"
target5="np2sxnt.exe"
target6="np2nt.exe"
target7="np2.exe"
target8="np2w.exe"
target9="np2x64w.exe"
target10="np21w.exe"
target11="np21x64w.exe"
target12="Next.EXE"
target13="WIZ6.EXE"
target14="ウィザードリィ6.EXE"
target15=
target16=
enable=true
hide_in_dark_zones=true
width=512
height=512
position_x=-1
position_y=-1
```

### target / target1～target16

接続対象として検索するWindowsプロセスの実行ファイル名です。

標準設定では`target1`～`target14`に既知の候補を登録し、`target15`と`target16`はユーザー追加用として空欄にしています。

大文字と小文字は区別しません。

次の形式を使用できます。

```ini
target15="My Emulator.exe"
target16="custom.exe"
```

空白を含む実行ファイル名は二重引用符で囲んでください。

旧バージョンとの後方互換性のため、従来の`target`も引き続き使用できます。

```ini
target="anex86.exe"
```

`target`が存在する場合は接続候補に含まれ、`target1`～`target16`より先に検索されます。

新規作成される標準CONFには`target`は書き出しません。

検索順序は以下です。

```text
target
target1
target2
...
target16
```

空欄のtargetは無視されます。

### enable

```ini
enable=true
```

AutoMap機能を有効にするかどうかを指定します。

`false`にするとRAM監視とマップ表示を無効にします。

通常は`true`のままで使用してください。

### hide_in_dark_zones

```ini
hide_in_dark_zones=true
```

`true`の場合、ゲーム内の暗闇エリアでAutoMap表示を制限します。

快適性を優先する場合は`false`にしてください。

この判定は元のWizardry 6 Automap Modの動作を引き継いでいます。

### width / height

```ini
width=512
height=512
```

AutoMapウィンドウの初期地図領域サイズを指定します。

```text
width  : 320～4096、または-1
height : 240～4096、または-1
-1     : Windows側の既定サイズ
```

標準では512×512ピクセルです。

起動後はウィンドウ枠をマウスでドラッグしてサイズを変更できます。

### position_x / position_y

```ini
position_x=-1
position_y=-1
```

AutoMapウィンドウ左上の初期位置を指定します。

```text
position_x : 画面左端からの横位置
position_y : 画面上端からの縦位置
-1         : Windows側の自動配置
```

## ノート機能

### ノートを追加・編集する

ノートを付けたいマスを左ダブルクリックします。

* ノートがないマス：新しいノートを追加
* ノートがあるマス：既存の内容を編集
* 入力内容を空欄にしてOK：ノートを削除
* Cancel：変更せずに閉じる

ノートは1件につき最大1024文字です。

### ノートを読む

ノートがあるマスへマウスカーソルを重ねると、待ち時間なしでノート本文を表示します。

### ノート色を変更する

ノートがあるマスを右クリックすると、Windowsの色選択画面が開きます。

選択した色はノートと一緒に保存されます。

### 座標リンクを使用する

Altキーを押しながらマスを左クリックすると、次の形式の座標をクリップボードへコピーします。

```text
{level:quadrant:x:y}
```

この文字列をノート本文へ書き込み、そのノートをCtrlキーを押しながら左クリックすると、座標リンクの場所へマップ表示を移動します。

ノートは以下へ保存されます。

```text
Config\Wizardry6Automap_notes.bin
```

ノートはゲームのセーブスロットごとではなく、すべてのセーブデータで共通です。

## 踏破状態

AutoMapの踏破状態は以下へ保存されます。

```text
Config\Wizardry6Automap_visited.bin
```

Wizardry 6にはWizardry 7のようなゲーム内AutoMap用の踏破情報がないため、本ツール自身が以下の情報を記録します。

* 実際に踏んだマス
* 壁や扉で遮られず見えた隣接マス

踏破状態はゲームのセーブスロットごとではなく、すべてのセーブデータで共通です。

新しい踏破情報がある場合は定期的に保存され、AutoMap終了時にも保存を試みます。

このファイルを削除すると、AutoMap側の踏破状態が初期化されます。

## ウィンドウ配置

AutoMapウィンドウは標準で512×512です。

初期位置を固定したい場合は、`Config\Wizardry6Automap.conf`の以下を変更してください。

```ini
position_x=0
position_y=0
```

起動後はウィンドウを通常のWindowsアプリケーションと同じように移動・サイズ変更できます。

マップ上でマウスの左ボタンを押したままドラッグすると、現在位置を中心とした表示から移動して周囲やマップの端を確認できます。

キャラクターが移動、方向転換、階層移動した場合は、表示位置が現在位置へ戻ります。

## WROOT/WMAZEの検出と再接続

AutoMapは、対象エミュレーターまたはゲームのWindowsプロセスへ接続した後、メモリ上からWizardry 6のWROOTコード署名とWMAZEデータを自動検索します。

検出済みのメモリが無効になった場合は、WROOT/WMAZEを再検索します。

待機中には、状態に応じて次のような表示が出ます。

```text
Waiting for configured target process...
Waiting for the maze...
Waiting for map data...
```

接続対象を変更している場合も、CONFに登録されたプロセスを順番に検索します。

## 現在の制限

本ツールは、PC-9801版Wizardry 6の既知のメモリ構造を使用しています。

そのため、MS-DOS版、DOS/V版、他機種版、または異なるメモリ構造を持つ版には対応していません。

暗闇判定や水面など、一部のマップ表示判定は元のWizardry 6 Automap Modの仕様を引き継いでいます。

そのため、`hide_in_dark_zones=true`の場合、一部のマップでは水面などの表示が暗闇判定の影響を受ける場合があります。

元のAutomap Modとの互換性と安定した挙動を優先し、これらの判定は現在そのまま使用しています。

## 注意事項

このツールは非公式です。

本ツールの導入、動作、不具合などについて、使用するエミュレーター、Wizardryの権利者・販売元、AutoMap Modの原作者・refactor作者、その他の公式サポート窓口へ問い合わせないでください。

また、ゲーム本体の著作物は一切含めていません。利用者自身が正規に所有しているゲームデータを使用してください。

本ツールの使用は利用者自身の責任で行ってください。利用前にセーブデータと`Config`フォルダをバックアップすることを推奨します。

本ツールは対象プロセスのRAMを読み取りますが、ゲーム側のRAMへの書き込みは行いません。

## Sourceフォルダについて

このリポジトリの`Source`フォルダには、Wizardry6Automap.exeに対応するソースコードとビルド用ファイルを格納しています。

主な構成は以下です。

```text
Source\
  Wizardry6Automap.cpp
  Wizardry6Automap.exe.manifest
  Wizardry6Automap.rc
  build_msys2_mingw64.bat
  Readme_build.txt
```

現在の外部版はWindows標準のWin32 APIとGDIを使用して描画します。

SDL2、OpenGL、およびMinGWランタイムDLLの別途配布は必要ありません。

ビルド方法の詳細は`Source\Readme_build.txt`を参照してください。

## License

This project is distributed under the **GNU General Public License version 2**.

This repository contains code derived from the Wizardry 6 & 7 Automap Mod.

See [`LICENSE.txt`](LICENSE.txt) and the copyright/license notices in the source files for details.

## Acknowledgements

This project is based on the work of the **Wizardry 6 & 7 Automap Mod**.

I would like to express my deepest gratitude to the original author and the refactor author.
Without their work, this PC-98 External AutoMap adaptation would not have been possible.

Original: Copyright (C) 2014 KoriTama
Wizardry 6 Automap Mod:
https://www.moddb.com/mods/wizardry-6-automap-mod

Wizardry 7 Automap Mod:
https://www.moddb.com/mods/wizardry-7-automap-mod

Refactor: Copyright (C) 2025 DungeonCrawl-Classics.com
Wizardry 7 Map Details:
https://dungeoncrawl-classics.com/wizardry-series/7-crusaders-of-the-dark-savant/wizardry-7-map-details/

This project is an unofficial adaptation for the PC-9801 version of Wizardry 6.
