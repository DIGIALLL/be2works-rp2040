#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void){return 0;}
static inline uint32_t to_ms_since_boot(absolute_time_t t){(void)t;return 0;}
static inline void sleep_ms(int x){(void)x;}
static inline void tight_loop_contents(void){}
