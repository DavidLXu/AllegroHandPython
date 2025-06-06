//
// 20141209: kcchang: changed window version to linux 

// myAllegroHand.cpp : Defines the entry point for the console application.
//
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>  //_getch
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "canAPI.h"
#include "rDeviceAllegroHandCANDef.h"
#include "RockScissorsPaper.h"
#include <BHand/BHand.h>

#define PEAKCAN (1)

typedef char    TCHAR;
#define _T(X)   X
#define _tcsicmp(x, y)   strcmp(x, y)

using namespace std;

/////////////////////////////////////////////////////////////////////////////////////////
// for CAN communication
const double delT = 0.003;
int CAN_Ch = 0;
bool ioThreadRun = false;
pthread_t        hThread;
int recvNum = 0;
int sendNum = 0;
double statTime = -1.0;
AllegroHand_DeviceMemory_t vars;

double curTime = 0.0;

/////////////////////////////////////////////////////////////////////////////////////////
// for BHand library
BHand* pBHand = NULL;
double q[MAX_DOF];
double q_des[MAX_DOF];
double tau_des[MAX_DOF];
double cur_des[MAX_DOF];

// DIY mode variables
bool diy_mode = false;
int selected_dof = 0;
const double diy_step = 0.1; // radian increment/decrement step

// Monitor mode variables
bool monitor_mode = false;
const int monitor_update_rate = 10; // Update display every N message cycles
int monitor_counter = 0;

// USER HAND CONFIGURATION
const bool	RIGHT_HAND = false;
const int	HAND_VERSION = 4;

const double tau_cov_const_v4 = 1200.0; // 1200.0 for SAH040xxxxx

/////////////////////////////////////////////////////////////////////////////////////////
// functions declarations
char Getch();
void PrintInstruction();
void MainLoop();
bool OpenCAN();
void CloseCAN();
int GetCANChannelIndex(const TCHAR* cname);
bool CreateBHandAlgorithm();
void DestroyBHandAlgorithm();
void ComputeTorque();
void PrintDOFPositions();
void PrintJointValues();

// Add global variable for program control
bool bRun = true;

// TCP server settings
#define TCP_PORT 12321
bool tcpThreadRun = false;
pthread_t tcpThread;
int server_fd;

// Add at the top with other global variables
struct termios orig_termios;  // Store original terminal settings

// Function to handle TCP client connections
static void* tcpThreadProc(void* inst) {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[1024] = {0};
    
    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        printf("TCP socket creation failed\n");
        return NULL;
    }
    
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        printf("TCP setsockopt failed\n");
        return NULL;
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        printf("TCP bind failed\n");
        return NULL;
    }
    
    if (listen(server_fd, 3) < 0) {
        printf("TCP listen failed\n");
        return NULL;
    }
    
    printf("TCP server listening on port %d\n", TCP_PORT);
    
    while (tcpThreadRun) {
        int client_socket;
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            printf("TCP accept failed\n");
            continue;
        }
        
        printf("New client connected\n");
        
        while (tcpThreadRun) {
            int valread = read(client_socket, buffer, 1024);
            if (valread <= 0) {
                printf("Client disconnected\n");
                break;
            }
            
            // Parse joint values from buffer
            // Format: "SET_JOINTS val1 val2 val3 ... val16"
            if (strncmp(buffer, "SET_JOINTS", 10) == 0) {
                char* token = strtok(buffer + 11, " ");
                int joint = 0;
                
                while (token != NULL && joint < MAX_DOF) {
                    q_des[joint] = atof(token);
                    token = strtok(NULL, " ");
                    joint++;
                }
                
                if (pBHand) pBHand->SetMotionType(eMotionType_JOINT_PD);
                
                // Send acknowledgment
                send(client_socket, "OK\n", 3, 0);
            }
            else if (strncmp(buffer, "GET_JOINTS", 10) == 0) {
                // Format joint positions into response string
                char response[1024];
                int offset = 0;
                for (int i = 0; i < MAX_DOF; i++) {
                    offset += snprintf(response + offset, sizeof(response) - offset, 
                                     "%.6f ", q[i]);
                }
                response[offset-1] = '\n';  // Replace last space with newline
                send(client_socket, response, offset, 0);
            }
            else if (strncmp(buffer, "GET_TORQUES", 11) == 0) {
                // Format joint torques into response string
                char response[1024];
                int offset = 0;
                for (int i = 0; i < MAX_DOF; i++) {
                    offset += snprintf(response + offset, sizeof(response) - offset, 
                                     "%.6f ", tau_des[i]);
                }
                response[offset-1] = '\n';  // Replace last space with newline
                send(client_socket, response, offset, 0);
            }
            else if (strncmp(buffer, "QUIT", 4) == 0) {
                // Acknowledge quit command
                send(client_socket, "OK\n", 3, 0);
                // Signal main loop to exit
                bRun = false;
                break;
            }
            
            memset(buffer, 0, sizeof(buffer));
        }
        
        close(client_socket);
    }
    
    close(server_fd);
    return NULL;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Function to restore terminal settings
