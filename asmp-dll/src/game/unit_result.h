#ifndef ASMP_UNIT_RESULT_H
#define ASMP_UNIT_RESULT_H

/* Return convention of unit_puppet, unit_combat and unit_death. Every public
   function of these modules that reports an outcome returns one of these.
   The values equal the older 1/0/-1 integers, so `!= UNIT_OK` and `< 0`
   comparisons keep their meaning. */
enum UnitResult
{
    UNIT_FAULT = -1,  /* A native fault; the slot is now faulted or abandoned. */
    UNIT_REFUSED = 0, /* Not done: invalid request, not ready or nothing found. */
    UNIT_OK = 1       /* Done. */
};

#endif
