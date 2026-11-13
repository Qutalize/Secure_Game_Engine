"""Explain engine/module choices. Standard-library-only; use build_runtime.py for selected native builds."""
import argparse
import copy
import json
import math
from pathlib import Path
import sys

from catalog import GAME_LABELS, game_guidance, packages

CATALOG_DATE = '2026-10-29'
OPTIONS = {
    'target': {'windows': 'Windows実行版', 'web': 'ブラウザー', 'either': 'どちらでもよい'},
    'game': GAME_LABELS,
    'priority': {'development': '制作の進めやすさ', 'runtime': '実行構成を小さくする', 'distribution': '配布の手軽さ'},
    'rendering': {'none': '描画なし（シミュレーション検証）', 'raster': 'ラスタライズ', 'hybrid': 'ラスタライズ＋一部レイトレーシング', 'path_tracing': 'パストレーシング'},
    'material': {'simple_lit': '簡易ライティング', 'unlit': '光の影響を受けない材質', 'pbr': 'PBR材質'},
    'lights': {'none': '光源なし', 'directional': '平行光', 'point_spot': '点光源・スポットライト', 'mixed': '複数種の動的光源', 'baked': '事前計算した照明'},
    'shadows': {'none': '影なし', 'shadow_map': 'シャドウマップ', 'ray_traced': 'レイトレーシングの影'},
    'reflections': {'none': '反射なし', 'environment_map': '環境マップ', 'screen_space': '画面内の情報を使う反射', 'ray_traced': 'レイトレーシングの反射'},
    'indirect_light': {'none': '間接光なし', 'ambient': '一定の環境光', 'baked': '事前計算GI', 'screen_space': '画面内の情報を使うGI', 'ray_traced': 'レイトレーシングGI'},
    'distribution': {'unsigned': '署名なし（開発用）', 'signed': '署名を検証して起動'},
    'ray_acceleration': {'software': 'GPUソフトウェアRT', 'unspecified': '加速方法は未指定', 'hardware_required': 'GPUの専用レイトレーシング機能が必須'},
    'model': {'none':'モデル読込なし','obj':'三角形OBJモデル','glb':'GLB 2.0モデル','fbx':'FBXモデル（ビルド時変換）'},
    'model_physics': {'none':'モデル用物理なし','convex_hull':'動的な凸包剛体','static_mesh':'固定の三角形メッシュ','deforming_mesh':'骨変形に追従する三角形衝突'},
    'model_ray_tracing': {'none':'モデルのRTなし','triangles':'実メッシュのRT影・反射'},
    'animation': {'none': '骨アニメーションなし', 'skeletal': '骨アニメーションあり'},
    'ragdoll': {'none':'ラグドールなし','passive':'関節付きの受動ラグドール'},
    'deformation_ccd': {'none':'変形の離散判定','conservative':'高速変形の連続判定（接触直前で停止）'},
    'audio': {'none': '音なし', 'basic': 'BGM・効果音', 'spatial': '位置に応じた3D音響'},
    'network': {'offline': 'オフライン', 'authoritative': 'サーバーがルールを判定'},
}
PHYSICS = {'collision': '当たり判定', 'rigid_body': '剛体（重力・反発・摩擦）',
           'joints': '関節・拘束', 'cloth': '布・変形体', 'fluid': '流体'}
DEFAULT = {
    'target': 'either', 'game': 'new_game', 'priority': 'development',
    'physics': ['collision'], 'physics_hz': 60,
    'rendering': 'raster', 'material': 'simple_lit', 'lights': 'directional',
    'shadows': 'none', 'reflections': 'none', 'indirect_light': 'ambient',
    'distribution': 'unsigned', 'ray_acceleration': 'unspecified', 'animation': 'none', 'audio': 'none',
    'ragdoll':'none', 'deformation_ccd':'none',
    'animation_clip':'', 'model':'none', 'model_path':'', 'model_scale':1.0, 'model_physics':'none', 'model_ray_tracing':'none',
    'network': 'offline', 'width': 1600, 'height': 900, 'target_fps': 60,
}
SOURCES = {
    'native': ['engine/math/Collision.*', 'engine/renderer/Renderer.*', 'assets/shaders/', 'CMakeLists.txt'],
    'three': ['https://threejs.org/manual/pages/game.html', 'https://threejs.org/manual/pages/physics.html',
              'https://threejs.org/docs/pages/MeshStandardMaterial.html'],
    'unity': ['https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/Ray-Tracing-Getting-Started.html'],
    'godot': ['https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_web.html',
              'https://docs.godotengine.org/en/stable/tutorials/physics/index.html'],
}
NATIVE = {'rendering.none', 'rendering.raster', 'material.unlit', 'material.simple_lit', 'material.pbr', 'lights.directional',
          'shadows.shadow_map', 'indirect_light.ambient', 'physics.collision', 'physics.rigid_body', 'rendering.hybrid', 'shadows.ray_traced', 'reflections.ray_traced', 'animation.skeletal', 'audio.basic'}


