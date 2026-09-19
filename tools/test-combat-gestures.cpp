#include "../src/combat_gestures.h"
#include <cassert>
#include <cstdio>

int main() {
    {
        CombatGripDetector g;
        bool valid[2]={true,true},forward[2]={true,true}; float grip[2]{};
        double now=1; uint32_t weapon=0; int holder=1; bool acquired=false;
        auto tick=[&]() { now+=0.01; return g.sample(now,weapon,holder,acquired,valid,grip,forward); };
        tick(); tick(); tick();
        grip[0]=1; assert(tick()==CombatNone);
        int pickups=0;
        for(int i=0;i<40;++i) pickups+=tick()==CombatPickupLeft;
        assert(pickups==1);
        weapon=100; holder=0; acquired=true; assert(tick()==CombatNone);
        grip[0]=0; assert(tick()==CombatDrop); assert(tick()==CombatNone);
        // An automatically received weapon stays latched until a fresh squeeze.
        grip[0]=1; tick(); weapon=101; acquired=false; tick(); grip[0]=0; assert(tick()==CombatNone);
        grip[0]=1; tick(); grip[0]=0; assert(tick()==CombatDrop);
        // Right grip and tracking loss cannot drop a left-held weapon.
        grip[0]=1; tick(); grip[1]=1; tick(); grip[1]=0; assert(tick()==CombatNone);
        valid[0]=false; assert(tick()==CombatNone); grip[0]=0; valid[0]=true;
        assert(tick()==CombatNone); assert(tick()==CombatNone);
        // Fresh bilateral chord wins over either delayed pickup and never repeats.
        weapon=0; tick(); grip[0]=1; tick(); grip[1]=1; assert(tick()==CombatDisarm);
        for(int i=0;i<50;++i) assert(tick()==CombatNone);
        grip[0]=grip[1]=0; tick(); forward[0]=false;
        grip[0]=grip[1]=1; assert(tick()==CombatNone);
        for(int i=0;i<40;++i) assert(tick()==CombatNone);
        grip[0]=grip[1]=0; tick(); grip[1]=1; tick();
        pickups=0; for(int i=0;i<40;++i) pickups+=tick()==CombatPickupRight;
        assert(pickups==1);
        // A pause/reacquisition with a grip already down produces no pickup.
        g.reset(); grip[0]=0; grip[1]=1;
        for(int i=0;i<40;++i) assert(tick()==CombatNone);
        CombatFourPunchCombo combo;
        assert(!combo.started(1)); assert(!combo.started(1.4)); assert(!combo.started(1.8));
        assert(combo.started(2.2)); assert(!combo.started(2.6));
        assert(!combo.started(5)); assert(!combo.started(5.4)); assert(!combo.started(5.8));
        assert(combo.started(6.2));
        // A 0.8-second pause now resets before the would-be fourth attack.
        assert(!combo.started(10)); assert(!combo.started(10.3)); assert(!combo.started(10.6));
        assert(!combo.started(11.4));
        assert(!combo.started(11.7)); assert(!combo.started(12)); assert(combo.started(12.3));
    }
    CombatPunchDetector d;
    double t = 1.0;
    auto sample = [&](float reach, bool grip = true, bool tracked = true) {
        t += 0.01;
        return d.sample(t, tracked, grip, {0.1f, -0.2f, -reach});
    };
    for (int i=0; i<20; ++i) assert(!sample(0.25f)); // held fist is not a punch
    int hits = 0;
    for (int i=1; i<=12; ++i) hits += sample(0.25f+i*0.02f);
    assert(hits == 1); // fast outward thrust emits exactly once
    for (int i=0; i<100; ++i) assert(!sample(0.49f)); // no repeat at extension
    for (int i=1; i<=12; ++i) assert(!sample(0.49f-i*0.02f)); // retract is not a punch
    hits = 0;
    for (int i=1; i<=12; ++i) hits += sample(0.25f+i*0.02f);
    assert(hits == 1); // retraction rearms while grip stays held
    d.reset();
    for (int i=0; i<50; ++i) assert(!sample(0.25f+i*0.004f)); // slow reaching
    d.reset();
    for (int i=0; i<50; ++i) assert(!sample(0.25f+(i%2)*0.003f)); // jitter
    d.reset();
    for (int i=0; i<30; ++i) assert(!sample(0.25f+i*0.02f, false)); // open hand
    for (int i=0; i<20; ++i) assert(!sample(0.25f));
    assert(!sample(0.8f)); // tracking jump rejected
    assert(!sample(0.25f, true, false)); // tracking loss resets stroke
    assert(!sample(0.6f)); // reacquisition doesn't punch
    t += 2.0;
    assert(!sample(0.3f)); // pause/resume doesn't punch
    puts("combat gesture tests passed");
}
