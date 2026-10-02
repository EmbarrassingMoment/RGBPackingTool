[English](Headless.md)

# ヘッドレス実行（Commandlet / Python / Blueprint）

パックとアンパックはツールウィンドウなしでも実行できるため、スクリプト・CI・AI エージェントから駆動できます。どの入口もエディタツールと同じ処理（`TextureChannelPackerCore`）を実行しますが、**ダイアログや通知は一切表示しません**。結果はすべて、安定したエラーコード付きの構造化データとして返ります。

| 入口 | 用途 |
| :--- | :--- |
| **Commandlet**（`-run=TextureChannelPacker`） | CI やエージェントからのバッチ実行。JSON のジョブファイルを渡し、JSON の結果ファイルと終了コードを受け取ります。 |
| **Python**（`unreal.TextureChannelPackerLibrary`） | エディタの Python スクリプトや、Python 経由で起動中のエディタを操作するツール。 |
| **Blueprint**（Editor Utility） | Editor Utility Widget / Blueprint。 |
| **Remote Control** | ライブラリの関数は `BlueprintCallable` な static 関数なので、Remote Control API 経由で起動中のエディタから呼ぶこともできます（クラスデフォルトオブジェクト `/Script/TextureChannelPacker.Default__TextureChannelPackerLibrary` に対して呼び出します）。 |

ヘッドレス実行は**エディタ専用**です（テクスチャのソースデータが必要なため）。また、ゲームスレッド上で実行する必要があります（上記の入口はすべて該当します）。

## Commandlet

```
UnrealEditor-Cmd.exe <Project>.uproject -run=TextureChannelPacker -Job=jobs.json -Result=result.json -unattended -nullrhi -nosplash
```

| 引数 | 説明 |
| :--- | :--- |
| `-Job=<path>` | 必須。ジョブファイル（後述）。 |
| `-Result=<path>` | 結果 JSON の出力先。省略するとログに出力します。 |
| `-DryRun` | すべてのジョブで `dryRun` を強制します。何も書き込まずに検証と名前解決だけを行います。 |

相対パスはコマンドを起動したディレクトリを基準に解決されます。

**終了コード**: `0` すべてのジョブが成功（`Skipped` と `DryRun` も成功扱い）・`1` 引数の誤り、またはジョブファイルを読めない・`2` 1つ以上のジョブが失敗。

ジョブは記述順に実行されるため、前のジョブが作ったアセットを後のジョブで使えます。コストが大きいのはエディタの起動なので、テクスチャごとに起動し直すのではなく、1つのファイルに多数のジョブをまとめてください。

### ジョブファイル

```json
{
  "jobs": [
    {
      "mode": "pack",
      "preset": "ORM",
      "inputs": {
        "R": "/Game/Rock/T_Rock_AO",
        "G": "/Game/Rock/T_Rock_Roughness",
        "B": { "texture": "/Game/Rock/T_Rock_Metallic", "channel": "R", "invert": false }
      },
      "outputPath": "/Game/Rock",
      "overwrite": "overwrite"
    },
    {
      "mode": "unpack",
      "preset": "ORM",
      "source": "/Game/Props/T_Crate_ORM",
      "outputPath": "/Game/Props/Split"
    }
  ]
}
```

未知のフィールドはエラー（`ErrorInvalidJob`）になるため、タイプミスは無視されずに表面化します。アセットパスは `/Game/Folder/T_Name`、`/Game/Folder/T_Name.T_Name`、`Texture2D'/Game/Folder/T_Name.T_Name'` のいずれの形式でも指定できます。

#### パックジョブ

