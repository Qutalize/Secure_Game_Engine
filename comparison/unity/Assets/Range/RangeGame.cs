using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.InputSystem;

namespace RangeComparison
{
    public sealed class RangeGame : MonoBehaviour
    {
        public Shader shader; // シーン参照でstripによるShader.Find欠落を防ぐ。
        public Shader hudShader;
        private Simulation sim;
        private List<Command> replay;
        private List<TraceRow> trace;
        private readonly Command liveCommand=new Command();
        private int replayStep,steps,flash;
        private double accumulator,lastTime,mouseYaw,mousePitch;
        private bool exitAfterReplay,quitting,restartPending;
        private string stateOutput;
        private Camera view;
        private Material material,hudMaterial;
        private int hudOrder;
        private MeshFilter scoreFilter,timeFilter,crossFilter,resultFilter;
        private readonly List<Mesh> scores=new List<Mesh>(),timers=new List<Mesh>();
        private Mesh cross,hitCross,win,lose;
        private GameObject[] targets;

        public static string Arg(string name,string fallback=null)
        {
            var args=Environment.GetCommandLineArgs();
            for(int i=0;i<args.Length;++i) if(args[i]==name)
            {
                if(i+1==args.Length)throw new ArgumentException("Missing value: "+name);
                return args[i+1];
            }
            return fallback;
        }
        private static bool Has(string flag) { return Array.IndexOf(Environment.GetCommandLineArgs(),flag)>=0; }
        private void Start()
        {
            try
            {
#if ENABLE_IL2CPP
                Debug.Log("RANGE BACKEND: IL2CPP");
#else
                Debug.Log("RANGE BACKEND: Mono");
#endif
                var spec=JsonUtility.FromJson<Spec>(File.ReadAllText(Arg("--range-data",Path.Combine(Application.streamingAssetsPath,"range.json"))));
                sim=new Simulation(spec);
                string path=Arg("--replay");
                if(path!=null)replay=JsonUtility.FromJson<Replay>(File.ReadAllText(path)).Expand();
                exitAfterReplay=Has("--exit-after-replay");stateOutput=Arg("--state-out");
                if(stateOutput!=null)trace=new List<TraceRow>(72000);
                if(exitAfterReplay&&replay==null)throw new ArgumentException("--exit-after-replay requires --replay");
                QualitySettings.vSyncCount=1;QualitySettings.antiAliasing=0;Application.targetFrameRate=-1;
                if(Has("--require-comparison-settings") &&
                    (SystemInfo.graphicsDeviceType!=UnityEngine.Rendering.GraphicsDeviceType.Direct3D12 ||
                     Screen.width!=1600 || Screen.height!=900 || QualitySettings.activeColorSpace!=ColorSpace.Gamma))
                    throw new InvalidOperationException("Comparison requires D3D12, 1600x900 and Gamma. Check Player arguments/log.");
                Debug.Log("RANGE SETTINGS: api="+SystemInfo.graphicsDeviceType+" gpu="+SystemInfo.graphicsDeviceName+
                    " resolution="+Screen.width+"x"+Screen.height+" color="+QualitySettings.activeColorSpace+" vsync=1 msaa=0");
                Application.runInBackground=true;
                Cursor.lockState=CursorLockMode.None;Cursor.visible=true;
                material=new Material(shader);
                hudMaterial=new Material(hudShader);
                view=new GameObject("Range camera").AddComponent<Camera>();
                view.fieldOfView=70;view.nearClipPlane=.1f;view.farClipPlane=500;
                view.clearFlags=CameraClearFlags.SolidColor;view.backgroundColor=new Color(.1f,.1f,.2f,1);
                view.allowHDR=false;view.allowMSAA=false;view.useOcclusionCulling=false;view.cullingMask=1;
                var hud=new GameObject("HUD camera").AddComponent<Camera>();
                hud.clearFlags=CameraClearFlags.Depth;hud.orthographic=true;hud.orthographicSize=1;
                hud.nearClipPlane=.1f;hud.farClipPlane=10;hud.depth=1;hud.cullingMask=1<<8;
                hud.allowHDR=false;hud.allowMSAA=false;hud.useOcclusionCulling=false;
                Spawn("Floor",MakeBox(new Box{x=0,y=-.1,z=0,sx=20,sy=.2,sz=20,r=.16,g=.21,b=.25}));
                foreach(var wall in spec.walls)Spawn("Wall",MakeBox(wall));
                targets=new GameObject[spec.targets.Length];
                for(int i=0;i<targets.Length;++i)targets[i]=Spawn("Target "+i,MakeBox(spec.targets[i])).gameObject;
                for(int i=0;i<=10;++i)scores.Add(Hud(i,-1,-1));
                for(int i=0;i<=60;++i)timers.Add(Hud(-1,i,-1));
                cross=Hud(-1,-1,0);hitCross=Hud(-1,-1,1);
                win=EndPanel.Build(true);lose=EndPanel.Build(false);
                scoreFilter=Spawn("Score",scores[0],8);timeFilter=Spawn("Timer",timers[60],8);crossFilter=Spawn("Crosshair",cross,8);
                resultFilter=Spawn("Result",null,8);
                UpdateView();
                lastTime=Time.realtimeSinceStartupAsDouble;
                GameEngine.Benchmark.EventBenchmark.MarkSceneReady();
                Debug.Log("RANGE READY: WASD / left stick; right stick / Tab+mouse; R2 / click; R / triangle retry; ESC exit");
            }
            catch(Exception error){Debug.LogException(error);quitting=true;Application.Quit(2);enabled=false;}
        }
        // Native版と同じ0.16の半径デッドゾーン。InputSystemの既定processorを二重適用しない。
        private static Vector2 Stick(UnityEngine.InputSystem.Controls.StickControl stick)
        {
            Vector2 v=stick.ReadUnprocessedValue();float length=v.magnitude;
            if(length<.16f)return Vector2.zero;
            v*=((length-.16f)/(1-.16f))/length;
            return new Vector2(Mathf.Clamp(v.x,-1,1),Mathf.Clamp(v.y,-1,1));
        }
        private void Update()
        {
            if(sim==null||quitting)return;
            var keyboard=Keyboard.current;var mouse=Mouse.current;var pad=Gamepad.current;
            if((keyboard!=null&&keyboard.escapeKey.wasPressedThisFrame)||(pad!=null&&pad.startButton.wasPressedThisFrame))
            {Application.Quit();quitting=true;return;}
            if(replay==null&&keyboard!=null&&keyboard.tabKey.wasPressedThisFrame)
            {Cursor.lockState=Cursor.lockState==CursorLockMode.Locked?CursorLockMode.None:CursorLockMode.Locked;Cursor.visible=Cursor.lockState!=CursorLockMode.Locked;}
            if(replay==null&&mouse!=null&&Cursor.lockState==CursorLockMode.Locked)
            {Vector2 delta=mouse.delta.ReadValue();mouseYaw+=delta.x*.08;mousePitch+=delta.y*.08;}
            restartPending|=(keyboard!=null&&keyboard.rKey.wasPressedThisFrame)||(pad!=null&&pad.buttonNorth.wasPressedThisFrame);
            double now=Time.realtimeSinceStartupAsDouble;accumulator+=now-lastTime;lastTime=now;
            double step=1.0/sim.Config.tickRate;
            Vector2 move=pad!=null?Stick(pad.leftStick):Vector2.zero,look=pad!=null?Stick(pad.rightStick):Vector2.zero;
            while(accumulator>=step)
            {
                accumulator-=step;Command command;
                if(replay!=null)
                {
                    if(replayStep==replay.Count)
                    {if(exitAfterReplay){Application.Quit();quitting=true;}accumulator=0;break;}
                    command=replay[replayStep++];
                }
                else
                {
                    command=liveCommand;
                    command.moveX=Simulation.Clamp(move.x+(keyboard!=null&&keyboard.dKey.isPressed?1:0)-(keyboard!=null&&keyboard.aKey.isPressed?1:0),-1,1);
                    command.moveZ=Simulation.Clamp(move.y+(keyboard!=null&&keyboard.wKey.isPressed?1:0)-(keyboard!=null&&keyboard.sKey.isPressed?1:0),-1,1);
                    command.yawDelta=look.x*120.0*step+mouseYaw;command.pitchDelta=look.y*120.0*step+mousePitch;
                    command.fire=(pad!=null&&pad.rightTrigger.ReadValue()>.5f)||(mouse!=null&&mouse.leftButton.isPressed&&Cursor.lockState==CursorLockMode.Locked);
                    command.restart=restartPending;
                    mouseYaw=mousePitch=0;restartPending=false;
                }
                sim.Step(command);if(flash>0)--flash;if(sim.Current.lastHit>=0)flash=12;
                if(stateOutput!=null)
                {
                    if(trace.Count==72000){Debug.LogError("Trace limit exceeded");quitting=true;Application.Quit(3);break;}
                    trace.Add(new TraceRow{step=steps,state=sim.Current});
                }
                ++steps;
            }
            UpdateView();
        }
        private void UpdateView()
        {
            var s=sim.Current;
            view.transform.SetPositionAndRotation(new Vector3((float)s.x,(float)sim.Config.eyeHeight,(float)s.z),Quaternion.Euler((float)-s.pitch,(float)s.yaw,0));
            for(int i=0;i<targets.Length;++i)targets[i].SetActive((s.mask&(1<<i))==0);
            scoreFilter.sharedMesh=scores[s.hits];timeFilter.sharedMesh=timers[Mathf.Clamp((sim.Config.durationTicks-s.tick+119)/120,0,60)];
            crossFilter.sharedMesh=flash>0?hitCross:cross;
            resultFilter.sharedMesh=s.status==0?null:(s.status==1?win:lose);
        }
        private void OnApplicationQuit()
        {
            try {Simulation.SaveTrace(stateOutput,trace);}catch(Exception ex){Debug.LogException(ex);}
        }
        private MeshFilter Spawn(string name,Mesh mesh,int layer=0)
        {
            var obj=new GameObject(name);obj.layer=layer;
            var filter=obj.AddComponent<MeshFilter>();filter.sharedMesh=mesh;
            var renderer=obj.AddComponent<MeshRenderer>();renderer.sharedMaterial=layer==8?hudMaterial:material;
            if(layer==8)renderer.sortingOrder=hudOrder++;
            renderer.shadowCastingMode=UnityEngine.Rendering.ShadowCastingMode.Off;renderer.receiveShadows=false;
            return filter;
        }
        private static Mesh MakeBox(Box b)
        {
            int[,] signs={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
            int[,] faces={{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
            float[] shade={1,.65f,.8f,.9f,1,.55f};
            var vertices=new List<Vector3>();var colors=new List<Color>();var indices=new List<int>();
            for(int f=0;f<6;++f)
            {
                int start=vertices.Count;
                for(int n=0;n<4;++n){int p=faces[f,n];vertices.Add(new Vector3((float)(b.x+signs[p,0]*b.sx*.5),(float)(b.y+signs[p,1]*b.sy*.5),(float)(b.z+signs[p,2]*b.sz*.5)));colors.Add(new Color((float)(b.r*shade[f]),(float)(b.g*shade[f]),(float)(b.b*shade[f]),1));}
                foreach(int index in new[]{0,1,2,0,2,3})indices.Add(start+index);
            }
            var mesh=new Mesh();mesh.SetVertices(vertices);mesh.SetColors(colors);mesh.SetTriangles(indices,0);return mesh;
        }
        public static void Quad(List<Vector3> v,List<Color> colors,List<int> indices,float x,float y,float w,float h,Color color)
        {
            int start=v.Count;float aspect=1600f/900f;
            foreach(var p in new[]{new Vector3(x*aspect,y,1),new Vector3((x+w)*aspect,y,1),new Vector3((x+w)*aspect,y+h,1),new Vector3(x*aspect,y+h,1)}){v.Add(p);colors.Add(color);}
            foreach(int index in new[]{0,1,2,0,2,3})indices.Add(start+index);
        }
        private static Mesh Hud(int score,int seconds,int crosshair)
        {
            var v=new List<Vector3>();var colors=new List<Color>();var indices=new List<int>();
            if(score>=0)for(int dot=0;dot<10;++dot)Quad(v,colors,indices,-.92f+dot*.085f,.82f,.06f,.045f,dot<score?new Color(1,.65f,.15f,1):new Color(.2f,.25f,.3f,1));
            if(seconds>=0){Quad(v,colors,indices,.25f,.82f,.67f,.045f,new Color(.15f,.2f,.25f,1));if(seconds>0)Quad(v,colors,indices,.25f,.82f,.67f*seconds/60,.045f,new Color(.2f,.85f,.75f,1));}
            if(crosshair>=0){Color c=crosshair==1?new Color(.2f,1,.4f,1):Color.white;Quad(v,colors,indices,-.0015f,-.017f,.003f,.034f,c);Quad(v,colors,indices,-.0096f,-.0027f,.0192f,.0054f,c);}
            var mesh=new Mesh();mesh.SetVertices(v);mesh.SetColors(colors);mesh.SetTriangles(indices,0);return mesh;
        }
    }
}
