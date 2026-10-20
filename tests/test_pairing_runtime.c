#include "platform_stubs.h"
#include "ble_mesh_pairing.h"
#include "mesh_pairing_crypto.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct { size_t size, read, write, count; uint8_t data[16][64]; } queue_t;
static uint64_t clock_ms;
static uint32_t nvs_sequence;
static esp_ble_mesh_ble_cb_t radio;
static char last_status[80];
static uint8_t last_adv[31];
static unsigned advertisements, onoff_reports;
static const uint8_t mac[6] = {0x11,0x22,0x33,0x44,0x55,0x66};
QueueHandle_t xQueueCreate(size_t count, size_t size) { assert(count==16 && size<=64); queue_t *q=calloc(1,sizeof(*q));q->size=size;return q; }
int xQueueSend(QueueHandle_t h,const void *data,int wait) { (void)wait;queue_t*q=h;if(q->count==16)return 0;memcpy(q->data[q->write++%16],data,q->size);q->count++;return 1; }
int xQueueReceive(QueueHandle_t h,void *data,int wait) { (void)wait;queue_t*q=h;if(!q->count)return 0;memcpy(data,q->data[q->read++%16],q->size);q->count--;return 1; }
int xQueueReset(QueueHandle_t h) { queue_t*q=h;q->read=q->write=q->count=0;return 1; }
int nvs_open(const char *name,int mode,nvs_handle_t*h) { (void)name;(void)mode;*h=1;return 0; }
int nvs_get_u32(nvs_handle_t h,const char*k,uint32_t*v) { (void)h;(void)k;if(!nvs_sequence)return ESP_ERR_NVS_NOT_FOUND;*v=nvs_sequence;return 0; }
int nvs_set_u32(nvs_handle_t h,const char*k,uint32_t v) { (void)h;(void)k;nvs_sequence=v;return 0; }
int nvs_commit(nvs_handle_t h) { (void)h;return 0; }
void nvs_close(nvs_handle_t h) { (void)h; }
void esp_fill_random(void *data,size_t length) { static uint8_t counter;for(size_t i=0;i<length;i++)((uint8_t*)data)[i]=++counter; }
int64_t esp_timer_get_time(void) { return clock_ms*1000; }
int esp_ble_mesh_register_ble_callback(esp_ble_mesh_ble_cb_t cb) { radio=cb;return 0; }
int esp_ble_mesh_start_ble_scanning(esp_ble_mesh_ble_scan_param_t *p) { (void)p;esp_ble_mesh_ble_cb_param_t event={0};radio(ESP_BLE_MESH_START_BLE_SCANNING_COMP_EVT,&event);return 0; }
int esp_ble_mesh_start_ble_advertising(const esp_ble_mesh_ble_adv_param_t *p,const esp_ble_mesh_ble_adv_data_t *d) {
 assert(p->count==0 && p->priority==ESP_BLE_MESH_BLE_ADV_PRIO_LOW && p->duration<=240 && d->adv_data_len<=31);memcpy(last_adv,d->adv_data,d->adv_data_len);advertisements++;return 0;
}
void ble_mesh_bridge_receive_authenticated_onoff(bool on) { assert(on);onoff_reports++; }
void ble_mesh_bridge_receive_authenticated_vendor(const uint8_t *data,uint16_t length) { assert(length==4 && data[1]==0 && data[2]==1 && data[3]==1);onoff_reports++; }
static void status(const char *s,void*context) { (void)context;snprintf(last_status,sizeof(last_status),"%s",s); }
static size_t unhex(const char *s,uint8_t *out) { size_t n=strlen(s)/2;for(size_t i=0;i<n;i++){unsigned x;assert(sscanf(s+i*2,"%2x",&x)==1);out[i]=x;}return n; }
static void receive(const uint8_t *p,size_t size,bool wrong_mac) {
 uint8_t ad[31]={2,1,6,size+3,0xff,1,0xa8};memcpy(ad+7,p,size);esp_ble_mesh_ble_cb_param_t event={0};
 memcpy(event.scan_ble_adv_pkt.addr,mac,6);if(wrong_mac)event.scan_ble_adv_pkt.addr[5]^=1;
 event.scan_ble_adv_pkt.data=ad;event.scan_ble_adv_pkt.length=size+7;
 radio(ESP_BLE_MESH_SCAN_BLE_ADVERTISING_PKT_EVT,&event);ble_mesh_pairing_poll();
}
static void mesh(const char *hex,uint8_t tid,bool corrupt) {
 uint8_t packet[29],fragment[24];size_t length=unhex(hex,packet);if(corrupt)packet[length-1]^=1;
 // Deliver fragments out of order, as real advertisements can be reordered.
 for(int index=2;index>=1;index--){size_t offset=(index-1)*18,n=length-offset;
  if(n>18)n=18;
  fragment[0]=14;fragment[1]=7;fragment[2]=0x68;fragment[3]=tid;fragment[4]=0x20|index;fragment[5]=n;
  memcpy(fragment+6,packet+offset,n);receive(fragment,n+6,false);
 }
}
int main(void) {
 uint8_t net[16],app[16];unhex("7dd7364cd842ad18c17c2b820c84c3d6",net);unhex("63964771734fbd76e3b40519d1d94a48",app);
 assert(ble_mesh_pairing_start()==ESP_ERR_INVALID_STATE);
 assert(ble_mesh_pairing_configure(mac,3,net,app,0x1201,0x7ff0,0,0,0,status,NULL)==0);
 assert(strcmp(last_status,"Idle")==0 && !advertisements);
 assert(ble_mesh_pairing_radio_start()==0);ble_mesh_pairing_poll();
 assert(ble_mesh_pairing_start()==0 && ble_mesh_pairing_busy());
 assert(ble_mesh_pairing_start()==ESP_ERR_INVALID_STATE);
 ble_mesh_pairing_poll();assert(advertisements==0);clock_ms+=12000;ble_mesh_pairing_poll();assert(last_adv[7]==14 && last_adv[8]==0 && memcmp(last_adv+11,last_adv+19,8)==0);
 uint8_t confirmation[20]={14,1,0x55,0x66};for(size_t i=0;i<16;i++)confirmation[4+i]=i;
 receive(confirmation,20,true);assert(strcmp(last_status,"Waiting for lamp")==0);
 receive(confirmation,19,false);assert(strcmp(last_status,"Waiting for lamp")==0);
 receive(confirmation,20,false);assert(strcmp(last_status,"Restoring network")==0 && last_adv[8]==2);
 uint8_t expected[24];assert(unhex("0e02c2e0713c604e0cb2beda738ec43cf4477ba6b4cc0888",expected)==24);assert(memcmp(last_adv+7,expected,24)==0);
 uint8_t complete[8]={14,3,0x11,0x22,0x33,0x44,0x55,0x66};receive(complete,8,false);
 assert(strcmp(last_status,"Installing application key")==0 && last_adv[8]==6 && nvs_sequence==0x200000);
 mesh("6870737bb0b7e92c930609cf073e002fdae00370107b86da",1,false);assert(ble_mesh_pairing_busy());
 mesh("6853034ff0c1410f42b981bb7408f49e79b0576ec8586428",2,true);assert(ble_mesh_pairing_busy());
 mesh("6853034ff0c1410f42b981bb7408f49e79b0576ec8586428",3,false);assert(strcmp(last_status,"Paired")==0 && !ble_mesh_pairing_busy());
 unsigned sent=advertisements;clock_ms+=61000;ble_mesh_pairing_poll();assert(advertisements==sent);
 mesh("688a23540175d532b954c261c47191c55b252ffec7e8856f7e",4,false);assert(onoff_reports==1);
 mesh("688a23540175d532b954c261c47191c55b252ffec7e8856f7e",4,false);assert(onoff_reports==1);
 assert(ble_mesh_pairing_start()==0);clock_ms+=12000;ble_mesh_pairing_poll();assert(last_adv[8]==0);
 sent=advertisements;clock_ms+=60000;ble_mesh_pairing_poll();assert(!ble_mesh_pairing_busy());
 assert(strcmp(last_status,"Failed: pairing timed out")==0 && advertisements==sent);
 assert(ble_mesh_pairing_start()==0);
 ble_mesh_pairing_poll();sent=advertisements;
 ble_mesh_pairing_confirm_existing();ble_mesh_pairing_poll();
 assert(strcmp(last_status,"Paired")==0 && !ble_mesh_pairing_busy());
 clock_ms+=1000;ble_mesh_pairing_poll();assert(advertisements==sent);
 puts("PASS: explicit start, MAC/length guards, complete handshake, invalid-key/MIC rejection, replay suppression, timeout and existing-pairing confirmation");
}
