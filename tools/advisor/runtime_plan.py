"""Validated mapping from advisor requests to native source, shader and initialization gates."""
import argparse
import json
import sys
from pathlib import Path
from catalog import GENRES


def make_plan(config):
    errors = []
    def require(condition, message):
        if not condition:
            errors.append(message)
    require(config['target'] != 'web', '自作ランタイムはWindows用です。WebはThree.js等の導入候補を確認してください。')
    require(config['game'] in GENRES, '既存range/card/hideは従来ビルドを使用。個別選択ではaim_training/chase等の制作サンプルを選んでください。')
    require(set(config['physics']) <= {'collision','rigid_body'}, '関節・布・流体の制作サンプルへの接続は未対応です。')
    require(config['rendering'] in ('none', 'raster', 'hybrid'), '全面パストレーシングは未対応です。hybridの影・反射を選べます。')
    require(config['audio'] in ('none', 'basic'), '空間音響は未接続です。音なし、またはbasicを指定してください。')
    require(config['network'] == 'offline', '新しい制作サンプルの権威サーバーは未実装。offlineで制作するか既存cardを使用してください。')
    if config['animation']=='skeletal':
        require(config['model'] in ('glb','fbx'), '骨アニメーションにはmodel=glb/fbxを選んでください。')
        require(config['rendering']!='none' or config['model_physics']=='deforming_mesh' or config['ragdoll']=='passive', '描画なしの骨アニメーションにはdeforming_meshを選んでください。')
        require(config['model_physics'] in ('none','deforming_mesh'), '骨変形モデルにはmodel_physics=none/deforming_meshを選んでください。')
    if config['model_physics']=='deforming_mesh':
        require(config['animation']=='skeletal', 'deforming_meshにはanimation=skeletalが必要です。')
    if config['ragdoll']=='passive':
        require(config['animation']=='skeletal' and config['model'] in ('glb','fbx'), 'ラグドールにはGLB/FBXとanimation=skeletalが必要です。')
    if config['deformation_ccd']!='none':
        require(config['model_physics']=='deforming_mesh', '変形の連続判定にはmodel_physics=deforming_meshが必要です。')
    require(config['lights'] in ('none','directional'), '個別ビルドで使える光源はnone/directionalです。')
    require(config['shadows'] in ('none','shadow_map','ray_traced'), '影の選択が不正です。')
    require(config['reflections'] in ('none','ray_traced'), '反射はnone/ray_tracedに対応しています。')
    require(config['indirect_light'] in ('none','ambient'), '間接光はnone/ambientのみ対応です。')
    draw = config['rendering'] != 'none'
    if not draw:
        require(config['material'] == 'unlit' and config['lights'] == config['shadows'] == config['reflections'] == config['indirect_light'] == 'none',
                '描画なしではmaterial=unlit、lights/shadows/reflections/indirect_light=noneを指定してください。')
    if config['material'] == 'unlit':
        require(config['lights'] == config['shadows'] == config['indirect_light'] == 'none',
                'unlitでは照明・影・環境光をnoneにしてください。無視してビルドすることはありません。')
    if config['shadows'] in ('shadow_map','ray_traced'):
        require(config['lights'] == 'directional', '影にはdirectional光源が必要です。')
    features = {'render': draw, 'collision': 'collision' in config['physics'],
                'audio': config['audio'] == 'basic', 'pbr': draw and config['material'] == 'pbr',
                'shadows': draw and config['shadows'] == 'shadow_map',
                'rigid_body': 'rigid_body' in config['physics'],
                'ray_tracing': config['rendering'] == 'hybrid',
                'rt_shadows': config['shadows'] == 'ray_traced',
                'rt_reflections': config['reflections'] == 'ray_traced',
                'rt_hardware': config['rendering'] == 'hybrid' and config['ray_acceleration'] == 'hardware_required',
                'distribution_signature': config['distribution'] == 'signed',
                'ragdoll': config['ragdoll']=='passive', 'deformation_ccd': config['deformation_ccd']=='conservative',
                'textures': draw and config['material'] == 'pbr', 'skinning': False}
    if config['rendering'] == 'hybrid':
        require(config['material'] != 'unlit', 'RT効果にはsimple_litまたはpbrを選んでください。')
    if config['rendering'] == 'none':
        require(config['ray_acceleration'] == 'unspecified', '描画なしにRTバックエンドは指定できません。')
    features.update(model_animation=config['animation']=='skeletal', model_geometry=config['model']!='none', model_convex=config['model_physics']=='convex_hull',
                    model_static=config['model_physics']=='static_mesh', model_deforming=config['model_physics']=='deforming_mesh', rt_mesh=config['model_ray_tracing']=='triangles')
    if config['model']!='none' and not draw:
        require(config['model_physics']!='none' or config['ragdoll']=='passive', '描画なしのモデル読込にはモデル物理を選んでください。')
    shaders = ['SimpleVS','SimplePS','OverlayVS','OverlayPS'] if draw else []
    if features['pbr']:
        shaders += ['PbrVS','PbrPS']
    if features['shadows']:
        shaders += ['ShadowVS']
    notes = ['選択した機能の制作サンプルを生成します。既存FPS/共通射撃場を自動改変するものではありません。',
             'target_fpsは比較用の目標値です。描画はVSync、シミュレーションはphysics_hz固定更新です。',
             '署名選択時はビルド用の秘密鍵・公開鍵をCLIで指定します。秘密鍵は配布物へ含めません。']
    if not features['collision']:
        notes.append('衝突なし: 壁・命中判定・捕獲/タッチ判定を組み込まず、移動・描画を確認するサンプルになります。')
    if features['ray_tracing']:
        notes.append('RT対象は最大32インスタンス。箱に加え、model_ray_tracing=trianglesなら実メッシュを追加。反射は1バウンス、GI・パストレーシングは未対応。')
        notes.append('DXR経路はTier 1.1/Shader Model 6.5を必須とし、非対応機では明示的に起動を拒否します。' if features['rt_hardware'] else 'GPUソフトウェアRTを明示的に使用します。専用RTコアを使用したという意味ではありません。')
    if features['model_geometry']:
        notes.append('OBJは事前に三角形化してください。GLB/FBXもビルド時に位置・面を変換。材質/UVは取り込まず単色表示。骨変形はanimation=skeletalで別途選択。凸包は凹部を埋める近似、static_meshは固定物専用。')
    if features['model_animation']:
        notes.append('選択クリップのボーン行列を60Hzで事前評価し、実行時に行列補間とCPUスキニング。描画とRTに同じ変形頂点を使用します。モーフは未対応。ラグドールは別選択で物理用のモデル個体を追加。')
    if features['model_deforming']:
        notes.append('骨変形と衝突・描画・RTで同じ頂点を共有。最大4096三角形。物理サブステップごとに静的形状を置換する実験実装で、骨への力の反作用・ラグドール・移動床の速度継承には未対応。')
    if features['ragdoll']:
        notes.append('最大32骨。初期姿勢から箱形の骨剛体を生成し、親子を可動範囲付き関節で接続。物理結果で別個体をスキニング。自己衝突・起き上がり・運動制御は未対応。')
    if features['deformation_ccd']:
        notes.append('更新間の三角形の直線移動を連続判定。剛体/プレイヤーの保守的な外接球に接触する直前で変形を制限。実形状より早く止まる場合があります。')
    return {'status': 'blocked' if errors else 'ready', 'blockers': errors,
            'entrypoint': 'game/creator/Main.cpp', 'features': features, 'shaders': shaders,
            'notes': notes}


