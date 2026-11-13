using System.Collections.Generic;
using UnityEngine;

namespace RangeComparison
{
    // 同じ5x7フォントの行ビットはprepare_unity.pyがOverlayText.cppから生成する。
    public static class EndPanel
    {
        public static Mesh Build(bool won)
        {
            string[] lines={won?"CLEAR !":"TIME UP","R=RETRY","ESC=QUIT"};
            const float px=.020f,pxx=px/(1600f/900f),charW=6*pxx,lineH=9*px;
            var v=new List<Vector3>();var colors=new List<Color>();var indices=new List<int>();
            int max=0;foreach(string line in lines)max=Mathf.Max(max,line.Length);
            float w=max*charW+8*pxx,h=lines.Length*lineH+6*px;
            RangeGame.Quad(v,colors,indices,-w/2,-h/2,w,h,new Color(.04f,.05f,.09f,1));
            for(int li=0;li<lines.Length;++li)
            {
                string line=lines[li];float left=-line.Length*charW/2,top=(lines.Length-1)*lineH/2-li*lineH;
                for(int ch=0;ch<line.Length;++ch)
                {
                    if(!FontRows.Rows.TryGetValue(line[ch],out int[] rows))continue;
                    for(int y=0;y<7;++y)for(int x=0;x<5;++x)if((rows[y]&(1<<(4-x)))!=0)
                        RangeGame.Quad(v,colors,indices,left+ch*charW+x*pxx,top-y*px-px,pxx,px,new Color(1,.95f,.35f,1));
                }
            }
            var mesh=new Mesh();mesh.SetVertices(v);mesh.SetColors(colors);mesh.SetTriangles(indices,0);return mesh;
        }
    }
}
