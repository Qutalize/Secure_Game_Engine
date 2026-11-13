tiny-AES-c の配置について
==========================

本ディレクトリには tiny-AES-c (https://github.com/kokke/tiny-AES-c) の
AES-128 実装 (aes.h / aes.c) を同梱しています。ライセンスは The Unlicense
（パブリックドメイン相当）で、LICENSE ファイルを参照してください。

通常の運用では git submodule で取得することを推奨します:

    git submodule add https://github.com/kokke/tiny-AES-c third_party/tiny-AES-c

submodule で取得し直す場合は、aes.h の AES128 が定義されていること
（AES192 / AES256 ではないこと）を確認してください。本プロジェクトは
AES-128-CBC を使用します。CMake は third_party/tiny-AES-c/aes.c を
crypto スタティックライブラリとしてビルドします。
