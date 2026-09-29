#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <signal.h>

//for timerfd
#include <sys/timerfd.h>
#include <time.h>

#include "./ini.h"



#define RESET_TIMER(fd) do { struct itimerspec s; timerfd_gettime(fd, &s); s.it_value = s.it_interval; timerfd_settime(fd, 0, &s, NULL); } while(0)



//#define HEAD_NUMBER 3
//#define FIRST_CH  (HEAD_NUMBER * 2 - 1) * 16
//#define LAST_CH  FIRST_CH + 31

uint8_t head_number;
uint8_t first_ch;
uint8_t last_ch;
const char* interfaceCan;





uint32_t channelStatus = 0;

#define bitRead(value, bit)            (((value) >> (bit)) & 0x01)
#define bitSet(value, bit)             ((value) |= (1UL << (bit)))
#define bitClear(value, bit)           ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit)          ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, bitval)   ((bitval) ? bitSet(value, bit) : bitClear(value, bit))

  

static int handlerIni(void* user, const char* section, const char* name,
                   const char* value){

     //struct main_config *pconfig = (struct main_config*)user;

     #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
     if (MATCH("head", "number")) {
        head_number = atoi(value);
        first_ch = (head_number * 2 -1) * 16;
        last_ch = first_ch + 31;
     } else 
     if (MATCH("system", "can")) {
        interfaceCan = strdup(value);

     }
     return 1;


}


// Callback function type
typedef uint8_t (*can_frame_callback_t)(const struct can_frame *frame);

// Initializes the vcan0 interface
int init_can_interface(const char *ifname) {
    int sock;
    struct sockaddr_can addr;
    struct ifreq ifr;

    if ((sock = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl error (interface may be down)");
        close(sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Bind error");
        close(sock);
        return -1;
    }

    return sock;
}



int init_timer(int second)
{
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd < 0) {
        perror("Error timerfd_create");
        return -1;
    }

    struct itimerspec update_interval;

    update_interval.it_value.tv_sec = second; 
    update_interval.it_value.tv_nsec = 0;

    update_interval.it_interval.tv_sec = second;
    update_interval.it_interval.tv_nsec = 0;
    if (timerfd_settime(timer_fd, 0, &update_interval, NULL) < 0) {
        perror("Error timerfd_settime");
        close(timer_fd);
        return -1;
    }
    
    return timer_fd;

}




 

ssize_t send_channel_status(int socketCan){

    struct can_frame txCanFrame;

    if (socketCan < 0) {
        fprintf(stderr, "[CAN TX ERROR]\n");
        return -1;
    }


    txCanFrame.can_id = 0x100;
    txCanFrame.can_dlc = 5;
    txCanFrame.data[0] = head_number;
    txCanFrame.data[1] = (channelStatus >> 0) & 0xFF;
    txCanFrame.data[2] = (channelStatus >> 8) & 0xFF;
    txCanFrame.data[3] = (channelStatus >> 16) & 0xFF;
    txCanFrame.data[4] = (channelStatus >> 24) & 0xFF;

    ssize_t bytesSent = write(socketCan, &txCanFrame, sizeof(struct can_frame));

    return bytesSent;
}



// Callback for can
uint8_t on_can_message_received(const struct can_frame *frame) {
    uint8_t channelState;
    uint8_t status;
    uint8_t needUpdate = 0;
    

    if (frame->can_id >= first_ch && frame->can_id <= last_ch){
        channelState = frame->can_id - first_ch;
        status = frame->data[0];
        if (status > 1){
            bitToggle(channelStatus, channelState);
            needUpdate = 1;
        } else {
            if (bitRead(channelStatus, channelState) != status){//if change bit
                bitWrite(channelStatus, channelState, status);
                needUpdate = 1;
            }
        }
        
    }

    return needUpdate;
}


void processing_loop(int socket_can,int timer_fd, can_frame_callback_t callback) {


   
    
    struct pollfd fds[2];
    struct can_frame frame;


    fds[0].fd = socket_can;
    fds[0].events = POLLIN; 

    fds[1].fd = timer_fd;
    fds[1].events = POLLIN;

    while (1) {
        
        int ret = poll(fds, 2, -1);

        if (ret < 0) {
            perror("Error poll");
            break;
        }

        
        if (fds[0].revents & POLLIN) {
            ssize_t nbytes = read(socket_can, &frame, sizeof(struct can_frame));
            
            if (nbytes < 0) {
                perror("error read SocketCAN");
                break;
            }

            if (nbytes == sizeof(struct can_frame) && callback != NULL) {
                if (callback(&frame)){
                    send_channel_status(socket_can);
                    RESET_TIMER(timer_fd);
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            uint64_t tmp;
            read(timer_fd, &tmp, sizeof(tmp)); 
            send_channel_status(socket_can);
        }



    }
}




void startFork()
{
    pid_t pid;
    pid = fork();
    if ( pid > 0){
        exit(EXIT_SUCCESS);
    }
    setsid();
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
   
}




int main() {
    
    #ifndef NO_FORK
        startFork();
    #endif

     if (ini_parse("./config.ini", handlerIni, NULL) < 0) {
         //printf("Can't load 'test.ini'\n");
       //  return 1;
     }

    
    int socket_can = init_can_interface(interfaceCan);
    if (socket_can < 0) {
        return 1;
    }

    int timer_fd = init_timer(30);
    if (timer_fd < 0) {
        return 1;
    }
   
    // run loop
    processing_loop(socket_can, timer_fd, on_can_message_received);

    // Закрываем сокет при выходе из цикла
    close(socket_can);
    return 0;
}