// soem_l7nh_win32.c
// Windows GUI program (Option C) using SOEM PDOs where possible and SDO fallback for velocity.
// - Adds a CONNECT button that initializes SOEM and maps PDOs (press Connect to discover the drive).
// - Start / Stop buttons: Start sends torque via PDO outputs; Stop zeros torque and issues quick-stop.
// - Displays realtime RPM on the GUI while running and final RPM after stop (reads velocity via SDO if not present in PDO).
// Build: use existing CMake for SOEM and link to soem.lib. Adjust interface name (command-line arg) and DRIVE_SLAVE index as needed.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ethercat.h"   // SOEM header (make sure include path is set and soem.lib linked)

#define EC_TIMEOUTMON 500
#define DRIVE_SLAVE 1   // index of the drive in ec_slave[] (1 = first slave). Adjust if needed.

// CiA402 object indexes (used for SDO fallback and safety checks)
#define IDX_CONTROLWORD 0x6040
#define IDX_STATUSWORD  0x6041
#define IDX_MODE_OF_OPERATION 0x6060
#define IDX_TARGET_TORQUE 0x6071
#define IDX_ACTUAL_VELOCITY 0x606C

// Controlword commands (simple masks used here)
#define CW_SHUTDOWN 0x0006
#define CW_SWITCH_ON 0x0007
#define CW_ENABLE_OPERATION 0x000F
#define CW_QUICK_STOP 0x0002
#define CW_DISABLE_VOLTAGE 0x0000
#define CW_FAULT_RESET 0x0080

// GUI handles
static HWND hWndMain = NULL, hBtnConnect = NULL, hBtnStart = NULL, hBtnStop = NULL, hStaticRPM = NULL, hStaticState = NULL;
static HANDLE hThread = NULL;
static volatile bool run_flag = false;
static volatile bool connected_flag = false;
static char ifname[128] = ""; // network interface name (set by command line or edit)

// EtherCAT IOmap pointer (filled by ec_config_map)
static uint8 ec_IOmap[4096]; // large enough IOmap buffer (make sure size covers your network)

// Forward
DWORD WINAPI EtherCATThread(LPVOID lpParam);
void UpdateStaticText(HWND hWnd, int id, const char *txt) {
    HWND h = GetDlgItem(hWnd, id);
    if (h) SetWindowTextA(h, txt);
}

// SDO helpers (wrap ec_SDOread / write)
int write_sdo_u8(uint16 slave, uint16 idx, uint8 sub, uint8 val) {
    return ec_SDOwrite(slave, idx, sub, FALSE, sizeof(uint8), &val, EC_TIMEOUTRXM);
}
int write_sdo_u16(uint16 slave, uint16 idx, uint8 sub, uint16 val) {
    return ec_SDOwrite(slave, idx, sub, FALSE, sizeof(uint16), &val, EC_TIMEOUTRXM);
}
int write_sdo_s32(uint16 slave, uint16 idx, uint8 sub, int32_t val) {
    return ec_SDOwrite(slave, idx, sub, FALSE, sizeof(int32_t), &val, EC_TIMEOUTRXM);
}
int read_sdo_s32(uint16 slave, uint16 idx, uint8 sub, int32_t *out) {
    int size = sizeof(int32_t);
    int ret = ec_SDOread(slave, idx, sub, FALSE, &size, out, EC_TIMEOUTRXM);
    return ret;
}

// Get pointer helpers to PDO areas. These assume the drive's Rx PDO maps Controlword first, then TargetTorque.
// The ESI supplied with the drive shows Rx PDO mapping that includes 0x6040(Controlword) and 0x6071(Target Torque).
// See the drive ESI for exact order — if different, adjust offsets here. (Reference: uploaded ESI file.)

static inline uint16_t *pdo_controlword_ptr(int slave) {
    // controlword assumed at outputs offset 0
    if (!ec_slave[slave].outputs) return NULL;
    return (uint16_t *)(ec_slave[slave].outputs + 0);
}
static inline int16_t *pdo_target_torque_ptr(int slave) {
    // target torque assumed at outputs offset 2 (signed 16-bit)
    if (!ec_slave[slave].outputs) return NULL;
    return (int16_t *)(ec_slave[slave].outputs + 2);
}
static inline uint16_t *pdo_statusword_ptr(int slave) {
    if (!ec_slave[slave].inputs) return NULL;
    return (uint16_t *)(ec_slave[slave].inputs + 0);
}
static inline int32_t *pdo_actual_velocity_ptr(int slave) {
    // many ESI variants map actual velocity in the first Transmit PDO entries after status.
    // If mapped as 32-bit immediately after statusword, it would be at inputs+2 (byte offset 2) => align to int32
    if (!ec_slave[slave].inputs) return NULL;
    return (int32_t *)(ec_slave[slave].inputs + 2);
}

