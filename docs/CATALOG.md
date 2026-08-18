# AviUtl2-LaTeX

AviUtl2上で **LaTeX数式・日本語文書・TikZ図形** を描画するメディアオブジェクトプラグインです。

LuaLaTeXで組版した結果を高解像度で描画し、AviUtl2側から文字色、表示ステップ、フェード、リビールなどを調整できます。

> [!IMPORTANT]
> このプラグインだけでは数式を描画できません。  
> **MiKTeXまたはTeX Live**と、**MuPDF**を別途導入してください。

## 作れるもの

### 数式

![数式の表示例](https://raw.githubusercontent.com/Estellellet/AviUtl2-LaTeX/refs/heads/main/docs/assets/math-examples.png)

### 日本語文書

![日本語文書の表示例](https://raw.githubusercontent.com/Estellellet/AviUtl2-LaTeX/refs/heads/main/docs/assets/document-example.png)

### TikZ図形

![TikZの表示例](https://raw.githubusercontent.com/Estellellet/AviUtl2-LaTeX/refs/heads/main/docs/assets/tikz-example.png)

### 累積表示

ソース内を`%<step>`で区切ると、式や図形を順番に追加表示できます。

![累積表示の例](https://raw.githubusercontent.com/Estellellet/AviUtl2-LaTeX/refs/heads/main/docs/assets/cumulative-demo.gif)

### TikZ図形の累積表示

TikZ図形も`%<step>`で区切ることで、点、線、補助線などを順番に追加表示できます。

![TikZ図形の累積表示](https://raw.githubusercontent.com/Estellellet/AviUtl2-LaTeX/refs/heads/main/docs/assets/tikz-cumulative-demo.gif)

---

## 動作要件

- Windows 10以降
- AviUtl2 2.00.3300以降
- LuaLaTeXを含むTeX環境
  - [MiKTeX](https://miktex.org/download)
  - または[TeX Live](https://www.tug.org/texlive/acquire-netinstall.html)
- [MuPDF](https://mupdf.com/releases)に含まれる`mutool.exe`

LuaLaTeX、MiKTeX、TeX Live、MuPDF、フォントは、本プラグインには含まれません。


## 導入手順

初めてLaTeX環境を導入する場合は、次の順序で進めてください。

1. AviUtl2-LaTeXをインストール
2. MiKTeXをインストール
3. MuPDFをダウンロードして展開
4. AviUtl2-LaTeXの「環境設定」で2つの実行ファイルを指定
5. 「環境確認」を実行

### 1. AviUtl2-LaTeX

AviUtl2カタログからインストールできます。

### 2. MiKTeX

1. [MiKTeX公式ダウンロードページ](https://miktex.org/download)からインストーラーをダウンロードして実行する。（設定はすべて変更なしでOK）

MiKTeXは不足しているLaTeXパッケージを追加導入できます。初回コンパイル時にパッケージ導入の確認が表示された場合は、内容を確認して許可してください。

> [!TIP]
> TeX Liveを既に使用している場合は、MiKTeXを追加で入れる必要はありません。TeX Liveに含まれる`lualatex.exe`を指定できます。

### 3. MuPDF

1. [MuPDF公式リリースページ](https://mupdf.com/releases)からWindows用のMuPDFをダウンロードする。
2. 展開したフォルダ内に`mutool.exe`があることを確認します。
3. 後から移動しなくて済む場所へフォルダを置きます。

> [!WARNING]
> 設定後に`mutool.exe`やフォントファイルを移動・削除すると、再コンパイルに失敗します。

## 環境設定の方法

LaTeXオブジェクトを追加し、設定画面下部の「環境」を展開して「環境設定」を押します。

自動検出で検出できなかった場合は以下の手順を参照してください。

### LuaLaTeX

「参照」を押し、TeX環境に含まれる`lualatex.exe`を選択します。

場所が分からない場合は、コマンドプロンプトを開いて次を実行してください。

```bat
where lualatex
```

パスが表示された場合は、その`lualatex.exe`を指定します。

`where lualatex`で見つからない場合でも、MiKTeXまたはTeX Liveが正しく入っていれば、環境設定の「参照」から直接選択できます。

### MuPDF

「参照」を押し、先ほど展開したMuPDFフォルダ内の`mutool.exe`を選択します。

### 環境確認

2つのパスを指定した後、「環境確認」を押します。

次がすべて正常であれば設定完了です。

```text
環境は正常です。
基本: 正常
日本語: 正常
TikZ: 正常
PDF変換: 正常
```

自動検出で見つからなくても異常とは限りません。PATHに登録されていない場合は、参照ボタンから手動で指定してください。


## 基本的な使い方

1. AviUtl2で「オブジェクトを追加」を開きます。
2. LaTeXオブジェクトを追加します。
3. テンプレートを選びます。
4. 入力欄へLaTeXソースを記述します。
5. 「コンパイル」を押します。

ソースやフォント設定を変更しただけでは画像は更新されません。変更後は、もう一度「コンパイル」を押してください。


## テンプレートの解説

入力欄には、テンプレートの外側に相当する`\begin{...}`や`\end{...}`を記述しません。プラグインが自動的に補います。

### インライン数式

1行の数式に適しています。

#### 例

```latex
E=mc^2
```

内部ではインライン数式として処理されます。文章全体ではなく、単独の短い式に使用してください。

### `align*`

複数行の式変形や、等号位置を揃えたい場合に使用します。

#### 例

```latex
(x+1)^2
&= x^2+2x+1\\
&= x(x+2)+1
```

`&`を置いた位置が揃います。一般には等号の直前へ置きます。

```latex
左辺 &= 右辺
```

`\begin{align*}`と`\end{align*}`は記述しません。

### `equation*`

番号なしの独立した数式を1つ表示します。

#### 例

```latex
\int_{-\infty}^{\infty} e^{-x^2}\mathrm{d}x=\sqrt{\pi}
```

`\begin{equation*}`と`\end{equation*}`は記述しません。

### `document`

日本語の文章、段落、複数の数式を含む文書に使用します。

#### 例

```latex
加速度$a(t)$が，位置$x(t)$，速度$v(t)$，および，時刻$t$の関数，すなわち，
\[
a(t) = f(x(t),v(t);t)
\]
と表されればよい。本書では$f$を「起動因子」とよぶことにしよう。
```

日本語を使用する場合は、「日本語対応」を有効にしてください。

#### 文書幅

「幅」で自動折り返しの基準幅を指定します。

- `0`: 自動幅を使用しない
- `0`より大きい値: 指定した幅の文書領域を作る

#### フォント

「一覧から選択」ではWindowsにインストールされた日本語フォントを選択できます。

「ファイルから選択」では、次のフォントファイルを直接指定できます。

- `.otf`
- `.ttf`
- `.ttc`

フォントを選択した後は「コンパイル」を押してください。選択したフォントは漢字・仮名・日本語の句読点と、数式内の`\text{日本語}`へ適用されます。欧文本文フォントと数式フォントはLaTeXの既定値から変更されません。

ディスプレイ数式は`\[ ... \]`での記述を推奨します。`$$ ... $$`も既存ソースとの互換性のため利用できますが、自動変換は行いません。

#### 和文字間

- 自動: プロポーショナルな日本語組版を優先
- 均等: 和文を均等幅に近い形で配置
- フォント準拠: フォント側の字間・カーニングを優先

### `tikzpicture`

TikZの図形描画命令を記述します。

#### 例
```latex
\draw[->] (-0.5,0) -- (4,0) node[right] {$x$};
\draw[->] (0,-0.5) -- (0,3) node[above] {$y$};
\draw[domain=0:3,samples=80] plot (\x,{0.25*\x*\x});
```

`\begin{tikzpicture}`と`\end{tikzpicture}`は記述しません。

追加のTikZライブラリが必要な場合は、「TikZ設定」のライブラリ欄へ記述します。

#### 例
```text
arrows.meta,calc,positioning
```


## 累積表示の解説

累積表示では、同じLaTeXオブジェクトの内容を段階的に追加できます。

### ソースの区切り方

表示を切り替えたい位置へ、単独の行として`%<step>`を記述します。

#### 例
```latex
x^2-1 &= 0\\
%<step>
(x-1)(x+1) &= 0\\
%<step>
x &= \pm 1
```

最大32ステップまで使用できます。

### 設定方法

1. 「表示モード」を累積表示にします。
2. ソースへ`%<step>`を追加します。
3. 「コンパイル」を押します。
4. 「表示ステップ」を時間変化させます。
5. 必要に応じて切替効果と効果時間を調整します。

切替効果は、新しく追加される部分にだけ適用されます。既に表示されている式は、そのまま残ります。

#### 例

```latex
-1 &= \sqrt{-1}\cdot\sqrt{-1}\\
%<step>
&= \sqrt{(-1)\cdot(-1)}\\
%<step>
&= \sqrt{1}\\
%<step>
&= 1
```

## バージョン履歴

- **v0.2.0**

  - 日本語フォントを欧文本文・数式フォントから分離
  - フォント設定、キャッシュ、ステップ解析と安全性を改善

- **v0.1.1** (2026-07-17)

  - フォント選択や環境設定のウィンドウのUIを改善

- **v0.1.0** (2026-07-15)

  - リリース


## ライセンス

AviUtl2-LaTeXは[MIT License](https://github.com/Estellellet/AviUtl2-LaTeX/blob/main/LICENSE)で公開しています。

LuaLaTeX、MiKTeX、TeX Live、MuPDF、各フォントには、それぞれ別のライセンスが適用されます。
