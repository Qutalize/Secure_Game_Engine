from pathlib import Path
import json, os
from xml.sax.saxutils import escape
from reportlab.pdfgen import canvas
from reportlab.lib.units import mm
from reportlab.lib.colors import toColor
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import Paragraph
from reportlab.lib.utils import ImageReader
from pypdf import PdfReader

ROOT=Path(__file__).resolve().parents[2]
ASSETS=ROOT/'poster_assets'
OUT=ROOT/'output/pdf'
TMP=ROOT/'tmp/pdfs/poster-a1'
OUT.mkdir(parents=True,exist_ok=True)
TMP.mkdir(parents=True,exist_ok=True)
DATA=json.loads((ASSETS/'benchmark_20261101/benchmark-results.json').read_text(encoding='utf-8'))
FONTS=Path(os.environ.get('WINDIR','C:/Windows'))/'Fonts'
pdfmetrics.registerFont(TTFont('JP',str(FONTS/'meiryo.ttc'),subfontIndex=0))
pdfmetrics.registerFont(TTFont('JPB',str(FONTS/'meiryob.ttc'),subfontIndex=0))
pdfmetrics.registerFontFamily('JP',normal='JP',bold='JPB',italic='JP',boldItalic='JPB')
print('Japanese fonts loaded',flush=True)
W,H=594*mm,841*mm
C=canvas.Canvas(str(OUT/'gameengine_poster_A1.pdf'),pagesize=(W,H),pageCompression=1)
C.setTitle('必要な機能とセキュリティを、作りたいゲームに合わせて。')
C.setSubject('自作ゲームエンジンの設計・実装と比較評価 / A1 594 x 841 mm')
INK='#17323D';MUTED='#526870';TEAL='#087F73';NAVY='#102D3B';GOLD='#B48347';LIGHT='#EAF4F0';LINE='#D1DFDA';BG='#F4F7F5'
checks=[]

def box(x,y,w,h,fill='white',stroke=None,r=0):
 C.setFillColor(toColor(fill));C.setStrokeColor(toColor(stroke or fill))
 if r:C.roundRect(x*mm,H-(y+h)*mm,w*mm,h*mm,r*mm,stroke=bool(stroke),fill=1)
 else:C.rect(x*mm,H-(y+h)*mm,w*mm,h*mm,stroke=bool(stroke),fill=1)

def line(x,y,x2,y2,color=LINE,width=1):
 C.setStrokeColor(toColor(color));C.setLineWidth(width);C.line(x*mm,H-y*mm,x2*mm,H-y2*mm)

def t(s,x,y,size=22,bold=False,color=INK):
 font='JPB' if bold else 'JP'
 tw=pdfmetrics.stringWidth(s,font,size)/mm
 assert x+tw<=579.5,(s,x,tw)
 C.setFont(font,size);C.setFillColor(toColor(color));C.drawString(x*mm,H-y*mm-size*.86,s)
 checks.append({'kind':'text','text':s,'x':x,'y':y,'w':tw,'size_pt':size})

def p(s,x,y,w,size=21,leading=1.4,color=INK,bold=False,maxh=None):
 sty=ParagraphStyle('p',fontName='JPB' if bold else 'JP',fontSize=size,leading=size*leading,textColor=toColor(color),wordWrap='CJK',splitLongWords=True)
 para=Paragraph(s.replace('\n','<br/>'),sty)
 aw,ah=para.wrap(w*mm,1000*mm);h=ah/mm
 if maxh is not None:assert h<=maxh+.05,(s,h,maxh)
 assert y+h<832,(s,y,h)
 para.drawOn(C,x*mm,H-y*mm-ah)
 checks.append({'kind':'paragraph','text':s,'x':x,'y':y,'w':w,'h':h,'size_pt':size})
 return h

def panel(x,y,w,h,n,title):
 box(x,y,w,h,'white',LINE,r=2)
 box(x+7,y+7,12,12,TEAL,r=1)
 t(str(n),x+10.1,y+8.0,25,True,'white')
 t(title,x+24,y+8.0,27,True)

