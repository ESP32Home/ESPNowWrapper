#include "ESPNowWrapper.h"

#include <ESPStringUtils.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"

#include "esp_now.h"

static xQueueHandle recv_queue;
static xQueueHandle send_queue;

#define ESPNOW_QUEUE_SIZE 6
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int data_len;
} event_recv_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} event_send_cb_t;


//reduced to singleton instance as the path such as mesh_event_handler, does not have data
MeshCallback message_callback = nullptr;

static void espnow_recv_cb_task(void *pvParameter)
{
    event_recv_cb_t evt;

    while (xQueueReceive(recv_queue, &evt, portMAX_DELAY) == pdTRUE) {
        String from = hextab_to_string(evt.mac_addr);
        evt.data[evt.data_len] = '\0';//null terminated string
        String payload(reinterpret_cast<char*>(evt.data));
        message_callback(payload,from);
        delete[] evt.data;
    }
}

static void espnow_send_cb_task(void *pvParameter)
{
    event_send_cb_t evt;

    while (xQueueReceive(send_queue, &evt, portMAX_DELAY) == pdTRUE) {
        String dest = hextab_to_string(evt.mac_addr);
        Serial.printf("  send_cb> Sent data to %s, status: %d\n", dest.c_str(), evt.status);
    }
}

NowApp::NowApp(){
}

NowApp::~NowApp(){
}

static esp_err_t example_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        Serial.print("WiFi started");
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    event_send_cb_t evt;

    if (mac_addr == NULL) {
        Serial.printf("  Error> Send cb arg error\n");
        return;
    }

    memcpy(evt.mac_addr,mac_addr,ESP_NOW_ETH_ALEN);
    evt.status = status;
    if (xQueueSend(send_queue, &evt, portMAX_DELAY) != pdTRUE) {
        Serial.printf("  Warning> Send send queue fail\n");
    }
}

static void example_espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (mac_addr == NULL || data == NULL || len <= 0) {
        Serial.printf("  Error> Receive cb arg error\n");
        return;
    }
    event_recv_cb_t evt;
    memcpy(evt.mac_addr,mac_addr,ESP_NOW_ETH_ALEN);
    evt.data = new uint8_t[len+1];//for null terminated string
    memcpy(evt.data,data,len);
    evt.data_len    = len;
    if (xQueueSend(recv_queue, &evt, portMAX_DELAY) != pdTRUE) {
        Serial.printf("  Warning> Send receive queue fail\n");
        delete[] evt.data;
    }
}

bool NowApp::start(DynamicJsonDocument &config,DynamicJsonDocument &secret){

    if(!config.containsKey("espnow")){
        Serial.printf("  Error> config has no key 'espnow'");
        return false;
    }
    if(!config.containsKey("wifi")){
        Serial.printf("  Error> config has no key 'wifi'");
        return false;
    }
    if(!secret.containsKey("wifi")){
        Serial.printf("  Error> secret has no key 'wifi'");
        return false;
    }
    tcpip_adapter_init();
    esp_err_t err;
    err = esp_event_loop_init(example_event_handler, NULL);
        if(err != ESP_OK)Serial.printf("esp_event_loop_init: 0x%X\n",err);
    
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
        if(err != ESP_OK)Serial.printf("esp_wifi_initt: 0x%X\n",err);
    String wifi = config["wifi"]["mode"];
    if(wifi.compareTo("STA")){
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }else{
        err = esp_wifi_set_mode(WIFI_MODE_AP);
    }
        if(err != ESP_OK)Serial.printf("esp_wifi_set_mode: 0x%X\n",err);
    err = esp_wifi_start();
        if(err != ESP_OK)Serial.printf("esp_wifi_start: 0x%X\n",err);



    //uint8_t channel = config["wifi"]["channel"];
    //err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    //    if(err != ESP_OK)Serial.printf("esp_wifi_set_channel: 0x%X\n",err);

    //-----------------------------     ESP NOW    -----------------------------

    recv_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(event_recv_cb_t));
    if (recv_queue == NULL) {
        Serial.print("Error> Create recv queue fail");
        return ESP_FAIL;
    }
    send_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(event_send_cb_t));
    if (send_queue == NULL) {
        Serial.print("Error> Create send queue fail");
        return ESP_FAIL;
    }

    err = esp_now_init();
        if(err != ESP_OK)Serial.printf("esp_now_init: 0x%X\n",err);
    
    err = esp_now_register_send_cb(example_espnow_send_cb);
        if(err != ESP_OK)Serial.printf("esp_now_register_send_cb: 0x%X\n",err);
    err = esp_now_register_recv_cb(example_espnow_recv_cb);
        if(err != ESP_OK)Serial.printf("esp_now_register_recv_cb: 0x%X\n",err);



    err = esp_now_set_pmk((uint8_t *)"pmk1234567890123");
        if(err != ESP_OK)Serial.printf("esp_now_set_pmk: 0x%X\n",err);

    esp_now_peer_info_t bcast_peer;
    memset(&bcast_peer, 0, sizeof(esp_now_peer_info_t));
    bcast_peer.channel = config["espnow"]["channel"];

    if(wifi.compareTo("STA")){
        bcast_peer.ifidx = ESP_IF_WIFI_STA;
    }else{
        bcast_peer.ifidx = ESP_IF_WIFI_AP;
    }
    bcast_peer.encrypt = false;
    string_to_hextab("ff:ff:ff:ff:ff:ff",bcast_peer.peer_addr);
    
    err = esp_now_add_peer(&bcast_peer);
        if(err != ESP_OK)Serial.printf("esp_now_add_peer: 0x%X\n",err);

    xTaskCreate(espnow_send_cb_task, "espnow_send_cb_task", 2048, NULL, 4, NULL);
    xTaskCreate(espnow_recv_cb_task, "espnow_recv_cb_task", 2048, NULL, 4, NULL);

    return true;
}

void NowApp::onMessage(MeshCallback cb){
    message_callback = cb;
}

bool NowApp::send(const String &dest, const String &message){

    uint8_t mac[ESP_NOW_ETH_ALEN];
    esp_err_t err;

    string_to_hextab(dest,mac);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(message.c_str());
    Serial.printf("send => (%s) '%s'\n",dest.c_str(),message.c_str());
    err = esp_now_send(mac, data, message.length());
    if(err == ESP_OK){
        return true;
    }
    else
    {
        Serial.printf("esp_now_send: 0x%X\n",err);
        return false;
    }
}

bool NowApp::broadcast(String &message){
    return send("ff:ff:ff:ff:ff:ff",message);
}

void NowApp::print_info(){
    esp_err_t err;
    String info = "";
    uint32_t version;
    err = esp_now_get_version(&version);
    if(err == ESP_OK){
        info += " Version="+String(version)+"; ";
    }
    uint8_t channel;
    wifi_second_chan_t sec;
    err = esp_wifi_get_channel(&channel,&sec);
    if(err == ESP_OK){
        info += " channel="+String(channel)+"; ";
    }

    esp_now_peer_num_t peer_num;
    err = esp_now_get_peer_num(&peer_num);
    if(err == ESP_OK){
        if(peer_num.total_num != 0){
            info += " peers="+String(peer_num.total_num)+" encrypted="+String(peer_num.encrypt_num)+"; ";
        }else{
            info += " no peers; ";
        }
    }

    Serial.println(info);
}
