// SPDX-License-Identifier: GPL-3.0-or-later

#include "ebpf.h"
#include "ebpf_shm.h"

static char *shm_dimension_name[NETDATA_SHM_END] = { "get", "at", "dt", "ctl" };
static netdata_syscall_stat_t shm_aggregated_data[NETDATA_SHM_END];
static netdata_publish_syscall_t shm_publish_aggregated[NETDATA_SHM_END];

static int read_thread_closed = 1;
netdata_publish_shm_t *shm_vector = NULL;

static netdata_idx_t shm_hash_values[NETDATA_SHM_END];
static netdata_idx_t *shm_values = NULL;

netdata_publish_shm_t **shm_pid = NULL;

struct config shm_config = { .first_section = NULL,
    .last_section = NULL,
    .mutex = NETDATA_MUTEX_INITIALIZER,
    .index = { .avl_tree = { .root = NULL, .compar = appconfig_section_compare },
        .rwlock = AVL_LOCK_INITIALIZER } };

static ebpf_local_maps_t shm_maps[] = {{.name = "tbl_pid_shm", .internal_input = ND_EBPF_DEFAULT_PID_SIZE,
                                         .user_input = 0,
                                         .type = NETDATA_EBPF_MAP_RESIZABLE | NETDATA_EBPF_MAP_PID,
                                         .map_fd = ND_EBPF_MAP_FD_NOT_INITIALIZED},
                                        {.name = "shm_ctrl", .internal_input = NETDATA_CONTROLLER_END,
                                         .user_input = 0,
                                         .type = NETDATA_EBPF_MAP_CONTROLLER,
                                         .map_fd = ND_EBPF_MAP_FD_NOT_INITIALIZED},
                                        {.name = "tbl_shm", .internal_input = NETDATA_SHM_END,
                                         .user_input = 0,
                                         .type = NETDATA_EBPF_MAP_STATIC,
                                         .map_fd = ND_EBPF_MAP_FD_NOT_INITIALIZED},
                                        {.name = NULL, .internal_input = 0, .user_input = 0}};

static struct bpf_link **probe_links = NULL;
static struct bpf_object *objects = NULL;

struct netdata_static_thread shm_threads = {"SHM KERNEL", NULL, NULL, 1,
                                             NULL, NULL,  NULL};

/*****************************************************************
 *  FUNCTIONS TO CLOSE THE THREAD
 *****************************************************************/

/**
 * Clean shm structure
 */
void clean_shm_pid_structures() {
    struct pid_stat *pids = root_of_pids;
    while (pids) {
        freez(shm_pid[pids->pid]);

        pids = pids->next;
    }
}

/**
 * Clean up the main thread.
 *
 * @param ptr thread data.
 */
static void ebpf_shm_cleanup(void *ptr)
{
    ebpf_module_t *em = (ebpf_module_t *)ptr;
    if (!em->enabled) {
        return;
    }

    heartbeat_t hb;
    heartbeat_init(&hb);
    uint32_t tick = 2 * USEC_PER_MS;
    while (!read_thread_closed) {
        usec_t dt = heartbeat_next(&hb, tick);
        UNUSED(dt);
    }

    ebpf_cleanup_publish_syscall(shm_publish_aggregated);

    freez(shm_vector);
    freez(shm_values);

    if (probe_links) {
        struct bpf_program *prog;
        size_t i = 0 ;
        bpf_object__for_each_program(prog, objects) {
            bpf_link__destroy(probe_links[i]);
            i++;
        }
        bpf_object__close(objects);
    }
}

/*****************************************************************
 *  COLLECTOR THREAD
 *****************************************************************/

/**
 * Apps Accumulator
 *
 * Sum all values read from kernel and store in the first address.
 *
 * @param out the vector with read values.
 */
static void shm_apps_accumulator(netdata_publish_shm_t *out)
{
    int i, end = (running_on_kernel >= NETDATA_KERNEL_V4_15) ? ebpf_nprocs : 1;
    netdata_publish_shm_t *total = &out[0];
    for (i = 1; i < end; i++) {
        netdata_publish_shm_t *w = &out[i];
        total->get += w->get;
        total->at += w->at;
        total->dt += w->dt;
        total->ctl += w->ctl;
    }
}