def photo(name,x,y,w,h):
 img=ImageReader(str(ASSETS/name));iw,ih=img.getSize()
 scale=min(w/iw,h/ih);dw,dh=iw*scale,ih*scale
 box(x,y,w,h,'#EAF0ED')
 C.drawImage(img,(x+(w-dw)/2)*mm,H-(y+(h+dh)/2)*mm,dw*mm,dh*mm,mask='auto')
 checks.append({'kind':'photo','name':name,'x':x,'y':y,'w':w,'h':h})

box(0,0,594,841,BG)
box(0,0,594,107,NAVY)
box(0,104,594,3,TEAL)
t('GAME ENGINE  /  BUILD & SECURITY',18,12,16,True,'#8FE0C8')
t('必要な機能とセキュリティを、',18,27,58,True,'white')
t('作りたいゲームに合わせて。',18,50,58,True,'white')
t('自作ゲームエンジンの設計・実装と比較評価',18,77,26,False,'white')
t('対象：ゲームの面白さに集中したい、チート対策を専門としない個人・少人数の制作者',18,92,18,False,'#C8DBDE')

# 1 / 2: development platform and minimal runtime
panel(18,117,274,147,1,'何を作ったのか')
p('C++／DirectX 12で、ゲームを動かす基盤を開発。\nFPS・かくれんぼ・カード・共通射撃場を実装した。',25,140,260,21,maxh=24)
photo('FPS2.png',25,167,127,71)
photo('hide.png',158,167,127,71)
t('自作エンジン：FPS',25,241,17,True)
t('自作エンジン：かくれんぼ',158,241,17,True)
p('保存済みの実画面。最新構成や今回の計測シーンの画像ではない。',25,252,260,14,color=MUTED,maxh=9)

panel(302,117,274,147,2,'ゲームエンジンと最小ランタイム')
p('ゲームエンジンは、入力・描画・物理・音声など、\nゲーム共通の処理を支える基盤。ランタイムは、\n完成したゲームを遊ぶためのプログラムとデータ。',309,140,260,21,maxh=33)
for i,(title,sub) in enumerate([('機能を選ぶ','描画・物理・音声'),('依存を確認','必要な部品だけ'),('ビルド・起動','署名も個別選択')]):
 x=309+i*89
 box(x,176,82,28,LIGHT,r=2)
 t(title,x+5,181,20,True,TEAL);t(sub,x+5,193,15,False,MUTED)
 if i<2:t('→',x+83,184,18,True,TEAL)
p('<b>必要な機能だけを組み込み、容量・起動負担を抑える。</b>\nさらにゲーム内容に合う対策までまとめ、\n選定・設定・統合の手間を減らすことを目指す。',309,211,260,20,maxh=30)
p('Unity・Godotにも機能削減がある［4,5］。差別化の軸は、機能とセキュリティを一続きの制作工程へまとめること。全対策の自動導入は今後。',309,244,260,14,color=MUTED,maxh=16)

# 3 / 4: threat illustration and selection criteria
panel(18,274,274,200,3,'チートの実装：ウォールハック')
p('壁の向こうにいる敵を不正に見えるようにする\nチート。位置が分かり、待ち伏せや追跡で\n不公平な有利さを得る［1］。',25,297,260,21,maxh=33)
photo('wallhack_off.png',25,334,127,72)
photo('wallhack_on.png',158,334,127,72)
box(28,337,20,10,NAVY,r=1);t('OFF',31,339,16,True,'white')
box(161,337,20,10,TEAL,r=1);t('ON',165,339,16,True,'white')
p('Unity TPS教材の実画面。撮影位置・時刻が異なるため、\n同一条件の効果測定ではない。',25,410,260,15,color=MUTED,maxh=16)
box(25,431,260,22,LIGHT,r=1)
p('<b>画面で隠しても、端末へ渡した情報は\n不正に利用される可能性がある。</b>',31,434,248,20,leading=1.3,color=TEAL,maxh=19)
p('自作ゲームでは描画の奥行き判定の変更や秘密情報の読み取りを実験。教材と自作環境で脅威を検証する。',25,457,260,15,maxh=16)

