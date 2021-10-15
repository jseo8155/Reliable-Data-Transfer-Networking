/*
 * File: rdt_time.h
 *
 * Header / API file for timing component of RDT library.
 *
 */
#include <sys/time.h>


/*
 * Get the current time (in milliseconds).
 *
 * @note The current time is given relative to the start of the UNIX epoch.
 *
 * @return The number of milliseconds since the UNIX epoch.
 */
int current_msec();

/*
 * Creates a timeval struct that represents the given number of milliseconds.
 *
 *
 * @param millis The number of milliseconds to convert.
 * @param out_timeval Pointer to timeval that will be filled in based on
 * 		millis.
 */
void msec_to_timeval(int millis, struct timeval *out_timeval);


/*
 * Converts a timeval struct to milliseconds
 *
 *
 * @param t The timeval struct to convert.
 * @return Number of milliseconds (as specified by t)
 */
int timeval_to_msec(struct timeval *t);