/**
 * Fill PID
 *
 * Fill PID structures
 *
 * @param current_pid pid that we are collecting data
 * @param out         values read from hash tables;
 */
static void shm_fill_pid(uint32_t current_pid, netdata_publish_shm_t *publish)
{
    netdata_publish_shm_t *curr = shm_pid[current_pid];
    if (!curr) {
        curr = callocz(1, sizeof(netdata_publish_shm_t));
        shm_pid[current_pid] = curr;
    }

    memcpy(curr, publish, sizeof(netdata_publish_shm_t));
}

/**
 * Read APPS table
 *
 * Read the apps table and store data inside the structure.
 */
static void read_apps_table()
{
    netdata_publish_shm_t *cv = shm_vector;
    uint32_t key;
    struct pid_stat *pids = root_of_pids;
    int fd = shm_maps[NETDATA_PID_SHM_TABLE].map_fd;
    size_t length = sizeof(netdata_publish_shm_t)*ebpf_nprocs;
    while (pids) {
        key = pids->pid;

        if (bpf_map_lookup_elem(fd, &key, cv)) {
            pids = pids->next;
            continue;
        }

        shm_apps_accumulator(cv);

        shm_fill_pid(key, cv);

        // now that we've consumed the value, zero it out in the map.
        memset(cv, 0, length);
        bpf_map_update_elem(fd, &key, cv, BPF_EXIST);

        pids = pids->next;
    }
}

/**
* Send global charts to netdata agent.
*/
static void shm_send_global()
{
    write_begin_chart(NETDATA_EBPF_SYSTEM_GROUP, NETDATA_SHM_GLOBAL_CHART);
    write_chart_dimension(
        shm_publish_aggregated[NETDATA_KEY_SHMGET_CALL].dimension,
        (long long) shm_hash_values[NETDATA_KEY_SHMGET_CALL]
    );
    write_chart_dimension(
        shm_publish_aggregated[NETDATA_KEY_SHMAT_CALL].dimension,
        (long long) shm_hash_values[NETDATA_KEY_SHMAT_CALL]
    );
    write_chart_dimension(
        shm_publish_aggregated[NETDATA_KEY_SHMDT_CALL].dimension,
        (long long) shm_hash_values[NETDATA_KEY_SHMDT_CALL]
    );
    write_chart_dimension(
        shm_publish_aggregated[NETDATA_KEY_SHMCTL_CALL].dimension,
        (long long) shm_hash_values[NETDATA_KEY_SHMCTL_CALL]
    );
    write_end_chart();
}

/**
 * Read global counter
 *
 * Read the table with number of calls for all functions
 */
static void read_global_table()
{
    netdata_idx_t *stored = shm_values;
    netdata_idx_t *val = shm_hash_values;
    int fd = shm_maps[NETDATA_SHM_GLOBAL_TABLE].map_fd;

    uint32_t i, end = NETDATA_SHM_END;
    for (i = NETDATA_KEY_SHMGET_CALL; i < end; i++) {
        if (!bpf_map_lookup_elem(fd, &i, stored)) {
            int j;
            int last = ebpf_nprocs;
            netdata_idx_t total = 0;
            for (j = 0; j < last; j++)
                total += stored[j];

            val[i] = total;
        }
    }
}

/**
 * Shared memory reader thread.
 *
 * @param ptr It is a NULL value for this thread.
 * @return It always returns NULL.
 */
void *ebpf_shm_read_hash(void *ptr)
{
    read_thread_closed = 0;

    heartbeat_t hb;
    heartbeat_init(&hb);

    ebpf_module_t *em = (ebpf_module_t *)ptr;
    usec_t step = NETDATA_SHM_SLEEP_MS * em->update_time;
    while (!close_ebpf_plugin) {
        usec_t dt = heartbeat_next(&hb, step);
        (void)dt;

        read_global_table();
    }

    read_thread_closed = 1;
    return NULL;
}

/**
 * Sum values for all targets.
 */
static void ebpf_shm_sum_pids(netdata_publish_shm_t *shm, struct pid_on_target *root)
{
    while (root) {
        int32_t pid = root->pid;
        netdata_publish_shm_t *w = shm_pid[pid];
        if (w) {
            shm->get += w->get;
            shm->at += w->at;
            shm->dt += w->dt;
            shm->ctl += w->ctl;

            // reset for next collection.
            w->get = 0;
            w->at = 0;
            w->dt = 0;
            w->ctl = 0;
        }
        root = root->next;
    }
}

