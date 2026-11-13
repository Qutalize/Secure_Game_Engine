using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

// Windows Standalone 専用。Editor では無効。既存教材や NGO に依存しない計測アダプター。
// 共通ミニゲームの準備が完了した場所から EventBenchmark.MarkSceneReady() を呼ぶ。
namespace GameEngine.Benchmark
{
    [DefaultExecutionOrder(-32000)]
    public sealed class EventBenchmark : MonoBehaviour
    {
        private const int MaxSamples = 100000;
        private struct Sample { public string Name; public long Ticks; public double Interval; }
        private static EventBenchmark instance;
        private Sample[] samples;
        private int count;
        private string outputPath;
        private long frequency, firstFrame, previous, sampleStart;
        private double seconds, warmup;
        private bool ready, done, overflow;

        [DllImport("kernel32.dll")]
        private static extern bool QueryPerformanceCounter(out long value);
        [DllImport("kernel32.dll")]
        private static extern bool QueryPerformanceFrequency(out long value);

        private static long Now()
        {
            if (!QueryPerformanceCounter(out long ticks)) throw new InvalidOperationException("QPC unavailable");
            return ticks;
        }

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        private static void Install()
        {
#if UNITY_STANDALONE_WIN && !UNITY_EDITOR
            instance = null;
            string[] args = Environment.GetCommandLineArgs();
            string path = null;
            double duration = 10.0, settling = 2.0;
            bool requested = false;
            try
            {
                for (int i = 1; i < args.Length; ++i)
                {
                    bool output = args[i] == "--metrics";
                    bool timing = args[i] == "--benchmark-seconds";
                    bool warming = args[i] == "--benchmark-warmup";
                    if (!output && !timing && !warming) continue;
                    requested = true;
                    if (++i >= args.Length) throw new ArgumentException("Missing benchmark value");
                    if (output) { path = args[i]; continue; }
                    double value = double.Parse(args[i], CultureInfo.InvariantCulture);
                    if (double.IsNaN(value) || double.IsInfinity(value) || value > 300.0 ||
                        value < (warming ? 0.0 : 0.001)) throw new ArgumentException("Invalid duration");
                    if (timing) duration = value; else settling = value;
                }
                if (!requested) return;
                if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("--metrics PATH is required");
                var go = new GameObject("EventBenchmark");
                DontDestroyOnLoad(go);
                instance = go.AddComponent<EventBenchmark>();
                if (!QueryPerformanceFrequency(out instance.frequency) || instance.frequency <= 0)
                    throw new InvalidOperationException("QPC frequency unavailable");
                instance.samples = new Sample[MaxSamples];
                instance.outputPath = path;
                instance.seconds = duration;
                instance.warmup = settling;
                instance.Mark("managed_bootstrap");
                instance.Mark(Debug.isDebugBuild ? "build_debug" : "build_release");
#if ENABLE_IL2CPP
                instance.Mark("backend_il2cpp");
#else
                instance.Mark("backend_mono");
#endif
                instance.Mark(QualitySettings.vSyncCount > 0 ? "vsync_on" : "vsync_off");
            }
            catch (Exception error)
            {
                if (instance != null) { instance.done = true; instance.outputPath = null; }
                Debug.LogError("Benchmark setup failed: " + error.Message);
                Application.Quit(2);
            }
#endif
        }

        // sceneLoaded だけでは非同期アセット・入力・接続が準備できたとは限らない。
        // 初期化担当のゲームコードが責任を持って一度呼ぶ。未呼出しはランチャーのtimeoutで失敗。
        public static void MarkSceneReady()
        {
            if (instance == null || instance.ready || instance.done) return;
            instance.Mark("scene_ready");
            instance.ready = true;
        }

        private void Add(string name, long ticks, double interval = -1.0)
        {
            if (count == MaxSamples) { overflow = true; return; }
            samples[count++] = new Sample { Name = name, Ticks = ticks, Interval = interval };
        }

        private void Mark(string name) { Add(name, Now()); }

        private void Update()
        {
            if (!ready || done) return;
            long now = Now();
            if (firstFrame == 0) { firstFrame = now; Add("first_update_begin", now); }
            if (sampleStart == 0)
            {
                if ((double)(now - firstFrame) / frequency >= warmup)
                {
                    sampleStart = now;
                    Add("sample_begin", now);
                }
            }
            else
            {
                Add("update_interval", now, 1000.0 * (now - previous) / frequency);
                if ((double)(now - sampleStart) / frequency >= seconds)
                {
                    Add("sample_end", now);
                    done = true;
                    Application.Quit(0);
                }
            }
            previous = now;
            if (overflow) { done = true; Application.Quit(3); }
        }

        private void OnApplicationQuit()
        {
            if (string.IsNullOrEmpty(outputPath)) return;
            try
            {
                using (var writer = new StreamWriter(outputPath, false, new UTF8Encoding(false)))
                {
                    writer.WriteLine("schema,kind,name,qpc_ticks,qpc_frequency,value_ms");
                    for (int i = 0; i < count; ++i)
                    {
                        Sample sample = samples[i];
                        writer.Write("1," + (sample.Interval < 0.0 ? "event" : "frame") + "," + sample.Name + ",");
                        writer.Write(sample.Ticks.ToString(CultureInfo.InvariantCulture) + "," +
                            frequency.ToString(CultureInfo.InvariantCulture) + ",");
                        if (sample.Interval >= 0.0) writer.Write(sample.Interval.ToString("G17", CultureInfo.InvariantCulture));
                        writer.WriteLine();
                    }
                    if (overflow) writer.WriteLine("1,event,buffer_overflow," + Now() + "," + frequency + ",");
                }
            }
            catch (Exception error)
            {
                Debug.LogError("Metrics write failed: " + error.Message);
                // CSV不在/不完全はランチャーも検出する。
            }
        }
    }
}
