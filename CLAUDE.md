# RGBPackingTool — 開発用の指示

このリポジトリで Texture Channel Packer プラグインそのものを開発するときの指示です。プラグインを**使う**プロジェクトのエージェント向けの指示は `RGBPackingTool/Plugins/TextureChannelPacker/AGENTS.md` で、これとは別物です。

## 構成

- `RGBPackingTool/` — 開発用の UE プロジェクト（`RGBPackingTool.uproject`）。
- `RGBPackingTool/Plugins/TextureChannelPacker/` — プラグイン本体。Fab で配布するのはこのフォルダと、`Config/FilterPlugin.ini` に書いた追加ファイルだけです。モジュールは `TextureChannelPacker`（`Type: Editor`）の 1 つで、ランタイムでは動きません。
- `Docs/API.md` / `Docs/API.ja.md` — 内部構造の開発者向けドキュメント。
- `Tools/check_headless_docs.py`、`Tools/ai_eval/` — ヘッドレス docs の検査と AI 評価。どちらも CI（`.github/workflows/headless-docs.yml`）で実行されます。

## C++

- UE 5.5〜5.8 をサポートし、開発とテストは 5.8 で行います。コードは 5.5 でもビルドできる必要があるので、新しい API を使うときは 5.5 にあるか確認してください。
- [Epic Games の C++ コーディング規約](https://dev.epicgames.com/documentation/en-us/unreal-engine/epic-cplusplus-coding-standard-for-unreal-engine) に従います。
- 新しいクラスを使うときは、それが属するモジュールを確認し、`TextureChannelPacker.Build.cs` の依存モジュールに足してください。

## ドキュメント

- 日英のドキュメントは常に同期します。片方を変えたら、指示がなくてももう片方にも同じ内容を反映してください。
  - `README.md` ↔ `README.ja.md`、`QUICK_START.md` ↔ `QUICK_START.ja.md`、`CHANGELOG.md` ↔ `CHANGELOG.ja.md`、`Docs/API.md` ↔ `Docs/API.ja.md`、`Plugins/TextureChannelPacker/Docs/Headless.md` ↔ `Headless.ja.md`
- ユーザーから見える変更は、両方の CHANGELOG の `[Unreleased]` に記録します（Keep a Changelog 形式）。
- ヘッドレス API（Commandlet、`UTextureChannelPackerLibrary`、`TextureChannelPackerTypes.h`）やエラーコードを変えた場合は、`Plugins/TextureChannelPacker/Docs/Headless.md` と `Headless.ja.md`（`Config/FilterPlugin.ini` 経由でプラグインに同梱されます）を更新し、次の 2 つが成功することを確認してください。ジョブファイルの解釈や命名を変えた場合は `Tools/ai_eval/jobsim.py` も合わせて更新します。
  - `python3 Tools/check_headless_docs.py`
  - `python3 Tools/ai_eval/run_eval.py selftest`
- `Plugins/TextureChannelPacker/AGENTS.md` は、プラグインをインストールしたプロジェクトで動く AI エージェント（Codex、Cursor、Copilot、Gemini CLI、Jules、Claude Code など）向けのベンダー中立な指示で、隣の `CLAUDE.md` はそれを取り込むだけです。
  - エージェント向けの内容は常に `AGENTS.md` に書き、プラグイン側の `CLAUDE.md` には書かないでください。
  - 入口（Commandlet の引数や終了コード、上書きポリシー、ドライラン）を変えた場合は `AGENTS.md` も更新します。
  - フィールド名やエラーコードの一覧は `Docs/Headless.md` に任せ、`AGENTS.md` には書きません（陳腐化を防ぐため）。

## コミットメッセージ

変更内容を明確に書き、次のプレフィックスを使います。

- `feat:` 新機能 / `fix:` バグ修正 / `docs:` ドキュメントのみ / `refactor:` 機能変更なしのリファクタリング / `perf:` 性能改善 / `test:` テスト / `ci:` CI / `chore:` リリース作業などその他

例: `feat: Add support for BC7 compression format`
