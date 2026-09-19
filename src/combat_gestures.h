#pragma once
#include <cmath>
#include <cstdint>

enum CombatGestureAction { CombatNone, CombatPickupLeft, CombatPickupRight,
    CombatDrop, CombatDisarm, CombatPunchLeft, CombatPunchRight, CombatCatchLeft, CombatCatchRight };

struct CombatGripDetector {
    bool seen[2]{}, held[2]{}, dropArmed = false;
    double pressed[2]{-100,-100}, previousTime = 0, pickupAt = 0;
    uint32_t weapon = 0;
    int pickupHand = -1;
    void reset() { *this = CombatGripDetector{}; }
    int sample(double now, uint32_t equipped, int holdingHand, bool acquiredByGrip,
               const bool valid[2], const float grip[2], const bool forward[2]) {
        bool rising[2]{};
        const bool continuous = previousTime > 0 && now > previousTime && now-previousTime < 0.10;
        previousTime = now;
        const bool changed = weapon != equipped;
        if (changed) { weapon = equipped; dropArmed = false; pickupHand = -1; }
        for (int h=0; h<2; ++h) {
            if (!continuous || !valid[h] || !std::isfinite(grip[h])) {
                seen[h]=false; held[h]=false; pressed[h]=-100;
                if (h==holdingHand) dropArmed=false;
                if (pickupHand==h) pickupHand=-1;
                continue;
            }
            const bool next = grip[h] >= (held[h] ? 0.35f : 0.65f);
            rising[h]=seen[h] && !held[h] && next;
            seen[h]=true; held[h]=next;
            if (rising[h]) pressed[h]=now;
        }
        if (equipped) {
            pickupHand=-1;
            if (holdingHand<0 || holdingHand>1 || !seen[holdingHand]) return CombatNone;
            if ((changed && acquiredByGrip && held[holdingHand]) || rising[holdingHand]) dropArmed=true;
            if (dropArmed && !held[holdingHand]) { dropArmed=false; return CombatDrop; }
            return CombatNone;
        }
        dropArmed=false;
        if ((rising[0] || rising[1]) && held[0] && held[1] &&
            now-pressed[0]<=0.25 && now-pressed[1]<=0.25) {
            pickupHand=-1;
            // A two-hand chord is consumed even if it is not a disarm pose.
            return forward[0] && forward[1] ? CombatDisarm : CombatNone;
        }
        for (int h=0;h<2;++h) if(rising[h]) { pickupHand=h; pickupAt=now+0.25; }
        if (pickupHand>=0 && now>=pickupAt) {
            const int h=pickupHand; pickupHand=-1;
            if(seen[h] && held[h] && !held[1-h]) return h==0 ? CombatPickupLeft : CombatPickupRight;
        }
        return CombatNone;
    }
};

// Count accepted attack starts, not rejected input or the game's two-slot queue.
struct CombatFourPunchCombo {
    int count = 0;
    double last = 0;
    void reset() { count=0; last=0; }
    bool started(double now) {
        if (now-last>0.75 || now<last) count=0;
        last=now;
        if (++count==4) { count=0; return true; }
        return false;
    }
};

// Room-space hand minus head, in metres. No dependency on render cadence or game memory.
struct CombatPunchDetector {
    struct Point { float x, y, z; };
    bool have = false, armed = false, held = false;
    Point previous{};
    double previousTime = 0, gripSince = 0, strokeSince = 0, cooldownUntil = 0;
    float strokeStart = 0, furthest = 0;

    void reset() { *this = CombatPunchDetector{}; }

    bool sample(double now, bool valid, bool grip, Point p) {
        if (!valid || !std::isfinite(now) || !std::isfinite(p.x) ||
            !std::isfinite(p.y) || !std::isfinite(p.z)) { reset(); return false; }
        const float radius = std::sqrt(p.x*p.x + p.z*p.z);
        const double dt = now - previousTime;
        const float dx = p.x-previous.x, dy = p.y-previous.y, dz = p.z-previous.z;
        const bool continuous = have && dt >= 0.004 && dt <= 0.05 &&
                                dx*dx + dy*dy + dz*dz <= 0.0625f;
        const float prevRadius = std::sqrt(previous.x*previous.x + previous.z*previous.z);
        previous = p; previousTime = now; have = true;
        if (!grip || !continuous) {
            armed = grip; held = grip; gripSince = now;
            strokeSince = 0; furthest = radius;
            return false;
        }
        if (!held) {
            held = true; armed = true; gripSince = now;
            strokeSince = 0; furthest = radius;
            return false;
        }
        if (radius > furthest) furthest = radius;
        // Retract before another punch. A held fist at full extension never repeats.
        if (!armed && furthest - radius >= 0.08f) {
            armed = true; strokeSince = 0; furthest = radius;
        }
        if (!armed || now < cooldownUntil || now-gripSince < 0.06) return false;
        const float outwardSpeed = (radius-prevRadius)/static_cast<float>(dt);
        if (outwardSpeed < 0.35f || p.y < -0.65f || p.y > 0.35f) {
            strokeSince = 0; return false;
        }
        if (strokeSince == 0 || now-strokeSince > 0.20) {
            strokeSince = now; strokeStart = prevRadius;
        }
        if (outwardSpeed < 1.2f || radius-strokeStart < 0.10f) return false;
        armed = false; furthest = radius; strokeSince = 0;
        cooldownUntil = now+0.25;
        return true;
    }
};
