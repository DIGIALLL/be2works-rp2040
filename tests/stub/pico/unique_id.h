#pragma once
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
typedef struct { uint8_t id[8]; } pico_unique_board_id_t;
static inline void pico_get_unique_board_id(pico_unique_board_id_t *u){for(int i=0;i<8;i++)u->id[i]=0xA0+i;}
