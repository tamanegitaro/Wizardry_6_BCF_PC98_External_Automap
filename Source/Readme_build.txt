PC-98 Wizardry 6 Automap - ビルド手順
==========================================

この文書は、Wizardry6Automapをソースコードからビルドする人向けです。
通常プレイだけの場合は、配布されているWizardry6Automap.exeを使用してください。


1. 対象環境
------------

・64ビット版Windows
・MSYS2
・MSYS2 MINGW64シェル
・MinGW-w64 64-bit GCC
・MinGW-w64 binutils

UCRT64、CLANG64、MSYSシェルではなく、スタートメニューから「MSYS2 MINGW64」を開くことを推奨します。


2. MSYS2の準備
---------------

MSYS2をインストールした後、MSYS2 MINGW64シェルを起動します。

最初にMSYS2全体を更新します。

pacman -Syu

更新の途中でシェルを閉じるよう案内された場合は、一度ウィンドウを閉じ、MSYS2 MINGW64を再度起動して次を実行します。

pacman -Su

次に、ビルドに必要なパッケージを導入します。

pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils

主に使用するツールは次のとおりです。

・g++.exe    ：C++のコンパイルとリンク
・windres.exe：管理者権限要求マニフェストのリソース化
・objdump.exe：完成EXEのDLL依存確認


3. 必要なファイル
------------------

次のファイルを同じフォルダへ置きます。

Wizardry6Automap.cpp
Wizardry6Automap.exe.manifest
Wizardry6Automap.rc
build_msys2_mingw64.bat

LICENSE、Readme.txtそのものには必須ではありませんが、配布物には含めてください。


4. ビルド方法
--------------

4.1 MSYS2 MINGW64シェルから実行する場合

ソースフォルダへ移動します。
WindowsのC:\Project_work\Wizardry6Automapに置いた場合の例：

cd /c/Project_work/Wizardry6Automap

ビルドスクリプトを実行します。

./build_msys2_mingw64.bat

4.2 エクスプローラーから実行する場合

必要なMSYS2ツールがC:\msys64\mingw64\binに導入されていれば、build_msys2_mingw64.batをダブルクリックしてもビルドできます。


5. 成功時の出力
----------------

ビルドに成功すると、同じフォルダに次のファイルが作成されます。

Wizardry6Automap.exe

ビルドスクリプトは、Warningをエラーとして扱います。Warningが1件でも残っている場合はビルドを失敗させます。

成功時には、概ね次の内容が表示されます。

Built Wizardry6Automap.exe with warnings treated as errors.
Imported DLLs reported by objdump:
...
Verified: no MinGW runtime DLL dependency detected.
Administrator privileges are embedded in the EXE manifest.


6. ビルド設定
--------------

ビルドスクリプトは、概ね次の条件でコンパイルします。

・C++17
・最適化：-O2
・リリースビルド：-DNDEBUG
・警告：-Wall -Wextra -Werror
・Unicode：UNICODE / _UNICODE / -municode
・Windows GUIアプリ：-mwindows
・入力・実行文字コード：UTF-8
・MinGWランタイムの静的リンク
・未使用関数、未使用データセクションの削除
・デバッグシンボルの削除
・管理者権限要求マニフェストの埋め込み

主要なコンパイル・リンク指定：

-std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror
-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
-municode -mwindows
-finput-charset=UTF-8 -fexec-charset=UTF-8
-static -static-libgcc -static-libstdc++
-ffunction-sections -fdata-sections
-Wl,--gc-sections
-luser32 -lgdi32 -lcomdlg32
-s


7. DLL依存について
-------------------

MinGWランタイムは静的リンクするため、通常は次のDLLを別途配布する必要はありません。

libgcc_s_*.dll
libstdc++-6.dll
libwinpthread-1.dll

Windows標準DLLへの依存は残ります。

ビルドスクリプトはobjdump -pで完成EXEを調査し、上記MinGWランタイムDLLへの依存が残っていないか自動確認します。
objdumpが見つからない場合はEXEの作成自体は可能ですが、自動確認は行われません。


8. 管理者権限要求マニフェスト
------------------------------

Wizardry6Automap.exe.manifestには、requireAdministratorが指定されています。
Wizardry6Automap.rcをwindresでコンパイルし、Wizardry6Automap.res.oとしてEXEへリンクします。

ビルド後のWizardry6Automap.exeを起動すると、WindowsのUAC画面が表示されます。


9. ビルドに失敗する場合
------------------------

「g++.exe not found」
  mingw-w64-x86_64-gccが導入されていません。

  pacman -S --needed mingw-w64-x86_64-gcc

「windres.exe not found」または「objdump.exe not found」
  mingw-w64-x86_64-binutilsを導入してください。

  pacman -S --needed mingw-w64-x86_64-binutils

C++のerrorまたはwarningで停止する
  本プロジェクトは-Werrorを使用するため、warningも修正が必要です。表示されたファイル名と行番号を確認してください。

MinGWランタイムDLLが検出される
  -static -static-libgcc -static-libstdc++が有効であることを確認してください。追加ライブラリがlibwinpthreadへ依存していないかも確認してください。

リソースビルドに失敗する
  Wizardry6Automap.rcとWizardry6Automap.exe.manifestが同じフォルダに存在するか確認してください。


10. ZIPの構成
-------------------------

Wizardry6Automap.exe
Wizardry6Automap.cpp
Wizardry6Automap.exe.manifest
Wizardry6Automap.rc
build_msys2_mingw64.bat
Readme.txt
Readme_build.txt
LICENSE.txt
Config\Wizardry6Automap.conf

次のファイルは利用者ごとのデータなので、ZIPへ入れません。

Config\Wizardry6Automap_visited.bin
Config\Wizardry6Automap_notes.bin
Config\Wizardry6Automap_notes.tmp


11. ライセンス
---------------

本プロジェクトはGNU General Public License version 2で公開されています。
ソースを改変または再配布する場合は、GPL version 2の条件とLICENSEを確認してください。
