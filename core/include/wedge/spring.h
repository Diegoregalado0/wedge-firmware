#ifndef WEDGE_SPRING_H
#define WEDGE_SPRING_H

#include "wedge/wedge.h"

/* Motion in this firmware is spring-driven rather than curve-driven, for one
   reason: a spring can be re-targeted mid-flight without a discontinuity. A
   fixed-duration tween cannot, so anything a finger can touch while it moves
   would visibly snap.

   The two parameters are Apple's, not the physics triplet:
     damping  1.0 settles without overshoot; below 1.0 overshoots and bounces.
     response  seconds to reach the target. Not a duration; a spring has none. */
typedef struct {
    float value;
    float target;
    float velocity;
    float damping;
    float response;
} wg_spring_t;

void wg_spring_init(wg_spring_t *s, float value, float damping, float response);

/* Re-target without touching velocity. This is what makes motion interruptible:
   the value keeps moving through the change instead of restarting from it. */
void wg_spring_to(wg_spring_t *s, float target);

/* Jump the value with no animation, e.g. when a face is first composed. */
void wg_spring_set(wg_spring_t *s, float value);

/* Hand a gesture's release velocity to the spring so there is no seam between
   dragging and animating. */
void wg_spring_kick(wg_spring_t *s, float velocity);

void wg_spring_step(wg_spring_t *s, float dt);

bool wg_spring_settled(const wg_spring_t *s);

/* Where a flick is going, by exponential decay, so a release snaps to the
   target nearest the projected endpoint rather than the nearest one to the
   finger. Deceleration 0.998 is the normal scroll feel. */
float wg_project(float velocity, float deceleration);

/* Progressive resistance past a boundary. A hard stop reads as frozen; this
   reads as responsive with nothing more to reach. */
float wg_rubberband(float overshoot, float dimension, float constant);

#endif