def emit(config_path, output):
    from advisor import normalize
    config, notes = normalize(json.loads(config_path.read_text(encoding='utf-8-sig')))
    plan = make_plan(config)
    if plan['blockers']:
        raise ValueError('\n'.join(plan['blockers']))
    from model_asset import cook
    model = cook(config,Path(__file__).resolve().parents[2])
    output.mkdir(parents=True, exist_ok=True)
    if model:
        (output/'model.mesh').write_bytes(model['data'])
        plan['model_asset']=model['info']
        if model.get('animation_data'):(output/'model.anim').write_bytes(model['animation_data'])
        if model.get('rig_data'):(output/'model.rig').write_bytes(model['rig_data'])
    desc = {'schema': 1, 'game': config['game'], 'features': plan['features'], 'request': config,
            'package_kind': 'signed_creator_starter' if config['distribution']=='signed' else 'unsigned_creator_starter'}
    if model: desc['model_asset']=model['info']
    header = '#pragma once\n'
    for key, enabled in plan['features'].items():
        header += f'#define ENGINE_{key.upper()} {int(enabled)}\n'
    header += '#define ENGINE_TEXTURE_PIPELINE 0\n'
    for key in ('game','material','lights','indirect_light'):
        header += f'#define RUNTIME_{key.upper()} "{config[key]}"\n'
    for key in ('width','height','physics_hz'):
        header += f'#define RUNTIME_{key.upper()} {config[key]}\n'
    header += 'inline constexpr const char* RuntimeDescription = R"runtime(' + json.dumps(desc,ensure_ascii=True) + ')runtime";\n'
    (output/'SelectedFeatures.h').write_text(header, encoding='utf-8')
    lines = [f'set(RUNTIME_{k.upper()} {"ON" if v else "OFF"})' for k,v in plan['features'].items()]
    lines += [f'set(RUNTIME_MATERIAL {config["material"]})']
    if model:
        source=(Path(__file__).resolve().parents[2]/config['model_path']).as_posix()
        if ']====]' in source:raise ValueError('Unsupported source path')
        lines += ['set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS [====['+source+']====])']
    lines += ['set(RUNTIME_SHADER_NAMES ' + ' '.join(plan['shaders']) + ')']
    (output/'features.cmake').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    (output/'runtime-plan.json').write_text(json.dumps({'request':config,'dependency_notes':notes,**plan},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        emit(args.config, args.output)
    except (ValueError,OSError) as exc:
        parser.exit(2, str(exc)+'\n')
