[English](API.md)

# TextureChannelPacker API ドキュメント

このドキュメントでは、内部アーキテクチャの理解、機能の拡張、プロジェクトへの貢献を希望する開発者向けに、`TextureChannelPacker` モジュールの技術的な詳細を解説します。

## 概要

**TextureChannelPacker** は、個々のグレースケールまたはカラーテクスチャ (赤、緑、青、アルファ) を単一の RGBA テクスチャアセットにパッキングするための Slate ベースの UI ツールを提供する、エディタ専用のプラグインモジュールです。v1.8.0 からは逆の操作にも対応しており、**アンパック** タブでパック済みテクスチャをチャンネルごとのグレースケールアセットに分解できます。

### 主な機能
- **エディタ統合**: レベルエディタの「ツール (Tools)」メニューに統合されています。
- **スレッドセーフ**: 明示的なデータの抽出と再構築の手順を使用し、ゲームスレッド上で `UTexture2D` リソースを安全に扱います。
- **並列処理**: `ParallelFor` を使用して、テクスチャチャンネルの処理とリサイズを並行して行います。
- **スマート命名機能**: 入力アセットに基づいて出力ファイル名を自動生成します。
- **パック / アンパックモード**: ドックタブ上部のセグメンテッドコントロールで2つのワークフローを切り替えます（`SWidgetSwitcher`）。

## モジュールアーキテクチャ

モジュールの主要クラスは `FTextureChannelPackerModule`（`IModuleInterface` を継承）で、ドックタブとパックワークフローを所有します。アンパックワークフローは別クラス `FTextureChannelUnpacker` として実装され、モジュール起動時にインスタンス化されます。両ワークフローで共有される処理コードは `TextureChannelPackerUtils` 名前空間にあります。

*   **ソースパス**: `Plugins/TextureChannelPacker/Source/TextureChannelPacker/`
*   **ヘッダー**: `Public/TextureChannelPacker.h`
*   **実装**: `Private/TextureChannelPacker.cpp`
*   **共有ユーティリティ**: `Private/TextureChannelPackerShared.h` / `.cpp`（名前空間 `TextureChannelPackerUtils`）
*   **アンパックタブ**: `Private/TextureChannelUnpacker.h` / `.cpp`（クラス `FTextureChannelUnpacker`）

### パブリックインターフェース

モジュールは主にエディタ UI を介して利用されるため、パブリックインターフェースは最小限です。

```cpp
class FTextureChannelPackerModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
```

- `StartupModule`: "TextureChannelPacker" タブスポーナーを登録し、「ツール」メニューを拡張します。
- `ShutdownModule`: タブスポーナーの登録を解除し、メニューの拡張をクリーンアップします。

## 内部実装

### コアクラス: `FTextureChannelPackerModule`

このクラスは UI の状態を管理し、入力テクスチャへの参照を保持し、パッキングロジックを実行します。

#### 主要メソッド

*   **`OnSpawnPluginTab`**: メインの Slate UI を構築します。
*   **`CreateChannelInputSlot`**: 各チャンネル入力用の一貫した UI ウィジェット (ラベル + オブジェクトピッカー) を作成するヘルパーメソッドです。
*   **`OnGenerateClicked`**: 生成をトリガーする前にユーザー入力を検証します (例: 少なくとも1つのテクスチャが選択されているか、解像度が有効か)。
*   **`CreateTexture`**: テクスチャ生成プロセスのメインドライバーです。
*   **`AutoGenerateFileName`**: 入力ファイル名の最長共通接頭辞 (Longest Common Prefix) に基づいて、適切な出力ファイル名を決定するヒューリスティックロジックです。

### アンパッククラス: `FTextureChannelUnpacker`

アンパックタブの状態と UI をすべて所有します。`StartupModule` で作成され `ShutdownModule` まで保持されるため、ウィジェットのラムダは生の `this` ポインタを安全にキャプチャできます（モジュール自身と同じライフタイム契約です）。

#### 主要メソッド

