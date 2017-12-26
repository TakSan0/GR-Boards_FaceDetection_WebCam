#include "mbed.h"
#include "opencv.hpp"
#include "camera_if.hpp"
#include "face_detector.hpp"
#include "DisplayApp.h"
#include "rtos.h"
#include "HTTPServer.h"
#include "FATFileSystem.h"
#include "RomRamBlockDevice.h"
#include "file_table.h"

#define DBG_PCMONITOR (1)
#define FACE_DETECTOR_MODEL     "/storage/lbpcascade_frontalface.xml"

/**** User Selection *********/
/** Network setting **/
#define USE_DHCP               (1)                 /* Select  0(static configuration) or 1(use DHCP) */
#if (USE_DHCP == 0)
  #define IP_ADDRESS           ("192.168.0.2")     /* IP address      */
  #define SUBNET_MASK          ("255.255.255.0")   /* Subnet mask     */
  #define DEFAULT_GATEWAY      ("192.168.0.3")     /* Default gateway */
#endif
#define NETWORK_TYPE           (3)                 /* Select  0(Ethernet), 1(BP3595), 2(ESP32 STA), 3(ESP32 AP) */
#if (NETWORK_TYPE >= 1)
  #define WLAN_SSID            ("ESP32-lychee")    /* SSID */
  #define WLAN_PSK             ("1234567890")      /* PSK(Pre-Shared Key) */
  #define WLAN_SECURITY        NSAPI_SECURITY_WPA2 /* NSAPI_SECURITY_NONE, NSAPI_SECURITY_WEP, NSAPI_SECURITY_WPA, NSAPI_SECURITY_WPA2 or NSAPI_SECURITY_WPA_WPA2 */
#endif
/** JPEG out setting **/
#define JPEG_ENCODE_QUALITY    (15)                /* JPEG encode quality (min:1, max:75 (Considering the size of JpegBuffer, about 75 is the upper limit.)) */ 
#define VFIELD_INT_SKIP_CNT    (0)                 /* A guide for GR-LYCHEE.  0:60fps, 1:30fps, 2:20fps, 3:15fps, 4:12fps, 5:10fps */
/*****************************/

#if (NETWORK_TYPE == 0)
  #include "EthernetInterface.h"
  EthernetInterface network;
#elif (NETWORK_TYPE == 1)
  #include "LWIPBP3595Interface.h"
  #include "LWIPBP3595Interface_BssType.h"
  LWIPBP3595Interface network;
#elif (NETWORK_TYPE == 2)
  #include "ESP32Interface.h"
  #if defined(TARGET_RZ_A1H)
    ESP32Interface network(P3_10, P3_9, P2_14, P2_15, false, NC, NC, 230400);
  #elif defined(TARGET_GR_LYCHEE)
    ESP32Interface network(P5_3, P3_14, P7_1, P0_1, false, NC, NC, 230400);
  #endif
#elif (NETWORK_TYPE == 3)
  #include "ESP32InterfaceAP.h"
  #if defined(TARGET_RZ_A1H)
    ESP32InterfaceAP network(P3_10, P3_9, P2_14, P2_15, false, NC, NC, 230400);
  #elif defined(TARGET_GR_LYCHEE)
    ESP32InterfaceAP network(P5_3, P3_14, P7_1, P0_1, false, NC, NC, 230400);
  #endif
#else
  #error NETWORK_TYPE error
#endif /* NETWORK_TYPE */

#if (USE_DHCP == 0) && ((NETWORK_TYPE == 0) || ((NETWORK_TYPE == 1) && (BSS_TYPE == 0x02)))
#include "DhcpServer.h"
#endif

#if (NETWORK_TYPE == 1)
#include "SDBlockDevice_GRBoard.h"
#else
#include "SdUsbConnect.h"
#endif

using namespace cv;

/* Application variables */
static Mat frame_gray;     // Input frame (in grayscale)

static DigitalOut led1(LED1);
static DigitalOut led2(LED2);
static DigitalOut led3(LED3);
static DigitalOut led4(LED4);

static Thread httpTask(osPriorityAboveNormal, (1024 * 4));
static Thread displayTask(osPriorityAboveNormal, (1024 * 8));

static FATFileSystem fs("storage");
static RomRamBlockDevice romram_bd(512000, 512);
static rtos::Mutex mtx_data;

static char detect_str[32];
static char detect_str_send[32];
static char quality_str[3];
static int image_idx_snapshot = 0;

