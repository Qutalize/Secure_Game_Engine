"""Generate poster text/illustrated draft from real measurements and archived screenshots."""
from pathlib import Path
import csv, html, json, zipfile
ROOT=Path(__file__).resolve().parents[2]
E=ROOT/'output/benchmarks/poster-20261101'
A=ROOT/'poster_assets'
F=A/'benchmark_20261101'
def read(p): return json.loads(p.read_text(encoding='utf-8-sig'))

def main():
 import matplotlib
 matplotlib.use('Agg')
 import matplotlib.pyplot as plt
 from matplotlib.font_manager import FontProperties
 font=FontProperties(fname=r'C:\Windows\Fonts\meiryo.ttc')
 plt.rcParams.update({'font.family':font.get_name(),'font.size':10,'axes.spines.top':False,'axes.spines.right':False,'axes.unicode_minus':False,'svg.fonttype':'path'})
 rows=read(E/'range-comparison/runs.json'); stats=read(E/'range-comparison/summary.json')
 selected=read(E/'selected-demo/summary.json'); ss=next(iter(selected.values()))
 env=read(E/'environment.json'); inv=read(E/'package-inventory.json'); security=read(E/'security-audit/results.json'); sigtime=read(E/'signature-timing.json')['summary']
 assert all(v['attempts']==30 and v['valid']>0 for v in stats.values())
 assert ss['attempts']==30 and ss['valid']>0 and security['success']
 names=['Native_Unsigned','Native_Signed','Unity_IL2CPP']
 labels=['自作・署名なし\nDX12','自作・署名あり\nDX12','Unity IL2CPP\nD3D11']
 short=dict(zip(names,['自作・署名なし（DX12）','自作・署名あり（DX12）','Unity IL2CPP（D3D11）']))
 colors=['#4e7c86','#127166','#8c6b42']
 sizes={n:sum(f['bytes'] for f in inv[n]) for n in names}
 med={n:stats[n]['launch_to_first_update_ms']['p50'] for n in names}
 values=[[r['launch_to_first_update_ms'] for r in rows if r['case']==n and not r['warmup_run'] and r['status']=='ok'] for n in names]
 fig,ax=plt.subplots(figsize=(10.5,5.8),layout='constrained')
 ax.boxplot(values,tick_labels=labels,whis=(5,95),showfliers=False,medianprops={'color':'#202d39','linewidth':2})
 ymax=max(max(v) for v in values)
 for i,vals in enumerate(values):
  ax.scatter([i+1+((j*7)%13-6)*.017 for j in range(len(vals))],vals,s=20,alpha=.65,color=colors[i],zorder=3)
  ax.text(i+1,max(vals)+ymax*.035,f"中央値 {med[names[i]]:.1f} ms\nn={len(vals)} / 失敗={stats[names[i]]['failed']}",ha='center')
 ax.set_ylim(0,ymax*1.27);ax.set_ylabel('起動要求 → 最初の更新入口（ms）')
 ax.set_title('共通射撃場：起動時間の実測分布',loc='left',fontweight='bold',pad=16);ax.grid(axis='y',alpha=.2)
 fig.supxlabel('各30試行・予熱各1回を除外／1600×900・VSync ON\n描画API・実装・同梱機能が異なる構成比較。画面到達・入力遅延ではない。',fontsize=9)
 for ext in ['png','svg']: fig.savefig(F/f'startup.{ext}',dpi=190)
 plt.close(fig)
 fig,axes=plt.subplots(1,2,figsize=(12,5.1),layout='constrained')
 for ax,key,title in [(axes[0],'median_of_run_frame_p50_ms','各試行の更新間隔中央値の中央値'),(axes[1],'median_of_run_frame_p95_ms','各試行の更新間隔p95の中央値')]:
  vals=[stats[n][key] for n in names];bars=ax.bar(labels,vals,color=colors)
  ax.bar_label(bars,labels=[f'{v:.3f} ms' for v in vals],padding=4)
  ax.set_ylim(0,max(vals)*1.25);ax.set_ylabel('CPU更新間隔（ms）');ax.set_title(title,fontsize=11);ax.grid(axis='y',alpha=.2)
 fig.supxlabel('VSync・待ち時間込み。純粋なCPU処理時間、GPU処理時間、入力→表示遅延ではない。',fontsize=9)
 for ext in ['png','svg']: fig.savefig(F/f'cadence.{ext}',dpi=190)
 plt.close(fig)
 fig,ax=plt.subplots(figsize=(10.5,4.8),layout='constrained')
 vals=[sizes[n]/1e6 for n in names];bars=ax.barh(labels,vals,color=colors);ax.invert_yaxis()
 ax.bar_label(bars,labels=[f'{v:.3f} MB' for v in vals],padding=7);ax.set_xlim(0,max(vals)*1.22)
 ax.set_xlabel('配布ファイルの論理サイズ合計（MB = 1,000,000 bytes）')
 ax.set_title('共通射撃場：今回測定した配布構成の容量',loc='left',fontweight='bold');ax.grid(axis='x',alpha=.2)
 fig.supxlabel('OS・ドライバー・別途必要なVC++ランタイム・開発環境は含まない。Unityの最小到達容量ではない。',fontsize=9)
 for ext in ['png','svg']: fig.savefig(F/f'package-size.{ext}',dpi=190)
 plt.close(fig)
 ratio=sizes['Unity_IL2CPP']/sizes['Native_Signed']; reduction=(1-1/ratio)*100
 startup_reduction=(1-med['Native_Signed']/med['Unity_IL2CPP'])*100
 rejects=sum(c['name'].startswith('reject-') and c['success'] for c in security['cases'])
 conditions=('2026年11月1日、同一PC／Intel Core i7-1265U・Iris Xe／Windows 11 '+env['os'][0]['Version']+'／GPUドライバー '+env['gpu'][0]['DriverVersion']+'。1600×900・VSync ON、共通射撃場の同じtour入力を120Hzで再生。各構成30回＋予熱1回、各回2秒待機後に5秒計測。試行順はラウンドごとに無作為化。キャッシュ・温度・電力変動・常駐アプリは統制していない。ウィンドウのフォーカス・遮蔽は自動検査していない。')
 result={'date':'2026-11-01','conditions':conditions,'range_summary':stats,'distribution_bytes':sizes,'native_signed_capacity_reduction_percent_vs_this_unity_package':reduction,'native_signed_startup_median_reduction_percent_observed':startup_reduction,'selected_summary':selected,'selected_background_load_caveat':'Separate Unity installer detected during latter measurements; timings are reference observations, not isolated baseline','security_audit_cases':len(security['cases']),'tamper_missing_cases_rejected':rejects,'signature_timing':sigtime,'caveat':'DX12 vs D3D11, different implementation/runtime/features; not isolated engine performance or Unity minimum size','unmeasured':['RAM/VRAM','GPU execution time','input-to-display latency','graphics perceptual quality','separate PC','development effort reduction','UE/Godot comparison','cloud gaming']}
 (F/'benchmark-results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
 sample_fields=['case','round','warmup_run','status','launch_to_first_update_ms','frame_p50_ms','frame_p95_ms','frame_p99_ms','frame_count']
 with (F/'measurement-samples.csv').open('w',encoding='utf-8-sig',newline='') as dest:
  writer=csv.DictWriter(dest,fieldnames=sample_fields,extrasaction='ignore');writer.writeheader()
  writer.writerows(rows+read(E/'selected-demo/runs.json'))
 (F/'environment.json').write_text(json.dumps(env,ensure_ascii=False,indent=2),encoding='utf-8')

 blocks=[];txt=[]
 def h(s):blocks.append('<h2>'+html.escape(s)+'</h2>');txt.append('\n\n'+s+'\n')
 def p(s,cls=''):blocks.append('<p class="'+cls+'">'+html.escape(s)+'</p>');txt.append(s+'\n')
 def table(head,body):
  blocks.append('<div class="table-scroll"><table><thead><tr>'+''.join('<th>'+html.escape(str(x))+'</th>' for x in head)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in row)+'</tr>' for row in body)+'</tbody></table></div>')
  txt.append('｜'.join(head)+'\n'+'\n'.join('｜'.join(str(x) for x in row) for row in body)+'\n')
 def photos(items):
  blocks.append('<div class="gallery">')
  for file,caption in items:
   assert (A/file).is_file()
   blocks.append('<figure><a href="'+file+'"><img src="'+file+'" alt="'+html.escape(caption)+'"></a><figcaption>'+html.escape(caption)+'</figcaption></figure>')
   txt.append('[画像：'+file+'] '+caption+'\n')
  blocks.append('</div>')
 def chart(file,caption):
  blocks.append('<figure class="chart"><img src="benchmark_20261101/'+file+'" alt="'+html.escape(caption)+'"><figcaption>'+html.escape(caption)+'</figcaption></figure>')
  txt.append('[グラフ：benchmark_20261101/'+file+'] '+caption+'\n')
 title='必要な機能とセキュリティを、作りたいゲームに合わせて。'
 subtitle='ゲーム制作者の「選ぶ・組み合わせる・配布する」を支える自作ゲームエンジン'
 txt.extend(['ポスター一次案・追補版／2026年11月1日\n',title+'\n',subtitle+'\n'])
 h('1．制作物が何か ―― ゲーム制作者のための開発基盤')
 p('対象は、個人・少人数でゲームを作り、チート対策を専門としていない開発者。実現したいのは、面白さを作る時間を増やすことである。起動待ちや開発環境の重さ、必要な機能と対策を別々に調べて組み合わせる負担を、開発の課題として捉えた。','lead')
 p('Windows向けにC++／DirectX 12のゲームエンジンを開発し、FPS・かくれんぼ・カード・共通射撃場を実装した。制作サンプルでは、描画・物理・音声・署名検証などを選び、依存関係の確認からビルド・配布・起動へ接続した。')
 p('作りたいゲームの内容から、必要な実行機能とセキュリティをまとめて提案・構成することを目指す。Three.js・Unity・Godotや追加パッケージの候補提示もある。現在は開発・検証用の試作であり、全対策の自動導入や汎用GUIは未完成である。')
 photos([('FPS2.png','図1 自作エンジン：FPSの武器・テクスチャ・影。'),('hide.png','図2 自作エンジン：かくれんぼの屋外シーン。')])
 p('図1・2・5は保存済みの実画面。撮影日・ビルドは未特定で、今回の共通射撃場や最新RT構成の計測画面ではない。','note')
 h('2．ゲームエンジンって何？ なぜ最小ランタイムを作る？')
 p('ゲームエンジンは、入力・描画・物理・衝突判定・音声など、ゲーム共通の処理を支える基盤である。完成したゲームを遊ぶためのプログラムとデータを「ランタイム」と呼ぶ。')
 p('音声が不要なら音声処理を、単純なエイム練習なら複雑な剛体・関節物理を外す。必要な機能とその依存だけを組み込むことで、容量・初期化・資源使用を抑えることを狙う。さらに、ルール・通信・秘密情報に応じた対策まで構成にまとめ、設定・統合・配布の手間を減らしたい。')
 p('作りたいゲーム → 必要な機能とセキュリティの選択 → 依存確認 → ビルド・配布 → 面白さの制作に集中','flow')
 p('Unityにも未使用コードの除去、Godotにも機能を省くビルドがある［S4・S5］。本制作が目指す差別化は、ゲーム内容に応じた機能とセキュリティの選択を、一続きの制作工程へまとめることにある。「既存エンジンは全部重い」「他に絶対にない」とは主張しない。エディターの起動と完成ゲームの起動も区別する。','note')
 h('3．チートの実装 ―― ウォールハックとは？')
 p('ウォールハックは、壁や障害物の向こうにいる敵など、本来見えないものを不正に見えるようにするチートである。隠れた相手の位置や動きが分かり、待ち伏せや追跡で不公平な有利さを得る［S1］。','lead')
 p('自作ゲームでは奥行き判定を変更して敵を壁越しに表示する実験と、秘密情報の読み取り実験を用意した。「画面で隠す」だけでは、端末へ渡した情報を守れない場合があることを観察する。')
 photos([('wallhack_off.png','図3 Unity TPS教材：ESP・ウォールハックOFF。'),('wallhack_on.png','図4 Unity TPS教材：ESP・ウォールハックON。敵の枠・距離・シルエットを表示。')])
 p('図3・4は補助教材の実画面であり、自作エンジンの描画例ではない。撮影位置・時刻が異なるため、同一条件の効果測定ではない。自作ゲームと自分の教材環境を実験対象にしている。','note')
 h('4．どう防ぐか ―― 対策の種類と、ゲームへの割り当て')
 p('対策は、①許容できる入力から表示までの遅延と揺らぎ、②誰に何を隠すか、③対戦・ランキング・経済の重要度、④オフライン要件、⑤サーバー費用・帯域・同時接続数を基準に選ぶ。次表のゲーム別選択は設計方針であり、自動判定器の完成を意味しない。')
 table(['対策のパターン','守り方と対象','現状・限界'],[
 ['端末内の検査','秘密データの移動、整合性監視、描画深度の検査、敵タグのマスク','自作FPS／かくれんぼ等に実装。端末内の検証器自体を改造する攻撃への完全防御ではない。'],
 ['サーバーでルールを判定','命中・移動・手番・勝敗などを、クライアントの申告だけで決めない','カードで実装。FPSの命中・移動の権威化は未完成。'],
 ['秘密情報を非配信','他人の手札や不要な敵情報を、受信者の端末へ送らない','カードの席別配信を実装。FPSの可視性に応じた配信は今後。'],
 ['通信の暗号化・認証','経路上の盗聴・改変と、不正な席接続へ対処','カードでTLSと席認証。正規クライアントが受信済みの情報は隠せない。'],
 ['配布物の署名検証','ファイルの改ざん・欠落・追加を起動時に確認','選択式ビルドに実装。逆解析不能化や、検証器改造の防止を保証しない。'],
 ['クラウドでゲームを実行','ゲーム処理と描画をサーバーへ置き、入力と映像を送受信','本プロジェクトでは未実装。映像からの自動照準や入力自動化は残り、遅延・費用・帯域の評価が必要。']])
 table(['作るゲーム・条件','割り当ての方針'],[
 ['FPS／TPS・対戦アクション','ローカル入力・描画を優先し、サーバー判定＋必要情報だけの配信を組み合わせる。クラウドを一律に禁止せず、実測遅延が許容値を満たす場合のみ候補。'],
 ['LoL型のMOBA・リアルタイム戦略','操作タイミングと視界情報が重要。権威サーバー＋視界に応じた情報配信を軸にする。「遅延を要求しないゲーム」とは扱わない。'],
 ['鬼ごっこ／正体隠匿','位置・役割・視界を誰に見せるかを定義し、秘密を渡し分ける。追跡操作の応答性と公開タイミングも確認。'],
 ['カード／ターン制','サーバーで手番・勝敗を判定し、本人の手札だけを配信。描画を手元で行えば、全画面をクラウド生成する必要はない。'],
 ['オフラインRPG／個人用エイム練習','ローカル実行を基本に、必要な物理・描画と配布整合性を選ぶ。オンライン順位・売買があれば別途サーバー判定を追加。'],
 ['遅延の許容幅が大きく、実行コード・素材の端末配布を避けたいゲーム','常時接続・運用費が許され、入力→表示の実測p95や揺らぎが設計上の許容値内ならクラウド実行を検討。']])
 p('LoLでも15msと35msのping差が競技者の操作感に影響するとRiotは説明している［S2］。クラウドは通信に加えて映像の生成・圧縮・復号・表示が加わるため、pingだけでは判定しない。回線の帯域や遅延条件も評価する［S3］。上記の推奨方針はこれらを踏まえた本制作の設計判断である。','note')
 p('現在の実装上の割り当て：FPS／かくれんぼは4つの端末内対策を独立選択、オフラインカードはデータ移動と監視、オンラインカードはサーバー判定・席別配信・TLSを必須とする。共通射撃場には4つのローカル対策を組み込まず、配布署名は別に選べる。新しい制作サンプルでは描画・物理・音声・署名をビルドへ反映する。全ゲームへ全対策を自動統合する経路は今後の課題。')
 h('5．結果 ―― 数値で確認できた範囲')
 p(conditions,'note')
 table(['共通射撃場の構成','成功/測定回数','起動中央値 ms','起動p95 ms','更新間隔中央値 ms','更新間隔p95 ms','配布 MB'],[[short[n],f"{stats[n]['valid']}/{stats[n]['attempts']}",f"{med[n]:.1f}",f"{stats[n]['launch_to_first_update_ms']['p95']:.1f}",f"{stats[n]['median_of_run_frame_p50_ms']:.3f}",f"{stats[n]['median_of_run_frame_p95_ms']:.3f}",f"{sizes[n]/1e6:.3f}"] for n in names])
 p(f"今回の署名付き自作版は、このUnity構成に対して配布容量が{reduction:.2f}%少なく、約{ratio:.0f}分の1だった。起動中央値は{med['Native_Signed']:.1f}ms対{med['Unity_IL2CPP']:.1f}msで、今回の条件では{startup_reduction:.1f}%短かった。これは小さな共通射撃場の配布構成について観測した差である。")
 p('自作はDX12、Unityは既知のD3D12終了障害を避けたD3D11。実装・ランタイム・同梱機能も異なるため、差をエンジン固有の効果や最小化だけの効果に分解できない。Unityの最小到達サイズでもない。UE・Godotとは未比較である。','note')
 chart('startup.png','図6 起動の全測定点。箱は四分位範囲、ひげは5～95パーセンタイル。')
 chart('package-size.png','図7 配布容量の比較。完成ゲームの構成の比較であり、エディター容量ではない。')
 chart('cadence.png','図8 更新間隔。p95欄は各試行のp95を求めた後の中央値。VSyncを含む。')
 p('起動は外部起動要求から最初のCPU更新入口まで。画面の初回表示や入力遅延ではない。更新間隔は待ち時間を含むため、60fps付近の値からGPU余力や防御コストゼロは導けない。配布容量にOS・ドライバー・別途必要なVC++ランタイムは含めない。','note')
 p('自作エンジンの表現・動作性能')
 photos([('FPS.png','図5 自作エンジン：キャラクター・遮蔽物・影の表示例。画質を評価するための共通シーン比較ではない。')])
 table(['観点','実装・確認できた範囲'],[
 ['グラフィックス','DX12、テクスチャ、PBR、影、フォグ、骨アニメーション。制作ランタイムではソフトウェアRTの影・1回反射を選択できる。DXR対応GPUでの成功は未検証。'],
 ['モデル・物理','OBJ／GLB／FBXの変換、剛体・変形追従衝突、受動ラグドール、保守的な連続変形判定を選択可能。ラグドールは箱近似・最大32骨。連続変形判定は動的剛体とプレイヤーの外接球へ接触直前で止める方式。'],
 ['最新構成の反復計測（参考値）',f"PBR＋ソフトウェアRT＋ラグドール＋連続衝突＋署名、1600×900で{ss['valid']}/{ss['attempts']}回正常終了。起動中央値{ss['launch_to_first_update_ms']['p50']:.1f}ms、起動p95 {ss['launch_to_first_update_ms']['p95']:.1f}ms。CPU更新間隔中央値{ss['median_of_run_frame_p50_ms']:.3f}ms、各試行p95の中央値{ss['median_of_run_frame_p95_ms']:.3f}ms。"],
 ['計測したシーンの規模','2骨・4三角形の小モデルを2個体使用し、箱などを併置。各回5秒・無操作、予熱1起動を除外、計測内の待機0秒。大規模シーンの性能・画質や最大容量の試験ではない。'],
 ['機能の正しさ（既存記録）','2026-10-29：GLB／FBX計426件の物理判定、GPU RT 96判定、5構成の未選択機能除外確認。これらは速度・画質の得点ではない。']])
 p('統合デモの後半に別途Unityインストーラーの稼働を検出したため、背景負荷ありの参考値として扱う。最大約31.1秒の起動も除外せず残した。起動内訳には署名検証以前・以後の待ちや初期化が含まれ、負荷だけを原因と断定はできない。署名検証単体の時間も同時期の参考値である。共通射撃場の比較は当該インストーラーの開始前に完了した。','note')
 p('グラフィック力は、対応する表現、同じシーンを描いた画質、必要な計算時間を分けて評価する。今回の数値は小規模デモのCPU更新間隔であり、RAM／VRAM、GPU処理時間、同画質での描画性能は未測定である。')
 p('セキュリティの検証')
 table(['検証','結果と意味'],[
 ['今回の署名検証',f"{len(security['cases'])}/{len(security['cases'])}ケース成功。内訳は正常系3、改ざん・欠落・追加等{rejects}、配布物内へのログ出力拒否2。検証器を改造しない条件での検出結果。"],
 ['署名処理の時間（今回）',f"同じ検証用ツールで各30回。射撃場の中央値{sigtime['range_signed']['median_ms']:.3f}ms、最新デモの中央値{sigtime['selected_signed']['median_ms']:.3f}ms。署名・一覧・ファイル内容の検査区間で、プロセス起動時間は含まない。背景でUnityインストールが稼働していた時期の参考値。"],
 ['ローカル対策（既存記録）','36構成の起動・処理選択、6構成×7回＝42回の反復を確認。一般のチートを36種類防いだという意味ではない。'],
 ['情報分離・通信（既存記録）','カードの単体試験6件、TLS関連11件、同一PCで2席・2対局の送受信照合。別PCでの非配信検証は未完了。'],
 ['Unityとのルール一致（既存記録）','同じ射撃場の3シナリオ、13,582ステップでゲーム状態が一致。画面・入力遅延・操作感の一致は未検証。']])
 p('攻撃成功率・誤検知率・未知のチートへの耐性を網羅的に評価したものではない。署名有無の起動差にも、検証処理以外の変動が含まれる。署名付きの中央値が小さいことを、署名による高速化とは解釈しない。','note')
 h('6．結論と教育への展開')
 p('ゲームを動かす段階から、機能を選んで実行構成を作り、その動作・容量・起動を検証できる段階へ進めた。小さな共通ゲームでは、今回のUnity配布構成より小さい容量と短い起動中央値を観測した。一方、全ゲームでの性能優位や開発工数の削減は未確認である。')
 p('目指すのは、ゲーム制作者が機能とセキュリティの選定・設定・統合にかける負担を減らし、ゲームの面白さに集中できること。次は、対策提案の自動化、別PC・実プレイ・同画質での比較、作業時間の測定を進める。')
 p('教育用Unity TPS教材：公式Multiplayer Third Person Gameplayサンプル上で、チートを段階的に実装・体験し、その脅威と成立理由、対策の考え方を学ぶ日本語コード・レッスン集を作成した。自作エンジンの本体とは分け、動画・補助スライドで展開する。')
 p('00 環境準備 → 01 ESP・ウォールハック → 02 照準・誘導 → 03 ダメージ・弾数・反動 → 04 移動速度・体力など → 05 サーバー側で判定する意義と限界。攻撃側のコードと対策用のコード・雛形を用意したが、命中検証などの接続は一部未完了。学習効果の定量評価は今後行う。')
 h('制作メモ・根拠（ポスター本文への掲載は任意）')
 p('Claudeへ：本HTMLは画像付きの内容確認版で、印刷用の完成レイアウトではありません。指定の6項目の順を保ち、自作エンジン→最小ランタイム→Unity教育の順に主従をつけてください。図1～5は実画面、図6～8は測定値から生成したグラフです。キャプションと条件・出典を残し、架空の改善率・画面・QR・作者所属を追加しないでください。')
 p('主張の強さ：容量・起動の観測値／機能試験／未実装の選定方針／将来の優位性を区別します。機能削減や情報非配信そのものを独自発明とは書きません。クラウドゲーミングは未実装、LoLは遅延に敏感、FPSも条件次第でクラウド候補です。')
 p('今回の性能測定は保存済みRelease配布物を使用し、エンジンやUnityを再ビルドしていません。配布ファイルのSHA-256、実行条件、全試行と失敗、生CSV・ログをoutput/benchmarks/poster-20261101/へ保存しています。画像の由来はbenchmark_20261101/images.json、数値の抜粋は同benchmark-results.jsonを参照してください。')
 p('新規計測：comparison/cases.poster-20261101.json、tools/benchmark/run.py、tools/benchmark/measure_selected_poster.py。既存根拠：CURRENT_IMPLEMENTATION.txt、output/validation/selected/dynamics/summary.json、docs/reference/VALIDATION_20261004.md、output/release-validation/unity-protection-player/results.json、cheat-education-unity/README.md。')
 blocks.append('<ol class="sources">')
 for s in read(F/'sources.json')['sources']:
  blocks.append('<li id="'+s['id']+'">'+s['id']+' <a href="'+html.escape(s['url'])+'">'+html.escape(s['title'])+'</a></li>')
  txt.append(s['id']+' '+s['title']+'\n'+s['url']+'\n')
 blocks.append('</ol>')
 (A/'poster_draft_20261101.txt').write_text('\n'.join(txt),encoding='utf-8-sig')
 css='''
 *{box-sizing:border-box}body{overflow-wrap:anywhere;margin:0;background:#eef3f2;color:#17313c;font:16px/1.85 "Meiryo","Yu Gothic",sans-serif}
 main{max-width:1240px;margin:32px auto;background:white;padding:44px 54px;box-shadow:0 5px 25px #1232}
 header{border-bottom:4px solid #127166;padding-bottom:26px}header small{color:#54646c}h1{font-size:32px;line-height:1.45;margin:14px 0}header p{font-size:19px;color:#127166;margin:0}
 h2{font-size:24px;border-left:6px solid #127166;padding-left:16px;margin:52px 0 22px;line-height:1.6}
 p{margin:16px 0}.lead{font-size:18px;font-weight:600}.note{font-size:13px;color:#54646c;background:#f2f6f5;padding:14px 18px;border-left:3px solid #b6c9c5}
 .flow{font-weight:bold;background:#e5f1ed;padding:22px;border-radius:5px;text-align:center}
 .gallery{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:20px;align-items:start;margin:25px 0}
 figure{margin:0}figure img{display:block;width:100%;height:auto;border:1px solid #d4e0df}figcaption{font-size:13px;line-height:1.65;color:#54646c;margin-top:8px}
 .gallery figure:only-child{grid-column:1/-1;max-width:750px;margin:auto}.chart{margin:30px 0}.chart img{border:0}
 .table-scroll{overflow-x:auto;margin:24px 0}table{border-collapse:collapse;width:100%;font-size:14px;line-height:1.7}th{background:#e6efed;text-align:left}td,th{padding:12px 14px;border:1px solid #d4e0df;vertical-align:top}td:first-child{font-weight:600;min-width:150px}
 a{color:#12695e}.sources{font-size:13px;overflow-wrap:anywhere}footer{font-size:12px;color:#54646c;margin-top:32px}
 @media(max-width:760px){main{padding:22px 18px;margin:0}h1{font-size:25px}h2{font-size:21px}.gallery{grid-template-columns:1fr}table{min-width:650px}}
 @media print{body{background:white}main{margin:0;box-shadow:none;padding:0}h2{break-after:avoid}figure,tr{break-inside:avoid}.table-scroll{overflow:visible}}
 '''
 page='<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>自作ゲームエンジン：ポスター一次案</title><style>'+css+'</style><body><main><header><small>ポスター一次案・画像付き内容確認版 ｜ 2026年11月1日</small><h1>'+html.escape(title)+'</h1><p>'+html.escape(subtitle)+'</p></header>'+''.join(blocks)+'<footer>原稿・実画面・測定条件をClaudeでのレイアウト用にまとめた資料です。</footer></main></body></html>'
 for source in read(F/'sources.json')['sources']:
  sid=source['id']; page=page.replace('［'+sid+'］','<a href="#'+sid+'">［'+sid+'］</a>')
 (A/'poster_draft_20261101.html').write_text(page,encoding='utf-8')
 files=[A/'poster_draft_20261101.txt',A/'poster_draft_20261101.html']+[A/i['path'] for i in read(F/'images.json')]
 files += [f for f in F.iterdir() if f.suffix in ('.png','.svg','.json','.txt','.csv')]
 with zipfile.ZipFile(A/'poster_handoff_20261101.zip','w',zipfile.ZIP_DEFLATED) as z:
  for f in files:z.write(f,f.relative_to(A).as_posix())
 print('Saved text, illustrated HTML, charts and ZIP.')
 print('Startup medians:',med)
 print('Selected:',ss)
if __name__=='__main__':main()