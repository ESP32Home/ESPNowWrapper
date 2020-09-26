#include "ESPMeshWrapper.h"

#include <ESPStringUtils.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"

#include "esp_now.h"
#include "espnow_example.h"

static xQueueHandle s_example_espnow_queue;

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_example_espnow_seq[EXAMPLE_ESPNOW_DATA_MAX] = { 0, 0 };

//reduced to singleton instance as the path such as mesh_event_handler, does not have data
MeshCallback message_callback = nullptr;


static void example_espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    int recv_magic = 0;
    bool is_broadcast = false;
    int ret;

    vTaskDelay(5000 / portTICK_RATE_MS);
    ESP_LOGI(TAG, "Start sending broadcast data");

    /* Start sending broadcast ESPNOW data. */
    example_espnow_send_param_t *send_param = (example_espnow_send_param_t *)pvParameter;
    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
        ESP_LOGE(TAG, "Send error");
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

    while (xQueueReceive(s_example_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case EXAMPLE_ESPNOW_SEND_CB:
            {
                example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

                ESP_LOGD(TAG, "Send data to "MACSTR", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);

                if (is_broadcast && (send_param->broadcast == false)) {
                    break;
                }

                if (!is_broadcast) {
                    send_param->count--;
                    if (send_param->count == 0) {
                        ESP_LOGI(TAG, "Send done");
                        example_espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    }
                }

                /* Delay a while before sending the next data. */
                if (send_param->delay > 0) {
                    vTaskDelay(send_param->delay/portTICK_RATE_MS);
                }

                ESP_LOGI(TAG, "send data to "MACSTR"", MAC2STR(send_cb->mac_addr));

                memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
                example_espnow_data_prepare(send_param);

                /* Send the next data after the previous data is sent. */
                if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                    ESP_LOGE(TAG, "Send error");
                    example_espnow_deinit(send_param);
                    vTaskDelete(NULL);
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

                    /* If receive unicast ESPNOW data, also stop sending broadcast ESPNOW data. */
                    send_param->broadcast = false;
                }
                else {
                    ESP_LOGI(TAG, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", evt.id);
                break;
        }
    }
}

MeshApp::MeshApp(){
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
    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_example_espnow_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

static void example_espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_example_espnow_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

void example_espnow_data_prepare(example_espnow_send_param_t *send_param)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(example_espnow_data_t));

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? EXAMPLE_ESPNOW_DATA_BROADCAST : EXAMPLE_ESPNOW_DATA_UNICAST;
    buf->state = send_param->state;
    buf->seq_num = s_example_espnow_seq[buf->type]++;
    buf->crc = 0;
    buf->magic = send_param->magic;
    /* Fill all remaining bytes after the data with random values */
    esp_fill_random(buf->payload, send_param->len - sizeof(example_espnow_data_t));
    buf->crc = crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

bool MeshApp::start(DynamicJsonDocument &config,DynamicJsonDocument &secret){

    if(!config.containsKey("espnow")){
        Serial.printf("Error> config has no key 'espnow'");
        return false;
    }
    if(!config.containsKey("wifi")){
        Serial.printf("Error> config has no key 'wifi'");
        return false;
    }
    if(!secret.containsKey("wifi")){
        Serial.printf("Error> secret has no key 'wifi'");
        return false;
    }
    tcpip_adapter_init();
    esp_err_t err;
    err = esp_event_loop_init(example_event_handler, NULL);
        if(err != ESP_OK)Serial.printf("esp_event_loop_init: 0x%X\n",err);
    
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
        if(err != ESP_OK)Serial.printf("esp_wifi_initt: 0x%X\n",err);
    String wifi(config["wifi"]["mode"]);
    if(wifi.compareTo("STA")){
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }else{
        err = esp_wifi_set_mode(WIFI_MODE_AP);
    }
        if(err != ESP_OK)Serial.printf("esp_wifi_set_mode: 0x%X\n",err);
    err = esp_wifi_start();
        if(err != ESP_OK)Serial.printf("esp_wifi_start: 0x%X\n",err);
    err = esp_wifi_set_channel(config["wifi"]["channel"], 0);
        if(err != ESP_OK)Serial.printf("esp_wifi_set_channel: 0x%X\n",err);

    //-----------------------------     ESP NOW    -----------------------------

    s_example_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_example_espnow_queue == NULL) {
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

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(esp_now_peer_info_t));
    peer.channel = config["espnow"]["channel"];

    if(wifi.compareTo("STA")){
        peer.ifidx = WIFI_MODE_STA;
    }else{
        peer.ifidx = WIFI_MODE_AP;
    }
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    
    err = esp_now_add_peer(peer);
        if(err != ESP_OK)Serial.printf("esp_now_add_peer: 0x%X\n",err);


    example_espnow_send_param_t send_param;
    memset(&send_param, 0, sizeof(example_espnow_send_param_t));
    send_param.unicast = false;
    send_param.broadcast = true;
    send_param.state = 0;
    send_param.magic = esp_random();
    send_param.count = config["espnow"]["send_count"];
    send_param.delay = config["espnow"]["send_delay"];
    send_param.len =   config["espnow"]["send_len"];
    send_param.buffer = buffer;//fixed to 200 TODO alloc and free on destruction
    memcpy(send_param.dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);

    example_espnow_data_prepare(send_param);

    xTaskCreate(example_espnow_task, "example_espnow_task", 2048, send_param, 4, NULL);

    return res;
}

void MeshApp::onMessage(MeshCallback cb){
    message_callback = cb;
}

void MeshApp::print_info(){
    esp_err_t err;
    String info;
    info = "*Layer="+String(esp_mesh_get_layer());

    mesh_addr_t bssid;
    err = esp_mesh_get_parent_bssid(&bssid);
    if(err == ESP_OK){
        info += " Parent="+hextab_to_string(bssid.addr)+" ";

    }
    if(!esp_mesh_is_root()){
        info += " Not";
    }
    info += " root  ";

    info += "NbNodes="+String(esp_mesh_get_total_node_num());
    info += "  TableSize="+String(esp_mesh_get_routing_table_size());


    Serial.println(info);
}