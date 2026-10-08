#include <math.h>
#include <string.h>
#include "pose_buffer.h"

#define POSE_DELAY_MS 100.0
#define POSE_DEFAULT_RATE 0.25     /* sender ticks per millisecond until measured */
#define POSE_MIN_RATE 0.05
#define POSE_MAX_RATE 2.0
#define POSE_EXTRAPOLATE_MS 100.0
#define POSE_RESYNC_MS 60.0
#define POSE_STALL_MS 250
#define POSE_LOCK_MS 300.0         /* time constant of the clock correction */
#define POSE_SNAP_DISTANCE 160.0f

/* Measured over the whole history, then smoothed so one late packet does not
   change the playback speed. */
static void measure_rate(PoseBuffer* b)
{
    const PoseSample* first = &b->samples[0]; const PoseSample* last = &b->samples[b->count - 1];
    int32_t span = (int32_t)(last->at - first->at);
    if (b->count < 2 || span < 60) return;
    double estimate = (double)(int32_t)(last->tick - first->tick) / span;
    if (estimate < POSE_MIN_RATE) estimate = POSE_MIN_RATE;
    if (estimate > POSE_MAX_RATE) estimate = POSE_MAX_RATE;
    b->rate = b->rate > 0 ? b->rate * 0.9 + estimate * 0.1 : estimate;
}

void pose_reset(PoseBuffer* b) { memset(b, 0, sizeof(*b)); }

void pose_push(PoseBuffer* b, uint32_t sequence, uint32_t tick, uint32_t now, float x, float y, float z)
{
    if (b->has_sequence && b->sequence == sequence) return;
    b->sequence = sequence; b->has_sequence = 1;
    if (b->count) {
        const PoseSample* last = &b->samples[b->count - 1];
        /* Two packets from one sender tick carry the same state. */
        if ((int32_t)(tick - last->tick) <= 0) return;
        float dx = x - last->x, dy = y - last->y, dz = z - last->z;
        if (dx * dx + dy * dy + dz * dz > POSE_SNAP_DISTANCE * POSE_SNAP_DISTANCE) { b->count = 0; b->locked = 0; }
    }
    if (b->count == POSE_HISTORY) {
        memmove(b->samples, b->samples + 1, (POSE_HISTORY - 1u) * sizeof(b->samples[0]));
        --b->count;
    }
    PoseSample* s = &b->samples[b->count++];
    s->tick = tick; s->at = now; s->x = x; s->y = y; s->z = z;
    measure_rate(b);
}

int pose_sample(PoseBuffer* b, uint32_t now, float* x, float* y, float* z)
{
    if (!b->count) return 0;
    const PoseSample* newest = &b->samples[b->count - 1];
    double rate = b->rate > 0 ? b->rate : POSE_DEFAULT_RATE;
    double desired = newest->tick + rate * (int32_t)(now - newest->at) - rate * POSE_DELAY_MS;
    int32_t dt = (int32_t)(now - b->render_at);
    if (!b->locked || dt < 0 || dt > POSE_STALL_MS) {
        b->render_tick = b->count > 1 ? desired : newest->tick; b->locked = b->count > 1;
    } else {
        /* Run at the sender's rate and bend slowly toward the target, so the
           shown speed does not follow packet arrival jitter. */
        double previous = b->render_tick, error;
        b->render_tick += rate * dt;
        error = desired - b->render_tick;
        if (fabs(error) > rate * POSE_RESYNC_MS) b->render_tick = desired;
        else b->render_tick += error * (dt / (dt + POSE_LOCK_MS));
        if (b->render_tick < previous) b->render_tick = previous;
    }
    b->render_at = now;
    const PoseSample* first = &b->samples[0];
    if (b->count == 1 || b->render_tick <= first->tick) { *x = first->x; *y = first->y; *z = first->z; return 1; }
    for (unsigned int i = 0; i + 1 < b->count; ++i) {
        const PoseSample* a = &b->samples[i]; const PoseSample* c = &b->samples[i + 1];
        if (b->render_tick < c->tick) {
            float t = (float)((b->render_tick - a->tick) / (double)(c->tick - a->tick));
            *x = a->x + (c->x - a->x) * t; *y = a->y + (c->y - a->y) * t; *z = a->z + (c->z - a->z) * t;
            return 1;
        }
    }
    /* Starved: continue along the last segment for a short while, then hold. */
    const PoseSample* prev = &b->samples[b->count - 2];
    double ahead = b->render_tick - newest->tick, limit = rate * POSE_EXTRAPOLATE_MS;
    if (ahead > limit) ahead = limit;
    double per_tick = ahead / (double)(newest->tick - prev->tick);
    *x = newest->x + (float)((newest->x - prev->x) * per_tick);
    *y = newest->y + (float)((newest->y - prev->y) * per_tick);
    *z = newest->z + (float)((newest->z - prev->z) * per_tick);
    return 1;
}
