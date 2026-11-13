# カードのサーバー権威と受信者別配信（計画6）

4人ババ抜きの配札・手番・引く相手・ペア除去・勝敗・再開をRustサーバーが管理する。
C++側は「相手の何番目を引くか」を送り、サーバーの返信から既存のカード画面を描く。
返信に含む札のIDは認証した本人の手札だけ。他の席は枚数だけで、クライアントで配札やCPU対戦を実行しない。
従来のUDP座標中継 `GameEngine.exe --server` とは別のTLS/TCPサーバーである。
2026-10-04にTLSを必須化した。平文TCPへのフォールバックはない。旧UDP中継は今回のTLS対象外。

## 今すぐ1人＋CPU3人で遊ぶ

今回ビルド済み。PowerShellを2つ開き、両方とも `プロジェクトルート` で実行する。

```powershell
# ターミナルA：TLS用の新しいディレクトリを作る（初回だけ）
.\output\tools\card_authority.exe --init-tls output\card-demo-tls
# 新しいkeysディレクトリなら席の認証ファイルも生成する
.\output\tools\card_authority.exe --bind 127.0.0.1:7778 --humans 1 --keys-dir output\card-demo-keys --tls-dir output\card-demo-tls
```

`CARD READY` が出たら次を実行する。

```powershell
# ターミナルB：席0として接続（オンライン用対策は省略してもこの2つが既定）
.\output\packages\game-protected-final\GameEngine.exe --mode card --card-server 127.0.0.1:7778 --card-seat 0 --card-token-file output\card-demo-keys\seat0.key --card-cert output\card-demo-tls\server.der --security authority,private-hands
```

A/Dまたは方向キー左右で札を選び、Spaceまたはパッドの×で引く。決着後もSpace/×で次の対局を開始できる。
ESCでクライアント終了、サーバーはCtrl+Cで終了。サーバー再起動時に同じキーは使えるが、対局状態の保存はしない。
`--card-autoplay` は入力の自動試験用で、1対局が終わるとクライアントを正常終了する。

既存のkeysディレクトリは4つの正しいファイルが揃っている場合だけ再利用する。
各ファイルは改行なし32桁の16進数。サーバーがOS乱数から生成するので手入力する必要はない。
キーはログや計測ファイルに出さず、リポジトリにも含めない（`*.key` を除外）。

## 別PC・2人での確認手順

サーバー用PCで `--bind 0.0.0.0:7778 --humans 2` として起動する。
サーバーには `--tls-dir` も指定する。席0にはseat0.key、席1にはseat1.keyと、両者へ公開証明書server.derを渡す。
他の席のキー、TLS秘密鍵server.key、全員分の監査記録はサーバーに残す。
各PCで `--card-server サーバーPCのIPv4:7778`、自分の `--card-seat` / `--card-token-file`、`--card-cert server.der` を指定する。
human席は0から連番、残りの席はサーバー内のCPU。指定したhuman席がすべて接続すると始まる。
切断中は進行を止め、同じキーでクライアントを起動し直すと同じ手札・手番へ戻る。
自動再接続はまだない。決着済みの結果は他のプレイヤーが終了しても表示できる。
再開は全humanが接続中の決着状態で受け付け、全員の投票方式ではない。

今回確認したのは同一PC上の別プロセス間。別PCでの通信・Windows Firewall越しの接続は未確認。
TCP 7778の通信許可が必要になる場合がある。ファイアウォールを無効化する手順は不要。

この版はTLS 1.2以上による暗号化と、事前に配った証明書によるサーバー認証を行う実験版。
盗聴への保護と、認証済みの相手へ何を渡すかの制限は別々に検証する。
端末にサーバー全体や全員のキーも置くデモは、手元の管理者から他人の札を隠せた証明にならない。
別PC評価では信頼するサーバーを分離し、各クライアントの受信バイトだけを記録して照合する。
既に自分が見た札の記憶・推測・他プレイヤーとの情報共有や入力自動化は防がない。

## ビルドと検証

```powershell
cargo build --manifest-path server_rs\Cargo.toml --release --locked
cargo test --manifest-path server_rs\Cargo.toml --locked
cmake -S . -B build-event -DGAME_PROFILE=all -DHARDENED=OFF
cmake --build build-event --config Release --target game card_client_probe security_test
python tools\card_client_probe\test_tls.py
python tools\card_client_probe\test_field.py
```

