using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;

namespace RangeComparison.Editor
{
    public static class RangeBuild
    {
        private static string Root => Path.GetFullPath(Path.Combine(Application.dataPath,"../../.."));
        [Serializable]
        private sealed class BuildEvidence
        {
            public string unityVersion,backend,stripping,compiler,graphicsApis,output,buildGuid;
            public bool development;
            public double seconds;
            public ulong bytes;
        }

        private static string BuildOutput(bool protectedBuild)
        {
            string requested=null;
            var args=Environment.GetCommandLineArgs();
            for(int i=0;i<args.Length;++i)if(args[i]=="-rangeBuildOutput")
            {
                if(i+1>=args.Length)throw new ArgumentException("Missing -rangeBuildOutput value");
                requested=args[++i];
            }
            string path=Path.GetFullPath(requested??Path.Combine(Root,protectedBuild?
                "output/packages/range-unity-il2cpp/RangeUnity.exe":"output/packages/range-unity/RangeUnity.exe"));
            if(!path.EndsWith(".exe",StringComparison.OrdinalIgnoreCase))throw new ArgumentException("Build output must be an .exe path");
            var folder=Path.GetDirectoryName(path);
            if((protectedBuild||requested!=null)&&Directory.Exists(folder)&&Directory.GetFileSystemEntries(folder).Length!=0)
                throw new InvalidOperationException("Use a new empty build output directory: "+folder);
            return path;
        }
        public static void Validate()
        {
            string output=Path.Combine(Root,"output/range-validation");Directory.CreateDirectory(output);
            var spec=JsonUtility.FromJson<Spec>(File.ReadAllText(Path.Combine(Root,"comparison/data/range.json")));
            foreach(string name in new[]{"validation","victory","tour"})
            {
                var sim=new Simulation(spec);
                var commands=JsonUtility.FromJson<Replay>(File.ReadAllText(Path.Combine(Root,"comparison/data/"+name+".json"))).Expand();
                var rows=new List<TraceRow>();int step=0;
                foreach(var c in commands){sim.Step(c);rows.Add(new TraceRow{step=step++,state=sim.Current});}
                Simulation.SaveTrace(Path.Combine(output,"unity_"+name+".csv"),rows);
                if(name=="victory"&&(sim.Current.status!=1||sim.Current.mask!=1023))throw new Exception("Victory scenario failed");
                if(name=="validation"&&(sim.Current.status!=2||sim.Current.tick!=7200))throw new Exception("Timeout scenario failed");
                Debug.Log("RANGE VALIDATION: "+name+" ticks="+step+" hits="+sim.Current.hits);
            }
        }
        public static void Build()
        {
            BuildVariant(false);
        }

        // Separate output; restore backend settings even if the build fails.
        public static void BuildProtected()
        {
            string variations=Path.Combine(EditorApplication.applicationContentsPath,"PlaybackEngines/windowsstandalonesupport/Variations");
            if(!Directory.Exists(variations)||Directory.GetDirectories(variations,"win64*il2cpp*").Length==0)
                throw new InvalidOperationException("Windows Build Support (IL2CPP) for this exact Unity Editor version is required. Mono output was not changed.");
            var target=UnityEditor.Build.NamedBuildTarget.Standalone;
            var previousBackend=PlayerSettings.GetScriptingBackend(target);
            var previousStripping=PlayerSettings.GetManagedStrippingLevel(target);
            var previousCompiler=PlayerSettings.GetIl2CppCompilerConfiguration(target);
            try { BuildVariant(true); }
            finally {
                PlayerSettings.SetScriptingBackend(target,previousBackend);
                PlayerSettings.SetManagedStrippingLevel(target,previousStripping);
                PlayerSettings.SetIl2CppCompilerConfiguration(target,previousCompiler);
                AssetDatabase.SaveAssets();
            }
        }

