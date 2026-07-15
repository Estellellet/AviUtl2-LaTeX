# AviUtl2LaTeX

AviUtl2でLaTeXを透明背景の高品質画像として扱うメディアオブジェクトプラグインです。インライン数式、`align*`、`equation*`、自由記述の`document`、単色の`tikzpicture`に対応します。

## 必要な外部ソフト

本パッケージはLuaLaTeX、TeXディストリビューション、MuPDF、フォントを同梱しません。利用者が別途、[MiKTeX](https://miktex.org/download)または[TeX Live](https://tug.org/texlive/)と、[MuPDF](https://mupdf.com/releases/)の`mutool.exe`を導入してください。プラグインは外部ソフトを自動ダウンロードせず、shell escapeも使用しません。

オブジェクト設定の「環境」→「環境設定」で`lualatex.exe`と`mutool.exe`を自動検出または明示指定し、「環境確認」で必要パッケージを診断します。

## インストールと使用方法

ZIP内の`AviUtl2LaTeX`フォルダをAviUtl2の`Plugin`フォルダ直下へ配置します。LaTeXオブジェクトを追加し、ソースとテンプレートを設定して「コンパイル」を押します。入力中の自動コンパイルは行いません。

ソース中で、前後の空白を除いた行が`%<step>`と完全一致すると表示ステップを分割します。最大32ステップで、累積表示・フェード・リビールに利用できます。

## データ保存場所

- 設定: `%APPDATA%\AviUtl2LaTeX\settings.json`
- キャッシュ: `%LOCALAPPDATA%\AviUtl2LaTeX\cache`
- 一時作業: `%LOCALAPPDATA%\AviUtl2LaTeX\work`
- ログ: `%LOCALAPPDATA%\AviUtl2LaTeX\logs\latest.log`

アンインストールはPlugin内の`AviUtl2LaTeX`フォルダを削除します。利用者データも不要なら上記AppDataフォルダを手動で削除してください。

## 不具合報告

AviUtl2のバージョン、プラグインのバージョン、使用テンプレート、再現手順、使用フォント名、`latest.log`と「診断情報をコピー」で取得した情報を添えてGitHub Issuesへ報告してください。LaTeXソースに機密情報が含まれる場合は、そのまま公開せず最小の再現用ソースへ置き換えてください。

## トラブルシューティング

- コンパイルに失敗したら、オブジェクト設定の「環境」→「情報」を開き、短いエラー分類と失敗段階を確認します。
- 「診断情報をコピー」で、個人パスやLaTeXソースを含まない報告用情報を取得できます。詳細は「ログを開く」で`latest.log`を確認します。
- 外部ツールが未設定なら「環境設定」でLuaLaTeXとMuPDFを指定し、「環境確認」を実行します。
- フォントが見つからない場合は、インストール済みのファミリー名か、存在する`.otf`、`.ttf`、`.ttc`を選択してください。参照中のフォントファイルを移動・削除しないでください。
- 再起動後に対応する永続キャッシュがない場合は、手動で「コンパイル」を押す必要があります。ユーザー向けのキャッシュ削除UIはありません。

本プラグインはSDKサンプルが要求するAviUtl2本体バージョン`2.00.3300`以降を宣言します。

## ライセンス

[LICENSE](LICENSE)と[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)を参照してください。
