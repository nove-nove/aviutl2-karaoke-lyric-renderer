# AviUtl2 Karaoke Lyric Renderer

`AviUtl2 Karaoke Lyric Renderer` は、タイムタグ付き歌詞を AviUtl2 上で直接レンダリングするプラグインです。

正式名称は `AviUtl2 Karaoke Lyric Renderer` です。プラグイン情報には `Timed karaoke lyric renderer for AviUtl2 by novenove` が表示され、AviUtl2 上では `カスタムオブジェクト` ラベルで利用します。

## 特徴

- `.exo` を生成せず、AviUtl2 上でカラオケ歌詞を直接描画します
- `ASSルビカラオケ` 形式のパーサーで `RhythmicaLyrics` 形式も処理できます
- ルビ、ワイプ色、縁取り、余白、表示行数などを調整できます
- 歌詞ファイルを直接読むため、元ファイルの編集内容が反映されます

## ディレクトリ構成

- `src`: プラグイン本体
- `scripts/build.bat`: ローカルビルド用スクリプト
- `docs/Usage.md`: 使い方
- `vendor/aviutl2_sdk`: ビルドに必要な AviUtl2 SDK ヘッダ

## ローカルビルド

1. Visual Studio Developer PowerShell または Native Tools Command Prompt を開きます。
2. リポジトリルートへ移動します。
3. `scripts\build.bat` を実行します。
4. `build\AviUtl2KaraokeLyricRenderer.auf2` が生成されます。

## 導入方法

1. Release に含まれる `AviUtl2KaraokeLyricRenderer.auf2` を取得します。
2. `AviUtl2KaraokeLyricRenderer.auf2` を AviUtl2 のプラグインフォルダへ配置するか、AviUtl2 のプレビュー画面へドラッグアンドドロップして既定のフォルダへインストールします。
3. AviUtl2 を起動し、`カスタムオブジェクト` から本プラグインを追加します。
4. タイムタグ付き歌詞ファイルを選び、各種描画設定を調整します。

## GitHub Actions

- `v` で始まるタグを push すると、Release 用ワークフローが動作します。
- ワークフローは Windows 上でビルドし、`aviutl2-karaoke-lyric-renderer.zip` を作成して GitHub Release に添付します。
- `workflow_dispatch` から手動実行した場合は、Release を作らずに成果物を取得できます。

## 同梱 SDK ファイル

`vendor/aviutl2_sdk` には、AviUtl ExEdit2 Plugin SDK 由来の `filter2.h`、`logger2.h`、ライセンスファイルを同梱しています。詳細は `vendor/aviutl2_sdk/LICENSE.txt` を参照してください。

## ライセンス

このリポジトリ内の本プラグイン本体は MIT License です。詳細は `LICENSE` を参照してください。

同梱している AviUtl ExEdit2 Plugin SDK 由来ファイルについては `vendor/aviutl2_sdk/LICENSE.txt` を参照してください。

## 使用上の注意

本プラグインの利用によって生じた、いかなる損害についてもプラグイン作者は責任を負いません。
