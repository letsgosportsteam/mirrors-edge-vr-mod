#pragma once
#include <cstdint>
#include <cmath>

namespace mevr {
struct PipeExitSnapshot {
    uintptr_t pawn=0, move=0, volume=0;
    uint64_t sampledAt=0;
    float top[3]{};
    int lastStep=-1;
    uint8_t state=255;
    bool valid=false, canExit=false;
    bool matches(uintptr_t p,uintptr_t v,uint64_t now) const {
        return valid && pawn==p && volume==v && now>=sampledAt && now-sampledAt<500;
    }
};
inline bool pipeExitReady(bool canExit,int state,int currentStep,int lastStep,
                          float z,float top,bool playing,bool precise,bool sliding) {
    return canExit && state==0 && lastStep>=0 && currentStep==lastStep &&
        std::isfinite(z) && std::isfinite(top) && std::fabs(z-top)<=8.f &&
        !playing && !precise && !sliding;
}
}
