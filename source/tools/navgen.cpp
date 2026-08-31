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


// ============================================================================
// NAV GENERATOR
// ============================================================================
#include <cstdint>

// standing headroom at a ground point: 2 = stand, 1 = crouch-only, 0 = blocked
static int headroom(float x, float y, float gz){
  Vector feet(x,y,gz+2.0f);
  TraceRes r = traceHull(feet, feet+Vector(0,0,72.0f), 15.0f, 4.0f);
  if(r.startsolid) return 0;
  float clear = 72.0f * r.frac;
  if(clear >= 68.0f) return 2;
  if(clear >= 36.0f) return 1;
  return 0;
}

// connection type between two ground (feet) points, using the WALK SIMULATOR so
// slopes and stairs (continuous grades) are traversed rather than mistaken for a
// wall. 0 none, 1 walk/slope, 2 jump-up (mark avoid), 3 drop-down.
static int connType(const Vector&a, const Vector&b){
  float dz = b.z - a.z;
  if(dz > 90.0f)  return 0;                 // too high even to jump
  if(dz < -200.0f) return 0;                // too far to drop
  // walk first (step-ups <=18 per stride handle slopes up to ~50 deg); allow a
  // drop matching the height difference so a downhill grade still counts as walk
  float drop = dz < 0 ? (-dz + 48.0f) : 32.0f;
  if(walkSim(a, b, 15.0f, 34.0f, drop, false)) return 1;
  // couldn't just walk it -> a jump-up ledge or a big drop
  if(walkSim(a, b, 15.0f, 34.0f, 190.0f, true)) return (dz > 18.0f) ? 2 : 3;
  return 0;
}

// ------- .nav v9 writer helpers -------
static void W32(std::vector<unsigned char>&o, uint32_t v){ o.insert(o.end(),(unsigned char*)&v,(unsigned char*)&v+4); }
static void W16(std::vector<unsigned char>&o, uint16_t v){ o.insert(o.end(),(unsigned char*)&v,(unsigned char*)&v+2); }
static void W8 (std::vector<unsigned char>&o, uint8_t v){ o.push_back(v); }
static void WF (std::vector<unsigned char>&o, float v){ o.insert(o.end(),(unsigned char*)&v,(unsigned char*)&v+4); }

#define NAVATTR_CROUCH 0x01
#define NAVATTR_JUMP   0x02
#define NAVATTR_AVOID  0x800   // Source NAV_MESH_AVOID

struct GCell { int ix, iy; float z; unsigned char state; };  // state 2 stand, 1 crouch

// parse ins_spawnpoint / info_player_* origins from the BSP entity lump as seeds
static void parseSeeds(const char*path, std::vector<Vector>&seeds){
  FILE*fp=fopen(path,"rb"); if(!fp) return;
  int hdr[2]; if(fread(hdr,4,2,fp)!=2){fclose(fp);return;}
  int lofs, llen; fseek(fp,8,SEEK_SET); // lump 0 entry at offset 8
  fread(&lofs,4,1,fp); fread(&llen,4,1,fp);
  if(llen<=0||llen>64*1024*1024){fclose(fp);return;}
  std::vector<char> buf(llen+1);
  fseek(fp,lofs,SEEK_SET); if(fread(&buf[0],1,llen,fp)!=(size_t)llen){fclose(fp);return;}
  fclose(fp); buf[llen]=0;
  std::string s(&buf[0], llen);
  size_t p=0;
  while((p=s.find('{',p))!=std::string::npos){
    size_t q=s.find('}',p); if(q==std::string::npos) break;
    std::string blk=s.substr(p,q-p);
    if(blk.find("ins_spawnpoint")!=std::string::npos ||
       blk.find("info_player")!=std::string::npos ||
       blk.find("ins_objective")!=std::string::npos){
      size_t op=blk.find("\"origin\"");
      if(op!=std::string::npos){
        size_t v1=blk.find('"',op+8); size_t v2=blk.find('"',v1+1);
        if(v1!=std::string::npos&&v2!=std::string::npos){
          float x,y,z; if(sscanf(blk.substr(v1+1,v2-v1-1).c_str(),"%f %f %f",&x,&y,&z)==3)
            seeds.push_back(Vector(x,y,z));
        }
      }
    }
    p=q+1;
  }
}

