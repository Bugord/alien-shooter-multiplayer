#ifndef ASMP_RETRY_POLICY_H
#define ASMP_RETRY_POLICY_H

/* Retry timing shared by the player and monster coordinators (decision D3 in
   the architecture plan): retries never stop. The first delay is 2 seconds, it
   doubles after each failure and is capped at 30 seconds. */
#define MP_RETRY_FIRST_MS 2000u
#define MP_RETRY_MAX_MS   30000u

/* The delay after `delay`: doubled, capped at MP_RETRY_MAX_MS. Starting at
   MP_RETRY_FIRST_MS this gives 2, 4, 8, 16, 30, 30, ... seconds. */
static inline unsigned long mp_retry_next_delay(unsigned long delay)
{
    if (delay >= MP_RETRY_MAX_MS / 2u)
        return MP_RETRY_MAX_MS;
    return delay * 2u;
}

/* The delay to wait after `failures` earlier failures in a row. */
static inline unsigned long mp_retry_delay(unsigned int failures)
{
    unsigned long delay = MP_RETRY_FIRST_MS;
    for (unsigned int i = 0; i < failures && delay < MP_RETRY_MAX_MS; ++i)
    {
        delay = mp_retry_next_delay(delay);
    }
    return delay;
}
#endif