// Thread: main EtherCAT loop (called after Connect -> Start will create a separate short loop). 
// This thread implements the cyclic PDO-based control while run_flag is true.
DWORD WINAPI EtherCATThread(LPVOID lpParam) {
    char txt[256];
    int slavecount;

    if (!ec_init(ifname)) {
        sprintf_s(txt, sizeof(txt), "ec_init('%s') failed. Check interface name and cable.", ifname);
        UpdateStaticText(hWndMain, (int)hStaticState, txt);
        connected_flag = false;
        run_flag = false;
        return 1;
    }

    if (ec_config_init(FALSE) <= 0) {
        UpdateStaticText(hWndMain, (int)hStaticState, "No slaves found or ec_config_init failed");
        ec_close();
        connected_flag = false;
        run_flag = false;
        return 1;
    }

    slavecount = ec_slavecount;
    sprintf_s(txt, sizeof(txt), "Found %d slaves", slavecount);
    UpdateStaticText(hWndMain, (int)hStaticState, txt);

    // Map process data into our IOmap buffer
    ec_config_map(ec_IOmap);
    ec_configdc();

    // Set all slaves to OP state
    ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    for (int s = 1; s <= ec_slavecount; s++) {
        ec_slave[s].state = EC_STATE_OPERATIONAL;
    }
    ec_writestate(0);
    ec_statecheck(0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);

    if (ec_slave[DRIVE_SLAVE].state != EC_STATE_OPERATIONAL) {
        UpdateStaticText(hWndMain, (int)hStaticState, "Drive failed to reach OPERATIONAL state");
        ec_close();
        connected_flag = false;
        run_flag = false;
        return 1;
    }

    connected_flag = true;
    UpdateStaticText(hWndMain, (int)hStaticState, "Connected. Ready (press Start)");

    // Keep thread alive while connected (but not running torque). Start/Stop will control run_flag separately.
    while (connected_flag) {
        // small sleep to keep responsive to GUI; Start button will spawn the run loop below.
        Sleep(100);
    }

    // If disconnected requested, close ec
    ec_close();
    UpdateStaticText(hWndMain, (int)hStaticState, "Disconnected");
    return 0;
}

// Start/Stop handlers implement torque control loop (uses PDO outputs). This uses the same IOmap and ec_send/receive.
static HANDLE hRunThread = NULL;
DWORD WINAPI RunLoop(LPVOID lpParam) {
    char txt[256];

    // basic state machine to enable drive via SDO for safety, then use PDO torque set
    // Set Mode of Operation to CST (Cyclic Synchronous Torque) — CiA402 value 10
    uint8 mode = 10; // CST
    write_sdo_u8(DRIVE_SLAVE, IDX_MODE_OF_OPERATION, 0x00, mode);
    Sleep(20);

    // Use SDO to move CiA402 state machine to Operation Enabled: shutdown -> switch on -> enable
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_SHUTDOWN);
    Sleep(50);
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_SWITCH_ON);
    Sleep(50);
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_ENABLE_OPERATION);
    Sleep(100);

    // Now run cyclic PDO loop. We'll send processdata and set torque via PDO outputs.
    int16_t torque_set = 500; // small safe torque — tune for your motor (units per ESI). Use positive small value.

    while (run_flag) {
        // send processdata and receive to update inputs/outputs
        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);

        // write PDO outputs directly
        uint16_t *cw = pdo_controlword_ptr(DRIVE_SLAVE);
        int16_t *tt = pdo_target_torque_ptr(DRIVE_SLAVE);
        if (cw && tt) {
            *cw = CW_ENABLE_OPERATION; // keep enabled
            *tt = torque_set; // write torque
        } else {
            // if PDO not mapped as expected, fallback to SDO write
            write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_ENABLE_OPERATION);
            write_sdo_s32(DRIVE_SLAVE, IDX_TARGET_TORQUE, 0x00, torque_set);
        }

        // read velocity for display: try PDO first, then SDO fallback
        int has_pdo_vel = 0;
        int32_t vel_raw = 0;
        int32_t *pdo_vel = pdo_actual_velocity_ptr(DRIVE_SLAVE);
        if (pdo_vel) {
            vel_raw = *pdo_vel;
            has_pdo_vel = 1;
        } else {
            // SDO read fallback
            if (read_sdo_s32(DRIVE_SLAVE, IDX_ACTUAL_VELOCITY, 0x00, &vel_raw) <= 0) {
                // could not read velocity
                UpdateStaticText(hWndMain, (int)hStaticRPM, "RPM: (no velocity)");
            }
        }
        if (has_pdo_vel || vel_raw) {
            sprintf_s(txt, sizeof(txt), "RPM: %d%s", (int)vel_raw, has_pdo_vel?" (pdo)":" (sdo)");
            UpdateStaticText(hWndMain, (int)hStaticRPM, txt);
        }

        Sleep(50); // 50 ms cycle (adjust per your network / ESI timing)
    }

    // On stop: zero torque and quick stop
    ec_send_processdata();
    ec_receive_processdata(EC_TIMEOUTRET);
    int16_t *tt = pdo_target_torque_ptr(DRIVE_SLAVE);
    if (tt) {
        *tt = 0;
    } else {
        write_sdo_s32(DRIVE_SLAVE, IDX_TARGET_TORQUE, 0x00, 0);
    }
    write_sdo_u16(DRIVE_SLAVE, IDX_CONTROLWORD, 0x00, (uint16)CW_QUICK_STOP);
    Sleep(50);

    // Read final velocity (prefer SDO to get correct scaling)
    int32_t last_vel = 0;
    if (read_sdo_s32(DRIVE_SLAVE, IDX_ACTUAL_VELOCITY, 0x00, &last_vel) > 0) {
        sprintf_s(txt, sizeof(txt), "Final RPM: %d", last_vel);
        UpdateStaticText(hWndMain, (int)hStaticRPM, txt);
    } else {
        UpdateStaticText(hWndMain, (int)hStaticRPM, "Stopped - final RPM unknown");
    }

    return 0;
}

