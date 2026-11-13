"""Game archetypes and optional packages. Recommendations are not installed automatically."""
CHECKED = '2026-10-29'
# References describe mechanics, not bundled content or complete game implementations.
GENRES = {
    'new_game': ('自由な3Dゲーム', '自由移動の土台', ['ルール・シーン制作']),
    'fps': ('FPS', '一人称・射撃判定', ['武器・敵AI・ステージ']),
    'tps': ('TPS', '三人称追従・射撃判定', ['カメラ遮蔽・照準補正・キャラクター']),
    'aim_training': ('エイム練習', '一人称・的当て・スコア', ['トラッキング課題・難易度・成績保存']),
    'monster_capture': ('モンスター捕獲・育成', '三人称探索・近接インタラクション', ['捕獲確率・図鑑・育成・セーブ']),
    'action_rpg': ('探索アクションRPG', '三人称探索・近接インタラクション', ['戦闘・スキル・クエスト・広域マップ']),
    'chase': ('非対称鬼ごっこ', '三人称・移動する対象へのタッチ', ['役割別能力・勝敗・権威サーバー']),
    'social_deduction': ('タスク・正体隠匿', '三人称・タスク地点へのインタラクション', ['秘密役職・会議・投票・受信者別配信']),
}
GAME_LABELS = {key: row[0] for key, row in GENRES.items()}
GAME_LABELS.update(range='既存の共通比較射撃場', card='既存カード', hide='既存かくれんぼ')

def game_guidance(game):
    if game not in GENRES:
        return {'status': 'legacy_game', 'label': GAME_LABELS[game], 'note': '既存ゲームは従来のall/fps/rangeビルドで起動。新しい機能別サンプルとは別です。'}
    label, starter, todo = GENRES[game]
    return {'status': 'starter_template', 'label': label, 'starter': starter,
            'remaining_gameplay': todo, 'note': '制作の出発点です。完成作品や既存タイトルの再現ではありません。原神型の探索アクション、ポケモン型の捕獲育成、第五人格型の追跡、Among Us型の正体隠匿を別の設計項目として扱います。'}


def packages(config):
    rows = []
    def add(id, name, engine, reason, url, caution):
        if engine == 'three_web' and config['target'] == 'windows':
            return
        rows.append({'id': id, 'name': name, 'engine': engine, 'reason': reason,
                     'status': 'optional_candidate_not_integrated', 'source': url,
                     'checked': CHECKED, 'verification_needed': caution})
    if set(config['physics']) & {'rigid_body', 'joints'}:
        if config['target'] != 'web':
            add('jolt', 'Jolt Physics', 'custom_windows', '剛体を選ぶとJolt Physics 5.5.0を静的リンク。箱、モデル凸包、固定三角形、骨変形衝突、受動ラグドールを選択可能。高速変形は別選択の保守的連続判定に対応。',
                'https://github.com/jrouwe/JoltPhysics', '同梱版を固定。箱/モデル凸包/固定メッシュ/骨変形衝突を接続。骨変形は最大4096三角形の静的形状交換。汎用関節UI、動的凹剛体、他機種間決定性は未対応／未検証。')
            rows[-1]['status'] = 'integrated_when_selected' if 'joints' not in config['physics'] else 'partially_integrated_rigid_boxes_only'
        add('rapier', '@dimforge/rapier3d', 'three_web', 'Web側で剛体・衝突を組み合わせる候補。',
            'https://rapier.rs/docs/user_guides/javascript/getting_started_js/', 'WASM初期転送・初期化時間、ブラウザー、Three.jsとの接続を検証。')
    if config['game'] in ('fps','tps','aim_training','monster_capture','action_rpg','chase','social_deduction'):
        add('cinemachine', 'Cinemachine', 'unity', '一人称・三人称のカメラ制作を支援する候補。',
            'https://docs.unity3d.com/Packages/com.unity.cinemachine@3.1/manual/index.html', 'Unityとパッケージの版、カメラ制御・入力との接続を固定して確認。')
    if config['game'] in ('monster_capture','action_rpg','chase'):
        if config['target'] != 'web':
            add('recast', 'Recast / Detour', 'custom_windows', '敵やモンスターの経路探索候補。追跡AIの意思決定は別途制作。',
                'https://github.com/recastnavigation/recastnavigation', 'ナビメッシュ生成、経路更新、動的障害物、追加容量を検証。未接続。')
        add('unity_navigation', 'AI Navigation', 'unity', 'NavMeshによる移動・経路探索の候補。',
            'https://docs.unity3d.com/Packages/com.unity.ai.navigation@2.0/manual/index.html', '地形・動的障害物・エージェント数と対象版を検証。')
    if config['network'] == 'authoritative':
        add('unity_netcode', 'Netcode for GameObjects', 'unity', 'Unityの複数人通信を構成する候補。',
            'https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.4/manual/index.html', 'サーバー判定と秘密情報の配信先制御はゲーム側に実装。対象プラットフォーム・トランスポートを検証。')
    if config.get('model')=='fbx':
        add('ufbx', 'ufbx 0.23.1（ビルド時のみ）', 'custom_windows', 'FBXのポリゴンとボーンを専用配布形式へ変換。Blenderのインストールは不要。',
            'https://github.com/ufbx/ufbx', '同梱版を固定。プレイヤーにはリンクしません。材質・UV・モーフ・4本を超える重み・デュアルクォータニオンは未対応。')
        rows[-1]['status']='integrated_build_time_only'
    return rows
