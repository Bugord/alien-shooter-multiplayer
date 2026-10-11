#ifndef ASMP_RETRY_POLICY_H
#define ASMP_RETRY_POLICY_H

/* Retry timing shared by the player and monster coordinators (decision D3 in
   the architecture plan): retries never stop. The first delay is 2 seconds, it
   doubles after each failure and is capped at 30 seconds. */
#define MP_RETRY_FIRST_MS 2000u
#define MP_RETRY_MAX_MS   30000u
#endif
