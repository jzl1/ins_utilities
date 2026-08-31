// native test harness: synthesize a v9 nav file, parse it, run A*
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
struct Vector { float x,y,z; Vector(){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
  float Length()const{return sqrtf(x*x+y*y+z*z);} };
#define TESTING_NATIVE 1
#include "../src/nav.h"
#include "../src/nav.cpp"

static void put32(FILE*f,unsigned v){fwrite(&v,4,1,f);} 
static void put16(FILE*f,unsigned short v){fwrite(&v,2,1,f);} 
static void put8(FILE*f,unsigned char v){fwrite(&v,1,1,f);} 
static void putf(FILE*f,float v){fwrite(&v,4,1,f);} 

int main(){
  // build a 3x3 grid of 100x100 areas, 4-connected
  FILE*f=fopen("/tmp/test.nav","wb");
  put32(f,0xFEEDFACE); put32(f,9); put32(f,12345); // magic, version, bspsize
  put16(f,1); const char*pn="TestPlace"; put16(f,(unsigned short)(strlen(pn)+1)); fwrite(pn,1,strlen(pn)+1,f); // place dir
  put32(f,9); // area count
  for(int gy=0;gy<3;gy++)for(int gx=0;gx<3;gx++){
    unsigned id=gy*3+gx+1;
    put32(f,id); put16(f,(id==5)?0x01:0); // area 5 crouch
    putf(f,gx*100.f); putf(f,gy*100.f); putf(f,0.f);          // lo
    putf(f,gx*100.f+100.f); putf(f,gy*100.f+100.f); putf(f,0.f); // hi
    putf(f,0.f); putf(f,0.f);                                  // neZ swZ
    // connections NORTH(gy-1), EAST(gx+1), SOUTH(gy+1), WEST(gx-1)
    int cn[4][2]={{0,-1},{1,0},{0,1},{-1,0}};
    for(int d=0;d<4;d++){
      int nx=gx+cn[d][0], ny=gy+cn[d][1];
      if(nx>=0&&nx<3&&ny>=0&&ny<3){put32(f,1);put32(f,ny*3+nx+1);}else put32(f,0);
    }
    put8(f,1); put32(f,77); putf(f,5);putf(f,5);putf(f,0); put8(f,3); // 1 hiding spot (id,pos,flags)
    put8(f,1); put32(f,1);put32(f,2);put8(f,0);put32(f,3);put8(f,1);  // 1 approach area
    put32(f,1); put32(f,1);put8(f,0);put32(f,2);put8(f,1); put8(f,2); // 1 encounter, 2 spots
      put32(f,77);put8(f,128); put32(f,77);put8(f,200);
    put16(f,1); // place index
    put32(f,0); put32(f,0); // ladder connects both dirs
    putf(f,1.f); putf(f,2.f); // occupy times
  }
  put32(f,0); // ladder count
  fclose(f);

  CNavMesh m;
  if(!m.Load("/tmp/test.nav")){printf("FAIL: load\n");return 1;}
  printf("loaded %d areas\n",m.Count());
  if(m.Count()!=9){printf("FAIL: count\n");return 1;}
  int s=m.FindArea(Vector(10,10,0));       // area 1 (0,0)
  int g=m.FindArea(Vector(290,290,0));     // area 9 (2,2)
  printf("start=%d goal=%d\n",s,g);
  int path[64]; int n=m.BuildPath(s,g,path,64);
  printf("path len=%d:",n); for(int i=0;i<n;i++)printf(" %d(id=%u)",path[i],m.Area(path[i])->id); printf("\n");
  if(n<5){printf("FAIL: path\n");return 1;}
  // crouch attr survives?
  int mid=m.FindArea(Vector(150,150,0));
  printf("mid area attr=%x\n",m.Area(mid)->attributes);
  printf("ALL OK\n");
  return 0;
}
