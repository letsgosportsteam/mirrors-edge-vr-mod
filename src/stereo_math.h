#pragma once
#include <cmath>

inline bool StereoInverseMatrix(const float* matrix,float* inverse)
{
    double rows[4][8]{};
    for(int r=0;r<4;++r){for(int c=0;c<4;++c){
        if(!std::isfinite(matrix[r*4+c]))return false;
        rows[r][c]=matrix[r*4+c];
    }rows[r][r+4]=1;}
    for(int c=0;c<4;++c){
        int pivot=c;for(int r=c+1;r<4;++r)if(fabs(rows[r][c])>fabs(rows[pivot][c]))pivot=r;
        if(fabs(rows[pivot][c])<1e-12)return false;
        if(pivot!=c)for(int k=0;k<8;++k){const double v=rows[c][k];rows[c][k]=rows[pivot][k];rows[pivot][k]=v;}
        const double scale=rows[c][c];for(int k=0;k<8;++k)rows[c][k]/=scale;
        for(int r=0;r<4;++r)if(r!=c){const double scaleOther=rows[r][c];
            for(int k=0;k<8;++k)rows[r][k]-=scaleOther*rows[c][k];}
    }
    for(int r=0;r<4;++r)for(int c=0;c<4;++c){
        inverse[r*4+c]=(float)rows[r][c+4];if(!std::isfinite(inverse[r*4+c]))return false;
    }
    return true;
}

inline void StereoMultiplyMatrix(const float* a,const float* b,float* out)
{
    float result[16]{};
    for(int r=0;r<4;++r)for(int c=0;c<4;++c){
        double sum=0;for(int k=0;k<4;++k)sum+=(double)a[r*4+k]*b[k*4+c];
        result[r*4+c]=(float)sum;
    }
    for(int i=0;i<16;++i)out[i]=result[i];
}

// UE's shadow shaders reconstruct from (NDC.x*depth,NDC.y*depth,depth,1),
// not from a homogeneous clip position. Rebase that space from the eye camera
// to the original camera, preserving the engine's light/shadow transform.
inline bool StereoDepthRebase(const float* original,const float* eye,float* out)
{
    double length=0,dot=0;
    for(int r=0;r<3;++r){length+=(double)original[r*4+3]*original[r*4+3];
        dot+=(double)original[r*4+2]*original[r*4+3];}
    if(length<1e-10)return false;
    const float a=(float)(dot/length),b=original[14]-a*original[15];
    const float depthToClip[16]={1,0,0,0, 0,1,0,0, 0,0,a,1, 0,0,b,0};
    float invDepth[16],invEye[16],tmp[16];
    if(!StereoInverseMatrix(depthToClip,invDepth)||!StereoInverseMatrix(eye,invEye))return false;
    StereoMultiplyMatrix(depthToClip,invEye,tmp);
    StereoMultiplyMatrix(tmp,original,tmp);
    StereoMultiplyMatrix(tmp,invDepth,out);
    return true;
}