panel(302,274,274,200,4,'どう防ぐか：条件から選ぶ')
p('選定基準：許容遅延・秘密情報・対戦の公平性・\nオフライン要件・サーバー費用と帯域。',309,297,260,20,maxh=23)
cards=[('端末内検査','描画・データの整合性'),('サーバー判定','手番・勝敗を検証'),('秘密情報の非配信','他人の手札を送らない'),('TLS・席認証','通信経路と接続を保護'),('配布署名','改ざん・欠落を検出'),('クラウド実行［構想］','映像配信／遅延・費用')]
for i,(title,sub) in enumerate(cards):
 x=309+(i%2)*133;y=321+(i//2)*26
 box(x,y,127,23,LIGHT if i!=5 else '#F4EEE3',r=1)
 t(title,x+4,y+3,19,True,TEAL if i!=5 else '#815D2D')
 t(sub,x+4,y+14,16,False,MUTED)
t('ゲーム別の選定方針',309,403,20,True)
p('FPS・MOBA → 応答性を優先＋必要な情報だけ配信\nカード → 手札を分離＋サーバーで勝敗を判定\nオフラインRPG → 必要な表現＋配布整合性',309,415,260,18,leading=1.65,maxh=34)
p('FPSのサーバー判定・可視性配信は今後。クラウドは未実装。LoLも遅延に敏感［2］で、実測遅延と運用費から選ぶ［3］。署名・TLSだけで受信済み情報の悪用は防げない。',309,451,260,14,color=MUTED,maxh=21)

# 5: same-range measured comparison and capability / verification summary
panel(18,484,558,220,5,'結果：共通射撃場の実測比較と、機能の到達点')
names=['Native_Unsigned','Native_Signed','Unity_IL2CPP']
labels=['自作・署名なし','自作・署名あり','Unity IL2CPP']
colors=['#789A9B',TEAL,GOLD]
start=[DATA['range_summary'][n]['launch_to_first_update_ms']['p50'] for n in names]
sizes=[DATA['distribution_bytes'][n]/1e6 for n in names]

def bars(x,y,w,title,vals,fmt,maxv,unit):
 t(title,x,y,23,True)
 for i,(label,v,col) in enumerate(zip(labels,vals,colors)):
  yy=y+17+i*20
  t(label,x,yy+1,16,False)
  box(x+48,yy+1,79,8,'#EDF1ED')
  box(x+48,yy+1,79*v/maxv,8,col)
  t(fmt.format(v),x+130,yy-1,21,True,col)
  if unit=='ms':
   pv=DATA['range_summary'][names[i]]['launch_to_first_update_ms']['p95']
   t(f'p95 {pv:.0f} ms',x+48,yy+10,12,False,MUTED)
 t(unit,x+w-15,y+76,13,False,MUTED)

bars(25,510,171,'起動時間（中央値）',start,'{:.1f}',max(start),'ms')
bars(207,510,171,'配布容量',sizes,'{:.3f}',max(sizes),'MB')
line(389,510,389,639)
photo('FPS.png',399,510,170,72)
p('自作エンジン：キャラクター・遮蔽物・影。\n画質の共通条件比較や最新RT構成の画像ではない。',399,585,170,13,color=MUTED,maxh=15)
t('描画・物理の到達点',399,603,21,True)
p('PBR・影・骨アニメーション・ソフトウェアRT。\nGLB／FBX、剛体、受動ラグドール、変形追従衝突。',399,615,170,17,maxh=24)

p('<b>各構成30/30回正常終了。</b>同一PC・1600×900・VSync ON。\n同じ入力を120 Hzで再生。予熱1回後、各回2秒待機＋5秒計測。\ni7-1265U／Iris Xe。起動は最初のCPU更新入口まで。\nCPU更新間隔中央値は各構成約16.67 ms（待ち時間を含む）。',25,595,353,17,leading=1.4,maxh=36)
p('自作はDX12、UnityはD3D11。実装・同梱機能が異なり、キャッシュ等も未統制。今回の配布構成で観測した差で、エンジン全般の優位やUnityの最小容量を示すものではない。OS・ドライバー等の容量は含まない。',25,632,353,13,color=MUTED,maxh=16)
for i,(num,title,sub) in enumerate([
 ('21/21','署名検証（今回）','正常3／改ざん等16／ログ出力拒否2'),
 ('426','物理の判定（2026-10-29）','GLB・FBXの既存機能試験'),
 ('96','GPU RTの判定（2026-10-29）','2モデル更新の既存機能試験')]):
 x=25+i*182
 box(x,653,170,34,LIGHT,r=1)
 t(num,x+5,658,32,True,TEAL)
 t(title,x+48,658,16,True)
 p(sub,x+5,677,160,13,color=MUTED,maxh=8)
p('試験件数は防御率・画質スコアではない。DXR対応GPU・別PC・大規模モデルは未検証。最新統合デモの時間は背景負荷ありの参考値として主比較から分離。',25,692,544,13,color=MUTED,maxh=10)

# 6: take-away, next evaluation and the educational companion
box(18,714,558,80,NAVY,r=2)
box(25,721,12,12,TEAL,r=1);t('6',28.1,722,25,True,'white')
t('結論：選び、組み立て、検証する基盤へ',42,722,27,True,'white')
p('<b>必要な機能を選んでビルドし、\n動作・容量・起動まで検証できる段階に進んだ。</b>',25,740,352,25,color='white',maxh=27)
p('次：対策選定の自動化／別PC・実プレイ／同画質・開発工数の比較。\n全ゲームの性能優位・制作時間の短縮は、これから検証する。',25,771,352,16,color='#C8DBDE',maxh=19)
line(390,723,390,785,'#446370')
t('教育への展開：Unity TPS',399,723,20,True,'#8FE0C8')
p('補助教材でチートの脅威を段階的に学ぶ。\n壁越し表示 → 照準 → 弾数・移動の改変\n→ サーバー側の対策と限界。',399,739,170,17,color='white',maxh=31)
p('コード・レッスンを作成。対策の接続には\n未完部分があり、学習効果は今後評価する。',399,774,170,13,color='#C8DBDE',maxh=15)

# Human-readable references; hyperlink destinations are preserved in the PDF.
sources=json.loads((ASSETS/'benchmark_20261101/sources.json').read_text(encoding='utf-8'))['sources']
refs=[
 ('[1] Riot Games / VALORANT: Demolishing Wallhacks',18,803,1),
 ('[2] Riot Games / LoL: Artificial Latency for Remote Competitors',18,810,2),
 ('[3] NVIDIA / GeForce NOW: System Requirements',18,817,3),
 ('[4] Unity Manual / Managed Code Stripping',310,803,4),
 ('[5] Godot Docs / Optimizing a Build for Size',310,810,5),
]
for label,x,y,n in refs:
 t(label,x,y,12,False,MUTED)
 u=next(s['url'] for s in sources if s['id']==f'S{n}')
 width=pdfmetrics.stringWidth(label,'JP',12)
 C.linkURL(u,(x*mm,H-(y+5)*mm,x*mm+width,H-y*mm),relative=0,thickness=0)
t('計測 2026-11-01  /  開発・検証用プロトタイプ',310,817,12,False,MUTED)
t('詳細な条件・未検証項目・出典は、画像付き詳細原稿と測定記録に掲載。写真は保存済みの実画面を使用。',18,827,12,False,MUTED)
C.showPage();C.save()
reader=PdfReader(OUT/'gameengine_poster_A1.pdf')
assert len(reader.pages)==1
page=reader.pages[0]
assert abs(float(page.mediabox.width)/mm-594)<.01
assert abs(float(page.mediabox.height)/mm-841)<.01
text=page.extract_text()
for word in ['何を作ったのか','最小ランタイム','ウォールハック','どう防ぐか','実測比較','結論','398.4','1137.0','67.553','21/21']:
 assert word in text,word
photos=[v for v in checks if v['kind']=='photo'];assert len(photos)==5
fonts=page['/Resources']['/Font'].get_object()
embedded = [f.get_object()['/FontDescriptor'].get_object() for f in fonts.values() if '/FontDescriptor' in f.get_object()]
assert sum('/FontFile2' in f for f in embedded)>=2, 'Japanese fonts must be embedded'
meta={'pages':1,'width_mm':float(page.mediabox.width)/mm,'height_mm':float(page.mediabox.height)/mm,'photo_count':len(photos),'vector_charts':2,'paragraph_overflow_checks':'passed','content_check':'passed','sources':len(refs),'pdf_bytes':(OUT/'gameengine_poster_A1.pdf').stat().st_size}
(TMP/'layout-check.json').write_text(json.dumps({'summary':meta,'elements':checks},ensure_ascii=False,indent=2),encoding='utf-8')
(TMP/'extracted.txt').write_text(text,encoding='utf-8')
print(json.dumps(meta,indent=2))