| フィールド | 既定値 | 説明 |
| :--- | :--- | :--- |
| `mode` | — | `"pack"` |
| `preset` | — | 組み込み（`Custom`、`ORM`、`MRA`）またはユーザープリセット名。ソースチャンネル・反転・圧縮設定・ファイル名サフィックスの既定値を与え、明示したフィールドがそれを上書きします。 |
| `inputs` | — | 必須。キー `R`/`G`/`B`/`A` を持つオブジェクト。値はアセットパス、または `{ "texture", "channel": "R\|G\|B\|A", "invert": bool }`。空のスロットは 0（R/G/B）または 255（A）で埋められます。 |
| `width`, `height` | `0`, `0` | 出力サイズ（1〜16384）。両方 `0` なら最大の入力と同じサイズ。 |
| `outputPath` | `"/Game"` | 出力先のコンテンツフォルダ。 |
| `outputName` | 自動 | 出力アセット名。省略時はツールの自動命名（入力名の共通接頭辞 + `suffix`、`T_` を付与）。 |
| `suffix` | プリセット / `"_ORM"` | 自動命名で使うサフィックス。 |
| `compression` | プリセット / `"Masks"` | `"Masks"`、`"Grayscale"`、`"Default"` のいずれか。 |
| `overwrite` | `"fail"` | `"fail"`、`"overwrite"`、`"skip"`（後述）。 |
| `save` | `true` | パッケージをディスクに保存します。 |
| `dryRun` | `false` | ピクセルの読み込みや書き込みを行わず、検証と名前・サイズの解決だけを行います。 |

#### アンパックジョブ

| フィールド | 既定値 | 説明 |
| :--- | :--- | :--- |
| `mode` | — | `"unpack"` |
| `preset` | — | チャンネルごとのサフィックスを与えます（ORM: `_AO` / `_Roughness` / `_Metallic` / `_A`）。 |
| `source` | — | 必須。パック済みテクスチャ。 |
| `channels` | `"auto"` | `"auto"`: 均一なチャンネル（全ピクセルが同じ値。未使用のアルファなど）を除くすべて。`"all"`: すべてのチャンネル。`["R", "G"]`: 指定したチャンネルのみ。 |
| `outputPath` | `"/Game"` | 出力先のコンテンツフォルダ。 |
| `baseName` | 自動 | ベース名（これにチャンネルのサフィックスが付きます）。省略時はソース名から導出（`T_Rock_ORM` → `T_Rock`）。 |
| `suffixes` | プリセット / `_R` `_G` `_B` `_A` | キー `R`/`G`/`B`/`A` を持つオブジェクト。 |
| `overwrite`, `save`, `dryRun` | | パックと同じ。アンパックの `"skip"` は、出力が*1つでも*存在すれば何も書き込みません。 |

### 結果ファイル

```json
{
  "version": 1,
  "total": 2,
  "failed": 0,
  "results": [
    {
      "index": 0,
      "mode": "pack",
      "status": "Succeeded",
      "errorCode": "",
      "message": "Texture saved: /Game/Rock/T_Rock_ORM",
      "outputs": [ "/Game/Rock/T_Rock_ORM.T_Rock_ORM" ],
      "warnings": [],
      "width": 2048,
      "height": 2048
    },
    {
      "index": 1,
      "mode": "unpack",
      "status": "Succeeded",
      "errorCode": "",
      "message": "Unpacked 3 texture(s) to /Game/Props/Split",
      "outputs": [ "/Game/Props/Split/T_Crate_AO.T_Crate_AO", "..." ],
      "warnings": [],
      "width": 1024,
      "height": 1024,
      "skippedUniformChannels": [ "A" ]
    }
  ]
}
```

- `status`: `Succeeded`、`Skipped`、`DryRun`、`Failed` のいずれか。
- `errorCode`: 言語に依存しない安定した識別子です。分岐にはこちらを使ってください（`message` はエディタの言語設定に従います）。
- `outputs`: 書き込んだ（`save` が有効なら保存まで完了した）アセットのオブジェクトパス。`DryRun` / `Skipped` の場合は、ジョブの対象となるパス。ジョブが失敗した場合は空です（アンパックで、失敗前に書き込み済みのチャンネルがある場合はそれだけを含みます）。
- `warnings`: 致命的でない問題。各要素は `{ "code", "channel", "message" }`。パックの場合、入力を読み込めなかったためスロットをデフォルト値で埋めたことを意味します。
- 実行を開始できなかった場合（引数の誤り、ジョブファイルを読めないなど）は、`"error": { "code", "message" }` と空の `results` 配列が出力されます。

## Python

```python
import unreal

lib = unreal.TextureChannelPackerLibrary

red = unreal.ChannelPackerInput()
red.texture = unreal.load_asset("/Game/Rock/T_Rock_AO")
green = unreal.ChannelPackerInput()
green.texture = unreal.load_asset("/Game/Rock/T_Rock_Roughness")

request = unreal.ChannelPackerPackRequest()
request.red = red
request.green = green
request.output_path = "/Game/Rock"
request.overwrite_policy = unreal.ChannelPackerOverwritePolicy.OVERWRITE

result = lib.pack_textures(request)
if result.status == unreal.ChannelPackerStatus.FAILED:
    unreal.log_error(f"{result.error_code}: {result.message}")
else:
    print(result.output_assets)
```