def normalize(request):
    if not isinstance(request, dict):
        raise ValueError('入力はJSONオブジェクトにしてください。')
    unknown = set(request) - set(DEFAULT)
    if unknown:
        raise ValueError('未知の入力項目: ' + ', '.join(sorted(unknown)))
    config = copy.deepcopy(DEFAULT)
    config.update(copy.deepcopy(request))
    for field, choices in OPTIONS.items():
        if not isinstance(config[field], str) or config[field] not in choices:
            raise ValueError(f'{field}: 次から選択してください: {", ".join(choices)}')
    for field, low, high in [('width', 1, 16384), ('height', 1, 16384), ('target_fps', 1, 1000), ('physics_hz', 1, 1000)]:
        if type(config[field]) is not int or not low <= config[field] <= high:
            raise ValueError(f'{field}: {low}～{high}の整数が必要です。')
    modules = config['physics']
    if not isinstance(modules, list) or any(not isinstance(x, str) or x not in PHYSICS for x in modules):
        raise ValueError('physics: collision/rigid_body/joints/cloth/fluidの配列。不要なら[]。')
    if len(set(modules)) != len(modules):
        raise ValueError('physics: 同じ機能が重複しています。')
    notes = []
    if type(config['model_scale']) not in (int,float) or not math.isfinite(config['model_scale']) or not .001<=config['model_scale']<=1000:
        raise ValueError('model_scale: 0.001～1000の有限な数値が必要です。')
    config['model_scale']=float(config['model_scale'])
    if config['model']=='none':
        if config['model_path']!='' or config['model_scale']!=1 or config['model_physics']!='none' or config['model_ray_tracing']!='none':
            raise ValueError('モデル設定を使う場合はmodel=obj/glb/fbxを選んでください。')
    else:
        from model_asset import validate_path
        validate_path(config['model_path'],config['model'])
        if config['model_physics']!='none' and 'rigid_body' not in modules:
            modules.append('rigid_body');notes.append('モデル物理の依存としてrigid_bodyを追加しました。')
        if config['model_ray_tracing']=='triangles' and config['rendering']!='hybrid':
            raise ValueError('モデルRTにはrendering=hybridとRT影または反射が必要です。')
    if not isinstance(config['animation_clip'],str) or len(config['animation_clip'])>128 or any(ord(c)<32 for c in config['animation_clip']):
        raise ValueError('animation_clip: 128文字以内のクリップ名が必要です。')
    if config['animation']=='none' and config['animation_clip']:
        raise ValueError('クリップ指定にはanimation=skeletalを選んでください。')
    if config['ragdoll']=='passive' and 'rigid_body' not in modules:
        modules.append('rigid_body');notes.append('ラグドールの依存としてrigid_bodyを追加しました。')
    # These are this planner's integration dependencies, not universal claims about solvers.
    for feature, dependency in [('joints', 'rigid_body'), ('rigid_body', 'collision')]:
        if feature in modules and dependency not in modules:
            modules.append(dependency)
            notes.append(f'{feature}の構成依存として{dependency}を追加しました。')
    config['physics'] = sorted(modules)
    ray_effects = [f for f in ['shadows', 'reflections', 'indirect_light'] if config[f] == 'ray_traced']
    if config['rendering'] == 'raster' and ray_effects:
        raise ValueError('レイトレーシング効果を使う場合はrenderingをhybridまたはpath_tracingにしてください。')
    if config['rendering'] == 'hybrid' and not ray_effects:
        raise ValueError('hybridでは影・反射・間接光の少なくとも一つをray_tracedにしてください。')
    if config['ray_acceleration'] != 'unspecified' and config['rendering'] == 'raster':
        raise ValueError('ラスタライズのみの構成にRTバックエンドは指定できません。')
    if config['material'] == 'unlit' and (config['lights'] != 'none' or config['indirect_light'] != 'none'):
        notes.append('unlit材質自体は照明を受けません。光を受ける別材質を併用するか、不要な照明を外してください。')
    if config['rendering'] == 'path_tracing':
        notes.append('パストレーシングと個別の影・反射・GI設定の統合方法は、採用するレンダラーで要検証です。')
    return config, notes


