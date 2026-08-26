#include "wedge/spring.h"

#include <math.h>

void wg_spring_init(wg_spring_t *s, float value, float damping, float response)
{
    s->value = value;
    s->target = value;
    s->velocity = 0.0f;
    s->damping = damping;
    s->response = response > 1e-4f ? response : 1e-4f;
}

void wg_spring_to(wg_spring_t *s, float target)
{
    s->target = target;
}

void wg_spring_set(wg_spring_t *s, float value)
{
    s->value = value;
    s->target = value;
    s->velocity = 0.0f;
}

void wg_spring_kick(wg_spring_t *s, float velocity)
{
    s->velocity = velocity;
}

/* Implicit Euler. Solving for the next velocity rather than stepping from the
   current one makes the integrator unconditionally stable, so a long frame (a
   flash write, a Wi-Fi reconnect) cannot make the motion explode. */
void wg_spring_step(wg_spring_t *s, float dt)
{
    if (dt <= 0.0f) {
        return;
    }
    if (dt > 0.1f) {
        dt = 0.1f;
    }
    const float omega = 6.2831853f / s->response;
    const float denom = 1.0f + 2.0f * s->damping * omega * dt + omega * omega * dt * dt;
    s->velocity = (s->velocity - omega * omega * dt * (s->value - s->target)) / denom;
    s->value += s->velocity * dt;
}

bool wg_spring_settled(const wg_spring_t *s)
{
    return fabsf(s->value - s->target) < 0.002f && fabsf(s->velocity) < 0.02f;
}

float wg_project(float velocity, float deceleration)
{
    if (deceleration >= 1.0f) {
        return 0.0f;
    }
    return (velocity / 1000.0f) * deceleration / (1.0f - deceleration);
}

float wg_rubberband(float overshoot, float dimension, float constant)
{
    float a = overshoot < 0.0f ? -overshoot : overshoot;
    float r = (a * dimension * constant) / (dimension + constant * a);
    return overshoot < 0.0f ? -r : r;
}
