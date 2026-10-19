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

## 送信される内容

- 入力中の読み (カタカナ)
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

   環境変数 `MOZKEY_CLOUD_API_KEY` または `OPENCODE_API_KEY` でも指定できます
   (資格情報マネージャーが優先されます)。
3. 必要なら `%LOCALAPPDATA%\Mozkey\cloud_scorer.ini` で接続先とモデルを変えます。
   ファイルは要求ごとに読み直されるので、変更は即時反映されます。

   ```ini
   [cloud]
   ; OpenAI 互換 chat completions のエンドポイント (https 必須。localhost のみ http 可)
   endpoint=https://opencode.ai/inference/go/openai/v1/chat/completions
   ; OpenCode Go の Chat Completions 対応モデル ID
   model=glm-5.3-flash
   max_tokens=128
   ; リクエスト本文に追加する JSON メンバー (カンマ区切り)
   extra_body="temperature":0
   ; 0 にするとクラウド失敗時にローカル Zenz へフォールバックしない
   local_fallback=1
   ```

   環境変数 `MOZKEY_CLOUD_ENDPOINT` / `MOZKEY_CLOUD_MODEL` は ini より優先されます。

## タイムアウト

`zenz_cloud_correction_timeout_msec` (既定 1500 ms、最大 3000 ms) を超えた
クラウド要求は打ち切られ、ローカル Zenz の結果が使われます。
推論 (思考) を行うモデルは遅くなりがちなので、`*-flash` 系の高速なモデルを
推奨します。モデルが対応していれば `extra_body` で推論を無効化してください。

## デバッグ

環境変数 `MOZKEY_CLOUD_SCORER_DEBUG=1` を設定すると、`OutputDebugString` に
処理結果 (成否の理由と出力のバイト数のみ。入力文字列は出しません) を出力します。
DebugView などで確認できます。
