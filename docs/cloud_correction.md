# クラウド AI 補正 (Windows)

Zenz ライブ補正をクラウドの LLM で行う機能です。OpenAI 互換の
`/chat/completions` API であれば利用できます。既定の接続先は
[OpenCode Go](https://opencode.ai/docs/go/) です。

## 仕組み

```
打鍵 → Mozc ライブ変換 (即時表示)
         ↓ 入力が止まる (既定 700 ms)
       mozc_server ──ZNZ1 named pipe──▶ mozkey_cloud_scorer.exe ──HTTPS──▶ クラウド API
                                          └─(並行)─▶ mozc_zenz_scorer.exe (ローカル Zenz)
         ↓ 結果を検証 (既存の Zenz 出力検証・採用ポリシー)
       補正結果に差し替え
```

- `mozkey_cloud_scorer.exe` はローカル Zenz scorer と同じ ZNZ1 プロトコルを
  話すので、IME 側は送り先パイプ名を切り替えるだけです。
- クラウドへの要求と並行してローカル Zenz にも問い合わせ、クラウドが失敗・
  タイムアウトした場合はローカルの結果を返します。
- ネットワーク通信を行うのは `mozkey_cloud_scorer.exe` だけです。
  `mozc_server.exe` などの既存プロセスは引き続きファイアウォールで外向き通信が
  遮断されます。
- クラウド補正中は「補正結果まで表示を待つ」設定は使われません
  (Mozc の結果を先に表示し、補正結果が届いたら差し替えます)。

## 英語の自動判別と打ち間違いの修正

ローマ字入力では、実際に打ったキー (例: `kyouhameetingdesu`) もクラウドに送り、
次の補正を許可しています。

- 英語の語として打った部分は英字のまま残す (`今日はmeetingです`)。
- 明らかな打ち間違いを直す (`arigaou` → `ありがとう`)。

安全のため、補正結果に含まれる英字は「実際に打ったキーに含まれる綴り」だけが
採用されます (大文字・小文字の違いは可)。打っていない英字が出てきた場合
(`kare` → `care` など) は補正を採用せず、Mozc の結果のままにします。
かな入力のときはこの機能は使われません。

## 送信される内容

- 入力中の読み (カタカナ) と、ローマ字入力時は実際に打ったキー
- 直前の文脈 (既定で最大 24 文字。既存のプライバシー判定で除外されたものは送られません)
- 有効にしている場合は直後の文脈、プロフィール・話題・文体・補足の各設定

パスワード欄ではライブ変換自体が動かないため送信されません。

## 設定

1. 設定画面の「Zenz ライブ補正を有効にする」と「クラウド AI で補正する」を
   オンにします。
2. API キーを Windows 資格情報マネージャーに登録します。

   ```bat
   cmdkey /generic:Mozkey/CloudScorer /user:opencode /pass:<API キー>
   ```

   環境変数でも指定できます。優先順位は `MOZKEY_CLOUD_API_KEY` →
   資格情報マネージャー → `OPENCODE_API_KEY` です。
3. 必要なら `%LOCALAPPDATA%\Mozkey\cloud_scorer.ini` で接続先とモデルを変えます。
   ファイルは要求ごとに読み直されるので、変更は即時反映されます。

   ```ini
   [cloud]
   ; OpenAI 互換 chat completions のエンドポイント (https 必須。localhost のみ http 可)
   endpoint=https://opencode.ai/inference/go/openai/v1/chat/completions
   ; OpenCode Go の Chat Completions 対応モデル ID
   model=deepseek-v4.1-flash
   max_tokens=128
   ; リクエスト本文に追加する JSON メンバー (カンマ区切り)
   ; 省略時、モデルが deepseek-* なら思考を止める "thinking":{"type":"disabled"}、
   ; それ以外は何も付けません。空 (extra_body=) にすると何も付けません。
   extra_body="thinking":{"type":"disabled"}
   ; 0 にするとクラウド失敗時にローカル Zenz へフォールバックしない
   local_fallback=1
   ```

   環境変数 `MOZKEY_CLOUD_ENDPOINT` / `MOZKEY_CLOUD_MODEL` /
   `MOZKEY_CLOUD_EXTRA_BODY` / `MOZKEY_CLOUD_LOCAL_FALLBACK` は ini より優先されます。

   OpenCode Go は会話ごとの `x-opencode-session` ヘッダーを必須としているため、
   scorer は起動ごとにランダムなセッション ID を付けて送ります。
   なお OpenCode Go は「OpenCode などのコーディングエージェント向け」とされており、
   IME からの利用は想定用途の外です。

## タイムアウト

`zenz_cloud_correction_timeout_msec` (既定 2500 ms、最大 3000 ms) を超えた
クラウド要求は打ち切られ、ローカル Zenz の結果が使われます。
実測 (OpenCode Go、deepseek-v4.1-flash、思考なし) では接続確立込みの初回が
約 2.2 秒、2 回目以降が約 1.2 秒でした。思考を行うモデルは 2〜3 秒以上かかります
(glm-5.3-flash は思考を止められず、`max_tokens` を使い切って空の応答になることが
あります)。

## デバッグ

環境変数 `MOZKEY_CLOUD_SCORER_DEBUG=1` を設定すると、`OutputDebugString` に
処理結果 (成否の理由と出力のバイト数のみ。入力文字列は出しません) を出力します。
DebugView などで確認できます。API がエラーを返した場合は
`cloud_http_status_400:<API のエラーメッセージ>` のように理由も出ます。

開発中のビルドをインストール済みの scorer と並べて動かすときは、
`MOZKEY_CLOUD_SCORER_INSTANCE=<名前>` を設定すると
`\\.\pipe\mozkey_cloud_scorer_<名前>` で待ち受けます。