void RestoreTerminal() {
    static bool restored = false;
    if (!restored) {
        // Get current settings
        struct termios term;
        tcgetattr(0, &term);
        
        // Enable canonical mode and echo
        term.c_lflag |= (ICANON | ECHO);
        
        // Flush any pending input
        tcflush(0, TCIFLUSH);
        
        // Apply settings
        tcsetattr(0, TCSAFLUSH, &term);
        restored = true;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
// Read keyboard input (one char) from stdin
char Getch()
{
    char buf = 0;
    struct termios old = {0};
    struct termios new_settings = {0};
    
    // Get current settings
    if(tcgetattr(0, &old) < 0) {
        perror("tcgetattr()");
        return 0;
    }
    
    // Save original settings (only if not already saved)
    static bool settings_saved = false;
    if (!settings_saved) {
        orig_termios = old;
        settings_saved = true;
    }
    
    // Copy settings
    new_settings = old;
    
    // Modify for raw input while keeping echo
    new_settings.c_lflag &= ~ICANON;  // Disable canonical mode
    new_settings.c_lflag |= ECHO;     // Enable echo
    new_settings.c_cc[VMIN] = 1;
    new_settings.c_cc[VTIME] = 0;
    
    // Apply new settings
    if(tcsetattr(0, TCSANOW, &new_settings) < 0) {
        perror("tcsetattr ICANON");
        return 0;
    }
    
    // Read single character
    if(read(0, &buf, 1) < 0) {
        perror("read()");
        tcsetattr(0, TCSANOW, &old);
        return 0;
    }
    
    // Print newline since we're in raw mode
    write(1, "\n", 1);
    
    // Restore original settings
    if(tcsetattr(0, TCSANOW, &old) < 0) {
        perror("tcsetattr ~ICANON");
    }
    
    return buf;
}

/////////////////////////////////////////////////////////////////////////////////////////
// CAN communication thread
static void* ioThreadProc(void* inst)
{
    int id;
    int len;
    unsigned char data[8];
    unsigned char data_return = 0;
    int i;

    while (ioThreadRun)
    {
        /* wait for the event */
        while (0 == get_message(CAN_Ch, &id, &len, data, FALSE))
        {
//            printf(">CAN(%d): ", CAN_Ch);
//            for(int nd=0; nd<len; nd++)
//                printf("%02x ", data[nd]);
//            printf("\n");

            switch (id)
            {
            case ID_RTR_HAND_INFO:
            {
                printf(">CAN(%d): AllegroHand hardware version: 0x%02x%02x\n", CAN_Ch, data[1], data[0]);
                printf("                      firmware version: 0x%02x%02x\n", data[3], data[2]);
                printf("                      hardware type: %d(%s)\n", data[4], (data[4] == 0 ? "right" : "left"));
                printf("                      temperature: %d (celsius)\n", data[5]);
                printf("                      status: 0x%02x\n", data[6]);
                printf("                      servo status: %s\n", (data[6] & 0x01 ? "ON" : "OFF"));
                printf("                      high temperature fault: %s\n", (data[6] & 0x02 ? "ON" : "OFF"));
                printf("                      internal communication fault: %s\n", (data[6] & 0x04 ? "ON" : "OFF"));
            }
                break;
            case ID_RTR_SERIAL:
            {
                printf(">CAN(%d): AllegroHand serial number: SAH0%d0 %c%c%c%c%c%c%c%c\n", CAN_Ch, HAND_VERSION
                       , data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
            }
                break;
            case ID_RTR_FINGER_POSE_1:
            case ID_RTR_FINGER_POSE_2:
            case ID_RTR_FINGER_POSE_3:
            case ID_RTR_FINGER_POSE_4:
            {
                int findex = (id & 0x00000007);

                vars.enc_actual[findex*4 + 0] = (short)(data[0] | (data[1] << 8));
                vars.enc_actual[findex*4 + 1] = (short)(data[2] | (data[3] << 8));
                vars.enc_actual[findex*4 + 2] = (short)(data[4] | (data[5] << 8));
                vars.enc_actual[findex*4 + 3] = (short)(data[6] | (data[7] << 8));
                data_return |= (0x01 << (findex));
                recvNum++;

//                printf(">CAN(%d): Encoder[%d] Count : %6d %6d %6d %6d\n"
//                    , CAN_Ch, findex
//                    , vars.enc_actual[findex*4 + 0], vars.enc_actual[findex*4 + 1]
//                    , vars.enc_actual[findex*4 + 2], vars.enc_actual[findex*4 + 3]);

                if (data_return == (0x01 | 0x02 | 0x04 | 0x08))
                {
                    // convert encoder count to joint angle
                    for (i=0; i<MAX_DOF; i++)
                    {
                        q[i] = (double)(vars.enc_actual[i])*(333.3/65536.0)*(3.141592/180.0);
                    }

                    // Update monitor if active
                    if (monitor_mode) {
                        monitor_counter++;
                        if (monitor_counter >= monitor_update_rate) {
                            PrintJointValues();
                            monitor_counter = 0;
                        }
                    }

                    // print joint angles
//                    for (int i=0; i<4; i++)
//                    {
//                        printf(">CAN(%d): Joint[%d] Pos : %5.1f %5.1f %5.1f %5.1f\n"
//                            , CAN_Ch, i, q[i*4+0]*RAD2DEG, q[i*4+1]*RAD2DEG, q[i*4+2]*RAD2DEG, q[i*4+3]*RAD2DEG);
//                    }

                    // compute joint torque
                    ComputeTorque();

                    // convert desired torque to desired current and PWM count
                    for (int i=0; i<MAX_DOF; i++)
                    {
                        cur_des[i] = tau_des[i];
                        if (cur_des[i] > 1.0) cur_des[i] = 1.0;
                        else if (cur_des[i] < -1.0) cur_des[i] = -1.0;
                    }

                    // send torques
                    for (int i=0; i<4;i++)
                    {
                        vars.pwm_demand[i*4+0] = (short)(cur_des[i*4+0]*tau_cov_const_v4);
                        vars.pwm_demand[i*4+1] = (short)(cur_des[i*4+1]*tau_cov_const_v4);
                        vars.pwm_demand[i*4+2] = (short)(cur_des[i*4+2]*tau_cov_const_v4);
                        vars.pwm_demand[i*4+3] = (short)(cur_des[i*4+3]*tau_cov_const_v4);

                        command_set_torque(CAN_Ch, i, &vars.pwm_demand[4*i]);
                        //usleep(5);
                    }
                    sendNum++;
                    curTime += delT;
                    data_return = 0;
                }
            }
                break;
            case ID_RTR_IMU_DATA:
            {
                printf(">CAN(%d): AHRS Roll : 0x%02x%02x\n", CAN_Ch, data[0], data[1]);
                printf("               Pitch: 0x%02x%02x\n", data[2], data[3]);
                printf("               Yaw  : 0x%02x%02x\n", data[4], data[5]);
            }
                break;
            case ID_RTR_TEMPERATURE_1:
            case ID_RTR_TEMPERATURE_2:
            case ID_RTR_TEMPERATURE_3:
            case ID_RTR_TEMPERATURE_4:
            {
                int sindex = (id & 0x00000007);
                int celsius = (int)(data[0]      ) |
                              (int)(data[1] << 8 ) |
                              (int)(data[2] << 16) |
                              (int)(data[3] << 24);
                printf(">CAN(%d): Temperature[%d]: %d (celsius)\n", CAN_Ch, sindex, celsius);
            }
                break;
            default:
                printf(">CAN(%d): unknown command %d, len %d\n", CAN_Ch, id, len);
                /*for(int nd=0; nd<len; nd++)
                    printf("%d \n ", data[nd]);*/
                //return;
            }
        }
    }
    return NULL;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Application main-loop. It handles the commands from rPanelManipulator and keyboard events
void MainLoop()
{
    // Remove local bRun variable to use global one
    
    // Start TCP server thread
    tcpThreadRun = true;
    pthread_create(&tcpThread, NULL, tcpThreadProc, 0);
    printf("TCP server thread started\n");

    while (bRun)
    {
        int c = Getch();
        if (c == 0) {  // Error in Getch
            break;
        }
        
        if (diy_mode) {
            // DIY mode controls
            if (c == 'x' || c == 'X') {
                diy_mode = false;
                printf("Exiting DIY mode\n");
                continue;
            }
            else if (c == ' ') {
                PrintDOFPositions();
                continue;
            }
            else if (c >= '0' && c <= '9') {
                selected_dof = c - '0';
                printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
                continue;
            }
            else if (c == '!') { // Shift + 1
                selected_dof = 10;
                printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
                continue;
            }
            else if (c == '@') { // Shift + 2
                selected_dof = 11;
                printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
                continue;
            }
            else if (c == '#') { // Shift + 3
                selected_dof = 12;
                printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
                continue;
            }
            else if (c == '$') { // Shift + 4
                selected_dof = 13;
                printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
                continue;
            }
            else if (c == '%') { // Shift + 5
                selected_dof = 14;
                printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
                continue;
            }
            else if (c == '^') { // Shift + 6
                selected_dof = 15;
                printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
                continue;
            }
            else if (c == '+' || c == '=') {
                q_des[selected_dof] += diy_step;
                printf("DOF %d position increased to: %6.3f\n", selected_dof, q_des[selected_dof]);
                if (pBHand) pBHand->SetMotionType(eMotionType_JOINT_PD);
                continue;
            }
            else if (c == '-' || c == '_') {
                q_des[selected_dof] -= diy_step;
                printf("DOF %d position decreased to: %6.3f\n", selected_dof, q_des[selected_dof]);
                if (pBHand) pBHand->SetMotionType(eMotionType_JOINT_PD);
                continue;
            }
        }
        else {
            // Normal mode controls
            switch (c)
            {
            case 'q':
                if (pBHand) pBHand->SetMotionType(eMotionType_NONE);
                bRun = false;
                break;

            case 'h':
                if (pBHand) pBHand->SetMotionType(eMotionType_HOME);
                break;

            case 'r':
                if (pBHand) pBHand->SetMotionType(eMotionType_READY);
                break;

            case 'g':
                if (pBHand) pBHand->SetMotionType(eMotionType_GRASP_3);
                break;

            case 'k':
                if (pBHand) pBHand->SetMotionType(eMotionType_GRASP_4);
                break;

            case 'p':
                if (pBHand) pBHand->SetMotionType(eMotionType_PINCH_IT);
                break;

            case 'm':
                if (pBHand) pBHand->SetMotionType(eMotionType_PINCH_MT);
                break;

            case 'a':
                if (pBHand) pBHand->SetMotionType(eMotionType_GRAVITY_COMP);
                break;

            case 'e':
                if (pBHand) pBHand->SetMotionType(eMotionType_ENVELOP);
                break;

            case 'f':
                if (pBHand) pBHand->SetMotionType(eMotionType_NONE);
                break;

            case 'd':
                diy_mode = true;
                printf("Entering DIY mode\n");
                PrintDOFPositions();
                break;

            case '1':
                MotionRock();
                break;

            case '2':
                MotionScissors();
                break;

            case '3':
                MotionPaper();
                break;

            case 'v':
                monitor_mode = !monitor_mode;
                if (monitor_mode) {
                    printf("Entering monitor mode - displaying real-time joint values\n");
                    monitor_counter = monitor_update_rate; // Force immediate update
                } else {
                    printf("Exiting monitor mode\n");
                }
                break;
            }
        }
    }
    
    // Stop TCP server thread
    tcpThreadRun = false;
    shutdown(server_fd, SHUT_RDWR);
    pthread_join(tcpThread, NULL);
    
    // Ensure terminal is restored
    RestoreTerminal();
}

/////////////////////////////////////////////////////////////////////////////////////////
// Compute control torque for each joint using BHand library
void ComputeTorque()
{
    if (!pBHand) return;
    pBHand->SetJointPosition(q); // tell BHand library the current joint positions
    pBHand->SetJointDesiredPosition(q_des);
    pBHand->UpdateControl(0);
    pBHand->GetJointTorque(tau_des);

//    static int j_active[] = {
//        0, 0, 0, 0,
//        0, 0, 0, 0,
//        0, 0, 0, 0,
//        1, 1, 1, 1
//    };
//    for (int i=0; i<MAX_DOF; i++) {
//        if (j_active[i] == 0) {
//            tau_des[i] = 0;
//        }
//    }
}

/////////////////////////////////////////////////////////////////////////////////////////
// Open a CAN data channel
bool OpenCAN()
{
#if defined(PEAKCAN)
    CAN_Ch = GetCANChannelIndex(_T("USBBUS1"));
#elif defined(IXXATCAN)
    CAN_Ch = 1;
#elif defined(SOFTINGCAN)
    CAN_Ch = 1;
#else
    CAN_Ch = 1;
#endif
    printf(">CAN(%d): open\n", CAN_Ch);

    int ret = command_can_open(CAN_Ch);
    if(ret < 0)
    {
        printf("ERROR command_can_open !!! \n");
        return false;
    }

    // initialize CAN I/O thread
    ioThreadRun = true;
    /*int ioThread_error = */pthread_create(&hThread, NULL, ioThreadProc, 0);
    printf(">CAN: starts listening CAN frames\n");

    // query h/w information
    printf(">CAN: query system information\n");
    ret = request_hand_information(CAN_Ch);
    if(ret < 0)
    {
        printf("ERROR request_hand_information !!! \n");
        command_can_close(CAN_Ch);
        return false;
    }
    ret = request_hand_serial(CAN_Ch);
    if(ret < 0)
    {
        printf("ERROR request_hand_serial !!! \n");
        command_can_close(CAN_Ch);
        return false;
    }

    // set periodic communication parameters(period)
    printf(">CAN: Comm period set\n");
    short comm_period[3] = {3, 0, 0}; // millisecond {position, imu, temperature}
    ret = command_set_period(CAN_Ch, comm_period);
    if(ret < 0)
    {
        printf("ERROR command_set_period !!! \n");
        command_can_close(CAN_Ch);
        return false;
    }

    // servo on
    printf(">CAN: servo on\n");
    ret = command_servo_on(CAN_Ch);
    if(ret < 0)
    {
        printf("ERROR command_servo_on !!! \n");
        command_set_period(CAN_Ch, 0);
        command_can_close(CAN_Ch);
        return false;
    }

    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Close CAN data channel
void CloseCAN()
{
    printf(">CAN: stop periodic communication\n");
    int ret = command_set_period(CAN_Ch, 0);
    if(ret < 0)
    {
        printf("ERROR command_can_stop !!! \n");
    }

    if (ioThreadRun)
    {
        printf(">CAN: stoped listening CAN frames\n");
        ioThreadRun = false;
        int status;
        pthread_join(hThread, (void **)&status);
        hThread = 0;
    }

    printf(">CAN(%d): close\n", CAN_Ch);
    ret = command_can_close(CAN_Ch);
    if(ret < 0) printf("ERROR command_can_close !!! \n");
}

/////////////////////////////////////////////////////////////////////////////////////////
// Load and create grasping algorithm
bool CreateBHandAlgorithm()
{
    if (RIGHT_HAND)
        pBHand = bhCreateRightHand();
    else
        pBHand = bhCreateLeftHand();

    if (!pBHand) return false;
    pBHand->SetMotionType(eMotionType_NONE);
    pBHand->SetTimeInterval(delT);
    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Destroy grasping algorithm
void DestroyBHandAlgorithm()
{
    if (pBHand)
    {
#ifndef _DEBUG
        delete pBHand;
#endif
        pBHand = NULL;
    }
}

////////////////////////////////////////////////////////////////////////////////////////
// Print program information and keyboard instructions
void PrintInstruction()
{
    printf("--------------------------------------------------\n");
    printf("myAllegroHand: ");
    if (RIGHT_HAND) printf("Right Hand, v%i.x\n\n", HAND_VERSION); else printf("Left Hand, v%i.x\n\n", HAND_VERSION);

    printf("Keyboard Commands:\n");
    printf("H: Home Position (PD control)\n");
    printf("R: Ready Position (used before grasping)\n");
    printf("G: Three-Finger Grasp\n");
    printf("K: Four-Finger Grasp\n");
    printf("P: Two-finger pinch (index-thumb)\n");
    printf("M: Two-finger pinch (middle-thumb)\n");
    printf("E: Envelop Grasp (all fingers)\n");
    printf("A: Gravity Compensation\n\n");
    printf("D: Enter DIY Mode\n");
    printf("   In DIY Mode:\n");
    printf("   0-9: Select DOF (0-9)\n");
    printf("   Shift + 0-5: Select DOF (10-15)\n");
    printf("   +: Increase selected DOF position\n");
    printf("   -: Decrease selected DOF position\n");
    printf("   Space: Show current DOF positions\n");
    printf("   X: Exit DIY Mode\n\n");
    printf("V: Toggle real-time joint monitoring\n");
    printf("F: Servos OFF (any grasp cmd turns them back on)\n");
    printf("Q: Quit this program\n");

    printf("--------------------------------------------------\n\n");
}

void PrintDOFPositions()
{
    printf("\nCurrent DOF Positions (in radians):\n");
    for(int i = 0; i < 4; i++) {
        printf("Finger %d: ", i);
        for(int j = 0; j < 4; j++) {
            printf("%6.3f ", q_des[i*4 + j]);
        }
        printf("\n");
    }
    printf("Selected DOF: %d (Current position: %6.3f)\n", selected_dof, q_des[selected_dof]);
}

void PrintJointValues()
{
    printf("\033[2J\033[H"); // Clear screen and move cursor to top
    printf("=== Real-time Joint Values ===\n");
    printf("Press 'v' again to exit monitor mode\n\n");
    
    for(int i = 0; i < 4; i++) {
        printf("Finger %d:\n", i);
        printf("  Current: %6.3f %6.3f %6.3f %6.3f\n", 
               q[i*4 + 0], q[i*4 + 1], q[i*4 + 2], q[i*4 + 3]);
        printf("  Desired: %6.3f %6.3f %6.3f %6.3f\n", 
               q_des[i*4 + 0], q_des[i*4 + 1], q_des[i*4 + 2], q_des[i*4 + 3]);
        printf("  Torque:  %6.3f %6.3f %6.3f %6.3f\n\n", 
               tau_des[i*4 + 0], tau_des[i*4 + 1], tau_des[i*4 + 2], tau_des[i*4 + 3]);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
// Get channel index for Peak CAN interface
int GetCANChannelIndex(const TCHAR* cname)
{
    if (!cname) return 0;

    if (!_tcsicmp(cname, _T("0")) || !_tcsicmp(cname, _T("PCAN_NONEBUS")) || !_tcsicmp(cname, _T("NONEBUS")))
        return 0;
    else if (!_tcsicmp(cname, _T("1")) || !_tcsicmp(cname, _T("PCAN_ISABUS1")) || !_tcsicmp(cname, _T("ISABUS1")))
        return 1;
    else if (!_tcsicmp(cname, _T("2")) || !_tcsicmp(cname, _T("PCAN_ISABUS2")) || !_tcsicmp(cname, _T("ISABUS2")))
        return 2;
    else if (!_tcsicmp(cname, _T("3")) || !_tcsicmp(cname, _T("PCAN_ISABUS3")) || !_tcsicmp(cname, _T("ISABUS3")))
        return 3;
    else if (!_tcsicmp(cname, _T("4")) || !_tcsicmp(cname, _T("PCAN_ISABUS4")) || !_tcsicmp(cname, _T("ISABUS4")))
        return 4;
    else if (!_tcsicmp(cname, _T("5")) || !_tcsicmp(cname, _T("PCAN_ISABUS5")) || !_tcsicmp(cname, _T("ISABUS5")))
        return 5;
    else if (!_tcsicmp(cname, _T("7")) || !_tcsicmp(cname, _T("PCAN_ISABUS6")) || !_tcsicmp(cname, _T("ISABUS6")))
        return 6;
    else if (!_tcsicmp(cname, _T("8")) || !_tcsicmp(cname, _T("PCAN_ISABUS7")) || !_tcsicmp(cname, _T("ISABUS7")))
        return 7;
    else if (!_tcsicmp(cname, _T("8")) || !_tcsicmp(cname, _T("PCAN_ISABUS8")) || !_tcsicmp(cname, _T("ISABUS8")))
        return 8;
    else if (!_tcsicmp(cname, _T("9")) || !_tcsicmp(cname, _T("PCAN_DNGBUS1")) || !_tcsicmp(cname, _T("DNGBUS1")))
        return 9;
    else if (!_tcsicmp(cname, _T("10")) || !_tcsicmp(cname, _T("PCAN_PCIBUS1")) || !_tcsicmp(cname, _T("PCIBUS1")))
        return 10;
    else if (!_tcsicmp(cname, _T("11")) || !_tcsicmp(cname, _T("PCAN_PCIBUS2")) || !_tcsicmp(cname, _T("PCIBUS2")))
        return 11;
    else if (!_tcsicmp(cname, _T("12")) || !_tcsicmp(cname, _T("PCAN_PCIBUS3")) || !_tcsicmp(cname, _T("PCIBUS3")))
        return 12;
    else if (!_tcsicmp(cname, _T("13")) || !_tcsicmp(cname, _T("PCAN_PCIBUS4")) || !_tcsicmp(cname, _T("PCIBUS4")))
        return 13;
    else if (!_tcsicmp(cname, _T("14")) || !_tcsicmp(cname, _T("PCAN_PCIBUS5")) || !_tcsicmp(cname, _T("PCIBUS5")))
        return 14;
    else if (!_tcsicmp(cname, _T("15")) || !_tcsicmp(cname, _T("PCAN_PCIBUS6")) || !_tcsicmp(cname, _T("PCIBUS6")))
        return 15;
    else if (!_tcsicmp(cname, _T("16")) || !_tcsicmp(cname, _T("PCAN_PCIBUS7")) || !_tcsicmp(cname, _T("PCIBUS7")))
        return 16;
    else if (!_tcsicmp(cname, _T("17")) || !_tcsicmp(cname, _T("PCAN_PCIBUS8")) || !_tcsicmp(cname, _T("PCIBUS8")))
        return 17;
    else if (!_tcsicmp(cname, _T("18")) || !_tcsicmp(cname, _T("PCAN_USBBUS1")) || !_tcsicmp(cname, _T("USBBUS1")))
        return 18;
    else if (!_tcsicmp(cname, _T("19")) || !_tcsicmp(cname, _T("PCAN_USBBUS2")) || !_tcsicmp(cname, _T("USBBUS2")))
        return 19;
    else if (!_tcsicmp(cname, _T("20")) || !_tcsicmp(cname, _T("PCAN_USBBUS3")) || !_tcsicmp(cname, _T("USBBUS3")))
        return 20;
    else if (!_tcsicmp(cname, _T("21")) || !_tcsicmp(cname, _T("PCAN_USBBUS4")) || !_tcsicmp(cname, _T("USBBUS4")))
        return 21;
    else if (!_tcsicmp(cname, _T("22")) || !_tcsicmp(cname, _T("PCAN_USBBUS5")) || !_tcsicmp(cname, _T("USBBUS5")))
        return 22;
    else if (!_tcsicmp(cname, _T("23")) || !_tcsicmp(cname, _T("PCAN_USBBUS6")) || !_tcsicmp(cname, _T("USBBUS6")))
        return 23;
    else if (!_tcsicmp(cname, _T("24")) || !_tcsicmp(cname, _T("PCAN_USBBUS7")) || !_tcsicmp(cname, _T("USBBUS7")))
        return 24;
    else if (!_tcsicmp(cname, _T("25")) || !_tcsicmp(cname, _T("PCAN_USBBUS8")) || !_tcsicmp(cname, _T("USBBUS8")))
        return 25;
    else if (!_tcsicmp(cname, _T("26")) || !_tcsicmp(cname, _T("PCAN_PCCBUS1")) || !_tcsicmp(cname, _T("PCCBUS1")))
        return 26;
    else if (!_tcsicmp(cname, _T("27")) || !_tcsicmp(cname, _T("PCAN_PCCBUS2")) || !_tcsicmp(cname, _T("PCCBUS2")))
        return 27;
    else
        return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Program main
int main(int argc, TCHAR* argv[])
{
    // Get initial terminal settings
    if(tcgetattr(0, &orig_termios) < 0) {
        perror("tcgetattr()");
        return 1;
    }
    
    // Make sure ECHO and ICANON are enabled in original settings
    orig_termios.c_lflag |= (ICANON | ECHO);
    if(tcsetattr(0, TCSAFLUSH, &orig_termios) < 0) {
        perror("tcsetattr()");
        return 1;
    }
    
    // Flush any pending input
    tcflush(0, TCIFLUSH);
    
    // Register cleanup for abnormal termination
    atexit(RestoreTerminal);

    // Set initial state of global control variables
    bRun = true;
    tcpThreadRun = false;

    PrintInstruction();

    memset(&vars, 0, sizeof(vars));
    memset(q, 0, sizeof(q));
    memset(q_des, 0, sizeof(q_des));
    memset(tau_des, 0, sizeof(tau_des));
    memset(cur_des, 0, sizeof(cur_des));
    curTime = 0.0;

    if (CreateBHandAlgorithm() && OpenCAN())
        MainLoop();

    // Ensure terminal is restored before cleanup
    RestoreTerminal();
    
    CloseCAN();
    DestroyBHandAlgorithm();

    return 0;
}