/**
 * Send data to Netdata calling auxiliar functions.
 *
 * @param root the target list.
*/
void ebpf_shm_send_apps_data(struct target *root)
{
    struct target *w;
    for (w = root; w; w = w->next) {
        if (unlikely(w->exposed && w->processes)) {
            ebpf_shm_sum_pids(&w->shm, w->root_pid);
        }
    }

    write_begin_chart(NETDATA_APPS_FAMILY, NETDATA_SHMGET_CHART);
    for (w = root; w; w = w->next) {
        if (unlikely(w->exposed && w->processes)) {
            write_chart_dimension(w->name, (long long) w->shm.get);
        }
    }
    write_end_chart();

    write_begin_chart(NETDATA_APPS_FAMILY, NETDATA_SHMAT_CHART);
    for (w = root; w; w = w->next) {
        if (unlikely(w->exposed && w->processes)) {
            write_chart_dimension(w->name, (long long) w->shm.at);
        }
    }
    write_end_chart();

    write_begin_chart(NETDATA_APPS_FAMILY, NETDATA_SHMDT_CHART);
    for (w = root; w; w = w->next) {
        if (unlikely(w->exposed && w->processes)) {
            write_chart_dimension(w->name, (long long) w->shm.dt);
        }
    }
    write_end_chart();

    write_begin_chart(NETDATA_APPS_FAMILY, NETDATA_SHMCTL_CHART);
    for (w = root; w; w = w->next) {
        if (unlikely(w->exposed && w->processes)) {
            write_chart_dimension(w->name, (long long) w->shm.ctl);
        }
    }
    write_end_chart();
}

/**
* Main loop for this collector.
*/
static void shm_collector(ebpf_module_t *em)
{
    shm_threads.thread = mallocz(sizeof(netdata_thread_t));
    shm_threads.start_routine = ebpf_shm_read_hash;

    netdata_thread_create(
        shm_threads.thread,
        shm_threads.name,
        NETDATA_THREAD_OPTION_JOINABLE,
        ebpf_shm_read_hash,
        em
    );

    int apps = em->apps_charts;
    while (!close_ebpf_plugin) {
        pthread_mutex_lock(&collect_data_mutex);
        pthread_cond_wait(&collect_data_cond_var, &collect_data_mutex);

        if (apps) {
            read_apps_table();
        }

        pthread_mutex_lock(&lock);

        shm_send_global();

        if (apps) {
            ebpf_shm_send_apps_data(apps_groups_root_target);
        }

        pthread_mutex_unlock(&lock);
        pthread_mutex_unlock(&collect_data_mutex);
    }
}

/*****************************************************************
 *  INITIALIZE THREAD
 *****************************************************************/

/**
 * Create apps charts
 *
 * Call ebpf_create_chart to create the charts on apps submenu.
 *
 * @param em a pointer to the structure with the default values.
 */
void ebpf_shm_create_apps_charts(struct ebpf_module *em, void *ptr)
{
    UNUSED(em);

    struct target *root = ptr;
    ebpf_create_charts_on_apps(NETDATA_SHMGET_CHART,
                               "Calls to syscall <code>shmget(2)</code>.",
                               EBPF_COMMON_DIMENSION_CALL,
                               NETDATA_APPS_IPC_SHM_GROUP,
                               NETDATA_EBPF_CHART_TYPE_STACKED,
                               20191,
                               ebpf_algorithms[NETDATA_EBPF_INCREMENTAL_IDX],
                               root, NETDATA_EBPF_MODULE_NAME_SHM);

    ebpf_create_charts_on_apps(NETDATA_SHMAT_CHART,
                               "Calls to syscall <code>shmat(2)</code>.",
                               EBPF_COMMON_DIMENSION_CALL,
                               NETDATA_APPS_IPC_SHM_GROUP,
                               NETDATA_EBPF_CHART_TYPE_STACKED,
                               20192,
                               ebpf_algorithms[NETDATA_EBPF_INCREMENTAL_IDX],
                               root, NETDATA_EBPF_MODULE_NAME_SHM);

    ebpf_create_charts_on_apps(NETDATA_SHMDT_CHART,
                               "Calls to syscall <code>shmdt(2)</code>.",
                               EBPF_COMMON_DIMENSION_CALL,
                               NETDATA_APPS_IPC_SHM_GROUP,
                               NETDATA_EBPF_CHART_TYPE_STACKED,
                               20193,
                               ebpf_algorithms[NETDATA_EBPF_INCREMENTAL_IDX],
                               root, NETDATA_EBPF_MODULE_NAME_SHM);

    ebpf_create_charts_on_apps(NETDATA_SHMCTL_CHART,
                               "Calls to syscall <code>shmctl(2)</code>.",
                               EBPF_COMMON_DIMENSION_CALL,
                               NETDATA_APPS_IPC_SHM_GROUP,
                               NETDATA_EBPF_CHART_TYPE_STACKED,
                               20194,
                               ebpf_algorithms[NETDATA_EBPF_INCREMENTAL_IDX],
                               root, NETDATA_EBPF_MODULE_NAME_SHM);
}