        private static void BuildVariant(bool protectedBuild)
        {
            string output=BuildOutput(protectedBuild);
            Validate();
            DescribeDeviceFilters();
            PlayerSettings.companyName="GameEngineComparison";PlayerSettings.productName="RangeUnity";
            PlayerSettings.SplashScreen.show=false; // 起動比較にロゴの表示待ちを混ぜない。
            PlayerSettings.colorSpace=ColorSpace.Gamma;
            PlayerSettings.defaultScreenWidth=1600;PlayerSettings.defaultScreenHeight=900;
            PlayerSettings.fullScreenMode=FullScreenMode.Windowed;
            PlayerSettings.resizableWindow=false;PlayerSettings.runInBackground=true;
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows64,false);
            // Intel内蔵GPUではUnityの既定フィルターがD3D11へ戻す。
            // D3D12だけをビルドすると通常起動でフォールバック先のshaderが欠落する。
            // 通常プレイはD3D11を優先。D3D12比較は終了クラッシュの診断用に明示指定する。
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows64,
                new[]{GraphicsDeviceType.Direct3D11,GraphicsDeviceType.Direct3D12});
            var target=UnityEditor.Build.NamedBuildTarget.Standalone;
            PlayerSettings.SetScriptingBackend(target,protectedBuild?ScriptingImplementation.IL2CPP:ScriptingImplementation.Mono2x);
            if(protectedBuild) {
                PlayerSettings.SetManagedStrippingLevel(target,ManagedStrippingLevel.Medium);
                PlayerSettings.SetIl2CppCompilerConfiguration(target,Il2CppCompilerConfiguration.Release);
            }
            PlayerSettings.stripEngineCode=true;
            QualitySettings.vSyncCount=1;QualitySettings.antiAliasing=0;
            QualitySettings.shadows=ShadowQuality.Disable;QualitySettings.lodBias=1;
            // InputSystem only (no legacy Input API calls).
            var settings=new SerializedObject(AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/ProjectSettings.asset")[0]);
            var input=settings.FindProperty("activeInputHandler");
            if(input!=null){input.intValue=1;settings.ApplyModifiedPropertiesWithoutUndo();}
            var scene=EditorSceneManager.NewScene(NewSceneSetup.EmptyScene,NewSceneMode.Single);
            var game=new GameObject("Range").AddComponent<RangeGame>();
            game.shader=AssetDatabase.LoadAssetAtPath<Shader>("Assets/Range/Range.shader");
            game.hudShader=AssetDatabase.LoadAssetAtPath<Shader>("Assets/Range/RangeHud.shader");
            if(game.shader==null||game.hudShader==null)throw new Exception("Range shaders missing");
            EditorSceneManager.SaveScene(scene,"Assets/RangeScene.unity");
            AssetDatabase.SaveAssets();
            Directory.CreateDirectory(Path.GetDirectoryName(output));
            var report=BuildPipeline.BuildPlayer(new BuildPlayerOptions{
                scenes=new[]{"Assets/RangeScene.unity"},locationPathName=output,
                target=BuildTarget.StandaloneWindows64,options=BuildOptions.None});
            if(report.summary.result!=BuildResult.Succeeded)throw new Exception("Unity build failed: "+report.summary.result);
            // Keep build provenance beside the package, outside the runtime directory.
            var evidence=new BuildEvidence{
                unityVersion=Application.unityVersion,backend=PlayerSettings.GetScriptingBackend(target).ToString(),
                stripping=PlayerSettings.GetManagedStrippingLevel(target).ToString(),compiler=PlayerSettings.GetIl2CppCompilerConfiguration(target).ToString(),
                graphicsApis=string.Join(",",PlayerSettings.GetGraphicsAPIs(BuildTarget.StandaloneWindows64)),
                output=output,buildGuid=report.summary.guid.ToString(),development=(report.summary.options&BuildOptions.Development)!=0,
                seconds=report.summary.totalTime.TotalSeconds,bytes=report.summary.totalSize};
            File.WriteAllText(Path.GetDirectoryName(output)+"-build.json",JsonUtility.ToJson(evidence,true));
            Debug.Log("RANGE BUILD SUCCEEDED: "+output);
        }

        public static void DescribeDeviceFilters()
        {
            var filters=PlayerSettings.d3D12DeviceFilterListAsset;
            bool usesDefault=filters==null;
            string output=Path.Combine(Root,"output/range-validation");Directory.CreateDirectory(output);
            // 未指定時の内部既定値は、このasset APIから列挙できない。空のassetを既定値と取り違えない。
            string asset=usesDefault?"null":EditorJsonUtility.ToJson(filters,true);
            File.WriteAllText(Path.Combine(output,"unity-device-filters.json"),
                "{\"usesDefault\":"+(usesDefault?"true":"false")+",\"projectAsset\":"+asset+"}");
            Debug.Log("RANGE DEVICE FILTERS: usesDefault="+usesDefault);
        }
    }
}