| 関数 | 説明 |
| :--- | :--- |
| `pack_textures(request)` | `ChannelPackerPackRequest` を実行し、`ChannelPackerResult` を返します。 |
| `unpack_texture(request)` | `ChannelPackerUnpackRequest` を実行し、`ChannelPackerResult` を返します。 |
| `make_pack_request_from_preset(name)` / `make_unpack_request_from_preset(name)` | プリセットの既定値でリクエストを埋めます。その名前のプリセットがなければ `False` を返します。 |
| `get_preset_names()` | 組み込みプリセットとユーザープリセットの名前一覧。 |

リクエストのフィールドはジョブファイルと対応しています（`width`、`height`、`output_path`、`output_name`、`file_name_suffix`、`compression`、`overwrite_policy`、`save`、`dry_run`。アンパックでは `source`、`export_red`〜`export_alpha`、`skip_uniform_channels`、`base_name`、`suffix_red`〜`suffix_alpha`）。エディタツールと異なり、`save` の既定値は `True` です。

## 動作に関する補足

- **上書きポリシー**: `Fail`（既定）は `ErrorAssetExists` で失敗します。`Overwrite` はアセットをその場で置き換えます（エディタツールが確認ダイアログの後に行うのと同じ動作です）。`Skip` は何も書き込まずに `Skipped` を返すため、同じジョブファイルを安全に再実行できます。クラスの異なる既存アセットは置き換えません（`ErrorAssetTypeMismatch`）。
- **保存**: `save` が有効な場合、テクスチャのビルド完了を待ってから `UPackage::SavePackage` でパッケージを書き込みます。ソース管理下のファイルは事前に書き込み可能（チェックアウト済み）にしておく必要があり、そうでなければ `ErrorSaveFailed` で失敗します。
- **出力形式**: エディタツールと同じです。パックは指定した圧縮設定の BGRA8、アンパックは `Grayscale` 圧縮の G8 を書き出します。どちらもリニア（`sRGB = false`）です。

## エラーコード

| コード | 意味 |
| :--- | :--- |
| `ErrorNoTextures` | パック: 入力が1つも指定されていない。 |
| `ErrorInputNotFound` | 入力/ソースのパスが `Texture2D` に解決できない。 |
| `ErrorInvalidResolution` | 幅/高さが 1〜16384 の範囲外、または片方だけが 0。 |
| `ErrorInvalidOutputName` / `ErrorInvalidOutputPath` | 出力名・出力フォルダが有効なアセットにならない（空白を含む、`/Game` などのコンテンツルート配下でない等）。 |
| `ErrorInvalidCompression` | 不明な圧縮設定名。 |
| `ErrorAssetExists` | 出力が既に存在し、上書きポリシーが `Fail`。 |
| `ErrorAssetTypeMismatch` | 出力名が別クラスのオブジェクトで使用されている。 |
| `ErrorSaveFailed` | パッケージを保存できなかった。 |
| `ErrorNoUnpackSource`, `ErrorNoChannelsSelected` | アンパック: ソースがない、またはチャンネルが1つも指定されていない。 |
| `ErrorNoChannelsToExport` | アンパック: 指定したチャンネルがすべて均一（それでも出力するには `"channels": "all"` を指定）。 |
| `ErrorDuplicateOutputNames` | アンパック: 2つのチャンネルが同じ名前になる（サフィックスを確認）。 |
| `ErrorUnsupportedFormat`, `ErrorLockFailed`, `ErrorTextureTooLarge`, `ErrorInvalidSourceData`, `ErrorInvalidUnpackSource` | テクスチャのソースデータを読み込めない。パックの入力では警告扱いです（スロットはデフォルト値で埋められます）。 |
| `ErrorInvalidJob` | Commandlet: ジョブの記述がスキーマに合わない（どのフィールドかはメッセージに表示）。 |
| `ErrorUsage`, `ErrorJobFileNotFound`, `ErrorInvalidJobFile` | Commandlet: 実行を開始できなかった。 |
