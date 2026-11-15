しずまる（Shizumaru）— 遅れゼロのノイズ除去（macOS / VST3 / Apple Silicon・Intel 両対応・無料）
GmS Audio（Genomersive Studio のオーディオブランド）

しずまるは、以前「GmS De-Noise」という名前だったプラグインです（無料）。名前が変わっても中身は同じで、内部のプラグインIDも同じなので、GmS De-Noise を使っていた DAW のプロジェクトはそのまま開けます。pkg のインストーラーは古い GmS De-Noise.vst3 を自動で消します（同じプラグインが2つ並ぶのを防ぐため）。DAW の一覧には「Shizumaru」（メーカー名 GmS Audio）と出ます（DAW によっては日本語の名前が文字化けするため、DAW に見せる名前は英字にしています）。

■ 入れ方（どちらか一方）

1. Shizumaru-x.x.x-macOS.pkg を開く（おすすめ）
   - 署名（Apple の有料の開発者登録）をしていないので、ダブルクリックすると
     「開けません」と出ます。ウイルスの警告ではありません。次のどちらかで開けます。
       a. Finder で pkg を右クリック（control+クリック）→「開く」→「開く」
       b. それでも開けないときは「システム設定」→「プライバシーとセキュリティ」を下まで見て、
          「このまま開く」を押す
   - /Library/Audio/Plug-Ins/VST3/Shizumaru.vst3 に入ります。

2. zip を展開して、Shizumaru.vst3 を自分で入れる
   - 「Shizumaru.vst3」を /Library/Audio/Plug-Ins/VST3（全ユーザー）か
     ~/Library/Audio/Plug-Ins/VST3（自分だけ）にコピーします。
   - ダウンロードした印が残っていると DAW が読み込めないので、ターミナルで次を実行してください。
       xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Shizumaru.vst3"
     （/Library に入れた場合は sudo を付けて、そのパスにしてください）

■ 消し方
  /Library/Audio/Plug-Ins/VST3/Shizumaru.vst3 をゴミ箱に入れてください。

入れたら、DAW を開き直してプラグインを再スキャンしてください。
  Logic Pro は VST3 を読み込めません（AU 専用）。REAPER / Cubase / Studio One / Ableton Live などで使えます。

■ 必要なもの
  - macOS 11 以降

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
