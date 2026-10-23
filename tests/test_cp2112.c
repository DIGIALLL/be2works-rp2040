#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "cp2112.h"
#include "smbus.h"
static uint8_t last[64]; static int last_n, nrep;
bool tud_hid_ready(void){return true;}
bool tud_hid_report(uint8_t id,const void*d,uint16_t n){last[0]=id;memcpy(last+1,d,n);last_n=n;nrep++;return true;}
void tud_disconnect(void){}
void smbus_init(uint32_t hz){(void)hz;} void smbus_set_clock(uint32_t hz){printf("clock %u\n",hz);}
static uint8_t seen_addr, seen_target[16]; static size_t seen_tl;
smbus_result_t smbus_write(uint8_t a,const uint8_t*d,size_t l,uint32_t t){seen_addr=a;(void)d;(void)l;(void)t;return a==0x0B?SMBUS_OK:SMBUS_ADDR_NACK;}
smbus_result_t smbus_read(uint8_t a,uint8_t*d,size_t l,uint32_t t){(void)t;if(a!=0x0B)return SMBUS_ADDR_NACK;for(size_t i=0;i<l;i++)d[i]=i;return SMBUS_OK;}
smbus_result_t smbus_write_read(uint8_t a,const uint8_t*tg,size_t tl,uint8_t*d,size_t l,uint32_t t){seen_addr=a;memcpy(seen_target,tg,tl);seen_tl=tl;return smbus_read(a,d,l,t);}
static void pump(void){for(int i=0;i<20;i++)cp2112_task();}
int main(void){
  cp2112_init(); uint8_t b[64];
  assert(cp2112_get_feature(0x05,b,63)==2 && b[0]==0x0C);
  assert(cp2112_get_feature(0x23,b,63)==63); printf("product len byte %u\n",b[0]);
  printf("usb strings: %s | %s | %s\n",cp2112_usb_string(1),cp2112_usb_string(2),cp2112_usb_string(3));
  // SMBus config: 100kHz, autosend 0, retry 2
  uint8_t c[13]={0x00,0x01,0x86,0xA0,0x02,0,0,0,0,0,0,0,2}; cp2112_set_feature(0x06,c,13);
  // Write-read: battery 0x16 (8-bit) reg 0x0D (RSOC), read 2
  uint8_t wr[20]={0x16,0x00,0x02,0x01,0x0D}; cp2112_on_output(0x11,wr,20); pump();
  assert(seen_addr==0x0B && seen_tl==1 && seen_target[0]==0x0D);
  uint8_t s=1; nrep=0; cp2112_on_output(0x15,&s,1); pump();
  assert(nrep==1 && last[0]==0x16 && last[1]==0x02 && last[2]==0x05 && last[6]==2); printf("status ok len=%d\n",last_n);
  uint8_t fr[2]={0,2}; nrep=0; cp2112_on_output(0x12,fr,2); pump();
  assert(nrep==1 && last[0]==0x13 && last[1]==0x02 && last[2]==2 && last[3]==0 && last[4]==1 && last_n==63);
  // NACK at other address
  uint8_t rr[3]={0x20,0,4}; cp2112_on_output(0x10,rr,3); pump(); nrep=0; cp2112_on_output(0x15,&s,1); pump();
  assert(last[1]==0x03 && last[2]==0x00 && last[4]==2); printf("nack ok retries=%d\n",last[4]);
  // Autosend 130-byte read -> 3 chunks
  c[5]=1; cp2112_set_feature(0x06,c,13); uint8_t rr2[3]={0x16,0,130}; nrep=0; cp2112_on_output(0x10,rr2,3); pump();
  assert(nrep==3 && last[1]==0x02 && last[2]==130-122); printf("autosend chunks=%d\n",nrep);
  // Гонка: запрос, статус и force read приходят в одном tud_task()
  c[5]=0; cp2112_set_feature(0x06,c,13);
  uint8_t wr2[20]={0x16,0x00,0x02,0x01,0x0F}; uint8_t st=1, f2[2]={0,2};
  cp2112_on_output(0x11,wr2,20); cp2112_on_output(0x15,&st,1); cp2112_on_output(0x12,f2,2);
  nrep=0; uint8_t got16=0, got13=0;
  for(int i=0;i<5;i++){ cp2112_task(); if(last[0]==0x16 && !got16){got16=last[1];} if(last[0]==0x13){got13=last[2];} }
  printf("race: status=%u data_len=%u reports=%d\n",got16,got13,nrep); assert(got16==0x02 && got13==2 && nrep==2);
  // Be2Works: запрос, force read и GET_REPORT 0x13 до того, как отработал cp2112_task()
  uint8_t wr3[20]={0x16,0x00,0x02,0x01,0x09}, f3[2]={0,2}, in[64];
  cp2112_on_output(0x11,wr3,20); cp2112_on_output(0x12,f3,2);
  assert(cp2112_get_input(0x13,in,63)==63 && in[0]==0x02 && in[1]==2 && in[2]==0 && in[3]==1 && seen_target[0]==0x09);
  assert(cp2112_get_input(0x13,in,63)==63 && in[0]==0x02 && in[1]==0);   // данных больше нет
  assert(cp2112_get_input(0x16,in,63)==6 && in[0]==0x02 && in[1]==0x05 && in[5]==2);
  pump(); printf("get_report ok\n");
  printf("ALL OK\n"); return 0; }