Rust 2021、getrandom 0.3.4 / rustls 0.23.45 / rcgen 0.14.10等をCargo.lockで固定。
サーバー自身のコードは`unsafe`を禁止し、構造体のメモリを直接送信しない。依存ライブラリ全体までunsafe不使用という意味ではない。
Rust単体試験は受信者以外の手札を変えても本人向けバイト列が変わらないこと、乱数取得失敗時の巻戻し、
不正操作の拒否、切断と再接続、29組の固定テスト乱数による53枚の保存・26ペアの消去・再開を確認する。
実プロセス試験は4席の送信範囲、誤った認証・二重接続、不正操作・再送、異常フレーム、再接続、
2台分のC++クライアントによる2対局、描画付きクライアントによる1対局を確認する。
出力は `output/card-authority-tests/<実行ID>/`。`*.wire` / `*.response` は返信だけを保存し、認証要求は保存しない。

2026-10-04の最新結果はRust単体6件・TLS実プロセス11件・別PC用収集コマンドのローカル試験1件すべて成功。
TLSの記録は `output/card-authority-tests/6954432d119a4530a3e2d4d272dff909/`。
収集試験は `output/card-field-tests/5df70593976649679b8e2b8afee11517/`（別PC確認済みとは判定しない）。
TLS11件は既存6件＋誤証明書、期限切れ、平文/旧TLS、暗号化記録、改ざん拒否の5件。
保護済み配布版GameEngineでも1対局完走。C++2席2対局では、サーバー側の席別送信記録と受信記録が完全一致した。
実カード画面の試験は描画しながら自動操作して完走した確認であり、人による見た目・操作感の確認ではない。

手動の受信記録は次のように取れる。`--out` は新しいファイル名にする。

```powershell
.\build-event\Release\card_client_probe.exe --host 127.0.0.1 --port 7778 --seat 0 --token-file output\card-demo-keys\seat0.key --cert output\card-demo-tls\server.der --out output\seat0-new.wire --autoplay --rounds 1
```

## ACAR v1の固定長プロトコル

TLSの内側の全整数はリトルエンディアン。要求は40バイト、返信は64バイト。未使用領域は0。
TCPの1回のreadが1メッセージになるとは限らないので、所定の長さまで読み、フレーム全体に2秒の期限を設ける。
クライアントは80msごとにPOLLする。2秒以上何も送らない接続も終了する。
サーバーの同時接続は16、認証後の要求は各接続100回/秒を上限にする。
Windowsでacceptしたソケットに非同期設定が継承されるため、ワーカースレッド内では明示的に同期モードへ戻す。

| 要求のバイト位置 | 内容 |
|---|---|
| 0–3 / 4 | `ACAR` / 版1 |
| 5 / 6 / 7 | 操作: HELLO=1, POLL=2, DRAW=3, RESTART=4 / 席または札の添字 / 予約0 |
| 8–15 / 16–23 | 操作番号 / クライアントが見たrevision（u64） |
| 24–39 | HELLOだけ席のトークン。他の操作は0 |

最初のHELLOで席を認証・固定し、その後の要求では席の変更を認めない。
POLLの操作番号/revisionは0。DRAW/RESTARTでは返信のnext sequenceとrevisionを返す。
番号が正しい試行は、手番違い等で拒否しても番号を消費する。古いrevision、重複番号、範囲外の添字は札を動かさない。
引く札のID・相手の席・残り枚数・勝敗は要求項目に含めない。

| 返信のバイト位置 | 内容 |
|---|---|
| 0–5 / 6 / 7 | `ACAR`,1,128 / エラー / 本人の席 |
| 8–15 / 16–23 / 24–27 | 次の操作番号 / revision / round |
| 28 / 29 / 30 / 31 | 手番 / 引く相手 / 敗者（不在255） / 本人の枚数 |
| 32–35 / 36 | 各席の枚数 / 捨てたペア数 |
| 37 / 38 / 39 | 状態0待機・1対局・2決着 / human席のbitmask / 予約0 |
| 40–53 | 本人の札IDだけ。未使用は0。IDは0–51、ジョーカー52 |
| 54–63 | 予約0 |

エラー: 0成功、1手番/対局状態違い、2添字違い、3古いrevision、4操作番号違い、5決着前の再開、6利用不可。
C++側も版・予約域・宛先・枚数・札の重複・総数とペア数の整合性を検査する。
全手札を送るデバッグ操作や配札seedの送信は用意しない。

## 設計を読む順番