def required_features(config):
    features = {'rendering.' + config['rendering'], 'material.' + config['material']}
    features.update('physics.' + name for name in config['physics'])
    for field in ['lights', 'shadows', 'reflections', 'indirect_light', 'animation', 'audio']:
        if config[field] != 'none':
            features.add(field + '.' + config[field])
    if config['network'] != 'offline':
        features.add('network.' + config['network'])
    if config['ray_acceleration'] == 'hardware_required':
        features.add('ray_acceleration.hardware_required')
    return sorted(features)


def assess_native(config, features):
    supported = NATIVE.copy()
    if config['game'] == 'card':
        supported.add('network.authoritative')
    supported.add('ray_acceleration.hardware_required')
    missing = sorted(set(features) - supported)
    blocked = ['現在の自作エンジンはWindows/DX12専用で、Web出力を実装していません。'] if config['target'] == 'web' else []
    profile = 'fps' if config['game'] == 'fps' else 'all'
    minimal_range = {'rendering.raster', 'material.simple_lit', 'physics.collision', 'lights.directional', 'indirect_light.ambient'}
    if config['game'] == 'range' and set(features) <= minimal_range:
        profile = 'range'
    notes = ['この欄は既存ゲーム群の部品対応です。新しい制作サンプルの生成可否は個別選択ビルド欄を確認してください。',
             '個別選択ビルドの対応可否はnative_build欄で別途確認します。既存ゲームの部品対応と、制作サンプルのビルド対応は別です。']
    unverified = config['ray_acceleration'] == 'hardware_required'
    if unverified:
        notes.append('DXR 1.1経路は実装済みですが対象GPUでの成功検証が必要です。非対応機では起動を拒否します。')
    if 'rigid_body' in config['physics'] or config['rendering'] == 'hybrid':
        profile = None
        notes.append('剛体・RTは個別選択ビルドに接続済みです。従来all/fps/range構成への追加ではありません。')
    if config['model']!='none':
        profile=None
        notes.append('モデル用経路は個別ビルド専用。OBJ/GLB/FBX形状・凸包または固定メッシュ・三角形RT・選択クリップの骨変形を選べます。')
    if config['game'] == 'new_game':
        notes.append('新しいゲームのルール・シーン・部品の接続を実装する必要があります。')
    if config['network'] == 'authoritative' and config['game'] != 'card':
        notes.append('現在のサーバー権威はカード限定です。FPS等への適用は追加実装です。')
    return {'id': 'custom_windows', 'name': '自作C++/DX12エンジン',
            'status': 'excluded' if blocked else ('implementation_required' if missing else ('requirements_unverified' if unverified else 'existing_components')),
            'blockers': blocked, 'missing_features': missing, 'notes': notes,
            'base_profile': None if blocked or missing else profile,
            'evidence': 'local_source_review', 'sources': SOURCES['native']}


