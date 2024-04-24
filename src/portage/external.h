#ifndef __PD_PORT_EXTERNAL_H
#define __PD_PORT_EXTERNAL_H

#include <stdint.h>

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