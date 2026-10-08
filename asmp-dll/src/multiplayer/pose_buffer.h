#ifndef ASMP_POSE_BUFFER_H
#define ASMP_POSE_BUFFER_H
#include <stdint.h>

/* Snapshot interpolation for one remote player. Samples are placed on the
   sender's own tick timeline (the sender sends at irregular wall-clock
   intervals, but its game tick advances evenly), and the shown position is
   read a fixed delay behind the newest sample. Pure data: no engine calls. */
#define POSE_HISTORY 8u
typedef struct PoseSample { uint32_t tick; uint32_t at; float x, y, z; } PoseSample;
typedef struct PoseBuffer {
    PoseSample samples[POSE_HISTORY];
    unsigned int count;
    uint32_t sequence;
    int has_sequence, locked;
    /* Playback position in sender ticks, and when it was last advanced. */
    double render_tick, rate;  /* rate: sender ticks per ms, smoothed; 0 = unmeasured */
    uint32_t render_at;
} PoseBuffer;

void pose_reset(PoseBuffer*);
/* Records a state once per sequence number. A jump of more than 160 units
   (respawn, teleport) discards older samples instead of sweeping across it. */
void pose_push(PoseBuffer*, uint32_t sequence, uint32_t tick, uint32_t now, float x, float y, float z);
/* Advances playback to `now` and returns the position to show; 0 while empty. */
int pose_sample(PoseBuffer*, uint32_t now, float* x, float* y, float* z);
#endif
