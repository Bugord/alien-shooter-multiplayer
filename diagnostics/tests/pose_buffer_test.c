#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../asmp-dll/src/multiplayer/pose_buffer.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* The sender advances 0.25 ticks per ms and moves 0.5 units per tick, so a
   steady walker covers 0.125 units/ms. Packets leave at uneven wall-clock
   spacing (31/47 ms) and arrive with jitter; the shown path must stay even. */
int main(void)
{
    PoseBuffer b; float x, y, z;
    pose_reset(&b);
    CHECK(!pose_sample(&b, 0, &x, &y, &z));

    unsigned int sent_at[200], arrive[200], count = 0, next = 0, t = 1000;
    for (; count < 60; ++count) {
        sent_at[count] = t; arrive[count] = t + 6 + (count * 7u) % 17u; t += (count & 1u) ? 47u : 31u;
    }
    float previous = 0; int have_previous = 0;
    float worst_step = 0, slowest = 1e9f;
    for (unsigned int now = 1000; now < 2300; now += 4) {
        while (next < count && arrive[next] <= now) {
            uint32_t tick = (uint32_t)(sent_at[next] / 4u);
            pose_push(&b, next + 1u, tick, arrive[next], tick * 0.5f, 7.0f, 3.0f); ++next;
        }
        if (!pose_sample(&b, now, &x, &y, &z)) continue;
        CHECK(y == 7.0f && z == 3.0f);
        if (now > 1700 && have_previous) {
            float step = x - previous;
            CHECK(step >= 0);
            if (step > worst_step) worst_step = step;
            if (step < slowest) slowest = step;
        }
        previous = x; have_previous = 1;
    }
    /* Nominal 0.5 per 4 ms step; jitter may bend it a little, never stall or lurch. */
    CHECK(slowest > 0.35f && worst_step < 0.65f);

    /* The shown position trails the newest sample by the playback delay. */
    pose_reset(&b);
    for (unsigned int k = 0; k < 10; ++k) pose_push(&b, k + 1u, 100u + k * 8u, 5000u + k * 32u, 10.0f * k, 0, 0);
    CHECK(pose_sample(&b, 5000u + 9u * 32u, &x, &y, &z) && x < 90.0f && x > 40.0f);

    /* A repeated sequence or tick adds no sample. */
    unsigned int before = b.count;
    pose_push(&b, 10u, 172u, 5400u, 999, 0, 0); pose_push(&b, 11u, 172u, 5400u, 999, 0, 0);
    CHECK(b.count == before);

    /* Starved: the path continues briefly, then holds. */
    pose_sample(&b, 5400u, &x, &y, &z);
    float held;
    pose_sample(&b, 5900u, &x, &y, &z); pose_sample(&b, 6400u, &held, &y, &z);
    pose_sample(&b, 6600u, &x, &y, &z);
    CHECK(held == x && x >= 90.0f && x < 135.0f);

    /* A teleport is not swept across. */
    pose_push(&b, 12u, 300u, 6700u, 5000.0f, 0, 0);
    CHECK(b.count == 1 && pose_sample(&b, 6701u, &x, &y, &z) && x == 5000.0f);

    puts("pose buffer ok");
    return 0;
}
