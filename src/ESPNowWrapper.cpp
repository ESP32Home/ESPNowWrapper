#include "ESPNowWrapper.h"

#include <ESPStringUtils.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"

#include "esp_now.h"

static xQueueHandle recv_queue;

#define ESPNOW_QUEUE_SIZE 6
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    const uint8_t *data;
    int data_len;
} event_recv_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} event_send_cb_t;


//reduced to singleton instance as the path such as mesh_event_handler, does not have data
MeshCallback message_callback = nullptr;

#if 0
static void espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    int recv_magic = 0;
    int ret;

    while (xQueueReceive(recv_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case EXAMPLE_ESPNOW_SEND_CB:
            {
                example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                if(IS_BROADCAST_ADDR(send_cb->mac_addr)){
                    Serial.printf("  callback> Broadcasted data, status1: %d", send_cb->status);
                }else{
                    String dest = hextab_to_string(send_cb->mac_addr);
                    Serial.printf("  callback> Sent data to %s, status1: %d", dest.c_str(), send_cb->status);
                }
                break;
            }
            case EXAMPLE_ESPNOW_RECV_CB:
            {
                example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                ret = example_espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_state, &recv_seq, &recv_magic);
                free(recv_cb->data);

                if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST) {
                    ESP_LOGI(TAG, "Receive %dth broadcast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    /* If MAC address does not exist in peer list, add it to peer list. */
                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
                        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                        if (peer == NULL) {
                            ESP_LOGE(TAG, "Malloc peer information fail");
                            example_espnow_deinit(send_param);
                            vTaskDelete(NULL);
                        }
                        memset(peer, 0, sizeof(esp_now_peer_info_t));
                        peer->channel = CONFIG_ESPNOW_CHANNEL;
                        peer->ifidx = ESPNOW_WIFI_IF;
                        peer->encrypt = true;
                        memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                        memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK( esp_now_add_peer(peer) );
                        free(peer);
                    }

                    /* Indicates that the device has received broadcast ESPNOW data. */
                    if (send_param->state == 0) {
                        send_param->state = 1;
                    }

                    /* If receive broadcast ESPNOW data which indicates that the other device has received
                     * broadcast ESPNOW data and the local magic number is bigger than that in the received
                     * broadcast ESPNOW data, stop sending broadcast ESPNOW data and start sending unicast
                     * ESPNOW data.
                     */
                    if (recv_state == 1) {
                        /* The device which has the bigger magic number sends ESPNOW data, the other one
                         * receives ESPNOW data.
                         */
                        if (send_param->unicast == false && send_param->magic >= recv_magic) {
                    	    ESP_LOGI(TAG, "Start sending unicast data");
                    	    ESP_LOGI(TAG, "send data to "MACSTR"", MAC2STR(recv_cb->mac_addr));

                    	    /* Start sending unicast ESPNOW data. */
                            memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                            example_espnow_data_prepare(send_param);
                            if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                                ESP_LOGE(TAG, "Send error");
                                example_espnow_deinit(send_param);
                                vTaskDelete(NULL);
                            }
                            else {
                                send_param->broadcast = false;
                                send_param->unicast = true;
                            }
                        }
                    }
                }
                else if (ret == EXAMPLE_ESPNOW_DATA_UNICAST) {
                    ESP_LOGI(TAG, "Receive %dth unicast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);
                }
                else {
                    ESP_LOGI(TAG, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                break;
            }
            default:
                Serial.printf("Error> Callback type error: %d\n", evt.id);
                break;
        }
    }
}

#endif

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

    //send_cb.mac_addr
    evt.status = status;
    if (xQueueSend(recv_queue, &evt, portMAX_DELAY) != pdTRUE) {
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
    //evt.mac_addr
    evt.data        = data;
    evt.data_len    = len;
    if (xQueueSend(recv_queue, &evt, portMAX_DELAY) != pdTRUE) {
        Serial.printf("  Warning> Send receive queue fail\n");
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
        Serial.print("Error> Create queue fail");
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
    //String ffff = "ff:ff:ff:ff:ff:ff";
    //string_to_hextab(ffff,bcast_peer.peer_addr);
    string_to_hextab("ff:ff:ff:ff:ff:ff",bcast_peer.peer_addr);
    
    err = esp_now_add_peer(&bcast_peer);
        if(err != ESP_OK)Serial.printf("esp_now_add_peer: 0x%X\n",err);

    //xTaskCreate(espnow_task, "espnow_task", 2048, NULL, 4, NULL);

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
    Serial.printf("send > => '%s' : '%s'\n",dest.c_str(),message.c_str());
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