/**
 * Allocate vectors used with this thread.
 *
 * We are not testing the return, because callocz does this and shutdown the software
 * case it was not possible to allocate.
 *
 * @param length is the length for the vectors used inside the collector.
 */
static void ebpf_shm_allocate_global_vectors()
{
    shm_pid = callocz((size_t)pid_max, sizeof(netdata_publish_shm_t *));
    shm_vector = callocz((size_t)ebpf_nprocs, sizeof(netdata_publish_shm_t));

    shm_values = callocz((size_t)ebpf_nprocs, sizeof(netdata_idx_t));

    memset(shm_hash_values, 0, sizeof(shm_hash_values));
}

/*****************************************************************
 *  MAIN THREAD
 *****************************************************************/

/**
 * Create global charts
 *
 * Call ebpf_create_chart to create the charts for the collector.
 */
static void ebpf_create_shm_charts()
{
    ebpf_create_chart(
        NETDATA_EBPF_SYSTEM_GROUP,
        NETDATA_SHM_GLOBAL_CHART,
        "Calls to shared memory system calls.",
        EBPF_COMMON_DIMENSION_CALL,
        NETDATA_SYSTEM_IPC_SHM_SUBMENU,
        NULL,
        NETDATA_EBPF_CHART_TYPE_LINE,
        NETDATA_CHART_PRIO_SYSTEM_IPC_SHARED_MEM_CALLS,
        ebpf_create_global_dimension,
        shm_publish_aggregated,
        NETDATA_SHM_END,
        NETDATA_EBPF_MODULE_NAME_SHM
    );

    fflush(stdout);
}

/**
 * Shared memory thread.
 *
 * @param ptr a pointer to `struct ebpf_module`
 * @return It always return NULL
 */
void *ebpf_shm_thread(void *ptr)
{
    netdata_thread_cleanup_push(ebpf_shm_cleanup, ptr);

    ebpf_module_t *em = (ebpf_module_t *)ptr;
    em->maps = shm_maps;

    ebpf_update_pid_table(&shm_maps[NETDATA_PID_SHM_TABLE], em);

    if (!em->enabled) {
        goto endshm;
    }

    probe_links = ebpf_load_program(ebpf_plugin_dir, em, kernel_string, &objects);
    if (!probe_links) {
        goto endshm;
    }

    ebpf_shm_allocate_global_vectors();

    int algorithms[NETDATA_SHM_END] = {
        NETDATA_EBPF_INCREMENTAL_IDX,
        NETDATA_EBPF_INCREMENTAL_IDX,
        NETDATA_EBPF_INCREMENTAL_IDX,
        NETDATA_EBPF_INCREMENTAL_IDX
    };
    ebpf_global_labels(
        shm_aggregated_data,
        shm_publish_aggregated,
        shm_dimension_name,
        shm_dimension_name,
        algorithms,
        NETDATA_SHM_END
    );

    pthread_mutex_lock(&lock);
    ebpf_create_shm_charts();
    pthread_mutex_unlock(&lock);

    shm_collector(em);

endshm:
    netdata_thread_cleanup_pop(1);
    return NULL;
}
