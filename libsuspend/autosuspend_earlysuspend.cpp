/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "libsuspend"
//#define LOG_NDEBUG 0

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include "autosuspend_ops.h"

using android::base::WriteStringToFd;

static int state_fd = -1;

static constexpr char sys_wait_for_fb_sleep[] = "/sys/power/wait_for_fb_sleep";
static constexpr char sys_wait_for_fb_wake[] = "/sys/power/wait_for_fb_wake";
static constexpr char sys_power_state[] = "/sys/power/state";
static constexpr char sleep_state[] = "mem";
static constexpr char on_state[] = "on";
static void (*wakeup_func)(bool success) = NULL;
static pthread_t earlysuspend_thread;
static pthread_mutex_t earlysuspend_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t earlysuspend_cond = PTHREAD_COND_INITIALIZER;
static bool wait_for_earlysuspend;
static enum {
    EARLYSUSPEND_ON,
    EARLYSUSPEND_MEM,
} earlysuspend_state = EARLYSUSPEND_ON;
static bool autosuspend_is_init = false;

static int wait_for_fb_wake(void) {
    int err = 0;
    char buf;
    int fd = TEMP_FAILURE_RETRY(open(sys_wait_for_fb_wake, O_CLOEXEC | O_RDONLY, 0));
    // if the file doesn't exist, the error will be caught in read() below
    err = TEMP_FAILURE_RETRY(read(fd, &buf, 1));
    if (err < 0) {
        PLOG(ERROR) << "*** ANDROID_WAIT_FOR_FB_WAKE failed";
    }
    close(fd);
    return err < 0 ? err : 0;
}

static int wait_for_fb_sleep(void) {
    int err = 0;
    char buf;
    int fd = TEMP_FAILURE_RETRY(open(sys_wait_for_fb_sleep, O_CLOEXEC | O_RDONLY, 0));
    // if the file doesn't exist, the error will be caught in read() below
    err = TEMP_FAILURE_RETRY(read(fd, &buf, 1));
    if (err < 0) {
        PLOG(ERROR) << "*** ANDROID_WAIT_FOR_FB_SLEEP failed";
    }
    close(fd);
    return err < 0 ? err : 0;
}

static void *earlysuspend_thread_func(void __unused *arg) {
    while (1) {
        if (wait_for_fb_sleep()) {
            LOG(ERROR) << "Failed reading wait_for_fb_sleep, exiting earlysuspend thread";
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_MEM;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);

        if (wait_for_fb_wake()) {
            LOG(ERROR) << "Failed reading wait_for_fb_wake, exiting earlysuspend thread";
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_ON;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);
    }
}

static void start_earlysuspend_thread(void) {
    char buf[80];
    int ret;

    ret = access(sys_wait_for_fb_sleep, F_OK);
    if (ret < 0) {
        return;
    }

    ret = access(sys_wait_for_fb_wake, F_OK);
    if (ret < 0) {
        return;
    }

    wait_for_fb_wake();

    ret = pthread_create(&earlysuspend_thread, NULL, earlysuspend_thread_func, NULL);
    if (ret) {
        LOG(ERROR) << "error creating thread: " << strerror(ret);
        return;
    }

    wait_for_earlysuspend = true;
}

static int init_state_fd(void) {
    if (state_fd >= 0) {
        return 0;
    }

    int fd = TEMP_FAILURE_RETRY(open(sys_power_state, O_CLOEXEC | O_RDWR));
    if (fd < 0) {
        PLOG(ERROR) << "error opening " << sys_power_state;
        return -1;
    }

    state_fd = fd;
    LOG(INFO) << "init_state_fd success";
    return 0;
}

static int autosuspend_init(void) {
    if (autosuspend_is_init) {
        return 0;
    }

    int ret = init_state_fd();
    if (ret < 0) {
        return -1;
    }

    bool success = WriteStringToFd(on_state, state_fd);
    if (!success) {
        PLOG(ERROR) << "error writing 'on' to " << sys_power_state;
        return NULL;
    }

    start_earlysuspend_thread();

    LOG(INFO) << "Early Suspend init success";
    autosuspend_is_init = true;

    return 0;
}

static int autosuspend_earlysuspend_enable(void) {
    LOG(VERBOSE) << "autosuspend_earlysuspend_enable";

    int ret = autosuspend_init();
    if (ret < 0) {
        LOG(ERROR) << "autosuspend_init failed";
        return ret;
    }

    bool success = WriteStringToFd(sleep_state, state_fd);
    if (!success) {
        PLOG(ERROR) << "error writing to " << sys_power_state;
        return -1;
    }

    void (*func)(bool success) = wakeup_func;
    if (func != NULL) {
        (*func)(success);
    }

    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_MEM) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }

    LOG(VERBOSE) << "autosuspend_earlysuspend_enable done";

    return 0;
}

static int autosuspend_earlysuspend_disable(void) {
    LOG(VERBOSE) << "autosuspend_earlysuspend_disable";

    if (!autosuspend_is_init) {
        return 0;  // always successful if no thread is running yet
    }

    bool success = WriteStringToFd(on_state, state_fd);
    if (!success) {
        PLOG(ERROR) << "error writing to " << sys_power_state;
        return -1;
    }

    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_ON) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }

    LOG(VERBOSE) << "autosuspend_earlysuspend_disable done";

    return 0;
}

static int force_suspend(int timeout_ms) {
    LOG(VERBOSE) << "force_suspend called with timeout: " << timeout_ms;

    int ret = init_state_fd();
    if (ret < 0) {
        return ret;
    }

    return WriteStringToFd(sleep_state, state_fd) ? 0 : -1;
}

static void autosuspend_set_wakeup_callback(void (*func)(bool success)) {
    if (wakeup_func != NULL) {
        LOG(ERROR) << "duplicate wakeup callback applied, keeping original";
        return;
    }
    wakeup_func = func;
}

struct autosuspend_ops autosuspend_earlysuspend_ops = {
    .enable = autosuspend_earlysuspend_enable,
    .disable = autosuspend_earlysuspend_disable,
    .force_suspend = force_suspend,
    .set_wakeup_callback = autosuspend_set_wakeup_callback,
};

struct autosuspend_ops *autosuspend_earlysuspend_init(void) {
    return &autosuspend_earlysuspend_ops;
}
