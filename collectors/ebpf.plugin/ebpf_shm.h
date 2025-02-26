// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_EBPF_SHM_H
#define NETDATA_EBPF_SHM_H 1

// Module name
#define NETDATA_EBPF_MODULE_NAME_SHM "shm"

#define NETDATA_SHM_SLEEP_MS 850000ULL

// charts
#define NETDATA_SHM_GLOBAL_CHART "shared_memory_calls"
#define NETDATA_SHMGET_CHART "shmget_call"
#define NETDATA_SHMAT_CHART "shmat_call"
#define NETDATA_SHMDT_CHART "shmdt_call"
#define NETDATA_SHMCTL_CHART "shmctl_call"

// configuration file
#define NETDATA_DIRECTORY_SHM_CONFIG_FILE "shm.conf"

typedef struct netdata_publish_shm {
    uint64_t get;
    uint64_t at;
    uint64_t dt;
    uint64_t ctl;
} netdata_publish_shm_t;

enum shm_tables {
    NETDATA_PID_SHM_TABLE,
    NETDATA_SHM_CONTROLLER,
    NETDATA_SHM_GLOBAL_TABLE
};

enum shm_counters {
    NETDATA_KEY_SHMGET_CALL,
    NETDATA_KEY_SHMAT_CALL,
    NETDATA_KEY_SHMDT_CALL,
    NETDATA_KEY_SHMCTL_CALL,

    // Keep this as last and don't skip numbers as it is used as element counter
    NETDATA_SHM_END
};

extern netdata_publish_shm_t **shm_pid;

extern void *ebpf_shm_thread(void *ptr);
extern void ebpf_shm_create_apps_charts(struct ebpf_module *em, void *ptr);
extern void clean_shm_pid_structures();

extern struct config shm_config;

#endif