1. `src/lib.rs` の `command`：入力の妥当性を調べてからサーバーの正本を更新。
2. 同ファイルの `snapshot`：公開項目と本人の手札だけを新しい0初期化バッファにコピー。
3. `src/main.rs`：席を認証して接続に固定、各処理をMutexで直列化。
4. `engine/network/CardClient.cpp`：通信を専用スレッドで進め、更新された画面用状態を受け取る。
5. `game/modes/CardMode.cpp` の `UpdateRemote`：本人の手札と他席の枚数を別々に保持。

FPS/hideの権威化・可視性による配信制御、クラウド描画、インターネット向けの運用は今回の実装外。

## TLSの仕組みと信頼する範囲

Rust側はrustls、Windows C++側はSchannelを使う。暗号方式そのものは自作しない。
サーバーが提示したDER証明書が `--card-cert` と完全一致し、有効期間内にあることを確かめてから席の認証を送る。
通常の公開CA＋ホスト名検証ではなく、事前配布した証明書そのものをサーバーの識別子とする方式。
信頼できる経路でserver.derを渡し、別経路でSHA256を照合する。初回接続を無条件に信頼するTOFUはしない。

`--init-tls` はECDSA P-256の自己署名証明書を作り、有効期限を生成時から90日にする。
server.keyは秘密、server.der/server.pemは公開。期限前の更新時は新ディレクトリで生成し、クライアントのpinも配り直す。
OSの信頼ストアは変更しない。証明書失効リストの運用や自動ローテーションは実装していない。
Schannelの新しい資格情報APIを使うためWindows 10 1809以降を想定し、今回の実測環境はWindows 11。
TLS 1.3はOS依存で、このPCのC++接続ではTLS1.3を確認した。Pythonの接続でTLS1.2互換性も確認した。

ハンドシェイクは全体3秒、メッセージは全体2秒、TLS受信バッファは64KiBまで。
Rust側は細切れの下位read/writeにも残り時間を適用する。C++も暗号化レコードの分割受信を扱う。
早期データとセッションチケットを無効化し、C++は再ネゴシエーション時に接続を閉じる。
暗号化記録の試験ではアプリケーション識別子と認証トークンが平文で現れず、レコードの1バイト改変は拒否された。
これは当該試験の結果であり、通信実装全体の第三者監査や公衆インターネットでの負荷試験の代わりではない。

実装根拠: [rustls StreamOwned](https://docs.rs/rustls/0.23.45/rustls/struct.StreamOwned.html)、
[Schannel資格情報の手動検証](https://learn.microsoft.com/en-us/windows/win32/secauthn/manually-validating-schannel-credentials)、
[TLS_PARAMETERS](https://learn.microsoft.com/en-us/windows/win32/api/schannel/ns-schannel-tls_parameters)。

## 別PCで記録を集めるコマンド

各PCにPython 3と `tools/card_client_probe/field_check.py` を置く。クライアント用probeは検証道具として別配布する。
信頼するサーバーPCで、次を実行しておく。出力先は毎回新しくする。

```powershell
python field_check.py serve --server .\card_authority.exe --out .\field-server-01 --humans 2 --bind 0.0.0.0:7778 --seconds 600
```

`field-server-01/ready.txt` に待受先が出たら、公開証明書と該当席のキーだけを各PCへ渡す。
異なる2台のクライアントで同時に以下を実行する。IPは実際のサーバーIPv4へ、席1ではseat1.keyと`--seat 1`へ変える。

```powershell
python field_check.py client --probe .\card_client_probe.exe --host 192.168.1.10 --seat 0 --token-file .\seat0.key --cert .\server.der --out .\field-client0-01 --rounds 2
```

終了後、client0/client1の結果フォルダをサーバー側へ回収し、そこで照合する。サーバー側の全記録をクライアントへ配らない。

```powershell
python field_check.py verify --server .\field-server-01 --clients .\field-client0-01 .\field-client1-01 --out .\field-report-01.json
```

照合は本人以外の席番号、予約域への余計なデータ、札IDの異常、欠損、バイト不一致、未完走を拒否する。
送信元/受信先のOS識別子のハッシュも記録し、同じホストの場合は `distinct_host_evidence=false`、終了コード2となる。
ホスト情報は自己申告であり、物理PCの存在を暗号学的に証明しない。使用した実機は運営者が確認する。
現在はこのローカル予行まで完了。実際の別PC・Firewall越しの確認は接続先が得られていないため未実施。

`--audit-dir` による席別記録は検証時だけ有効にする。全席を合わせると秘匿情報を含み、サーバー上で管理する。
各返信をディスクへflushする負担もあるので、通常プレイや通信性能測定では監査記録を無効にする。
受信記録の一致に加えて、Rust単体試験で「他席の札だけを変えても、本人向け返信が変わらない」ことも確認する。
