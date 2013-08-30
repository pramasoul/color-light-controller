/* UDP light control */
// RTOS at http://www.keil.com/pack/doc/arm/cmsis/cmsis/documentation/rtos/html/index.html

#include "mbed.h"
#include "rtos.h"
#include "EthernetInterface.h"

#define BUFFER_SIZE        1540
#define PORT      2327

PwmOut led1(LED1);
AnalogIn pot(p20);
DigitalIn pushbutton(p8);
PwmOut red(p21), green(p22), blue(p23), white(p24);
EthernetInterface eth;
char buffer[BUFFER_SIZE];

PwmOut *subject_color;
//PwmOut *color_list[] = { &led1, &red, &green, &blue, &white };

void watch_pot(void const *args) {
    char pushbutton_state, prior_pushbutton_state = pushbutton;
    PwmOut *color_list[] = { &led1, &red, &green, &blue, &white };
    char color_list_index = 0;
    float filtered_v = pot.read();
    while (true) {
        filtered_v = 0.8 * filtered_v + 0.2 * pot.read();
        *subject_color = filtered_v  *filtered_v;
        pushbutton_state = pushbutton;
        if (!prior_pushbutton_state && pushbutton_state) {
            color_list_index = (color_list_index + 1) % 5;
            subject_color = color_list[color_list_index];
        }
        //printf("\r%4.4s", pushbutton ? "up" : "down");
        //fflush(stdout);
        prior_pushbutton_state = pushbutton_state;
        Thread::wait(50);
    }
}

void service_net(void const *args) {
/*
    red = green = blue = white = 0.0;
    red.period(0.005);
    green.period(0.005);
    blue.period(0.005);
    white.period(0.005);
 */   
    float redCmd, greenCmd, blueCmd, whiteCmd;
        
    eth.init(); //Use DHCP
    eth.connect();
    //printf("DHCP got me IP Address %s\n", eth.getIPAddress());
        
    UDPSocket server;
    Endpoint endp;
    int receiveTimeouts, receiveErrors;
    receiveTimeouts = receiveErrors = 0;
    
    char cmdVersion;

// Without setting address, an Endpoint accepts from any    
//    printf("Remote endpoint @ %s:%d\n",REMOTE_IP,PORT);
//    endp.set_address(REMOTE_IP,PORT);
    
    printf("Binding result :%d\n", server.bind(PORT));
    
    server.set_blocking(false,1000);
    
    while(true)
    {
        if(true)
        {
            int size;
            size = server.receiveFrom(endp,buffer,sizeof(buffer));
            if(size <= 0)
            {
                if(!size)
                    //printf("Receive timeout\n");
                    receiveTimeouts++;
                else
                    //printf("Receive returned error\n");
                    receiveErrors++;
            }
            else
            {
                buffer[size]='\0';
                if (buffer[0] == 'a') {
                    sscanf(buffer, "%c%f%f%f%f", &cmdVersion, &redCmd, &greenCmd, &blueCmd, &whiteCmd);
                    //printf("version '%c', red %f, green %f, blue %f, white %f\n", cmdVersion, redCmd, greenCmd, blueCmd, whiteCmd);
                    red = redCmd; green = greenCmd; blue = blueCmd; white = whiteCmd;
                }
            }
        }
        //led1 = !led1;
        //Thread::wait(200);
    }
    //server.close();
}

void setup() {
    red.period(0.005);
/*    green.period(0.005);
    blue.period(0.005);
    white.period(0.005); */
    red = green = blue = white = 0.0;
    pushbutton.mode(PullUp);
    subject_color = &red;
}

int main() {
    setup();
    printf("\r\nUDP light control 0.0.9\r\n");
    fflush(stdout);
    Thread service_net_thread(service_net);
    Thread watch_pot_thread(watch_pot);
    while (true) {
        //led1 = !led1;
        Thread::wait(400);
    }
}