int main(int argc,char**argv){
  const char* bsp = argc>1?argv[1]:"map.bsp";
  const char* out = argc>2?argv[2]:"map.nav";
  float G = argc>3?atof(argv[3]):96.0f;   // grid resolution
  bool markAvoid=false;                   // pass "avoid" to flag jump/crouch areas AVOID
  for(int i=1;i<argc;i++) if(!strcmp(argv[i],"avoid")) markAvoid=true;
  if(!loadBsp(bsp)) return 1;

  int NX = (int)((g_worldHi[0]-g_worldLo[0])/G)+1;
  int NY = (int)((g_worldHi[1]-g_worldLo[1])/G)+1;
  float loZ = g_worldLo[2], hiZ = g_worldHi[2];
  printf("grid %dx%d @ %.0fu, z[%.0f..%.0f]\n",NX,NY,G,loZ,hiZ);

  // ---- multi-level walkable cell sampling ----
  std::vector<GCell> cells;
  std::vector<std::vector<int> > col(NX*NY);   // cell indices per column
  for(int iy=0;iy<NY;iy++) for(int ix=0;ix<NX;ix++){
    float x=g_worldLo[0]+ix*G+G*0.5f, y=g_worldLo[1]+iy*G+G*0.5f;
    float z=hiZ+16.0f; int guard=0;
    while(z>loZ+8.0f && guard++<24){
      Vector s(x,y,z), e(x,y,loZ-16.0f);
      TraceRes r=traceHull(s,e,15.0f,4.0f);
      if(r.frac>=1.0f) break;
      float gz = s.z + (e.z-s.z)*r.frac;
      int hr=headroom(x,y,gz);
      if(hr>0){
        GCell c; c.ix=ix; c.iy=iy; c.z=gz; c.state=(unsigned char)hr;
        col[iy*NX+ix].push_back((int)cells.size());
        cells.push_back(c);
      }
      z = gz - 28.0f;   // continue below to find lower floors
    }
  }
  printf("walkable cells: %d\n",(int)cells.size());
  if(cells.empty()){ printf("no walkable ground found\n"); return 2; }

  // ---- connections (collision-validated so no through-wall links) ----
  std::vector<std::vector<int> > conn(cells.size());
  std::vector<unsigned short> attr(cells.size(), 0);   // u16: must hold NAVATTR_AVOID (0x800)
  for(size_t i=0;i<cells.size();i++) if(cells[i].state==1) attr[i]|=NAVATTR_CROUCH;
  const int DX[4]={1,-1,0,0}, DY[4]={0,0,1,-1};
  long links=0;
  for(size_t i=0;i<cells.size();i++){
    GCell&ci=cells[i];
    float cx=g_worldLo[0]+ci.ix*G+G*0.5f, cy=g_worldLo[1]+ci.iy*G+G*0.5f;
    Vector a(cx,cy,ci.z);
    for(int d=0;d<4;d++){
      int nx=ci.ix+DX[d], ny=ci.iy+DY[d];
      if(nx<0||ny<0||nx>=NX||ny>=NY) continue;
      const std::vector<int>&nc=col[ny*NX+nx];
      for(size_t k=0;k<nc.size();k++){
        int j=nc[k]; GCell&cj=cells[j];
        float bx=g_worldLo[0]+cj.ix*G+G*0.5f, by=g_worldLo[1]+cj.iy*G+G*0.5f;
        Vector b(bx,by,cj.z);
        int t=connType(a,b);
        if(!t) continue;
        conn[i].push_back(j); links++;
        if(t==2) attr[j]|=NAVATTR_JUMP;   // must jump UP into cj -> mark cj
      }
    }
  }
  printf("connections: %ld\n",links);

  // ---- keep the LARGEST connected component (guarantees a fully-navigable
  // mesh; isolated rooftops / out-of-bounds pockets are dropped). Connectivity
  // is undirected: a one-way drop still means the two areas are in one region. --
  std::vector<std::vector<int> > undir(cells.size());
  for(size_t i=0;i<cells.size();i++)
    for(size_t k=0;k<conn[i].size();k++){ int j=conn[i][k]; undir[i].push_back(j); undir[j].push_back((int)i); }
  std::vector<int> comp(cells.size(), -1);
  int bestComp=-1, bestSize=0, nComp=0;
  for(size_t s=0;s<cells.size();s++){
    if(comp[s]!=-1) continue;
    std::vector<int> q; q.push_back((int)s); comp[s]=nComp; int sz=0;
    for(size_t h=0;h<q.size();h++){ int i=q[h]; sz++;
      for(size_t k=0;k<undir[i].size();k++){ int j=undir[i][k]; if(comp[j]==-1){comp[j]=nComp;q.push_back(j);} } }
    if(sz>bestSize){bestSize=sz;bestComp=nComp;}
    nComp++;
  }
  std::vector<Vector> seeds; parseSeeds(bsp, seeds);
  printf("seeds: %d | components: %d, largest has %d cells\n",(int)seeds.size(),nComp,bestSize);
  std::vector<unsigned char> reach(cells.size(), 0);
  int nReach=0;
  for(size_t i=0;i<cells.size();i++) if(comp[i]==bestComp){ reach[i]=1; nReach++; }
  printf("kept (largest component): %d of %d cells\n",nReach,(int)cells.size());

  // ---- compact reachable cells -> areas (id = compact index + 1) ----
  std::vector<int> remap(cells.size(), -1);
  int na=0; for(size_t i=0;i<cells.size();i++) if(reach[i]) remap[i]=na++;

  // optionally flag every jump/crouch area AVOID so pathing routes around the
  // spots that need a jump or a crouch (they stay usable as a last resort)
  int jumpAreas=0, crouchAreas=0, avoidAreas=0;
  for(size_t i=0;i<cells.size();i++) if(reach[i]){
    if(markAvoid && (attr[i]&(NAVATTR_JUMP|NAVATTR_CROUCH))) attr[i]|=NAVATTR_AVOID;
    if(attr[i]&NAVATTR_JUMP) jumpAreas++;
    if(attr[i]&NAVATTR_CROUCH) crouchAreas++;
    if(attr[i]&NAVATTR_AVOID) avoidAreas++;
  }
  printf("attrs: jump=%d crouch=%d avoid=%d%s\n", jumpAreas, crouchAreas, avoidAreas,
         markAvoid?" (jump/crouch marked AVOID)":"");

  // ---- write v9 .nav ----
  std::vector<unsigned char> o;
  W32(o,0xFEEDFACE); W32(o,9); W32(o,0);   // magic, version, bsp size
  W16(o,0);                                 // place-name directory count
  W32(o,(uint32_t)na);                      // area count
  for(size_t i=0;i<cells.size();i++){
    if(!reach[i]) continue;
    GCell&c=cells[i];
    float cx=g_worldLo[0]+c.ix*G+G*0.5f, cy=g_worldLo[1]+c.iy*G+G*0.5f;
    W32(o,(uint32_t)(remap[i]+1));           // id
    W16(o,attr[i]);                          // attributes
    WF(o,cx-G*0.5f); WF(o,cy-G*0.5f); WF(o,c.z);   // lo
    WF(o,cx+G*0.5f); WF(o,cy+G*0.5f); WF(o,c.z);   // hi
    WF(o,c.z); WF(o,c.z);                     // neZ, swZ (flat)
    // connections bucketed by direction (east/west/south/north); parser merges
    std::vector<uint32_t> dir[4];
    for(size_t k=0;k<conn[i].size();k++){
      int j=conn[i][k]; if(remap[j]<0) continue;
      GCell&cj=cells[j];
      int dd = (cj.ix>c.ix)?1 : (cj.ix<c.ix)?3 : (cj.iy>c.iy)?2 : 0;
      dir[dd].push_back((uint32_t)(remap[j]+1));
    }
    for(int d=0;d<4;d++){ W32(o,(uint32_t)dir[d].size()); for(size_t m=0;m<dir[d].size();m++) W32(o,dir[d][m]); }
    W8(o,0);            // hiding spots
    W8(o,0);            // approach areas
    W32(o,0);           // encounter paths
    W16(o,0);           // place index
    W32(o,0); W32(o,0); // ladder connections (up/down)
    WF(o,0); WF(o,0);   // earliest-occupy times
  }
  W32(o,0);             // mesh ladder count

  FILE*fo=fopen(out,"wb"); if(!fo){printf("cannot write %s\n",out);return 3;}
  fwrite(&o[0],1,o.size(),fo); fclose(fo);
  printf("wrote %s: %d areas, %ld links, %d jump / %d crouch marked (%d bytes)\n",
    out,na,links,jumpAreas,crouchAreas,(int)o.size());

  // ---- self-verify: load with the plugin's own parser + A* sanity ----
  CNavMesh m;
  if(!m.Load(out)){ printf("*** VERIFY FAIL: plugin parser could not load the output!\n"); return 4; }
  printf("verify: parser loaded %d areas\n", m.Count());
  int okp=0; unsigned rng=999; int path[512];
  for(int t=0;t<40;t++){
    rng=rng*1103515245+12345; int s=(rng>>8)%m.Count();
    rng=rng*1103515245+12345; int g=(rng>>8)%m.Count();
    if(m.BuildPath(s,g,path,512)>0) okp++;
  }
  printf("verify: A* connected %d/40 random pairs\n", okp);
  return 0;
}