static int snapshot_req(const char* rootPath, const char* path, const char ** pp_data) {
    if (strcmp(rootPath, "/camera") == 0) {
        uint8_t * jpeg_addr;
        int jpeg_size = (int)get_jpeg_buff(&image_idx_snapshot, &jpeg_addr);
        *pp_data = (const char *)jpeg_addr;
        return jpeg_size;
    } else if (strcmp(rootPath, "/quality") == 0) {
        int quality_resq = SetJpegQuality(atoi(path+1));
        int idx = 0;

        if (quality_resq >= 100) {
            quality_str[idx++] = ((quality_resq / 100) % 10) + 0x30;
        }
        if (quality_resq >= 10) {
            quality_str[idx++] = ((quality_resq /  10) % 10) + 0x30;
        }
        quality_str[idx++] = (quality_resq % 10) + 0x30;
        *pp_data = (const char *)quality_str;
        return idx;
    } else {
        mtx_data.lock();
        memcpy(detect_str_send, detect_str, sizeof(detect_str_send));
        mtx_data.unlock();
        *pp_data = (const char *)detect_str_send;
        return strlen(detect_str_send);
    }
}

static void send_end(const char* rootPath, const char* path, const char * p_data) {
    if (strcmp(rootPath, "/camera") == 0) {
        free_jpeg_buff((uint8_t *)p_data);
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

#if defined(TARGET_RZ_A1H) && (NETWORK_TYPE == 1)
    //Audio Camera Shield USB1 enable for WlanBP3595
    DigitalOut usb1en(P3_8);
    usb1en = 1;        //Outputs high level
    Thread::wait(5);
    usb1en = 0;        //Outputs low level
    Thread::wait(5);
#endif

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

#if (USE_DHCP == 0) && ((NETWORK_TYPE == 0) || ((NETWORK_TYPE == 1) && (BSS_TYPE == 0x02)))
    DhcpServer dhcp_server(&network, "HostName");
#endif

    SnapshotHandler::attach_req(&snapshot_req);
    SnapshotHandler::attach_req_send_end(&send_end);
    HTTPServerAddHandler<SnapshotHandler>("/camera");
    HTTPServerAddHandler<SnapshotHandler>("/quality");
    HTTPServerAddHandler<SnapshotHandler>("/pos");
    FSHandler::mount("/storage", "/");
    HTTPServerAddHandler<FSHandler>("/");
    HTTPServerStart(&network, 80);
}

#if (DBG_PCMONITOR == 1)
static void display_task(void) {
    DisplayApp display_app;
    int image_idx_display = 0;
    uint8_t * jpeg_addr;
    size_t jpeg_size;

    while (1) {
        jpeg_size = get_jpeg_buff(&image_idx_display, &jpeg_addr);
        display_app.SendJpeg(jpeg_addr, jpeg_size);
        free_jpeg_buff(jpeg_addr);
    }
}
#endif

int main() {
    printf("GR-Boards_FaceDetection_WebCam\r\n");

    // Setting of JPEG quality and JPEG frame rate.
    SetJpegQuality(JPEG_ENCODE_QUALITY);
#if (MBED_CONF_APP_CAMERA_TYPE == CAMERA_CVBS)
    // In the case of interlacing, one screen is set by two updates.
    SetVfieldIntSkipCnt((VFIELD_INT_SKIP_CNT * 2) + 1);
#else
    SetVfieldIntSkipCnt(VFIELD_INT_SKIP_CNT);
#endif

    // Camera
    camera_start();
    led4 = 1;

    // SD
    printf("Finding a storage...");
    // wait for the storage device to be connected
#if (NETWORK_TYPE == 1)
    // Since BP3595 is connected to USB1, the file system can not use USB.
    FATFileSystem fs("storage");
    SDBlockDevice_GRBoard sd;
    while (1) {
        if (sd.connect()) {
            fs.mount(&sd);
            break;
        }
        Thread::wait(250);
    }
#else
    SdUsbConnect storage("storage");
    storage.wait_connect();
#endif
    printf("done\n");
    led3 = 1;

    // Initialize the face detector
    printf("Initializing the face detector...");
    detectFaceInit(FACE_DETECTOR_MODEL);
    printf("done\n");
    led2 = 1;

    Timer detect_timer;
    mtx_data.lock();
    sprintf(detect_str, "0,0,0,0");
    mtx_data.unlock();
    detect_timer.reset();
    detect_timer.start();

#if (DBG_PCMONITOR == 1)
    displayTask.start(&display_task);
#endif
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
#if MBED_CONF_APP_LCD
            ClearSquare();
            DrawSquare(face_roi.x, face_roi.y, face_roi.width, face_roi.height, 0x0000f0f0);
#endif
            mtx_data.lock();
            sprintf(detect_str, "%d,%d,%d,%d", face_roi.x, face_roi.y, face_roi.width, face_roi.height);
            mtx_data.unlock();
            detect_timer.reset();
        } else {
            if (detect_timer.read_ms() >= 1000) {
#if MBED_CONF_APP_LCD
                ClearSquare();
#endif
                mtx_data.lock();
                sprintf(detect_str, "0,0,0,0");
                mtx_data.unlock();
            }
            led1 = 0;
        }
    }
}
