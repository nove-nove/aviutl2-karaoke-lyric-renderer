# 開発者向け情報

この文書では、リポジトリ構成、ビルド方法、GitHub Actions、同梱 SDK ファイルについて説明します。

## ディレクトリ構成

- `src`
  プラグイン本体です。
- `scripts/build.bat`
  ローカルビルド用スクリプトです。
- `vendor/aviutl2_sdk`
  ビルドに必要な AviUtl2 SDK ヘッダです。
- `.github/workflows/release.yml`
  GitHub Actions によるビルドと Release の定義です。

## ローカルビルド

1. Visual Studio Developer PowerShell または Native Tools Command Prompt を開きます。
2. リポジトリルートへ移動します。
3. `scripts\build.bat` を実行します。
4. `build\AviUtl2KaraokeLyricRenderer.auf2` が生成されます。

`scripts\build.bat` は引数でバージョン文字列を受け取れます。未指定時は `dev` になります。

例:

```powershell
scripts\build.bat v0.1.0
```

## GitHub Actions

- `v` で始まるタグを push すると、Release 用ワークフローが動作します。
- ワークフローは Windows 上でビルドし、`aviutl2-karaoke-lyric-renderer.zip` を作成して GitHub Release に添付します。
- `workflow_dispatch` から手動実行した場合は、Release を作らずに成果物を取得できます。
- タグ名に `rc` を含む場合は GitHub の pre-release として作成されます。
- `rc` を含まない `v` タグは通常の安定版 Release として作成されます。
- タグ push 時はタグ名がプラグイン情報のバージョン文字列として埋め込まれます。
- `workflow_dispatch` では任意のバージョン文字列を指定できます。未指定時は `dev-<short sha>` になります。

## 同梱 SDK ファイル

`vendor/aviutl2_sdk` には、AviUtl ExEdit2 Plugin SDK 由来のファイルを同梱しています。

- `filter2.h`
  AviUtl2 プラグインの UI 項目やフィルタ定義に必要なヘッダです。
- `logger2.h`
  AviUtl2 のログ出力機能を利用するためのヘッダです。
- `LICENSE.txt`
  同梱している SDK ファイルのライセンスです。

これらのファイルは本プラグインのビルドに必要な最小限の SDK ファイルです。
本リポジトリ本体のライセンスとは別に、SDK 由来ファイルには `vendor/aviutl2_sdk/LICENSE.txt` の条件が適用されます。
