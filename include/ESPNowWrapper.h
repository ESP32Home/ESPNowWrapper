#include <ArduinoJson.h>

typedef void (*MeshCallback)(String &payload,String from);
class NowApp{
    public:
        NowApp();
        ~NowApp();
        bool start(DynamicJsonDocument &config,DynamicJsonDocument &secret);
        void set_channel(uint8_t channel);
        void onMessage(MeshCallback cb);
        bool broadcast(String &message);
        bool send(const String &dest,const String &message);
        void print_info();
};
