/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (c) 2012-2014, NVIDIA CORPORATION.  All rights reserved.
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
#define LOG_TAG "powerHAL::common"

#include <hardware/hardware.h>
#include <hardware/power.h>

#include "powerhal_utils.h"
#include "powerhal.h"

static int get_input_count(void)
{
    int i = 0;
    int ret;
    char path[80];
    char name[50];

    while(1)
    {
        snprintf(path, sizeof(path), "/sys/class/input/input%d/name", i);
        ret = access(path, F_OK);
        if (ret < 0)
            break;
        memset(name, 0, 50);
        sysfs_read(path, name, 50);
        ALOGI("input device id:%d present with name:%s", i++, name);
    }
    return i;
}

static void find_input_device_ids(struct powerhal_info *pInfo)
{
    int i = 0;
    int status;
    int count = 0;
    char path[80];
    char name[MAX_CHARS];

    while (1)
    {
        snprintf(path, sizeof(path), "/sys/class/input/input%d/name", i);
        if (access(path, F_OK) < 0)
            break;
        else {
            memset(name, 0, MAX_CHARS);
            sysfs_read(path, name, MAX_CHARS);
            for (int j = 0; j < pInfo->input_cnt; j++) {
                status = (-1 == pInfo->input_devs[j].dev_id)
                    && (0 == strncmp(name,
                    pInfo->input_devs[j].dev_name, MAX_CHARS));
                if (status) {
                    ++count;
                    pInfo->input_devs[j].dev_id = i;
                    ALOGI("find_input_device_ids: %d %s",
                        pInfo->input_devs[j].dev_id,
                        pInfo->input_devs[j].dev_name);
                }
            }
            ++i;
        }

        if (count == pInfo->input_cnt)
            break;
    }
}

static int check_hint(struct powerhal_info *pInfo, power_hint_t hint, uint64_t *t)
{
    struct timespec ts;
    uint64_t time;

    if (hint >= MAX_POWER_HINT_COUNT) {
        ALOGE("Invalid power hint: 0x%x", hint);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    time = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    if (pInfo->hint_time[hint] && pInfo->hint_interval[hint] &&
        (time - pInfo->hint_time[hint] < pInfo->hint_interval[hint]))
        return -1;

    *t = time;

    return 0;
}

static bool is_available_frequency(struct powerhal_info *pInfo, int freq)
{
    int i;

    for(i = 0; i < pInfo->num_available_frequencies; i++) {
        if(pInfo->available_frequencies[i] == freq)
            return true;
    }

    return false;
}

void common_power_open(struct powerhal_info *pInfo)
{
    int i;
    int size = 256;
    char *pch;

    if (0 == pInfo->input_devs || 0 == pInfo->input_cnt)
        pInfo->input_cnt = get_input_count();
    else
        find_input_device_ids(pInfo);

    // Initialize timeout poker
    Barrier readyToRun;
    pInfo->mTimeoutPoker = new TimeoutPoker(&readyToRun);
    readyToRun.wait();

    // Read available frequencies
    char *buf = (char*)malloc(sizeof(char) * size);
    memset(buf, 0, size);
    sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies",
               buf, size);

    // Determine number of available frequencies
    pch = strtok(buf, " ");
    pInfo->num_available_frequencies = -1;
    while(pch != NULL)
    {
        pch = strtok(NULL, " ");
        pInfo->num_available_frequencies++;
    }

    // Store available frequencies in a lookup array
    pInfo->available_frequencies = (int*)malloc(sizeof(int) * pInfo->num_available_frequencies);
    sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies",
               buf, size);
    pch = strtok(buf, " ");
    for(i = 0; i < pInfo->num_available_frequencies; i++)
    {
        pInfo->available_frequencies[i] = atoi(pch);
        pch = strtok(NULL, " ");
    }

    pInfo->max_frequency = pInfo->available_frequencies[pInfo->num_available_frequencies - 1];

    // Store LP cluster max frequency
    sysfs_read("/sys/devices/system/cpu/cpuquiet/tegra_cpuquiet/idle_top_freq",
                buf, size);
    pInfo->lp_max_frequency = atoi(buf);

    pInfo->interaction_boost_frequency = pInfo->lp_max_frequency;
    pInfo->animation_boost_frequency = pInfo->lp_max_frequency;

    for (i = 0; i < pInfo->num_available_frequencies; i++)
    {
        if (pInfo->available_frequencies[i] >= 1326000) {
            pInfo->interaction_boost_frequency = pInfo->available_frequencies[i];
            break;
        }
    }

    for (i = 0; i < pInfo->num_available_frequencies; i++)
    {
        if (pInfo->available_frequencies[i] >= 1122000) {
            pInfo->animation_boost_frequency = pInfo->available_frequencies[i];
            break;
        }
    }

    // Initialize hint intervals in usec
    //
    // Set the interaction timeout to be slightly shorter than the duration of
    // the interaction boost so that we can maintain is constantly during
    // interaction.
    pInfo->hint_interval[POWER_HINT_INTERACTION] = 90000;
    pInfo->hint_interval[POWER_HINT_CPU_BOOST] = 2000000;

    // Initialize fds
    pInfo->fds.interactive_max_cpus = -1;

    free(buf);
}

