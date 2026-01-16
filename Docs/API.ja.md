[English](API.md)

# TextureChannelPacker API ドキュメント

このドキュメントでは、内部アーキテクチャの理解、機能の拡張、プロジェクトへの貢献を希望する開発者向けに、`TextureChannelPacker` モジュールの技術的な詳細を解説します。

## 概要

**TextureChannelPacker** は、個々のグレースケールまたはカラーテクスチャ (赤、緑、青、アルファ) を単一の RGBA テクスチャアセットにパッキングするための Slate ベースの UI ツールを提供する、エディタ専用のプラグインモジュールです。

### 主な機能
- **エディタ統合**: レベルエディタの「ツール (Tools)」メニューに統合されています。
- **スレッドセーフ**: 明示的なデータの抽出と再構築の手順を使用し、ゲームスレッド上で `UTexture2D` リソースを安全に扱います。
- **並列処理**: `ParallelFor` を使用して、テクスチャチャンネルの処理とリサイズを並行して行います。
- **スマート命名機能**: 入力アセットに基づいて出力ファイル名を自動生成します。

## モジュールアーキテクチャ

このモジュールは、`IModuleInterface` を継承する単一の主要クラス `FTextureChannelPackerModule` として実装されています。

*   **ソースパス**: `Plugins/TextureChannelPacker/Source/TextureChannelPacker/`
*   **ヘッダー**: `Public/TextureChannelPacker.h`
*   **実装**: `Private/TextureChannelPacker.cpp`

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

### データ構造

バックグラウンドスレッドから `UObject` のメソッド (例: `LockMip`) にアクセスせずにマルチスレッド処理をサポートするため、モジュールは `TextureChannelPacker.cpp` で定義された2つのヘルパー構造体を使用します。

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
    -   **フォーマット変換**: `TSF_BGRA8` (赤チャンネル抽出), `TSF_G8` (グレースケール), `TSF_G16` (16bit グレースケール), および Float 形式 (`TSF_R16F`, `TSF_R32F`, `TSF_RGBA32F`) をサポートします。すべて 8bit `uint8` に変換されます。
    -   **リサイズ**: 入力解像度が `TargetResolution` と異なる場合、`FImageUtils::ImageResize` が使用されます。

3.  **再構築 (ゲームスレッド)**
    -   新しい `UTexture2D` がパッケージ内に作成 (または更新) されます。
    -   書き込み用に `Source` ミップがロックされます。
    -   4つの `FTextureProcessResult` 配列からのデータが、最終的な `BGRA8` メモリレイアウトにインターリーブ (結合) されます。
    -   `UpdateResource()` と `PostEditChange()` が呼び出され、アセットがファイナライズされます。

## 拡張ポイント

### 新しい圧縮設定の追加
`StartupModule` を修正して、`CompressionOptions` に新しいエントリを追加します。
```cpp
CompressionOptions.Add(MakeShared<FString>("My New Setting"));
```
その後、`GetSelectedCompressionSettings` を更新して、適切な `TextureCompressionSettings` 列挙値を返すようにします。

### 新しい入力フォーマットのサポート
`ProcessTextureSourceData` 内の `switch(Input.Format)` ブロックを更新し、追加の `ETextureSourceFormat` 型 (例: `TSF_BC1`) を処理できるようにします。

### ローカリゼーション (多言語対応)
モジュールは `LOCTEXT_NAMESPACE` とヘルパー関数 `GetLocalizedMessage` を使用して、英語と日本語をサポートしています。新しいユーザー向けの文字列はすべて、このパターンを使用してバイリンガルサポートを維持する必要があります。
