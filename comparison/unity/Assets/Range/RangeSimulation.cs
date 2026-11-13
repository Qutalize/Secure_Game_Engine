using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace RangeComparison
{
    [Serializable] public class Box
    {
        public double x,y,z,sx,sy,sz,r,g,b;
    }
    [Serializable] public class Spec
    {
        public int version,tickRate,durationTicks,cooldownTicks;
        public double speed,radius,eyeHeight,extent,spawnX,spawnZ;
        public Box[] walls,targets;
        public void Validate()
        {
            if (version!=1 || tickRate!=120 || durationTicks!=7200 ||
                cooldownTicks<1 || cooldownTicks>120 || walls==null || walls.Length>64 || targets==null || targets.Length!=10)
                throw new ArgumentException("Invalid range spec version/counts");
            Check(speed,.1,10); Check(radius,.2,1); Check(eyeHeight,1,2); Check(extent,10,10);
            Check(spawnX,-extent+radius,extent-radius); Check(spawnZ,-extent+radius,extent-radius);
            foreach (var list in new[]{walls,targets}) foreach (var b in list)
            {
                Check(b.x,-50,50); Check(b.y,0,10); Check(b.z,-50,50);
                Check(b.sx,.05,100); Check(b.sy,.05,20); Check(b.sz,.05,100);
                Check(b.r,0,1); Check(b.g,0,1); Check(b.b,0,1);
            }
        }
        public static void Check(double value,double lo,double hi)
        {
            if (double.IsNaN(value)||double.IsInfinity(value)||value<lo||value>hi) throw new ArgumentException("Range value outside limits");
        }
    }
    [Serializable] public class Command
    {
        public double moveX,moveZ,yawDelta,pitchDelta;
        public bool fire,restart;
    }
    [Serializable] public class Segment : Command { public int ticks; }
    [Serializable] public class Replay
    {
        public int version;
        public Segment[] segments;
        public List<Command> Expand()
        {
            if (version!=1||segments==null) throw new ArgumentException("Invalid replay version");
            var result=new List<Command>();
            foreach(var segment in segments)
            {
                if (segment.ticks<1||segment.ticks>72000||result.Count+segment.ticks>72000) throw new ArgumentException("Replay too large");
                Spec.Check(segment.moveX,-1,1); Spec.Check(segment.moveZ,-1,1);
                Spec.Check(segment.yawDelta,-180,180); Spec.Check(segment.pitchDelta,-160,160);
                for(int n=0;n<segment.ticks;++n) result.Add(segment);
            }
            if (result.Count==0) throw new ArgumentException("Empty replay");
            return result;
        }
    }
    public struct State
    {
        public int tick,hits,shots,mask,nextShot,status,lastHit;
        public double x,z,yaw,pitch;
        public bool fired;
    }
    public struct TraceRow { public int step; public State state; }

    // C++版と同じdouble演算、120Hz、X→Z衝突、壁優先レイ。Unity Physicsには依存しない。
    public sealed class Simulation
    {
        public readonly Spec Config;
        public State Current;
        public Simulation(Spec config) { config.Validate(); Config=config; Reset(); }
        public void Reset() { Current=new State { x=Config.spawnX,z=Config.spawnZ,lastHit=-1 }; }
        public static double Clamp(double v,double lo,double hi) { return Math.Max(lo,Math.Min(hi,v)); }
        private bool CanStand(double x,double z)
        {
            foreach(var b in Config.walls)
                if(x>b.x-b.sx*.5-Config.radius&&x<b.x+b.sx*.5+Config.radius&&z>b.z-b.sz*.5-Config.radius&&z<b.z+b.sz*.5+Config.radius) return false;
            return true;
        }
        private static bool Slab(double o,double d,double center,double size,ref double near,ref double far)
        {
            double lo=center-size*.5,hi=center+size*.5;
            if(Math.Abs(d)<1e-12) return o>=lo&&o<=hi;
            double a=(lo-o)/d,b=(hi-o)/d;
            if(a>b) { double temp=a;a=b;b=temp; }
            near=Math.Max(near,a);far=Math.Min(far,b);return near<=far;
        }
        private static double RayBox(double x,double y,double z,double dx,double dy,double dz,Box b)
        {
            double near=0,far=100;
            if(!Slab(x,dx,b.x,b.sx,ref near,ref far)||!Slab(y,dy,b.y,b.sy,ref near,ref far)||!Slab(z,dz,b.z,b.sz,ref near,ref far)) return 101;
            return near;
        }
        public void Step(Command c)
        {
            if(c.restart) { Reset();return; }
            var s=Current;s.lastHit=-1;s.fired=false;
            if(s.status!=0) { Current=s;return; }
            s.yaw+=c.yawDelta;while(s.yaw>180)s.yaw-=360;while(s.yaw<=-180)s.yaw+=360;
            s.pitch=Clamp(s.pitch+c.pitchDelta,-80,80);
            double yaw=s.yaw*Math.PI/180,pitch=s.pitch*Math.PI/180;
            double norm=Math.Max(1,Math.Sqrt(c.moveX*c.moveX+c.moveZ*c.moveZ));
            double step=Config.speed/Config.tickRate/norm;
            double nx=Clamp(s.x+(c.moveX*Math.Cos(yaw)+c.moveZ*Math.Sin(yaw))*step,-Config.extent+Config.radius,Config.extent-Config.radius);
            if(CanStand(nx,s.z))s.x=nx;
            double nz=Clamp(s.z+(-c.moveX*Math.Sin(yaw)+c.moveZ*Math.Cos(yaw))*step,-Config.extent+Config.radius,Config.extent-Config.radius);
            if(CanStand(s.x,nz))s.z=nz;
            if(c.fire&&s.tick>=s.nextShot)
            {
                s.fired=true;++s.shots;s.nextShot=s.tick+Config.cooldownTicks;
                double dx=Math.Sin(yaw)*Math.Cos(pitch),dy=Math.Sin(pitch),dz=Math.Cos(yaw)*Math.Cos(pitch),nearest=101;
                foreach(var wall in Config.walls) nearest=Math.Min(nearest,RayBox(s.x,Config.eyeHeight,s.z,dx,dy,dz,wall));
                int hit=-1;
                for(int i=0;i<Config.targets.Length;++i)
                {
                    if((s.mask&(1<<i))!=0)continue;
                    double distance=RayBox(s.x,Config.eyeHeight,s.z,dx,dy,dz,Config.targets[i]);
                    if(distance<nearest){nearest=distance;hit=i;}
                }
                if(hit>=0){s.mask|=1<<hit;++s.hits;s.lastHit=hit;}
            }
            ++s.tick;
            if(s.hits==Config.targets.Length)s.status=1;
            else if(s.tick>=Config.durationTicks)s.status=2;
            Current=s;
        }
        public static void SaveTrace(string path,List<TraceRow> rows)
        {
            if(string.IsNullOrEmpty(path))return;
            using(var writer=new StreamWriter(path,false,new System.Text.UTF8Encoding(false)))
            {
                writer.WriteLine("step,tick,x,z,yaw,pitch,hits,shots,mask,status");
                foreach(var row in rows)
                {
                    var s=row.state;
                    writer.WriteLine(string.Join(",",row.step,s.tick,s.x.ToString("G17",CultureInfo.InvariantCulture),s.z.ToString("G17",CultureInfo.InvariantCulture),
                        s.yaw.ToString("G17",CultureInfo.InvariantCulture),s.pitch.ToString("G17",CultureInfo.InvariantCulture),s.hits,s.shots,s.mask,s.status));
                }
            }
        }
    }
}