void common_power_init(__attribute__ ((unused)) struct power_module *module,
        struct powerhal_info *pInfo)
{
    common_power_open(pInfo);

    pInfo->ftrace_enable = get_property_bool("nvidia.hwc.ftrace_enable", false);

    // Boost to max frequency on initialization to decrease boot time
    pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min", pInfo->max_frequency,
                                     s2ns(60));
    ALOGI("Boosting cpu_freq_min to %d for 60 seconds to make boot faster", pInfo->max_frequency);
}

void common_power_set_interactive(__attribute__ ((unused)) struct power_module *module,
        struct powerhal_info *pInfo, int on)
{
    int i;
    int dev_id;
    char path[80];
    const char* state = (0 == on)?"0":"1";
    const char* gov = (on == 0)?"conservative":"interactive";

    sysfs_write("/sys/devices/platform/host1x/nvavp/boost_sclk", state);
    ALOGI("Setting boost_sclk %s", state);

    if (0 != pInfo) {
        for (i = 0; i < pInfo->input_cnt; i++) {
            if (0 == pInfo->input_devs)
                dev_id = i;
            else if (-1 == pInfo->input_devs[i].dev_id)
                continue;
            else
                dev_id = pInfo->input_devs[i].dev_id;
            snprintf(path, sizeof(path), "/sys/class/input/input%d/enabled", dev_id);
            if (!access(path, W_OK)) {
                if (0 == on)
                    ALOGI("Disabling input device:%d", dev_id);
                else
                    ALOGI("Enabling input device:%d", dev_id);
                sysfs_write(path, state);
            }
        }
    }

    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov);
    ALOGI("Setting scaling_governor to %s", gov);

    if (pInfo->fds.interactive_max_cpus >= 0) {
        close(pInfo->fds.interactive_max_cpus);
        pInfo->fds.interactive_max_cpus = -1;
    }

    if (on == 0)
        pInfo->fds.interactive_max_cpus =
                pInfo->mTimeoutPoker->requestPmQos("/dev/max_online_cpus", 4);
}

void common_power_hint(__attribute__ ((unused)) struct power_module *module,
        struct powerhal_info *pInfo, power_hint_t hint, __attribute__ ((unused)) void *data)
{
    uint64_t t;

    if (!pInfo)
        return;

    if (check_hint(pInfo, hint, &t) < 0)
        return;

    switch (hint) {
    case POWER_HINT_VSYNC:
        break;
    case POWER_HINT_INTERACTION:
        if (pInfo->ftrace_enable) {
            sysfs_write("/sys/kernel/debug/tracing/trace_marker", "Start POWER_HINT_INTERACTION\n");
        }
        // Boost to max lp frequency
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 pInfo->interaction_boost_frequency,
                                                 ms2ns(100));
        // Stutters observed during transition animations at lower frequencies
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 pInfo->animation_boost_frequency,
                                                 ms2ns(1200));
        break;
    case POWER_HINT_LAUNCH_BOOST:
        // Boost to 1.8Ghz all core
        pInfo->mTimeoutPoker->requestPmQosTimed("dev/cpu_freq_min",
                                                 1810000,
                                                 s2ns(3));
        pInfo->mTimeoutPoker->requestPmQosTimed("dev/min_online_cpus",
                                                 4,
                                                 s2ns(3));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/gpu_freq_min",
                                                 400000,
                                                 s2ns(3));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/emc_freq_min",
                                                 624000,
                                                 s2ns(3));
        break;
    case POWER_HINT_CPU_BOOST:
         // Boost to 1.8Ghz all core
        pInfo->mTimeoutPoker->requestPmQosTimed("dev/cpu_freq_min",
                                                 1810000,
                                                 s2ns(3));
        pInfo->mTimeoutPoker->requestPmQosTimed("dev/min_online_cpus",
                                                 4,
                                                 s2ns(3));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/gpu_freq_min",
                                                 400000,
                                                 s2ns(3));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/emc_freq_min",
                                                 624000,
                                                 s2ns(3));
       break;
    default:
        ALOGE("Unknown power hint: 0x%x", hint);
        break;
    }

    pInfo->hint_time[hint] = t;
}
