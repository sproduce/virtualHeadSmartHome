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


#include "./ini.h"



#define HEAD_NUMBER 3
#define FIRST_CH  (HEAD_NUMBER * 2 - 1) * 16
#define LAST_CH  FIRST_CH + 31



uint32_t channelStatus = 0;

#define bitRead(value, bit)            (((value) >> (bit)) & 0x01)
#define bitSet(value, bit)             ((value) |= (1UL << (bit)))
#define bitClear(value, bit)           ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit)          ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, bitval)   ((bitval) ? bitSet(value, bit) : bitClear(value, bit))

  


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



ssize_t send_can_message(int socketCan){

    struct can_frame txCanFrame;

    if (socketCan < 0) {
        fprintf(stderr, "[CAN TX ERROR]\n");
        return -1;
    }


    txCanFrame.can_id = 0x100;
    txCanFrame.can_dlc = 5;
    txCanFrame.data[0] = HEAD_NUMBER;
    txCanFrame.data[1] = (channelStatus >> 0) & 0xFF;
    txCanFrame.data[2] = (channelStatus >> 8) & 0xFF;
    txCanFrame.data[3] = (channelStatus >> 16) & 0xFF;
    txCanFrame.data[4] = (channelStatus >> 24) & 0xFF;

    ssize_t bytesSent = write(socketCan, &txCanFrame, sizeof(struct can_frame));

}



// Callback for can
uint8_t on_can_message_received(const struct can_frame *frame) {
    uint8_t channelState;
    uint8_t status;
    uint8_t needUpdate = 0;
    

    if (frame->can_id >= FIRST_CH && frame->can_id <= LAST_CH){
        channelState = frame->can_id - FIRST_CH;
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


void can_processing_loop(int socket_can, can_frame_callback_t callback) {

    struct pollfd fds;

    fds.fd = socket_can;
    fds.events = POLLIN; 

    struct can_frame frame;

    while (1) {
        
        int ret = poll(&fds, 1, -1);

        if (ret < 0) {
            perror("Error poll");
            break;
        }

        
        if (fds.revents & POLLIN) {
            ssize_t nbytes = read(socket_can, &frame, sizeof(struct can_frame));
            
            if (nbytes < 0) {
                perror("error read SocketCAN");
                break;
            }

            if (nbytes == sizeof(struct can_frame) && callback != NULL) {
                if (callback(&frame)){
                    send_can_message(socket_can);
                }
            }
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
    
    int socket_can = init_can_interface("vcan0");
    if (socket_can < 0) {
        return 1;
    }

    // 2. Запускаем бесконечный цикл прослушивания
    can_processing_loop(socket_can, on_can_message_received);

    // Закрываем сокет при выходе из цикла
    close(socket_can);
    return 0;
}