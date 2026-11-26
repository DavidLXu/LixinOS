#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <mujoco/mujoco.h>

/* Win3 presents RGB565. Render depth-tested geometry to a Mesa pbuffer and
 * transfer the viewport to the window framebuffer, leaving the controls native. */
typedef struct { float x,y,z; } Point3;
typedef struct { float clip[4],rgb[3]; } Vertex3;
typedef struct {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
    GLuint program;
    int width,height,selected;
    unsigned char *rgba;
    mjvScene scene;
    mjvOption options;
    mjvCamera camera;
    mjvGLCamera eye;
    Point3 right,up,forward,origin;
    float sx,sy,za,zb;
} View3D;

static Point3 pt3(float x,float y,float z) { return (Point3){x,y,z}; }
static Point3 p3add(Point3 a,Point3 b) { return pt3(a.x+b.x,a.y+b.y,a.z+b.z); }
static Point3 p3sub(Point3 a,Point3 b) { return pt3(a.x-b.x,a.y-b.y,a.z-b.z); }
static float p3dot(Point3 a,Point3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static Point3 p3cross(Point3 a,Point3 b) { return pt3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x); }
static Point3 p3array(const float *a) { return pt3(a[0],a[1],a[2]); }

static void home3d(View3D *v) {
    mjv_defaultCamera(&v->camera);
    v->camera.azimuth=47; v->camera.elevation=-14;
    v->camera.distance=5.35;
}
static void close3d(View3D *v) {
    if (v->display!=EGL_NO_DISPLAY) {
        if (v->program) glDeleteProgram(v->program);
        eglMakeCurrent(v->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (v->surface) eglDestroySurface(v->display,v->surface);
        if (v->context) eglDestroyContext(v->display,v->context);
        eglTerminate(v->display);
    }
    mjv_freeScene(&v->scene);
    free(v->rgba);
    *v=(View3D){0};
}
static int resize3d(View3D *v,int width,int height) {
    if (width<=0 || height<=0) return 0;
    if (width==v->width && height==v->height) return 1;
    const EGLint attrs[]={EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    unsigned char *pixels=malloc((size_t)width*height*4);
    EGLSurface next=eglCreatePbufferSurface(v->display,v->config,attrs);
    if (!pixels || !next || !eglMakeCurrent(v->display,next,next,v->context)) {
        if (next) eglDestroySurface(v->display,next);
        free(pixels); return 0;
    }
    if (v->surface) eglDestroySurface(v->display,v->surface);
    free(v->rgba); v->rgba=pixels;
    v->surface=next; v->width=width; v->height=height;
    return 1;
}
static GLuint shader3d(GLenum kind,const char *source) {
    GLuint handle=glCreateShader(kind);
    glShaderSource(handle,1,&source,NULL); glCompileShader(handle);
    GLint ok=0;
    glGetShaderiv(handle,GL_COMPILE_STATUS,&ok);
    if (!ok) { glDeleteShader(handle); return 0; }
    return handle;
}
static int open3d(View3D *v,const mjModel *m,int w,int h) {
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,16,EGL_NONE};
    const EGLint version[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLint count=0;
    v->display=eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);
    if (!v->display || !eglInitialize(v->display,NULL,NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(v->display,attrs,&v->config,1,&count) || !count) goto fail;
    v->context=eglCreateContext(v->display,v->config,EGL_NO_CONTEXT,version);
    if (!v->context || !resize3d(v,w,h)) goto fail;
    GLuint vs=shader3d(GL_VERTEX_SHADER,"attribute vec4 position; attribute vec3 color;"
        "varying lowp vec3 tint; void main(){gl_Position=position;tint=color;}");
    GLuint fs=shader3d(GL_FRAGMENT_SHADER,"precision mediump float; varying lowp vec3 tint;"
        "void main(){gl_FragColor=vec4(tint,1.0);}");
    v->program=glCreateProgram();
    if (vs) glAttachShader(v->program,vs);
    if (fs) glAttachShader(v->program,fs);
    glBindAttribLocation(v->program,0,"position"); glBindAttribLocation(v->program,1,"color");
    glLinkProgram(v->program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    GLint ok=0; glGetProgramiv(v->program,GL_LINK_STATUS,&ok);
    if (!ok) goto fail;
    home3d(v); mjv_defaultOption(&v->options); mjv_defaultScene(&v->scene);
    mjv_makeScene(m,&v->scene,64); v->selected=-1;
    return 1;
fail:
    close3d(v); return 0;
}

static Vertex3 vertex3d(View3D *v,Point3 p,Point3 normal,Point3 color) {
    Point3 d=p3sub(p,v->origin);
    float depth=p3dot(d,v->forward);
    float light=.50f+.50f*fmaxf(0,p3dot(normal,pt3(-.42f,.48f,.77f)));
    return (Vertex3){{p3dot(d,v->right)*v->sx,p3dot(d,v->up)*v->sy,
                      v->za*depth+v->zb,depth},{color.x*light,color.y*light,color.z*light}};
}
static void triangles3d(Vertex3 *a,int count) {
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,sizeof *a,a->clip);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof *a,a->rgb);
    glDrawArrays(GL_TRIANGLES,0,count);
}
static Point3 orient3d(const float *r,Point3 p) {
    return pt3(r[0]*p.x+r[1]*p.y+r[2]*p.z,r[3]*p.x+r[4]*p.y+r[5]*p.z,
               r[6]*p.x+r[7]*p.y+r[8]*p.z);
}
/* A ring at each equator lets the same mesh describe capsules and spheres. */
enum { SLICES3=16, RINGS3=10 };
static void capsule3d(View3D *v,Point3 center,const float *rotation,float radius,float half,Point3 color) {
    Vertex3 rings[RINGS3][SLICES3],mesh[(RINGS3-1)*6*SLICES3];
    for (int j=0;j<RINGS3;++j) {
        int lower=j<RINGS3/2;
        float lat=(lower?j-4:j-5)*(float)(3.141592653589793/8);
        float z=sinf(lat),horizontal=cosf(lat);
        for (int i=0;i<SLICES3;++i) {
            float lon=i*(float)(2*3.141592653589793/SLICES3);
            Point3 local=pt3(horizontal*cosf(lon),horizontal*sinf(lon),z);
            Point3 p=pt3(local.x*radius,local.y*radius,local.z*radius+(lower?-half:half));
            rings[j][i]=vertex3d(v,p3add(center,orient3d(rotation,p)),orient3d(rotation,local),color);
        }
    }
    for (int j=1;j<RINGS3;++j) {
        for (int i=0;i<SLICES3;++i) {
            int next=(i+1)%SLICES3;
            Vertex3 *b=mesh+6*((j-1)*SLICES3+i);
            b[0]=rings[j-1][i]; b[1]=rings[j-1][next]; b[2]=rings[j][next];
            b[3]=b[0]; b[4]=b[2]; b[5]=rings[j][i];
        }
    }
    triangles3d(mesh,(RINGS3-1)*6*SLICES3);
}
static void ground3d(View3D *v) {
    Vertex3 mesh[12*12*6]; Point3 up=pt3(0,0,1);
    for (int j=-6;j<6;++j) {
        for (int i=-6;i<6;++i) {
            float scale=(i+j)%2?.86f:1.f;
            Point3 color=pt3(.27f*scale,.36f*scale,.40f*scale);
            Vertex3 *b=mesh+((j+6)*12+i+6)*6;
            float x=i*.55f,y=j*.55f;
            b[0]=vertex3d(v,pt3(x,y,-2.05f),up,color);
            b[1]=vertex3d(v,pt3(x+.55f,y,-2.05f),up,color);
            b[2]=vertex3d(v,pt3(x+.55f,y+.55f,-2.05f),up,color);
            b[3]=b[0]; b[4]=b[2]; b[5]=vertex3d(v,pt3(x,y+.55f,-2.05f),up,color);
        }
    }
    triangles3d(mesh,12*12*6);
}
/* The mounting stand and tiled floor give the fixed pivot a visible
 * reference. They are viewer geometry and never enter the dynamics. */
static void stand_box3d(View3D *v,Point3 center,Point3 size,Point3 color) {
    static const int faces[6][4]={{0,2,6,4},{1,5,7,3},{0,4,5,1},
                                 {2,3,7,6},{0,1,3,2},{4,6,7,5}};
    static const Point3 normals[6]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
    Vertex3 triangles[36];
    for (int f=0;f<6;++f) {
        Vertex3 corners[4];
        for (int i=0;i<4;++i) {
            int n=faces[f][i];
            Point3 offset=pt3(n&1?size.x:-size.x,n&2?size.y:-size.y,n&4?size.z:-size.z);
            corners[i]=vertex3d(v,p3add(center,offset),normals[f],color);
        }
        Vertex3 *out=triangles+6*f;
        out[0]=corners[0]; out[1]=corners[1]; out[2]=corners[2];
        out[3]=corners[0]; out[4]=corners[2]; out[5]=corners[3];
    }
    triangles3d(triangles,36);
}
static int draw3d(View3D *v,const mjModel *m,mjData *d,unsigned short *dst,int w,int h) {
    if (!resize3d(v,w,h)) return 0;
    mj_kinematics(m,d);
    mjv_updateScene(m,d,&v->options,NULL,&v->camera,mjCAT_ALL,&v->scene);
    v->eye=mjv_averageCamera(v->scene.camera,v->scene.camera+1);
    v->origin=p3array(v->eye.pos); v->up=p3array(v->eye.up); v->forward=p3array(v->eye.forward);
    v->right=p3cross(v->forward,v->up);
    float near=v->eye.frustum_near,far=v->eye.frustum_far;
    v->sy=2*near/(v->eye.frustum_top-v->eye.frustum_bottom);
    v->sx=v->sy*h/w; v->za=(far+near)/(far-near); v->zb=-2*far*near/(far-near);
    glViewport(0,0,w,h); glClearColor(.065f,.11f,.15f,1);
    glEnable(GL_DEPTH_TEST); glClear(GL_DEPTH_BUFFER_BIT|GL_COLOR_BUFFER_BIT);
    glUseProgram(v->program); glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    ground3d(v);
    stand_box3d(v,pt3(0,.36f,-1.98f),pt3(.45f,.31f,.065f),pt3(.36f,.48f,.54f));
    stand_box3d(v,pt3(0,.45f,-.92f),pt3(.065f,.065f,.995f),pt3(.53f,.67f,.71f));
    stand_box3d(v,pt3(0,.28f,.005f),pt3(.065f,.185f,.068f),pt3(.56f,.70f,.74f));
    static const Point3 palette3[]={{.26f,.88f,.85f},{.99f,.67f,.31f},{.62f,.62f,.99f}};
    static const float axle_rotation[]={1,0,0,0,0,1,0,-1,0};
    for (int i=0;i<v->scene.ngeom;++i) {
        const mjvGeom *g=&v->scene.geoms[i];
        if (g->objtype!=mjOBJ_GEOM || g->type!=mjGEOM_CAPSULE) continue;
        int body=m->geom_bodyid[g->objid];
        Point3 color=body==v->selected?pt3(1,.95f,.35f):palette3[(body-1)%3];
        Point3 center=p3array(g->pos);
        /* Add up to 1.5 pixels of visual thickness at a distance. Picking
         * still uses the physical model with a three-pixel input tolerance. */
        float depth=fmaxf(0,p3dot(p3sub(center,v->origin),v->forward));
        float radius=g->size[0]+fminf(.02f,3*depth/(v->sy*h));
        capsule3d(v,center,g->mat,radius,g->size[2],color);
        const mjtNum *pivot=d->xpos+3*body;
        capsule3d(v,pt3(pivot[0],pivot[1],pivot[2]),axle_rotation,body==1?.08f:.035f,
                  body==1?.16f:.034f,pt3(.76f,.81f,.86f));
    }
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,v->rgba);
    if (glGetError()!=GL_NO_ERROR) return 0;
    for (int y=0;y<h;++y) {
        const unsigned char *p=v->rgba+(size_t)(h-y-1)*w*4;
        for (int x=0;x<w;++x,p+=4)
            *dst++=(unsigned short)((p[0]>>3)<<11 | (p[1]>>2)<<5 | (p[2]>>3));
    }
    return 1;
}
static int pick3d(View3D *v,const mjModel *m,const mjData *d,int x,int y) {
    /* Thin physical rods can cover just a few pixels. Try the exact ray
     * before neighboring rays within a three-pixel mouse hit tolerance. */
    static const int offsets[][2]={{0,0},{-2,0},{2,0},{0,-2},{0,2},{-3,-2},{3,2},{-3,2},{3,-2}};
    for (size_t i=0;i<sizeof offsets/sizeof *offsets;++i) {
        int geom,flex,skin; mjtNum p[3];
        int body=mjv_select(m,d,&v->options,(double)v->width/v->height,
            (x+.5+offsets[i][0])/v->width,1-(y+.5+offsets[i][1])/v->height,
            &v->scene,p,&geom,&flex,&skin);
        if (body>0 && body<=3) return body;
    }
    return -1;
}