// Win32 callbacks and GUI creation
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hBtnConnect = CreateWindowA("BUTTON", "Connect", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 20, 100, 30, hwnd, (HMENU)10, NULL, NULL);
        hBtnStart = CreateWindowA("BUTTON", "Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            140, 20, 100, 30, hwnd, (HMENU)11, NULL, NULL);
        hBtnStop = CreateWindowA("BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            260, 20, 100, 30, hwnd, (HMENU)12, NULL, NULL);
        hStaticRPM = CreateWindowA("STATIC", "RPM: -", WS_CHILD | WS_VISIBLE | SS_SIMPLE,
            20, 70, 360, 24, hwnd, (HMENU)20, NULL, NULL);
        hStaticState = CreateWindowA("STATIC", "State: Idle", WS_CHILD | WS_VISIBLE | SS_SIMPLE,
            20, 100, 360, 24, hwnd, (HMENU)21, NULL, NULL);
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 10) { // Connect
            if (!connected_flag) {
                // spawn EtherCAT connect thread
                connected_flag = false; // ensure
                // copy interface name if provided via cmdline (already in ifname)
                hThread = CreateThread(NULL, 0, EtherCATThread, NULL, 0, NULL);
                if (hThread) {
                    UpdateStaticText(hwnd, 21, "Connecting...");
                }
            } else {
                // disconnect request: tell thread to cleanup
                connected_flag = false;
                UpdateStaticText(hwnd, 21, "Disconnecting...");
            }
        } else if (LOWORD(wParam) == 11) { // Start
            if (connected_flag && !run_flag) {
                run_flag = true;
                hRunThread = CreateThread(NULL, 0, RunLoop, NULL, 0, NULL);
                UpdateStaticText(hwnd, 21, "Running...");
            }
        } else if (LOWORD(wParam) == 12) { // Stop
            if (run_flag) {
                run_flag = false;
                // wait for run thread to finish
                if (hRunThread) {
                    WaitForSingleObject(hRunThread, 2000);
                    CloseHandle(hRunThread);
                    hRunThread = NULL;
                }
                UpdateStaticText(hwnd, 21, "Stopped (connected)");
            }
        }
        break;
    case WM_DESTROY:
        // ensure threads and EtherCAT closed
        run_flag = false;
        connected_flag = false;
        if (hRunThread) {
            WaitForSingleObject(hRunThread, 1000);
            CloseHandle(hRunThread);
        }
        if (hThread) {
            WaitForSingleObject(hThread, 1000);
            CloseHandle(hThread);
        }
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    MSG Msg;
    WNDCLASSEXA wc;

    // If user passed interface name as command line, copy it
    if (lpCmdLine && lpCmdLine[0] != '