*   **`CreateContent`**: アンパックタブの Slate UI（プリセットドロップダウン、ソースピッカー、2×2 チャンネルグリッド、出力設定、アンパックボタン）を構築します。
*   **`UpdatePreview`**: Mip 0 をロックして全ピクセルをちょうど1回走査し、4チャンネル分のボックスダウンサンプリングと均一チャンネル検出を同一パスで行った後、4枚のグレースケールプレビューテクスチャを構築します。フル解像度のコピーや `FColor` バッファは一切確保しません（後述の *メモリ特性* を参照）。
*   **`OnExtractClicked`**: 入力を検証し、上書き確認（対象アセットを一覧表示する単一ダイアログ）を行った後、選択された各チャンネルにつき1枚の `TSF_G8` テクスチャアセット（`TC_Grayscale`、`SRGB = false`）をソース解像度で書き出します。
*   **`AutoGenerateBaseName`**: ソース名から既知のパックサフィックス（各プリセットの `FileNameSuffix`）を除去し、`T_` プレフィックスを付与します。チャンネルごとの出力名は、選択中のプリセットのサフィックスを使った `<Base><UnpackSuffix>` になります。

アンパックタブはモジュール所有のプリセット配列を共有します（`FChannelPackerPreset` に `UnpackSuffixR/G/B/A` フィールドが追加され、既存の JSON ファイルは `_R`/`_G`/`_B`/`_A` をデフォルトとして読み込まれます）。プリセットの保存・削除後は、モジュールが `OnPresetListChanged` を呼び出してアンパック側のドロップダウンを同期します。

### データ構造

バックグラウンドスレッドから `UObject` のメソッド (例: `LockMip`) にアクセスせずにマルチスレッド処理をサポートするため、モジュールは `TextureChannelPackerUtils` 名前空間 (`TextureChannelPackerShared.h`) で定義された2つのヘルパー構造体を使用します。

#### `FTextureRawData`
ゲームスレッドからワーカースレッドへ生のピクセルデータを転送するために使用されます。
```cpp
struct FTextureRawData
{
    TArray<uint8> RawData;      // Mip 0 の生バイトデータ
    int32 Width;                // テクスチャの幅
    int32 Height;               // テクスチャの高さ
    ETextureSourceFormat Format;// 例: TSF_BGRA8, TSF_G8
    FString TextureName;        // ログ/デバッグ用
    bool bIsValid;              // 抽出に成功した場合 true
    FText ErrorMessage;         // 抽出失敗時のユーザー向けエラーメッセージ
};
```

#### `FTextureProcessResult`
ワーカースレッドからゲームスレッドへ、処理済みの単一チャンネルデータを返すために使用されます。
```cpp
struct FTextureProcessResult
{
    TArray<uint8> ProcessedData; // 常に 8bit シングルチャンネル (0-255)
    FText ErrorMessage;          // 失敗時のエラーメッセージ
    bool bSuccess;               // 成功フラグ
};
```

## 処理フロー

テクスチャ生成パイプライン (`CreateTexture`) は、応答性とスレッドセーフ性を考慮して設計されています。

1.  **抽出 (ゲームスレッド)**
    -   各入力 (R, G, B, A) に対して `ExtractTextureSourceData` が呼び出されます。
    -   `UTexture2D` の `Source` ミップマップをロックし、生バイトデータを `FTextureRawData` に `Memcpy` します。
    -   これにより、バックグラウンドスレッドを UObject の有効性チェックから分離します。

2.  **処理 (並列スレッド)**
    -   `ParallelFor` を使用して、全4チャンネルに対して `ProcessTextureSourceData` を並行して実行します。
    -   **フォーマット変換**: `TSF_BGRA8` (選択したソースチャンネルを抽出、デフォルトは Red), `TSF_G8` (グレースケール), `TSF_G16` (16bit グレースケール), および Float 形式 (`TSF_R16F`, `TSF_R32F`, `TSF_RGBA32F`) をサポートします。すべて 8bit `uint8` に変換されます。
    -   **リサイズ**: 入力解像度が `TargetWidth` や `TargetHeight` と異なる場合、`FImageUtils::ImageResize` が使用されます。

3.  **再構築 (ゲームスレッド)**
    -   新しい `UTexture2D` がパッケージ内に作成 (または更新) されます。
    -   書き込み用に `Source` ミップがロックされます。
    -   4つの `FTextureProcessResult` 配列からのデータが、最終的な `BGRA8` メモリレイアウトにインターリーブ (結合) されます。
    -   `UpdateResource()` と `PostEditChange()` が呼び出され、アセットがファイナライズされます。

