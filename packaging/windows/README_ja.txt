GmS De-Noise（Windows / VST3 / 64bit）
GmS Audio（Genomersive Studio のオーディオブランド）

DAW のプラグイン一覧では、メーカー名は「GmS Audio」と表示されます（以前の版は「T2」）。内部のプラグインIDは変えていないので、保存済みのプロジェクトはそのまま開けます。

■ 入れ方（どちらか一方）

1. GmS-De-Noise-x.x.x-Windows-Setup.exe を実行する（おすすめ）
   - 「WindowsによってPCが保護されました」と出たら、「詳細情報」→「実行」を押してください。
     署名（有料の証明書）をしていないので出る表示で、ウイルスの警告ではありません。
   - 「このアプリがデバイスに変更を加えることを許可しますか？」には「はい」。
   - C:\Program Files\Common Files\VST3\GmS De-Noise.vst3 に入ります。
   - 消すときは「設定」→「アプリ」→「GmS De-Noise (VST3)」→「アンインストール」。

2. zip を全部展開して install.bat を実行する（Setup.exe が使えないとき）
   - 展開せずに zip の中から実行すると失敗します。必ず先に展開してください。
   - 消すときは uninstall.bat。

入れたら、DAW を開き直してプラグインを再スキャンしてください。
  REAPER   : Options → Preferences → Plug-ins → VST → Re-scan
  FL Studio: Options → Manage plugins → Find more plugins
  Studio One / Cubase / Ableton Live: 起動時に自動で探します

■ 必要なもの
  - Windows 10 / 11（64bit）。32bit の DAW では使えません。
  - Visual C++ ランタイムなどの追加インストールは不要です。

■ OBS で使うとき
  OBS に最初から入っている「VST 2.x プラグイン」フィルターは VST2 専用なので、VST3 の GmS De-Noise は出てきません。
  OBS 用の VST3 ホストプラグインを使うか、DAW を通して仮想オーディオケーブルで OBS に戻してください。

■ 使い方
  まず MODE を選びます。
    AI (10 ms)     一番よく消えるモード（おすすめ）。10 ms の遅れが出ます（DAW が自動で補正します）。
    Zero latency   遅れゼロ。配信やモニターしながらの録音向け。
    Low latency / High quality   その中間。
  AMOUNT       どれだけ積極的にノイズと判定して消すか（既定 70）
  STRENGTH     Gentle / Medium（既定）/ Strong
               声が出た瞬間に背景のノイズが戻ってくる「揺れ」をどこまで許すか。
               Gentle は背景が一定で自然、Strong は無音部分を一番深く消します。
  REDUCTION    ノイズを最大何 dB 下げるか（既定 40 dB。0 dB で処理なし）
  GATE         入力がこのレベルより小さいときは完全に無音にする（左いっぱいで OFF）。
               声とノイズの差が大きい録音でだけ使ってください。小さい声や語尾も消えます。
  OUTPUT       出力の音量
  LEARN        ノイズだけの部分で押して、終わったらもう一度押すとそのノイズを覚えます（普段は不要）
  Adaptive     ノイズの変化に自動で追従（既定 ON）
  Strong voice protect（AI モードのみ）  叫び声を最優先で守る。周りの話し声は少し残りやすくなります。
  Pause cleanup（AI モードのみ）          声のない区間を REDUCTION の値まで深く下げる（STRENGTH が Strong のとき有効）。
