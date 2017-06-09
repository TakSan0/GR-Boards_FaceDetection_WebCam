#include "mbed.h"
#include "SdUsbConnect.h"
#include "opencv.hpp"
#include "camera_if.hpp"
#include "face_detector.hpp"
#include "DisplayApp.h"
#include "rtos.h"
#include "HTTPServer.h"
#include "FATFileSystem.h"
#include "RomRamBlockDevice.h"
#include "file_table.h"

#define DBG_CAPTURE   (0)
#define DBG_PCMONITOR (0)
#define FACE_DETECTOR_MODEL     "/storage/lbpcascade_frontalface.xml"

/**** User Selection *********/
/** Network setting **/
#define USE_DHCP               (1)                 /* Select  0(static configuration) or 1(use DHCP) */
#if (USE_DHCP == 0)
  #define IP_ADDRESS           ("192.168.0.2")     /* IP address      */
  #define SUBNET_MASK          ("255.255.255.0")   /* Subnet mask     */
  #define DEFAULT_GATEWAY      ("192.168.0.3")     /* Default gateway */
#endif
#define NETWORK_TYPE           (3)                 /* Select  0(Ethernet), 1(BP3595), 2(ESP32 STA) ,3(ESP32 AP) */
#if (NETWORK_TYPE >= 1)
  #define WLAN_SSID            ("ESP32-lychee")    /* SSID */
  #define WLAN_PSK             ("1234567890")      /* PSK(Pre-Shared Key) */
  #define WLAN_SECURITY        NSAPI_SECURITY_WPA2 /* NSAPI_SECURITY_NONE, NSAPI_SECURITY_WEP, NSAPI_SECURITY_WPA, NSAPI_SECURITY_WPA2 or NSAPI_SECURITY_WPA_WPA2 */
#endif
/*****************************/

#if (NETWORK_TYPE == 0)
  #include "EthernetInterface.h"
  EthernetInterface network;
#elif (NETWORK_TYPE == 1)
  #include "LWIPBP3595Interface.h"
  LWIPBP3595Interface network;
#elif (NETWORK_TYPE == 2)
  #include "ESP32Interface.h"
  ESP32Interface network(P5_3, P3_14, P3_15, P0_2);
#elif (NETWORK_TYPE == 3)
  #include "ESP32InterfaceAP.h"
  ESP32InterfaceAP network(P5_3, P3_14, P3_15, P0_2);
#else
  #error NETWORK_TYPE error
#endif /* NETWORK_TYPE */

using namespace cv;

/* Application variables */
Mat frame_gray;     // Input frame (in grayscale)

#if (DBG_PCMONITOR == 1)
/* For viewing image on PC */
static DisplayApp  display_app;
#endif

DigitalOut led1(LED1);
DigitalOut led2(LED2);
DigitalOut led3(LED3);
DigitalOut led4(LED4);

Thread httpTask(osPriorityNormal, (1024 * 4));

FATFileSystem fs("storage");
RomRamBlockDevice romram_bd(512000, 512);
rtos::Mutex mtx_data;

static char detect_str[32];
static char detect_str_send[32];
Timer  detect_timer;

static int snapshot_req(const char* rootPath, const char ** pp_data) {
    if (strcmp(rootPath, "/camera") == 0) {
        int jpeg_size = (int)create_jpeg();
        *pp_data = (const char *)get_jpeg_adr();

        mtx_data.lock();
        if (detect_timer.read_ms() < 1000) {
            memcpy(detect_str_send, detect_str, sizeof(detect_str_send));
        } else {
            sprintf(detect_str_send, "0,0,0,0");
        }
        mtx_data.unlock();

        return jpeg_size;
    } else {
        *pp_data = (const char *)detect_str_send;
        return strlen(detect_str_send);
    }
}

static void mount_romramfs(void) {
    FILE * fp;

    romram_bd.SetRomAddr(0x18000000, 0x1FFFFFFF);
    fs.format(&romram_bd, 512);
    fs.mount(&romram_bd);

    //index.htm
    fp = fopen("/storage/index.htm", "w");
    fwrite(index_htm_tbl, sizeof(char), sizeof(index_htm_tbl), fp);
    fclose(fp);
}

void http_task(void) {
    mount_romramfs();

#if (USE_DHCP == 0)
    network.set_dhcp(false);
    if (network.set_network(IP_ADDRESS, SUBNET_MASK, DEFAULT_GATEWAY) != 0) { //for Static IP Address (IPAddress, NetMasks, Gateway)
        printf("Network Set Network Error \r\n");
    }
#endif

#if (NETWORK_TYPE >= 1)
    network.set_credentials(WLAN_SSID, WLAN_PSK, WLAN_SECURITY);
#endif

    printf("\r\nConnecting...\r\n");
    if (network.connect() != 0) {
        printf("Network Connect Error \r\n");
        return;
    }
    printf("MAC Address is %s\r\n", network.get_mac_address());
    printf("IP Address is %s\r\n", network.get_ip_address());
    printf("NetMask is %s\r\n", network.get_netmask());
    printf("Gateway Address is %s\r\n", network.get_gateway());
    printf("Network Setup OK\r\n");

    SnapshotHandler::attach_req(&snapshot_req);
    HTTPServerAddHandler<SnapshotHandler>("/camera");
    HTTPServerAddHandler<SnapshotHandler>("/pos");
    FSHandler::mount("/storage", "/");
    HTTPServerAddHandler<FSHandler>("/");
    HTTPServerStart(&network, 80);
}

int main() {
#if (DBG_CAPTURE == 1)
    char file_name[32];
    int file_name_index_detected = 1;
#endif

    printf("GR-Boards_FaceDetection_WebCam\r\n");

    // Camera
    camera_start();
    led4 = 1;

    // SD & USB
    SdUsbConnect storage("storage");
    printf("Finding a storage...");
    // wait for the storage device to be connected
    storage.wait_connect();
    printf("done\n");
    led3 = 1;

    // Initialize the face detector
    printf("Initializing the face detector...");
    detectFaceInit(FACE_DETECTOR_MODEL);
    printf("done\n");
    led2 = 1;

    mtx_data.lock();
    sprintf(detect_str, "0,0,0,0");
    detect_timer.reset();
    detect_timer.start();
    mtx_data.unlock();

    httpTask.start(&http_task);

    while (1) {
        // Retrieve a video frame (grayscale)
        create_gray(frame_gray);
        if (frame_gray.empty()) {
            printf("ERR: There is no input frame, retry to capture\n");
            continue;
        }

        // Detect a face in the frame
        Rect face_roi;
        detectFace(frame_gray, face_roi);

        if (face_roi.width > 0 && face_roi.height > 0) {   // A face is detected
            led1 = 1;
            printf("Detected a face X:%d Y:%d W:%d H:%d\n",face_roi.x, face_roi.y, face_roi.width, face_roi.height);
            mtx_data.lock();
            sprintf(detect_str, "%d,%d,%d,%d", face_roi.x, face_roi.y, face_roi.width, face_roi.height);
            detect_timer.reset();
            mtx_data.unlock();

#if (DBG_CAPTURE == 1)
            sprintf(file_name, "/storage/detected_%d.bmp", file_name_index_detected++);
            imwrite(file_name, frame_gray);
#endif
        } else {
            led1 = 0;
        }

#if (DBG_PCMONITOR == 1)
        size_t jpeg_size = create_jpeg();
        display_app.SendJpeg(get_jpeg_adr(), jpeg_size);
#endif
        Thread::wait(5);
    }
}
