#ifndef __PD_PORT_EXTERNAL_H
#define __PD_PORT_EXTERNAL_H

#include <stdint.h>

#define __maybe_unused __attribute__((__unused__))
#define test_mockable_static static
#define __const_data
//#define __const_data __attribute__((__section__(".rodata")))

#define ATOMIC_BOOLS_DEFINE(VAR, SIZE) atomic_bool VAR[SIZE]

/* Microsecond timestamp. */
typedef union {
	uint64_t val;
	struct {
		uint32_t lo;
		uint32_t hi;
	} le /* little endian words */;
} timestamp_t;

extern timestamp_t get_time(void);

#endif /* __PD_PORT_EXTERNAL_H */