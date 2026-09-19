#pragma once
#include <cmath>
namespace mevr {
struct SnapTurn {
    bool armed=false;
    int sample(float x,bool enabled) {
        if(!enabled||!std::isfinite(x)){armed=false;return 0;}
        if(std::fabs(x)<.25f){armed=true;return 0;}
        if(armed&&std::fabs(x)>.7f){armed=false;return x>0?1:-1;}
        return 0;
    }
};
}
