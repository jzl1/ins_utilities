// ============================================================================
// navdoctor - validates a Source .nav mesh against the map's REAL collision
// geometry (.bsp), by simulating a player-sized hull walking every connection.
// Connections that no hull (standing or crouched, with step-ups and jumps) can
// physically traverse are cut, and a repaired .nav is written.
//
// This catches the class of defect nav_generate is famous for: connections
// through openings too small for a player (windows, fences, grates), because
// the nav file itself stores only floor rectangles - no walls, no clearance.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

struct Vector { float x,y,z; Vector():x(0),y(0),z(0){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
  Vector operator+(const Vector&o)const{return Vector(x+o.x,y+o.y,z+o.z);}
  Vector operator*(float s)const{return Vector(x*s,y*s,z*s);}
  float Length()const{return sqrtf(x*x+y*y+z*z);} };
static float dot(const Vector&a,const Vector&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static Vector cross(const Vector&a,const Vector&b){return Vector(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
static float xyd(const Vector&a,const Vector&b){float dx=a.x-b.x,dy=a.y-b.y;return sqrtf(dx*dx+dy*dy);}

#define TESTING_NATIVE 1
#include "../insbots/src/nav.h"
#include "../insbots/src/nav.cpp"

// ---------------------------------------------------------------- BSP formats
#pragma pack(push,1)
struct lump_t { int ofs,len,version; char fourCC[4]; };
struct dheader_t { int ident,version; lump_t lumps[64]; int mapRev; };
struct dplane_t { Vector n; float d; int type; };
struct dnode_t { int planenum; int children[2]; short mins[3],maxs[3];
                 unsigned short firstface,numfaces; short area,pad; };
struct dedge_t { unsigned short v[2]; };
struct dbrush_t { int firstside,numsides,contents; };
struct dbrushside_t { unsigned short planenum; short texinfo,dispinfo,bevel; };
struct dmodel_t { Vector mins,maxs,origin; int headnode,firstface,numfaces; };
struct dface_t { unsigned short planenum; unsigned char side,onNode; int firstedge;
                 short numedges,texinfo,dispinfo,fogvol; unsigned char styles[4];
                 int lightofs; float area; int lmins[2],lsize[2]; int origFace;
                 unsigned short numPrims,firstPrim; unsigned int smoothing; };
struct ddispvert_t { Vector vec; float dist,alpha; };
#pragma pack(pop)

#define C_SOLID 0x1
#define C_WINDOW 0x2
#define C_GRATE 0x8
#define C_PLAYERCLIP 0x10000
#define SOLID_MASK (C_SOLID|C_WINDOW|C_GRATE|C_PLAYERCLIP)

// ---------------------------------------------------------------- collision db
struct BrushSideP { Vector n; float d; };
struct CBrush { int firstSide,numSides; Vector lo,hi; };
struct Tri { Vector a,b,c; };

static std::vector<BrushSideP> g_sides;      // flattened side planes per brush
static std::vector<CBrush>     g_brushes;    // world solid brushes only
static std::vector<Tri>        g_tris;       // displacement triangles

// spatial grid
static float g_worldLo[3], g_worldHi[3];
static int   g_gx, g_gy;                     // cells in x,y (z ignored)
static const float CELL = 256.0f;
static std::vector<std::vector<int> > g_cellBrush, g_cellTri;

static int cellOf(float x,float y){
  int cx=(int)((x-g_worldLo[0])/CELL), cy=(int)((y-g_worldLo[1])/CELL);
  if(cx<0)cx=0; if(cy<0)cy=0; if(cx>=g_gx)cx=g_gx-1; if(cy>=g_gy)cy=g_gy-1;
  return cy*g_gx+cx;
}

static void gridInsert(std::vector<std::vector<int> >&grid,const Vector&lo,const Vector&hi,int idx){
  int cx0=(int)((lo.x-g_worldLo[0])/CELL), cy0=(int)((lo.y-g_worldLo[1])/CELL);
  int cx1=(int)((hi.x-g_worldLo[0])/CELL), cy1=(int)((hi.y-g_worldLo[1])/CELL);
  if(cx0<0)cx0=0; if(cy0<0)cy0=0; if(cx1>=g_gx)cx1=g_gx-1; if(cy1>=g_gy)cy1=g_gy-1;
  for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++)
    grid[cy*g_gx+cx].push_back(idx);
}

// ---------------------------------------------------------------- BSP loading
static bool loadBsp(const char*path){
  FILE*fp=fopen(path,"rb"); if(!fp){printf("no bsp\n");return false;}
  fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
  unsigned char*d=(unsigned char*)malloc(sz);
  if(fread(d,1,sz,fp)!=(size_t)sz){fclose(fp);return false;}
  fclose(fp);
  dheader_t*h=(dheader_t*)d;
  if(h->ident!=('V'|('B'<<8)|('S'<<16)|('P'<<24))){printf("bad magic\n");return false;}
  printf("BSP version %d\n",h->version);

  #define LUMP(i,T) ((T*)(d+h->lumps[i].ofs)); int n_##T = h->lumps[i].len/(int)sizeof(T)
  dplane_t   *planes = LUMP(1,dplane_t);   (void)n_dplane_t;
  Vector     *verts  = (Vector*)(d+h->lumps[3].ofs);
  dnode_t    *nodes  = LUMP(5,dnode_t);    (void)n_dnode_t;
  dface_t    *faces  = LUMP(7,dface_t);    (void)n_dface_t;
  dedge_t    *edges  = LUMP(12,dedge_t);   (void)n_dedge_t;
  int        *surfedges=(int*)(d+h->lumps[13].ofs);
  dmodel_t   *models = LUMP(14,dmodel_t);  int nModels=h->lumps[14].len/(int)sizeof(dmodel_t);
  unsigned short*leafbrushes=(unsigned short*)(d+h->lumps[17].ofs);
  dbrush_t   *brushes= LUMP(18,dbrush_t);  int nBrushes=h->lumps[18].len/(int)sizeof(dbrush_t);
  dbrushside_t*bsides= LUMP(19,dbrushside_t);(void)n_dbrushside_t;
  unsigned char*dispinfo=d+h->lumps[26].ofs; int nDisp=h->lumps[26].len/176;
  ddispvert_t*dispverts=(ddispvert_t*)(d+h->lumps[33].ofs);

  // leaf stride: v0=56 (has ambient cube), v1=32
  int leafStride = (h->lumps[10].version==0)?56:32;
  unsigned char*leafs=d+h->lumps[10].ofs;
  int nLeafs=h->lumps[10].len/leafStride;
  printf("models=%d brushes=%d leafs=%d(stride %d) disp=%d\n",nModels,nBrushes,nLeafs,leafStride,nDisp);

  // -- world brush set: walk model 0's node tree, collect leafbrushes
  std::vector<unsigned char> isWorld(nBrushes,0);
  {
    std::vector<int> stack; stack.push_back(models[0].headnode);
    while(!stack.empty()){
      int nn=stack.back(); stack.pop_back();
      if(nn>=0){ stack.push_back(nodes[nn].children[0]); stack.push_back(nodes[nn].children[1]); }
      else {
        int li=-1-nn; if(li>=nLeafs) continue;
        unsigned char*L=leafs+li*leafStride;
        unsigned short flb=*(unsigned short*)(L+24);   // firstleafbrush
        unsigned short nlb=*(unsigned short*)(L+26);   // numleafbrushes
        for(int k=0;k<nlb;k++){ int b=leafbrushes[flb+k]; if(b<nBrushes) isWorld[b]=1; }
      }
    }
  }

  // -- solid world brushes. NOTE: we deliberately do NOT try to exclude
  // displacement base brushes by the brushside dispinfo field (its "none"
  // sentinel is unreliable across compilers). The engine's own collision set is
  // "brushes referenced by the world model's leaves" - exactly what we walk -
  // and the ground self-test below verifies the result against the nav's floor
  // heights before any cutting is allowed.
  int worldCount=0;
  for(int b=0;b<nBrushes;b++){
    if(!isWorld[b]) continue;
    if(!(brushes[b].contents & SOLID_MASK)) continue;
    CBrush cb; cb.firstSide=(int)g_sides.size(); cb.numSides=brushes[b].numsides;
    cb.lo=Vector(1e9f,1e9f,1e9f); cb.hi=Vector(-1e9f,-1e9f,-1e9f);
    for(int s=0;s<brushes[b].numsides;s++){
      const dplane_t&pl=planes[bsides[brushes[b].firstside+s].planenum];
      BrushSideP sp; sp.n=pl.n; sp.d=pl.d; g_sides.push_back(sp);
      // axial planes give the AABB
      if(pl.n.x> .999f){ if(pl.d<cb.hi.x||cb.hi.x<-1e8f)cb.hi.x=pl.d; }
      if(pl.n.x<-.999f){ float v=-pl.d; if(v>cb.lo.x||cb.lo.x>1e8f)cb.lo.x=v; }
      if(pl.n.y> .999f){ if(pl.d<cb.hi.y||cb.hi.y<-1e8f)cb.hi.y=pl.d; }
      if(pl.n.y<-.999f){ float v=-pl.d; if(v>cb.lo.y||cb.lo.y>1e8f)cb.lo.y=v; }
      if(pl.n.z> .999f){ if(pl.d<cb.hi.z||cb.hi.z<-1e8f)cb.hi.z=pl.d; }
      if(pl.n.z<-.999f){ float v=-pl.d; if(v>cb.lo.z||cb.lo.z>1e8f)cb.lo.z=v; }
    }
    if(cb.lo.x>cb.hi.x||cb.lo.y>cb.hi.y||cb.lo.z>cb.hi.z){ // degenerate: huge box
      cb.lo=Vector(-16384,-16384,-16384); cb.hi=Vector(16384,16384,16384);
    }
    g_brushes.push_back(cb); worldCount++;
  }
  printf("world solid brushes kept=%d of %d\n",worldCount,nBrushes);

  // -- displacement triangles
  int triCount=0;
  for(int i=0;i<nDisp;i++){
    unsigned char*di=dispinfo+i*176;
    Vector startPos=*(Vector*)(di+0);
    int vertStart=*(int*)(di+12);
    int power=*(int*)(di+20);
    unsigned short mapFace=*(unsigned short*)(di+36);
    const dface_t&f=faces[mapFace];
    if(f.numedges!=4) continue;
    Vector c[4];
    for(int e=0;e<4;e++){
      int se=surfedges[f.firstedge+e];
      c[e]= se>=0 ? verts[edges[se].v[0]] : verts[edges[-se].v[1]];
    }
    // rotate so c[0] is the disp start position
    int best=0; float bd=1e18f;
    for(int k=0;k<4;k++){ float dd=(c[k]-startPos).Length(); if(dd<bd){bd=dd;best=k;} }
    Vector q[4]; for(int k=0;k<4;k++) q[k]=c[(best+k)&3];
    int size=(1<<power)+1;
    std::vector<Vector> P(size*size);
    for(int y=0;y<size;y++)for(int x=0;x<size;x++){
      float u=(float)x/(size-1), v=(float)y/(size-1);
      Vector base = q[0]*((1-u)*(1-v)) + q[1]*(u*(1-v)) + q[2]*(u*v) + q[3]*((1-u)*v);
      const ddispvert_t&dv=dispverts[vertStart+y*size+x];
      P[y*size+x]= base + dv.vec*dv.dist;
    }
    for(int y=0;y<size-1;y++)for(int x=0;x<size-1;x++){
      Tri t1={P[y*size+x],P[y*size+x+1],P[(y+1)*size+x]};
      Tri t2={P[y*size+x+1],P[(y+1)*size+x+1],P[(y+1)*size+x]};
      g_tris.push_back(t1); g_tris.push_back(t2); triCount+=2;
    }
  }
  printf("displacement tris=%d\n",triCount);

  // -- grid
  for(int a=0;a<3;a++){ g_worldLo[a]=(&models[0].mins.x)[a]-64; g_worldHi[a]=(&models[0].maxs.x)[a]+64; }
  g_gx=(int)((g_worldHi[0]-g_worldLo[0])/CELL)+1;
  g_gy=(int)((g_worldHi[1]-g_worldLo[1])/CELL)+1;
  g_cellBrush.resize(g_gx*g_gy); g_cellTri.resize(g_gx*g_gy);
  for(int b=0;b<(int)g_brushes.size();b++) gridInsert(g_cellBrush,g_brushes[b].lo,g_brushes[b].hi,b);
  for(int t=0;t<(int)g_tris.size();t++){
    Vector lo=g_tris[t].a,hi=g_tris[t].a;
    const Vector*vv[3]={&g_tris[t].a,&g_tris[t].b,&g_tris[t].c};
    for(int k=1;k<3;k++){ const Vector&p=*vv[k];
      if(p.x<lo.x)lo.x=p.x; if(p.y<lo.y)lo.y=p.y; if(p.z<lo.z)lo.z=p.z;
      if(p.x>hi.x)hi.x=p.x; if(p.y>hi.y)hi.y=p.y; if(p.z>hi.z)hi.z=p.z; }
    gridInsert(g_cellTri,lo,hi,t);
  }
  printf("grid %dx%d cells\n",g_gx,g_gy);
  // note: 'd' stays allocated (verts/planes pointers no longer needed - we copied)
  return true;
}

// ---------------------------------------------------------------- tracing
static unsigned g_stamp=1;
static std::vector<unsigned> g_brushStamp, g_triStamp;

// swept AABB vs brushes; box given as feet-origin: lo=(-hw,-hw,0) hi=(hw,hw,hh)
struct TraceRes { float frac; bool startsolid; };

static TraceRes traceBrushes(const Vector&start,const Vector&end,float hw,float hh){
  TraceRes r; r.frac=1.0f; r.startsolid=false;
  float lo[3]={-hw,-hw,0}, hi[3]={hw,hw,hh};
  // gather cells along the swept box
  float sl[2]={ (start.x<end.x?start.x:end.x)-hw, (start.y<end.y?start.y:end.y)-hw };
  float sh[2]={ (start.x>end.x?start.x:end.x)+hw, (start.y>end.y?start.y:end.y)+hw };
  int cx0=(int)((sl[0]-g_worldLo[0])/CELL), cy0=(int)((sl[1]-g_worldLo[1])/CELL);
  int cx1=(int)((sh[0]-g_worldLo[0])/CELL), cy1=(int)((sh[1]-g_worldLo[1])/CELL);
  if(cx0<0)cx0=0; if(cy0<0)cy0=0; if(cx1>=g_gx)cx1=g_gx-1; if(cy1>=g_gy)cy1=g_gy-1;
  g_stamp++;
  if(g_brushStamp.size()<g_brushes.size()) g_brushStamp.resize(g_brushes.size(),0);
  float zl=(start.z<end.z?start.z:end.z)-4, zh=(start.z>end.z?start.z:end.z)+hh+4;
  for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++){
    const std::vector<int>&cell=g_cellBrush[cy*g_gx+cx];
    for(size_t ci=0;ci<cell.size();ci++){
      int b=cell[ci];
      if(g_brushStamp[b]==g_stamp) continue; g_brushStamp[b]=g_stamp;
      const CBrush&cb=g_brushes[b];
      if(cb.hi.z<zl||cb.lo.z>zh) continue;
      float enter=-1.0f, leave=2.0f; bool startout=false; bool skip=false;
      for(int s=0;s<cb.numSides;s++){
        const BrushSideP&sp=g_sides[cb.firstSide+s];
        // support offset: push plane out by box extent
        float ofs= (sp.n.x<0?hi[0]:lo[0])*sp.n.x
                 + (sp.n.y<0?hi[1]:lo[1])*sp.n.y
                 + (sp.n.z<0?hi[2]:lo[2])*sp.n.z;
        float dd=sp.d - ofs;
        float d1=dot(start,sp.n)-dd, d2=dot(end,sp.n)-dd;
        if(d1>0) startout=true;
        if(d1>0&&d2>=d1){skip=true;break;}           // fully in front, moving away
        if(d1<=0&&d2<=0) continue;                    // fully behind this plane
        if(d1>d2){ float f=(d1-0.03125f)/(d1-d2); if(f>enter)enter=f; }
        else     { float f=(d1+0.03125f)/(d1-d2); if(f<leave)leave=f; }
      }
      if(skip) continue;
      if(!startout){ r.startsolid=true; continue; }
      if(enter<leave && enter>-1.0f && enter<r.frac) r.frac= enter<0?0:enter;
    }
  }
  return r;
}

// ray vs displacement tris (Moller-Trumbore), several rays covering the hull
static float rayTris(const Vector&start,const Vector&end){
  float best=1.0f;
  Vector dir=end-start;
  float sl[2]={ (start.x<end.x?start.x:end.x)-1, (start.y<end.y?start.y:end.y)-1 };
  float sh[2]={ (start.x>end.x?start.x:end.x)+1, (start.y>end.y?start.y:end.y)+1 };
  int cx0=(int)((sl[0]-g_worldLo[0])/CELL), cy0=(int)((sl[1]-g_worldLo[1])/CELL);
  int cx1=(int)((sh[0]-g_worldLo[0])/CELL), cy1=(int)((sh[1]-g_worldLo[1])/CELL);
  if(cx0<0)cx0=0; if(cy0<0)cy0=0; if(cx1>=g_gx)cx1=g_gx-1; if(cy1>=g_gy)cy1=g_gy-1;
  g_stamp++;
  if(g_triStamp.size()<g_tris.size()) g_triStamp.resize(g_tris.size(),0);
  for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++){
    const std::vector<int>&cell=g_cellTri[cy*g_gx+cx];
    for(size_t ci=0;ci<cell.size();ci++){
      int t=cell[ci];
      if(g_triStamp[t]==g_stamp) continue; g_triStamp[t]=g_stamp;
      const Tri&T=g_tris[t];
      Vector e1=T.b-T.a, e2=T.c-T.a;
      Vector p=cross(dir,e2); float det=dot(e1,p);
      if(det>-1e-6f&&det<1e-6f) continue;
      float inv=1.0f/det;
      Vector tv=start-T.a;
      float u=dot(tv,p)*inv; if(u<-0.02f||u>1.02f) continue;
      Vector q=cross(tv,e1);
      float v=dot(dir,q)*inv; if(v<-0.02f||u+v>1.02f) continue;
      float f=dot(e2,q)*inv;
      if(f>0.0f&&f<best) best=f;
    }
  }
  return best;
}

static TraceRes traceHull(const Vector&start,const Vector&end,float hw,float hh){
  TraceRes r=traceBrushes(start,end,hw,hh);
  // displacement: rays from hull corners at foot + top, and center
  static const float cf[5][2]={{0,0},{1,1},{1,-1},{-1,1},{-1,-1}};
  for(int zi=0;zi<2;zi++){
    float z= zi? hh-2.0f : 2.0f;
    for(int k=0;k<5;k++){
      Vector o(cf[k][0]*(hw-1),cf[k][1]*(hw-1),z);
      float f=rayTris(start+o,end+o);
      if(f<r.frac) r.frac=f;
    }
  }
  return r;
}

static Vector lerpV(const Vector&a,const Vector&b,float t){ return a+(b-a)*t; }

// find ground z at (x,y) starting from zhint upward a bit; robust against the
// probe starting embedded in geometry (nav z a touch below the real floor)
static bool groundAt(float x,float y,float zhint,float&outZ){
  for(int lift=0;lift<4;lift++){
    Vector s(x,y,zhint+40+lift*20), e(x,y,zhint-240);
    TraceRes r=traceHull(s,e,14.0f,4.0f);
    if(r.startsolid) continue;              // started inside something: lift & retry
    if(r.frac>=1.0f) return false;
    outZ = s.z + (e.z-s.z)*r.frac;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- walk sim
// Simulate game movement from S to E: step-ups to 18, jump-ups to 50 when
// blocked, falls allowed to maxDrop below current. Returns true if E reached.
static bool walkSim(Vector S,Vector E,float hw,float hh,float maxDrop,bool allowJump){
  // settle S on ground
  float gz;
  if(groundAt(S.x,S.y,S.z,gz)) S.z=gz+1.0f;
  float best=xyd(S,E); int stall=0;
  Vector cur=S;
  for(int it=0;it<80;it++){
    float d=xyd(cur,E);
    if(d<26.0f && cur.z>E.z-maxDrop-40 && cur.z<E.z+90) return true;
    Vector dir=E-cur; dir.z=0;
    float L=sqrtf(dir.x*dir.x+dir.y*dir.y); if(L<1e-3f) return true;
    dir.x/=L; dir.y/=L;
    float step= L<14?L:14;

    // raise (step)
    float up=18.0f;
    TraceRes ur=traceHull(cur,cur+Vector(0,0,up),hw,hh);
    Vector raised=cur+Vector(0,0,up*ur.frac);
    // forward
    TraceRes fr=traceHull(raised,raised+dir*step,hw,hh);
    if(fr.frac<0.12f && allowJump){
      // try a jump-height climb
      TraceRes jr=traceHull(cur,cur+Vector(0,0,50),hw,hh);
      Vector jraised=cur+Vector(0,0,50*jr.frac);
      TraceRes jf=traceHull(jraised,jraised+dir*step,hw,hh);
      if(jf.frac>fr.frac){ raised=jraised; fr=jf; }
    }
    if(fr.frac<0.08f){
      // blocked; tiny sidestep to defeat corner-catching, else fail via stall
      Vector side(-dir.y,dir.x,0);
      TraceRes s1=traceHull(raised,raised+side*10.0f,hw,hh);
      if(s1.frac>0.5f) raised=raised+side*(10.0f*s1.frac);
      else{
        TraceRes s2=traceHull(raised,raised+side*-10.0f,hw,hh);
        if(s2.frac>0.5f) raised=raised+side*(-10.0f*s2.frac);
      }
    }
    Vector fwd=raised+dir*(step*fr.frac);
    // drop
    float dropLen=(raised.z-cur.z)+18.0f+maxDrop;
    TraceRes dr=traceHull(fwd,fwd+Vector(0,0,-dropLen),hw,hh);
    if(dr.frac>=1.0f) return false;              // walked off into a void
    cur=fwd+Vector(0,0,-dropLen*dr.frac+0.5f);

    float nb=xyd(cur,E);
    if(nb<best-2.0f){best=nb;stall=0;}
    else if(++stall>=5) return false;
  }
  return false;
}

// ---------------------------------------------------------------- validation
struct CutRec { unsigned fromId,toId; Vector fromC,toC; float distStuck; };

int main(int argc,char**argv){
  const char*bspPath=argc>1?argv[1]:"ins_baghdad.bsp";
  const char*navPath=argc>2?argv[2]:"ins_baghdad.nav";
  const char*outPath=argc>3?argv[3]:"ins_baghdad_repaired.nav";
  float focusX=argc>4?atof(argv[4]):-1300.0f, focusY=argc>5?atof(argv[5]):-420.0f;
  // manual cuts: pairs of area IDs from argv[6..], each cut in BOTH directions
  std::vector<unsigned> manualIds;
  for(int a=6;a<argc;a++) manualIds.push_back((unsigned)strtoul(argv[a],0,10));

  if(!loadBsp(bspPath)) return 1;
  CNavMesh m;
  if(!m.Load(navPath)){printf("FAIL nav load\n");return 1;}
  printf("nav areas=%d\n",m.Count());

  // ---- per-area ground trust: only validate connections whose endpoints our
  // collision model demonstrably understands (ground probe matches the nav's
  // floor height). Areas on geometry we can't see (static props, entity
  // brushes) are left alone - their links are KEPT, never cut.
  std::vector<unsigned char> trust(m.Count(),0);
  {
    int ok=0,bad=0,fail=0; float errSum=0; int shown=0;
    for(int i=0;i<m.Count();i++){
      const NavAreaData*a=m.Area(i);
      float gz;
      if(groundAt(a->center.x,a->center.y,a->center.z+20,gz)){
        float e=fabsf(gz-a->center.z);
        errSum+=e;
        if(e<26){ok++;trust[i]=1;}
        else{
          bad++;
          if(shown<12){
            printf("  mismatch: area %u (%.0f %.0f %.0f) my_ground=%.0f (delta %+.0f)\n",
              a->id,a->center.x,a->center.y,a->center.z,gz,gz-a->center.z);
            shown++;
          }
        }
      } else fail++;
    }
    printf("ground trust: trusted=%d off=%d no-ground=%d avg_err=%.1f (%.1f%% trusted)\n",
      ok,bad,fail,errSum/(ok+bad>0?ok+bad:1),100.0f*ok/m.Count());
    if(ok < m.Count()*60/100){ printf("ABORT: too little of the map is understood - not safe to cut\n"); return 2; }
  }

  // ---- validate every connection ----
  std::vector<CutRec> cuts;
  int total=0, tested=0, skipped=0;
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    for(int k=0;k<a->connCount;k++){
      int j=a->connIdx[k]; const NavAreaData*b=m.Area(j); if(!b)continue;
      total++;
      if(!trust[i]||!trust[j]){skipped++;continue;}   // unknown collision -> keep
      // portal overlap rect
      float lx=a->lo.x>b->lo.x?a->lo.x:b->lo.x, hx=a->hi.x<b->hi.x?a->hi.x:b->hi.x;
      float ly=a->lo.y>b->lo.y?a->lo.y:b->lo.y, hy=a->hi.y<b->hi.y?a->hi.y:b->hi.y;
      bool adjacent = !(lx>hx+24.0f||ly>hy+24.0f);
      float drop = a->center.z - b->center.z;
      float maxDrop = drop>0 ? drop+120.0f : 90.0f;
      bool crouchA=(a->attributes&NAVATTR_CROUCH)!=0, crouchB=(b->attributes&NAVATTR_CROUCH)!=0;
      bool passed=false;
      tested++;
      static const float TS[3]={0.5f,0.2f,0.8f};
      for(int s=0;s<3&&!passed;s++){
        Vector S,E;
        if(adjacent){
          // sample along the shared overlap span
          float px = lx==hx ? lx : lx+(hx-lx)*TS[s];
          float py = ly==hy ? ly : ly+(hy-ly)*TS[s];
          if(hx-lx<0.1f&&hy-ly<0.1f&&s>0) break;   // corner touch: only one sample
          // S inside A, E inside B, 20 units off the portal
          float ax=px,ay=py,bx=px,by=py;
          float amx=(a->lo.x+a->hi.x)*0.5f, amy=(a->lo.y+a->hi.y)*0.5f;
          float bmx=(b->lo.x+b->hi.x)*0.5f, bmy=(b->lo.y+b->hi.y)*0.5f;
          float dax=amx-px,day=amy-py; float al=sqrtf(dax*dax+day*day); if(al>1){dax/=al;day/=al;}
          float dbx=bmx-px,dby=bmy-py; float bl=sqrtf(dbx*dbx+dby*dby); if(bl>1){dbx/=bl;dby/=bl;}
          float ain=al<20?al:20, bin=bl<20?bl:20;
          ax=px+dax*ain; ay=py+day*ain; bx=px+dbx*bin; by=py+dby*bin;
          S=Vector(ax,ay,m.GetZ(i,ax,ay)+2);
          E=Vector(bx,by,m.GetZ(j,bx,by)+2);
        } else {
          if(s>0) break;   // drop links: single attempt
          float bx=b->center.x,by=b->center.y;
          float cx=bx<a->lo.x?a->lo.x:(bx>a->hi.x?a->hi.x:bx);
          float cy=by<a->lo.y?a->lo.y:(by>a->hi.y?a->hi.y:by);
          S=Vector(cx,cy,m.GetZ(i,cx,cy)+2);
          E=Vector(bx,by,m.GetZ(j,bx,by)+2);
        }
        // stand, then crouch
        if(!crouchA&&!crouchB)
          passed=walkSim(S,E,15.0f,70.0f,maxDrop,true);
        if(!passed)
          passed=walkSim(S,E,15.0f,34.0f,maxDrop,true);
      }
      if(!passed){
        CutRec c; c.fromId=a->id; c.toId=b->id; c.fromC=a->center; c.toC=b->center;
        c.distStuck=xyd(Vector(a->center.x,a->center.y,0),Vector(focusX,focusY,0));
        cuts.push_back(c);
      }
    }
    if(i%1000==0){ printf("  ...area %d/%d (cuts so far %d)\n",i,m.Count(),(int)cuts.size()); fflush(stdout); }
  }
  printf("\nconnections=%d  tested=%d  kept-untrusted=%d  impassable=%d (%.2f%% of tested)\n",
    total,tested,skipped,(int)cuts.size(),100.0f*cuts.size()/(tested?tested:1));

  // sort by distance from the focus point (user's stuck spot first)
  for(size_t i=0;i<cuts.size();i++)for(size_t j=i+1;j<cuts.size();j++)
    if(cuts[j].distStuck<cuts[i].distStuck){CutRec t=cuts[i];cuts[i]=cuts[j];cuts[j]=t;}
  printf("\n=== impassable connections (nearest to your stuck spot first) ===\n");
  for(size_t i=0;i<cuts.size()&&i<60;i++){
    CutRec&c=cuts[i];
    printf("  %u(%.0f %.0f %.0f) -> %u(%.0f %.0f %.0f)   [%.0f from focus]\n",
      c.fromId,c.fromC.x,c.fromC.y,c.fromC.z,c.toId,c.toC.x,c.toC.y,c.toC.z,c.distStuck);
  }
  if(cuts.size()>60) printf("  ... and %d more\n",(int)cuts.size()-60);

  float ratio=100.0f*cuts.size()/(total?total:1);
  if(ratio>4.0f)
    printf("\nWARNING: cut ratio %.1f%% is high - review before trusting!\n",ratio);

  // ---- focus diagnostics: exactly what the sim thinks near the stuck spot ----
  printf("\n=== focus zone detail (within 260 of %.0f,%.0f) ===\n",focusX,focusY);
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    if(xyd(Vector(a->center.x,a->center.y,0),Vector(focusX,focusY,0))>260) continue;
    printf("area %u (%.0f %.0f %.0f) z[%.0f..%.0f] trust=%d\n",
      a->id,a->center.x,a->center.y,a->center.z,a->neZ,a->swZ,(int)trust[i]);
    for(int k=0;k<a->connCount;k++){
      int j=a->connIdx[k]; const NavAreaData*b=m.Area(j); if(!b)continue;
      float lx=a->lo.x>b->lo.x?a->lo.x:b->lo.x, hx=a->hi.x<b->hi.x?a->hi.x:b->hi.x;
      float ly=a->lo.y>b->lo.y?a->lo.y:b->lo.y, hy=a->hi.y<b->hi.y?a->hi.y:b->hi.y;
      bool adjacent=!(lx>hx+24.0f||ly>hy+24.0f);
      float drop=a->center.z-b->center.z;
      float maxDrop=drop>0?drop+120.0f:90.0f;
      // single midpoint sample, all hull variants, verbose
      float px=lx==hx?lx:(lx+hx)*0.5f, py=ly==hy?ly:(ly+hy)*0.5f;
      float amx=(a->lo.x+a->hi.x)*0.5f, amy=(a->lo.y+a->hi.y)*0.5f;
      float bmx=(b->lo.x+b->hi.x)*0.5f, bmy=(b->lo.y+b->hi.y)*0.5f;
      float dax=amx-px,day=amy-py,al=sqrtf(dax*dax+day*day); if(al>1){dax/=al;day/=al;}
      float dbx=bmx-px,dby=bmy-py,bl=sqrtf(dbx*dbx+dby*dby); if(bl>1){dbx/=bl;dby/=bl;}
      Vector S(px+dax*(al<20?al:20),py+day*(al<20?al:20),0);
      Vector E(px+dbx*(bl<20?bl:20),py+dby*(bl<20?bl:20),0);
      S.z=m.GetZ(i,S.x,S.y)+2; E.z=m.GetZ(j,E.x,E.y)+2;
      bool st=walkSim(S,E,15.0f,70.0f,maxDrop,true);
      bool cr=st?true:walkSim(S,E,15.0f,34.0f,maxDrop,true);
      bool wide=walkSim(S,E,16.4f,70.0f,maxDrop,true);
      bool wideCr=wide?true:walkSim(S,E,16.4f,35.4f,maxDrop,true);
      printf("   -> %u (%.0f %.0f %.0f)  adj=%d  slim: stand=%d crouch=%d | REAL-size: stand=%d crouch=%d%s\n",
        b->id,b->center.x,b->center.y,b->center.z,(int)adjacent,
        (int)st,(int)cr,(int)wide,(int)wideCr,
        (cr&&!wideCr)?"   <== passes slim hull only: TOO TIGHT for a real player":"");
    }
  }

  // ---- manual cuts (user-identified impassable pairs, both directions) ----
  for(size_t mi=0;mi+1<manualIds.size();mi+=2){
    for(int dirn=0;dirn<2;dirn++){
      unsigned f=dirn?manualIds[mi+1]:manualIds[mi];
      unsigned t=dirn?manualIds[mi]:manualIds[mi+1];
      // only add if that directed link actually exists
      bool exists=false; Vector fc,tc;
      for(int i=0;i<m.Count()&&!exists;i++){
        const NavAreaData*a=m.Area(i);
        if(a->id!=f) continue;
        fc=a->center;
        for(int k=0;k<a->connCount;k++){
          const NavAreaData*b=m.Area(a->connIdx[k]);
          if(b&&b->id==t){exists=true;tc=b->center;break;}
        }
      }
      if(exists){
        CutRec c; c.fromId=f; c.toId=t; c.fromC=fc; c.toC=tc; c.distStuck=0;
        cuts.push_back(c);
        printf("manual cut: %u -> %u\n",f,t);
      } else printf("manual cut %u -> %u: link not found (skipped)\n",f,t);
    }
  }

  // ---- rewrite the nav with cuts removed ----
  if(cuts.empty()){ printf("nothing to cut - nav already clean\n"); return 0; }

  FILE*fp=fopen(navPath,"rb");
  fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
  unsigned char*src=(unsigned char*)malloc(sz);
  if(fread(src,1,sz,fp)!=(size_t)sz){printf("re-read fail\n");return 1;}
  fclose(fp);
  std::vector<unsigned char> out; out.reserve(sz);

  // cut lookup
  struct Pair{unsigned f,t;};
  std::vector<Pair> cutPairs;
  for(size_t i=0;i<cuts.size();i++){Pair p={cuts[i].fromId,cuts[i].toId};cutPairs.push_back(p);}
  #define IS_CUT(F,T) ({bool r=false;for(size_t q=0;q<cutPairs.size();q++)if(cutPairs[q].f==(F)&&cutPairs[q].t==(T)){r=true;break;}r;})

  unsigned pos=0;
  #define RD32() (*(unsigned*)(src+pos)); pos+=4
  #define RD16() (*(unsigned short*)(src+pos)); pos+=2
  #define RD8()  (src[pos]); pos+=1
  #define CP(n)  do{ out.insert(out.end(),src+pos,src+pos+(n)); pos+=(n);}while(0)
  #define W32(v) do{ unsigned _v=(v); out.insert(out.end(),(unsigned char*)&_v,(unsigned char*)&_v+4);}while(0)

  CP(4);                      // magic
  unsigned ver=*(unsigned*)(src+pos); CP(4);   // version
  if(ver!=9){printf("rewriter expects v9, got %u\n",ver);return 1;}
  CP(4);                      // bsp size
  unsigned short pc=*(unsigned short*)(src+pos); CP(2);   // place count
  for(int i=0;i<pc;i++){ unsigned short l=*(unsigned short*)(src+pos); CP(2); CP(l); }
  unsigned areaCount=*(unsigned*)(src+pos); CP(4);
  int removed=0;
  for(unsigned ai=0;ai<areaCount;ai++){
    unsigned id=*(unsigned*)(src+pos); CP(4);
    CP(2);                    // attributes (v9: u16)
    CP(4*6+4+4);              // lo,hi (6 floats) + neZ + swZ
    for(int d=0;d<4;d++){
      unsigned cnt=*(unsigned*)(src+pos); pos+=4;
      // gather surviving ids
      std::vector<unsigned> keep;
      for(unsigned k=0;k<cnt;k++){
        unsigned to=*(unsigned*)(src+pos); pos+=4;
        if(IS_CUT(id,to)) removed++;
        else keep.push_back(to);
      }
      W32((unsigned)keep.size());
      for(size_t k2=0;k2<keep.size();k2++) W32(keep[k2]);
    }
    unsigned char hides=src[pos]; CP(1); CP(hides*(4+12+1));
    unsigned char app=src[pos]; CP(1); CP(app*(4+4+1+4+1));
    unsigned enc=*(unsigned*)(src+pos); CP(4);
    for(unsigned e=0;e<enc;e++){ CP(4+1+4+1); unsigned char sp=src[pos]; CP(1); CP(sp*(4+1)); }
    CP(2);                    // place
    for(int d=0;d<2;d++){ unsigned lc=*(unsigned*)(src+pos); CP(4); CP(lc*4); }
    CP(8);                    // occupy times
  }
  CP((unsigned)(sz-pos));     // remainder (mesh ladders etc) verbatim
  printf("rewrote: %d directed links removed\n",removed);

  FILE*fo=fopen(outPath,"wb");
  fwrite(&out[0],1,out.size(),fo); fclose(fo);
  printf("wrote %s (%d bytes, was %ld)\n",outPath,(int)out.size(),sz);

  // ---- verify: reload repaired, compare, A* sanity ----
  CNavMesh m2;
  if(!m2.Load(outPath)){printf("VERIFY FAIL: repaired nav did not parse!\n");return 3;}
  int c1=0,c2=0;
  for(int i=0;i<m.Count();i++) c1+=m.Area(i)->connCount;
  for(int i=0;i<m2.Count();i++) c2+=m2.Area(i)->connCount;
  printf("verify: areas %d->%d, conns %d->%d (expected -%d)\n",
    m.Count(),m2.Count(),c1,c2,removed);

  unsigned rng=12345; int okOrig=0,okRep=0,tests=60;
  int path[512];
  for(int t=0;t<tests;t++){
    rng=rng*1103515245+12345;
    int s=(rng>>8)%m.Count();
    rng=rng*1103515245+12345;
    int g=(rng>>8)%m.Count();
    if(m.BuildPath(s,g,path,512)>0) okOrig++;
    if(m2.BuildPath(s,g,path,512)>0) okRep++;
  }
  printf("A* sanity: original %d/%d, repaired %d/%d\n",okOrig,tests,okRep,tests);
  if(okRep<okOrig-2) printf("WARNING: repaired mesh lost connectivity!\n");
  else printf("connectivity preserved.\n");
  return 0;
}