def assess_external(engine, target, config):
    labels = {'three': 'Three.js', 'unity': 'Unity', 'godot': 'Godot'}
    blocked, notes, unverified = [], [], []
    if config['target'] not in ('either', target):
        blocked.append('要求された配布先と異なります。')
    ray = config['rendering'] in ('hybrid', 'path_tracing')
    advanced_physics = sorted(set(config['physics']) & {'cloth', 'fluid'})
    if advanced_physics:
        unverified.append('選択した布・流体の方式、ライブラリ、実行先への対応: ' + ', '.join(advanced_physics))
    if engine == 'three':
        notes.append('描画以外のゲーム更新・入力・UIを接続します。物理が必要なら別モジュールを組み合わせます。')
        if set(config['physics']) & {'rigid_body', 'joints'}:
            notes.append('Rapier/Jolt等の物理ライブラリを候補にし、必要な形状・拘束を検証します。')
        if ray:
            unverified.append('レイトレーシング用の追加レンダラー、ブラウザー、GPU機能への対応。通常のThree.js設定だけでは判定しません。')
    elif engine == 'unity':
        notes.append('採用するUnityの版とレンダーパイプラインを固定し、選択した機能の対応を確認します。')
        if ray and target == 'web':
            blocked.append('このカタログが扱うUnityのレイトレーシング経路はHDRP/DX12で、Web出力には適合しません。')
        elif ray:
            notes.append('Windows/HDRP/DX12の構成を候補にします。参考はHDRP 17.0文書で、既存比較用プロジェクトへの導入は未実施です。')
            unverified.append('HDRPの対象バージョン、レイトレーシング対応GPU・ドライバー、対象効果。')
    elif engine == 'godot':
        notes.append('物理ボディを候補にでき、描画機能は選ぶレンダラーと版の対応を確認します。')
        if target == 'web':
            notes.append('参照した公式Web出力はCompatibility/WebGL 2。ネイティブ側と同じ描画機能が使えるとは判定しません。')
        if ray or (target == 'web' and (config['reflections'] == 'screen_space' or config['indirect_light'] == 'screen_space')):
            unverified.append('要求したレイトレーシング／画面空間効果への対応。未検証なので適合候補から外します。')
    if config['ray_acceleration'] == 'hardware_required':
        unverified.append('対象機の専用レイトレーシング機能。API名だけでは達成済みにしません。')
    if config['distribution'] == 'signed':
        unverified.append('選択ビルドのP-256署名検証は自作エンジン用です。この環境への署名・検証の接続は未実装です。')
    if target == 'web':
        notes.append('初回転送量、キャッシュ後の起動、ブラウザーと実機の操作を別々に測ります。')
        if config['network'] == 'authoritative':
            notes.append('既存TLS/TCPへ直接接続せず、WSS等の通信窓口を追加します。秘密を送らない設計は維持します。')
    if config['network'] == 'authoritative':
        notes.append('ルール検証・受信者別配信はゲーム側の設計が必要で、エンジン選択だけでは付きません。')
    return {'id': engine + '_' + target, 'name': labels[engine] + (' / Web' if target == 'web' else ' / Windows'),
            'status': 'excluded' if blocked else ('requirements_unverified' if unverified else 'integration_candidate'),
            'blockers': blocked, 'verification_needed': unverified, 'notes': notes,
            'evidence': 'documentation_only_not_tested', 'sources': SOURCES[engine]}


def advise(request):
    from runtime_plan import make_plan
    config, notes = normalize(request)
    features = required_features(config)
    candidates = [assess_native(config, features)]
    candidates += [assess_external(engine, target, config) for engine, target in
                   [('three', 'web'), ('unity', 'windows'), ('unity', 'web'), ('godot', 'windows'), ('godot', 'web')]]
    shortlist = [c['id'] for c in candidates if c['status'] in ('existing_components', 'integration_candidate')]
    # No invented performance score or overall winner; native build readiness is checked separately.
    return {'schema': 1, 'catalog_checked': CATALOG_DATE, 'request': config, 'dependency_notes': notes,
            'game_guidance': game_guidance(config['game']), 'recommended_packages': packages(config),
            'native_build': make_plan(config),
            'required_features': features, 'candidates': candidates, 'comparison_candidates': shortlist,
            'performance': {'status': 'not_measured', 'target_fps': config['target_fps'],
                            'resolution': [config['width'], config['height']], 'physics_hz': config['physics_hz']},
            'next_steps': [
                '必須機能が未確認の候補は、追加実装・検証の後に比較へ戻す。候補ゼロも有効な結果。',
                'priorityは希望として保存する。現段階では開発工数・性能の根拠が足りず、自動順位付けしない。',
                '物理更新、描画、準備完了、転送容量を同じゲーム・実機条件で測る。',
                'native_buildがreadyならbuild_runtime.pyで選択機能のWindowsサンプルを生成・起動できます。外部エンジンへの自動導入・Webデプロイは未実装。']}


