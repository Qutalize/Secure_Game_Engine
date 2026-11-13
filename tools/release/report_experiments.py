"""Plot recorded trials without pooling frames or inventing independent samples."""
import json
from pathlib import Path
import statistics
import os
ROOT=Path(__file__).resolve().parents[2]
os.environ.setdefault('MPLCONFIGDIR',str(ROOT/'output/matplotlib-cache'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

OUT=ROOT/'output/release-validation'
def load(path):return json.loads(path.read_text(encoding='utf8'))
def main():
    rows=load(ROOT/'output/benchmarks/defenses-repeat-20261004/results.json')
    policies=['none','shuffle','watchdog','depth-guard','tag-mask','shuffle,watchdog,depth-guard,tag-mask']
    labels=['None','Shuffle','Watchdog','Depth guard','Tag mask','All four']
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'axes.spines.top':False,'axes.spines.right':False})
    fig,axes=plt.subplots(1,2,figsize=(12,4.8),layout='constrained')
    for ax,key,title,unit in [(axes[0],'launch_to_scene_ready_ms','Launch to scene ready','ms'),(axes[1],'frame_p95_ms','Update interval p95 per run','ms')]:
        for i,policy in enumerate(policies):
            values=[r['trace'][key] for r in rows if r['policy']==policy and r['status']=='ok']
            xs=[i+(j-3)*.035 for j in range(len(values))]
            ax.scatter(xs,values,s=26,alpha=.8,color='#247b8c')
            ax.hlines(statistics.median(values),i-.27,i+.27,color='#c34826',linewidth=2)
        ax.set_xticks(range(6),labels,rotation=28,ha='right');ax.set_title(title);ax.set_ylabel(unit);ax.grid(axis='y',alpha=.2)
    axes[0].set_ylim(bottom=0);axes[1].set_ylim(16,19)
    fig.suptitle('FPS defenses: 7 runs each | dots = runs; orange = median\nVSync ON, 5 s samples; CPU update intervals, not input latency',fontsize=13)
    fig.savefig(OUT/'defenses-repeat.png',dpi=180);plt.close(fig)
    phases=['native_entry','window_ready','renderer_ready','audio_init_returned','pak_check_finished','network_init_returned','scene_ready']
    phase_report={}
    for index,phase in enumerate(phases):
        values=[r['trace']['events'][phase]-(r['trace']['events'][phases[index-1]] if index else 0) for r in rows]
        phase_report[phase]={'median_ms':statistics.median(values),'min_ms':min(values),'max_ms':max(values)}
    (OUT/'startup-phases.json').write_text(json.dumps(phase_report,indent=2),encoding='utf8')
    runs=load(ROOT/'output/benchmarks/protection-repeat-20261004/runs.json')
    fig,axes=plt.subplots(1,2,figsize=(9,4.5),layout='constrained')
    for ax,key,title in [(axes[0],'launch_to_scene_ready_ms','Launch to scene ready (ms)'),(axes[1],'frame_p95_ms','Update interval p95 (ms)')]:
        for i,name in enumerate(['normal_range','protected_range']):
            values=[r[key] for r in runs if r['case']==name and not r['warmup_run'] and r['status']=='ok']
            ax.scatter([i+(j-3)*.04 for j in range(len(values))],values,s=32,color='#247b8c')
            ax.hlines(statistics.median(values),i-.25,i+.25,color='#c34826',linewidth=2)
        ax.set_xticks([0,1],['Normal','Protected']);ax.set_xlim(-.6,1.6);ax.set_title(title);ax.grid(axis='y',alpha=.2)
    axes[0].set_ylim(bottom=0)
    fig.suptitle('Same native range replay | 7 runs each after warmup\nDots = runs; orange = median. No claim of a speed advantage.',fontsize=12)
    fig.savefig(OUT/'protection-repeat.png',dpi=180);plt.close(fig)
    print(json.dumps(phase_report,indent=2))
if __name__=='__main__':main()
