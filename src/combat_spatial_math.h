#pragma once
#include <cmath>

// Distances are metres. A small tolerance makes a thin pistol selectable without
// requiring pixel-perfect head aim. Fifteen degrees includes a pistol slightly
// below the center of the view while retaining the two-metre distance limit.
inline float CombatGazeScore(float forward,float sideSquared,float distance)
{
    if(!std::isfinite(forward) || !std::isfinite(sideSquared) || !std::isfinite(distance) ||
        forward<=0 || distance>2.0f || distance<0.05f || sideSquared<0) return -1;
    const float radius=forward*0.267949f>0.12f?forward*0.267949f:0.12f;
    if(sideSquared>radius*radius) return -1;
    return sideSquared/(distance*distance)+distance*0.002f;
}

inline float CombatSegmentDistanceSquared(const float point[3],const float from[3],const float to[3])
{
    float length=0,dot=0;
    for(int i=0;i<3;++i){const float d=to[i]-from[i];length+=d*d;dot+=(point[i]-from[i])*d;}
    float t=length>1e-8f?dot/length:0; t=t<0?0:(t>1?1:t);
    float result=0;
    for(int i=0;i<3;++i){const float d=point[i]-from[i]-(to[i]-from[i])*t;result+=d*d;}
    return result;
}