def text_report(result):
    lines = ['ゲーム制作環境・実装機能の構成提案（試作）', '仕様確認日: ' + result['catalog_checked'],
             '必須機能: ' + ', '.join(result['required_features'])]
    lines += result['dependency_notes']
    labels = {'excluded': '条件不適合', 'implementation_required': '追加実装が必要',
              'existing_components': '部品実装あり・ゲームへの接続が必要',
              'requirements_unverified': '必須機能の対応が未確認', 'integration_candidate': '導入を検討する候補（実機未検証）'}
    for item in result['candidates']:
        lines += ['', item['name'] + ': ' + labels[item['status']]]
        if item.get('base_profile'):
            lines.append('  従来ゲーム用の構成: ' + item['base_profile'] + '（新しい制作サンプルの生成経路とは別）')
        for field in ['blockers', 'missing_features', 'verification_needed', 'notes']:
            lines.extend('  - ' + note for note in item.get(field, []))
        lines.append('  根拠: ' + ', '.join(item['sources']))
    build = result['native_build']
    lines += ['', '個別選択ビルド: ' + build['status']]
    lines.extend('  - ' + reason for reason in build['blockers'])
    if not build['blockers']:
        lines.append('  組み込む機能: ' + ', '.join(k for k,v in build['features'].items() if v))
    lines.extend('  - ' + note for note in build['notes'])
    guidance = result['game_guidance']
    lines += ['', 'ゲーム制作: ' + guidance['label'], guidance['note']]
    lines.extend('  今後の制作: ' + s for s in guidance.get('remaining_gameplay', []))
    lines.append('\n追加パッケージ候補（自動インストールはしません）')
    for row in result['recommended_packages']:
        lines += ['  ' + row['name'] + ' / ' + row['engine'] + ': ' + row['reason'],
                  '    状態: ' + row['status'], '    要確認: ' + row['verification_needed'], '    出典: ' + row['source']]
    lines += ['', '性能: 未計測。目標fps・物理更新Hzは達成を保証する値ではありません。'] + result['next_steps']
    return '\n'.join(lines) + '\n'


def interactive():
    request = copy.deepcopy(DEFAULT)
    print('番号で選択。Enterは表示された既定値を使用します。')
    for field, choices in OPTIONS.items():
        names = list(choices)
        while True:
            print('\n' + field + ': ' + ' / '.join(f'{i+1}={choices[n]}' for i, n in enumerate(names)))
            answer = input('既定=' + choices[request[field]] + ': ').strip()
            if not answer:
                break
            if answer.isdigit() and 1 <= int(answer) <= len(names):
                request[field] = names[int(answer)-1]
                break
            print('表示した番号を入力してください。')
    if request['model']!='none':
        request['model_path']=input('プロジェクト相対のモデルパス（選択形式と同じ拡張子）: ').strip()
        raw=input('model_scale（既定=1）: ').strip()
        request['model_scale']=float(raw or '1')
    if request['animation']=='skeletal':
        request['animation_clip']=input('クリップ名（空欄=最初のクリップ）: ').strip()
    print('\nphysics: 0=不要 / ' + ' / '.join(f'{i+1}={v}' for i, v in enumerate(PHYSICS.values())))
    while True:
        answer = input('複数選択はカンマ区切り（既定=1）: ').strip() or '1'
        try:
            ids = [int(x.strip()) for x in answer.split(',')]
            if ids == [0]:
                request['physics'] = []
            elif not ids or min(ids) < 1 or max(ids) > len(PHYSICS) or len(set(ids)) != len(ids):
                raise ValueError()
            else:
                request['physics'] = [list(PHYSICS)[i-1] for i in ids]
            break
        except ValueError:
            print('0、または1～5の重複しない番号を入力してください。')
    for field in ['physics_hz', 'width', 'height', 'target_fps']:
        while True:
            answer = input(f'{field}（既定={request[field]}）: ').strip()
            if not answer:
                break
            if answer.isdigit() and 1 <= int(answer) <= (16384 if field in ('width', 'height') else 1000):
                request[field] = int(answer)
                break
            print('有効な正の整数を入力してください。')
    return request


def main(argv=None):
    # Windows pipes otherwise use CP932 while saved reports and build logs use UTF-8.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--config', type=Path)
    group.add_argument('--interactive', action='store_true')
    group.add_argument('--list-options', action='store_true')
    parser.add_argument('--json', action='store_true', help='選択と判定をJSONで出力')
    parser.add_argument('--out', type=Path, help='新規ファイルへ保存。既存ファイルは上書きしない')
    args = parser.parse_args(argv)
    try:
        if args.list_options:
            output = json.dumps({'options': OPTIONS, 'physics': PHYSICS, 'defaults': DEFAULT}, ensure_ascii=False, indent=2) + '\n'
        else:
            request = interactive() if args.interactive else json.loads(args.config.read_text(encoding='utf-8-sig'))
            result = advise(request)
            output = json.dumps(result, ensure_ascii=False, indent=2) + '\n' if args.json else text_report(result)
        if args.out:
            with args.out.open('x', encoding='utf8') as stream:
                stream.write(output)
            print('保存: ' + str(args.out))
        else:
            print(output, end='')
        return 0
    except (OSError, ValueError, EOFError) as exc:
        parser.exit(2, '入力・出力エラー: ' + str(exc) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
