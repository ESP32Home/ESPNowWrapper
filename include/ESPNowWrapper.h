#include <ArduinoJson.h>

typedef void (*MeshCallback)(String &payload,String from,int flag);
class NowApp{
    public:
        NowApp();
        bool start(DynamicJsonDocument &config,DynamicJsonDocument &secret);
        void onMessage(MeshCallback cb);
        bool broadcast(String message);
        bool send(String dest, String message);
        void print_info();
    private:
        uint8_t buffer[200];
};
