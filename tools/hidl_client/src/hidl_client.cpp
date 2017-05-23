/******************************************************************************
 *  Copyright (c) 2017, The Linux Foundation. All rights reserved
 *  Not a Contribution.
 *
 *  Copyright (C) 2017 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#include "android/hardware/bluetooth/1.0/IBluetoothHci.h"
#include <android/hardware/bluetooth/1.0/IBluetoothHciCallbacks.h>
#include <android/hardware/bluetooth/1.0/types.h>
#include <hwbinder/ProcessState.h>

#include <sys/un.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <errno.h>
#include <stdlib.h>
#include <utils/Log.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include "hidl_client.h"
#include "hci_internals.h"

using android::hardware::bluetooth::V1_0::IBluetoothHci;
using android::hardware::bluetooth::V1_0::IBluetoothHciCallbacks;
using android::hardware::bluetooth::V1_0::HciPacket;
using android::hardware::bluetooth::V1_0::Status;
using android::hardware::ProcessState;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;

extern void initialization_complete();
extern void *process_tool_data(void *arg);
static int safe_write(int server_fd, unsigned char* buf, int write_len);

static pthread_t client_rthread;

#define BT_CMD_PACKET_TYPE 0x01
#define BT_ACL_PACKET_TYPE 0x02
#define BT_SCO_PACKET_TYPE 0x03
#define BT_EVT_PACKET_TYPE 0x04

#define INVALID_FD (-1)

static sem_t s_cond;
static int mode_type;
static volatile bool hidl_init = false;
int server_fd = -1;

android::sp<IBluetoothHci> btHci;

class BluetoothHciCallbacks : public IBluetoothHciCallbacks {

    public:
    Return<void> initializationComplete(Status status) {
        if (status == Status::SUCCESS) {
            hidl_init = true;
        } else {
            ALOGE("Error in HIDL initialization");
        }
        sem_post(&s_cond);
        return Void();
    }

    Return<void> hciEventReceived(const hidl_vec<uint8_t>& event) {
        int len = static_cast<int>(event.size());
        unsigned char val = BT_EVT_PACKET_TYPE;
        if (safe_write(server_fd, &val, 1) == -1) {
            ALOGE("%s: failed to write event to tool socket", __func__);
            return Void();
        }

        if (safe_write(server_fd, const_cast<unsigned char*>(event.data()), len) == -1) {
            ALOGE("%s: failed to write event to tool socket", __func__);
            return Void();
        }
        return Void();
    }

    Return<void> aclDataReceived(const hidl_vec<uint8_t>& data) {
        int len = static_cast<int>(data.size());
        unsigned char val = BT_ACL_PACKET_TYPE;
        if (safe_write(server_fd, &val, 1) == -1) {
            ALOGE("%s: failed to write event to tool socket", __func__);
            return Void();
        }

        if (safe_write(server_fd, const_cast<unsigned char*>(data.data()), len) == -1) {
            ALOGE("%s: failed to write event to tool socket", __func__);
            return Void();
        }
        return Void();
    }

    Return<void> scoDataReceived(const hidl_vec<uint8_t>& data) {
        int len = static_cast<int>(data.size());
        unsigned char val = BT_SCO_PACKET_TYPE;
        if (safe_write(server_fd, &val, 1) == -1) {
            ALOGE("%s: failed to write event to tool socket", __func__);
            return Void();
        }

        if (safe_write(server_fd, const_cast<unsigned char*>(data.data()), len) == -1) {
            ALOGE("%s: failed to write event to tool socket", __func__);
            return Void();
        }
        return Void();
    }
};

#ifdef __cplusplus
extern "C"
{
#endif

bool hidl_client_initialize(int mode, int *tool_fd) {

    mode_type = mode;

    if (tool_fd == NULL) {
        ALOGE("%s: Invalid tool FD", __func__);
        return false;
    }

    switch (mode) {
        case MODE_BT:
            ALOGI("%s: Initialize the HIDL with Mode BT", __func__);
            btHci = IBluetoothHci::getService();
            // If android.hardware.bluetooth* is not found, Bluetooth can not continue.
            if (btHci != nullptr) {
                 ALOGI("%s: IBluetoothHci::getService() returned %p (%s)",
                    __func__, btHci.get(), (btHci->isRemote() ? "remote" : "local"));
            } else {
                ALOGE("Unable to initialize the HIDL");
                return false;
            }
            {
                android::sp<IBluetoothHciCallbacks> callbacks = new BluetoothHciCallbacks();
                hidl_init = false;
                sem_init(&s_cond, 0, 0);
                btHci->initialize(callbacks);
                // waiting for initialisation callback
                sem_wait(&s_cond);
            }

            if (hidl_init == true) {
                break;
            } else {
                ALOGE("%s: HIDL failed to initialize, sending invalid FD to tool", __func__);
                return false;
            }

        case MODE_ANT:
            // Block for ANT initialization
            return false;
            break;

        case MODE_FM:
            // Block for FM initialization
            return false;
            break;

        default:
            ALOGE("Unsupported mode");
            return false;
    }

    // creating a scoket pair
    int fds[2] = {INVALID_FD, INVALID_FD};

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, fds) == -1) {
        ALOGE("%s error creating socketpair: %s", __func__,strerror(errno));
        return false;
    }

    server_fd = fds[0];

    if (pthread_create(&client_rthread, NULL, process_tool_data, NULL) != 0) {
        ALOGE("%s:Unable to create pthread err = %s", __func__,  strerror(errno));
        close(server_fd);
        close(fds[1]);
        return false;
    }

    *tool_fd = fds[1];
    return true;
}

// Close HIDL Client to prevent callbacks.
void hidl_client_close() {

    if (hidl_init == false) {
        return;
    }

    if (server_fd != -1) {
        close(server_fd);
    }

    pthread_join(client_rthread, NULL);
    server_fd = -1;

    switch (mode_type) {
        case MODE_BT:
            ALOGI("%s: Close HIDL with Mode BT", __func__);
            btHci->close();
            btHci = nullptr;
            break;

        case MODE_ANT:
            //Block for ANT Close
            return;
            break;

        case MODE_FM:
            //Block for FM Close
            return;
            break;

        default:
            ALOGE("Unsupported mode");
            break;
    }
    return;
}

#ifdef __cplusplus
}
#endif

static int safe_write(int server_fd, unsigned char* buf, int write_len) {

    int bytes_written = 0;
    int ret;

    if (buf == NULL) {
        ALOGE("%s: The buffer is NULL", __func__);
        return -1;
    }

    if (!write_len) {
        ALOGE("%s: No data to write", __func__);
        return 0;
    }

    while (write_len > 0) {
        CLIENT_NO_INTR(ret = write(server_fd, buf+bytes_written, write_len));
        if (ret < 0) {
            ALOGE("%s: Write error: (%s)", __func__, strerror(errno));
            return -1;
        } else if (ret == 0) {
            ALOGE("%s: Write returned 0, err = %s, bytes written: %d",
                              __func__, strerror(errno), (unsigned int)(bytes_written));
            return -1;
        } else {
            write_len -= ret;
            bytes_written += ret;
        }
    }
    return bytes_written;
}