### アンパックフロー

アンパックパスは `FTextureRawData` / `ProcessTextureSourceData` を経由しません。出力解像度は常にソース解像度と一致するためリサイズが不要であり、したがって `FColor` の中間バッファも不要で、ロックしたミップから直接チャンネルを読み出します。

1.  **サンプラーのディスパッチ**: `VisitChannelSampler` が `ETextureSourceFormat` で1回だけ分岐し、8bit へのインライン変換を行う `uint8 (int64 PixelIndex, int32 ChannelIndex)` 形式のサンプラーを呼び出し側に渡します。ピクセルループの外側で分岐することで内側のループから条件分岐を排除し、ピクセルインデックスが 64bit のため 2GB を超えるソース（例: 16K の `RGBA32F`）も中間バッファなしで扱えます。
2.  **抽出 (並列スレッド)**: `ExtractChannelBytes` が `ParallelFor` でサンプラーを通して読み出し、選択された各チャンネルにつき1バイト/ピクセルの配列を1つ埋めます。この間ミップはロックされたままです。
3.  **アセット作成 (ゲームスレッド)**: 選択された各チャンネルについて `TSF_G8` の Source を初期化し、チャンネルのバイト列を memcpy した後、`TC_Grayscale` 圧縮・`SRGB = false` でアセットをファイナライズします。

単一チャンネル形式（`G8`/`G16`/`R16F`/`R32F`）は、R/G/B にその唯一の値を、Alpha には不透明の `255` を返します。これはパッキングパイプラインが `FColor` へ拡張する際の扱いと一致します。（`ProcessTextureSourceData` 側は意図的に「どのチャンネルを要求されても輝度値を返す」ルールを維持しています。パックタブでは、グレースケールマスクを Alpha スロットに割り当てる際にこの挙動に依存しているためです。）

### メモリ特性

アンパックの両パスは、ピーク使用量がソースのピクセルあたりバイト数に比例しないよう設計されています。

*   **プレビュー**: `ScanAndDownsampleChannels` は全ソースピクセルをちょうど1回走査し、ボックスフィルタの合計を4つの `DstW*DstH` 出力配列へ直接累積します。処理は出力行単位で分割され、各行が互いに素なソース行の帯を担当して自身の出力スライスのみに書き込むため、同期もマージも不要です。均一値検出も同じパスに統合されており、行ごとの判定を後段でマージすることで厳密性を保っています（平均化だけでは1ピクセルの差異が埋もれてしまうため）。確保量はソースサイズによらず約 256KB なので、16K テクスチャを選択してもメモリが跳ね上がりません。
*   **抽出**: ピークは「選択チャンネル数 × 1バイト/ピクセル」（例: 8K を全チャンネル展開して 4 × 67MB）であり、ソース形式には依存しません。

なお、作成された*出力アセット*自体は VRAM を消費します。`TC_Grayscale` は非圧縮の `PF_G8` になるため、8K のチャンネル1枚あたり約 67MB（＋ミップ）です。

## 拡張ポイント

### 新しい圧縮設定の追加
`StartupModule` を修正して、`CompressionOptions` に新しい `FCompressionOption` エントリを追加します。
```cpp
FCompressionOption MyOption;
MyOption.InternalName = "MyNewSetting";
MyOption.CompressionSetting = TC_HDR; // 適用する TextureCompressionSettings 列挙値
MyOption.DisplayNameEn = "My New Setting";
MyOption.DisplayNameJa = "新しい設定";
CompressionOptions.Add(MakeShared<FCompressionOption>(MyOption));
```
これ以外の変更は不要です。`GetSelectedCompressionSettings` は選択中オプションの `CompressionSetting` メンバーを自動的に返します。

### 新しい入力フォーマットのサポート
`ProcessTextureSourceData` 内の `switch(Input.Format)` ブロックを更新し、追加の `ETextureSourceFormat` 型 (例: `TSF_BC1`) を処理できるようにします。

### ローカリゼーション (多言語対応)
モジュールは `LOCTEXT_NAMESPACE` とヘルパー関数 `GetLocalizedMessage` を使用して、英語と日本語をサポートしています。新しいユーザー向けの文字列はすべて、このパターンを使用してバイリンガルサポートを維持する必要があります。
