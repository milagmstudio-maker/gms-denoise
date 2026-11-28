# しずまる 組み込みキット（Windows / 自作ミキサー向け）

プラグインと同じ処理（6モードすべて、AI Deep の悲鳴保護込み）を、JUCE なしで自分のアプリから呼ぶための C API です。C / C++ からそのまま、C# などからは DLL として呼べます。

## 中身

| ファイル | 内容 |
|---|---|
| `bin/shizumaru.dll` | 本体（AI Deep の2モデル込み、約60 MB）。Visual C++ ランタイム不要 |
| `lib/shizumaru.lib` | リンク用インポートライブラリ（MSVC） |
| `include/shizumaru.h` | API ヘッダ（これ1つだけ include） |
| `bin/shz_example.exe` | 動作確認と WAV 変換のサンプル |
| `example/shz_example.cpp` | サンプルのソース |
| `source/` | 全ソース（自分でビルドする場合） |
| `ThirdPartyNotices.txt` | 同梱ライブラリのライセンス表記（配布時に一緒に付けてください） |

## 最小の使い方（C++）

```cpp
#include "shizumaru.h"   // lib/shizumaru.lib をリンク、shizumaru.dll を exe の横に置く

// 初期化（オーディオスレッド以外で。AI Deep はここでモデルを読むので約0.5秒かかる）
shz_engine* nr = shz_create(48000.0, 2, 512);      // サンプルレート, ch数, 最大ブロック長
shz_set_mode(nr, SHZ_MODE_AI_DEEP);                // 0..5（下の表）
shz_preload(nr, SHZ_MODE_AI_DEEP);                 // 先に読み込んでおくと切替直後も素通しにならない
shz_params p = shz_default_params();
p.amount = 50; p.reduction_db = 40;                // プラグインのつまみと同じ意味
shz_set_params(nr, &p);
int delay = shz_get_latency(nr);                   // この分だけ出力が遅れる → ミキサー側で他chを揃える

// オーディオコールバック内（チャンネル別 float 配列をその場で書き換え）
float* ch[2] = { left, right };
shz_process(nr, ch, numSamples);

// 終了
shz_destroy(nr);
```

## モード

| 値 | 名前 | 遅延 @48 kHz | 向き |
|---|---|---|---|
| 0 | Zero latency | 0 | 配信・モニター |
| 1 | Low latency | 128 smp (2.7 ms) | |
| 2 | High quality | 512 smp (10.7 ms) | |
| 3 | AI (10 ms) | 480 smp | 軽い AI |
| 4 | AI Deep (20 ms) | 960 smp | DeepFilterNet3 低遅延版。CPU 重め（1コア約25%） |
| 5 | AI Deep HQ (40 ms) | 1920 smp | DeepFilterNet3 標準版。語間をより深く消す（約11%） |

44.1 / 96 kHz などでも動きます（AI Deep は内部で 48 kHz に変換、遅延は約21 / 41 ms）。正確な値は `shz_get_latency()` / `shz_latency_for(mode, rate)` で取ってください。

## ミキサーに組み込むときの注意

- **スレッド**: `shz_create` / `shz_prepare` / `shz_preload` / `shz_destroy` はオーディオスレッドで呼ばないでください（モデル読み込みで止まります）。`shz_process` はオーディオスレッドから。`shz_set_mode` / `shz_set_params` はどのスレッドからでも可。
- **遅延補正**: モードを変えると遅延が変わります。ミキサーの遅延補正（PDC）にはモード変更のたびに `shz_get_latency()` を読み直して反映してください。
- **ch ごとに1インスタンス**: マイクが複数なら、マイク（トラック）ごとに `shz_create` してください。ステレオ1本なら ch=2 で1つ。
- **バッファサイズ**: AI Deep は 480 サンプルごとにまとめて計算するので、そのブロックだけ処理が重くなります（このマシンで1回 約2.4 ms、たまに5 ms）。オーディオバッファは 256 以上を推奨（推定。お使いの PC で確認してください）。
- **AI Deep 初回**: `shz_preload` しないで切り替えた場合、裏で読み込む約0.5秒は「遅延だけかかった素通し」になり、その後なめらかに切り替わります（`shz_is_loading()` が 1 の間）。
- **メーター**: `shz_get_meter(nr, out, 48)` で帯域ごとの削減量（dB）が取れます。

## C# から使う例

```csharp
[DllImport("shizumaru.dll")] static extern IntPtr shz_create(double rate, int ch, int maxBlock);
[DllImport("shizumaru.dll")] static extern void shz_set_mode(IntPtr e, int mode);
[DllImport("shizumaru.dll")] static extern int shz_get_latency(IntPtr e);
[DllImport("shizumaru.dll")] static extern unsafe void shz_process(IntPtr e, float** ch, int n);
[DllImport("shizumaru.dll")] static extern void shz_destroy(IntPtr e);
```

## 動作確認

```
shz_example.exe                     … 全モードの自己テスト（ノイズ除去量・遅延・モード切替）
shz_example.exe in.wav out.wav 4    … WAV をモード4で処理（出力は遅延分ずれます）
```

## 自分でビルドする場合（source/）

Visual Studio 2022、CMake 3.22 以上、Rust（https://rustup.rs）、git が必要です。

```
cd source
cmake -B build -G "Visual Studio 17 2022" -A x64 -DZD_BUILD_PLUGIN=OFF
cmake --build build --config Release --target shizumaru shz_example
```

初回の configure で DeepFilterNet（MIT/Apache-2.0）を `build-deps/` に取得します。AI Deep が要らなければ `-DZD_WITH_DFN=OFF` で Rust 不要・DLL も小さくなります（モード4/5は AI モードとして動きます）。

## ライセンス

同梱の RNNoise（BSD）、pffft（BSD系）、DeepFilterNet3 と Rust ライブラリ（MIT / Apache-2.0 ほか）の表記は `ThirdPartyNotices.txt` にあります。ミキサーを配布するときはこのファイルも一緒に付けてください。
