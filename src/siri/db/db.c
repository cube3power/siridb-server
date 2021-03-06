/*
 * db.c - contains functions  and constants for a SiriDB database.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 10-03-2016
 *
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <cfgparser/cfgparser.h>
#include <lock/lock.h>
#include <lock/lock.h>
#include <logger/logger.h>
#include <math.h>
#include <procinfo/procinfo.h>
#include <siri/db/db.h>
#include <siri/db/series.h>
#include <siri/db/servers.h>
#include <siri/db/shard.h>
#include <siri/db/shards.h>
#include <siri/db/time.h>
#include <siri/db/users.h>
#include <siri/err.h>
#include <siri/siri.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include <xpath/xpath.h>
#include <slist/slist.h>

/*
 * database.dat
 *
 * SCHEMA       -> SIRIDB_SHEMA
 * UUID         -> UUID for 'this' server
 * DBNAME       -> Database name
 *
 *
 */

static siridb_t * SIRIDB_new(void);

static int SIRIDB_from_unpacker(
        qp_unpacker_t * unpacker,
        siridb_t ** siridb,
        const char * dbpath,
        char * err_msg);

#define READ_DB_EXIT_WITH_ERROR(ERROR_MSG)  \
    strcpy(err_msg, ERROR_MSG);             \
    siridb__free(*siridb);                  \
    *siridb = NULL;                         \
    return -1;

/*
 * Check if at least database.conf and database.dat exist in the path.
 */
int siridb_is_db_path(const char * dbpath)
{
    char buffer[SIRI_PATH_MAX];
    snprintf(buffer,
            SIRI_PATH_MAX,
            "%sdatabase.conf",
            dbpath);
    if (!xpath_file_exist(buffer))
    {
        return 0;  /* false */
    }
    snprintf(buffer,
            SIRI_PATH_MAX,
            "%sdatabase.dat",
            dbpath);
    if (!xpath_file_exist(buffer))
    {
        return 0;  /* false */
    }
    return 1;  /* true */
}

/*
 * Returns a siridb object or NULL in case of an error.
 *
 * (lock_flags are simple parsed to the lock function)
 *
 */
siridb_t * siridb_new(const char * dbpath, int lock_flags)
{
    size_t len = strlen(dbpath);
    lock_t lock_rc;
    char buffer[SIRI_PATH_MAX];
    cfgparser_t * cfgparser;
    cfgparser_option_t * option = NULL;
    qp_unpacker_t * unpacker;
    siridb_t * siridb;
    char err_msg[512];
    int rc;

    if (!len || dbpath[len - 1] != '/')
    {
        log_error("Database path should end with a slash. (got: '%s')",
                dbpath);
        return NULL;
    }

    if (!xpath_is_dir(dbpath))
    {
        log_error("Cannot find database path '%s'", dbpath);
        return NULL;
    }

    lock_rc = lock_lock(dbpath, lock_flags);

    switch (lock_rc)
    {
    case LOCK_IS_LOCKED_ERR:
    case LOCK_PROCESS_NAME_ERR:
    case LOCK_WRITE_ERR:
    case LOCK_READ_ERR:
    case LOCK_MEM_ALLOC_ERR:
        log_error("%s (%s)", lock_str(lock_rc), dbpath);
        return NULL;
    case LOCK_NEW:
        log_info("%s (%s)", lock_str(lock_rc), dbpath);
        break;
    case LOCK_OVERWRITE:
        log_warning("%s (%s)", lock_str(lock_rc), dbpath);
        break;
    default:
        assert (0);
        break;
    }

    /* read database.conf */
    snprintf(buffer,
            SIRI_PATH_MAX,
            "%sdatabase.conf",
            dbpath);

    cfgparser = cfgparser_new();
    if (cfgparser == NULL)
    {
        return NULL;  /* signal is raised */
    }
    if ((rc = cfgparser_read(cfgparser, buffer)) != CFGPARSER_SUCCESS)
    {
        log_error("Could not read '%s': %s",
                buffer,
                cfgparser_errmsg(rc));
        cfgparser_free(cfgparser);
        return NULL;
    }

    snprintf(buffer,
            SIRI_PATH_MAX,
            "%sdatabase.dat",
            dbpath);

    if ((unpacker = qp_unpacker_ff(buffer)) == NULL)
    {
        /* qp_unpacker has done some logging */
        cfgparser_free(cfgparser);
        return NULL;
    }

    if ((rc = SIRIDB_from_unpacker(
            unpacker,
            &siridb,
            dbpath,
            err_msg)) < 0)
    {
        log_error("Could not read '%s': %s", buffer, err_msg);
        qp_unpacker_ff_free(unpacker);
        cfgparser_free(cfgparser);
        return NULL;
    }

    qp_unpacker_ff_free(unpacker);

    if (rc > 0 && siridb_save(siridb))
    {
        log_error("Could not write file: %s", buffer);
        cfgparser_free(cfgparser);
        siridb_decref(siridb);
        return NULL;
    }

    log_info("Start loading database: '%s'", siridb->dbname);

    /* read buffer_path from database.conf */
    rc = cfgparser_get_option(
                &option,
                cfgparser,
                "buffer",
                "path");

    if (rc == CFGPARSER_SUCCESS && option->tp == CFGPARSER_TP_STRING)
    {
        len = strlen(option->val->string);
        siridb->buffer_path = NULL;
        if (option->val->string[len - 1] == '/')
        {
            siridb->buffer_path = strdup(option->val->string);
        }
        else if (asprintf(
                &siridb->buffer_path,
                "%s/",
                option->val->string) < 0)
        {
            siridb->buffer_path = NULL;
        }
    }
    else
    {
        siridb->buffer_path = siridb->dbpath;
    }

    /* free cfgparser */
    cfgparser_free(cfgparser);

    if (siridb->buffer_path == NULL)
    {
        ERR_ALLOC
        siridb_decref(siridb);
        return NULL;
    }

    /* load users */
    if (siridb_users_load(siridb))
    {
        log_error("Could not read users for database '%s'", siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    /* load servers */
    if (siridb_servers_load(siridb))
    {
        log_error("Could not read servers for database '%s'", siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    /* load series */
    if (siridb_series_load(siridb))
    {
        log_error("Could not read series for database '%s'", siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    /* load buffer */
    if (siridb_buffer_load(siridb))
    {
        log_error("Could not read buffer for database '%s'", siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    /* open buffer */
    if (siridb_buffer_open(siridb))
    {
        log_error("Could not open buffer for database '%s'", siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    /* load shards */
    if (siridb_shards_load(siridb))
    {
        log_error("Could not read shards for database '%s'", siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    /* load groups */
    if ((siridb->groups = siridb_groups_new(siridb)) == NULL)
    {
        log_error("Cannot read groups for database '%s'", siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    /* update series props */
    log_info("Updating series properties");

    /* create a copy since 'siridb_series_update_props' might drop a series */
    slist_t * slist = imap_2slist(siridb->series_map);

    if (slist == NULL)
    {
        log_error("Could update series properties for database '%s'",
                siridb->dbname);
        siridb_decref(siridb);
        return NULL;
    }

    for (size_t i = 0; i < slist->len; i++)
    {
        siridb_series_update_props(siridb, (siridb_series_t * )slist->data[i]);
    }

    slist_free(slist);

    /* generate pools, this can raise a signal */
    log_info("Initialize pools");
    siridb_pools_init(siridb);

    if (!siri_err)
    {
        siridb->reindex = siridb_reindex_open(siridb, 0);
        if (siridb->reindex != NULL && siridb->replica == NULL)
        {
            siridb_reindex_start(siridb->reindex->timer);
        }
    }

    siridb->start_ts = time(NULL);

    uv_mutex_lock(&siri.siridb_mutex);

    /* append SiriDB to siridb_list (reference counter is already 1) */
    if (llist_append(siri.siridb_list, siridb))
    {
        siridb_decref(siridb);
        return NULL;
    }

    uv_mutex_unlock(&siri.siridb_mutex);

    /* start groups update thread */
    siridb_groups_start(siridb->groups);

    log_info("Finished loading database: '%s'", siridb->dbname);

    return siridb;
}

/*
 * Read SiriDB from unpacker. (reference counter is initially set to 1)
 *
 * Returns 0 if successful or a value greater than 0 if the file needs to be
 * saved. In case of an error -1 will be returned.
 *
 * (a SIGNAL can be set in case of an error)
 */
static int SIRIDB_from_unpacker(
        qp_unpacker_t * unpacker,
        siridb_t ** siridb,
        const char * dbpath,
        char * err_msg)
{
    *siridb = NULL;
    qp_obj_t qp_obj;
    qp_obj_t qp_schema;

    if (!qp_is_array(qp_next(unpacker, NULL)) ||
        qp_next(unpacker, &qp_schema) != QP_INT64)
    {
        sprintf(err_msg, "Corrupted database file.");
        return -1;
    }

    /* check schema */
    if (qp_schema.via.int64 == 1 || qp_schema.via.int64 == 2)
    {
        log_info(
                "Found an old database schema (v%d), "
                "migrating to schema v%d...",
                qp_schema.via.int64,
                SIRIDB_SCHEMA);
    }
    else if (qp_schema.via.int64 != SIRIDB_SCHEMA)
    {
        sprintf(err_msg, "Unsupported schema found: %" PRId64,
                qp_schema.via.int64);
        return -1;
    }

    /* create a new SiriDB structure */
    *siridb = SIRIDB_new();
    if (*siridb == NULL)
    {
        sprintf(err_msg, "Cannot create SiriDB instance.");
        return -1;
    }

    /* set dbpath */
    if (((*siridb)->dbpath = strdup(dbpath)) == NULL)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot set dbpath.")
    }

    /* read uuid */
    if (qp_next(unpacker, &qp_obj) != QP_RAW || qp_obj.len != 16)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read uuid.")
    }

    /* copy uuid */
    memcpy(&(*siridb)->uuid, qp_obj.via.raw, qp_obj.len);

    /* read database name */
    if (qp_next(unpacker, &qp_obj) != QP_RAW ||
            qp_obj.len >= SIRIDB_MAX_DBNAME_LEN)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read database name.")
    }

    /* alloc mem for database name */
    (*siridb)->dbname = strndup((const char *) qp_obj.via.raw, qp_obj.len);
    if ((*siridb)->dbname == NULL)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot allocate database name.")
    }

    /* read time precision */
    if (qp_next(unpacker, &qp_obj) != QP_INT64 ||
            qp_obj.via.int64 < SIRIDB_TIME_SECONDS ||
            qp_obj.via.int64 > SIRIDB_TIME_NANOSECONDS)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read time-precision.")
    }

    /* bind time precision to SiriDB */
    (*siridb)->time =
            siridb_time_new((siridb_timep_t) qp_obj.via.int64);
    if ((*siridb)->time == NULL)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot create time instance.")
    }

    /* read buffer size, same buffer_size requirements are used in request.c */
    if (    qp_next(unpacker, &qp_obj) != QP_INT64 ||
            qp_obj.via.int64 % 512 ||
            qp_obj.via.int64 < 512)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read buffer size.")
    }

    /* bind buffer size and len to SiriDB */
    (*siridb)->buffer_size = (size_t) qp_obj.via.int64;
    (*siridb)->buffer_len = (*siridb)->buffer_size / sizeof(siridb_point_t);

    /* read number duration  */
    if (qp_next(unpacker, &qp_obj) != QP_INT64)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read number duration.")
    }

    /* bind number duration to SiriDB */
    (*siridb)->duration_num = (uint64_t) qp_obj.via.int64;

    /* calculate 'shard_mask_num' based on number duration */
    (*siridb)->shard_mask_num =
            (uint16_t) sqrt((double) siridb_time_in_seconds(
                    *siridb, (*siridb)->duration_num)) / 24;

    /* read log duration  */
    if (qp_next(unpacker, &qp_obj) != QP_INT64)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read log duration.")
    }

    /* bind log duration to SiriDB */
    (*siridb)->duration_log = (uint64_t) qp_obj.via.int64;

    /* calculate 'shard_mask_log' based on log duration */
    (*siridb)->shard_mask_log =
            (uint16_t) sqrt((double) siridb_time_in_seconds(
                    *siridb, (*siridb)->duration_log)) / 24;

    log_debug("Set number duration mask to %d", (*siridb)->shard_mask_num);
    log_debug("Set log duration mask to %d", (*siridb)->shard_mask_log);

    /* read timezone */
    if (qp_next(unpacker, &qp_obj) != QP_RAW)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read timezone.")
    }

    /* bind timezone to SiriDB */
    char * tzname = strndup((const char *) qp_obj.via.raw, qp_obj.len);

    if (tzname == NULL)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot allocate timezone name.")
    }

    if (((*siridb)->tz = iso8601_tz(tzname)) < 0)
    {
        log_critical("Unknown timezone found: '%s'.", tzname);
        free(tzname);
        READ_DB_EXIT_WITH_ERROR("Cannot read timezone.")
    }
    free(tzname);

    /* read drop threshold */
    if (qp_next(unpacker, &qp_obj) != QP_DOUBLE ||
            qp_obj.via.real < 0.0 || qp_obj.via.real > 1.0)
    {
        READ_DB_EXIT_WITH_ERROR("Cannot read drop threshold.")
    }
    (*siridb)->drop_threshold = qp_obj.via.real;

    if (qp_schema.via.int64 == 1)
    {
        (*siridb)->select_points_limit = DEF_SELECT_POINTS_LIMIT;
        (*siridb)->list_limit = DEF_LIST_LIMIT;
    }
    else
    {
        /* read select points limit */
        if (qp_next(unpacker, &qp_obj) != QP_INT64 || qp_obj.via.int64 < 1)
        {
            READ_DB_EXIT_WITH_ERROR("Cannot read select points limit.")
        }
        (*siridb)->select_points_limit = qp_obj.via.int64;

        /* read list limit */
        if (qp_next(unpacker, &qp_obj) != QP_INT64 || qp_obj.via.int64 < 1)
        {
            READ_DB_EXIT_WITH_ERROR("Cannot read list limit.")
        }
        (*siridb)->list_limit = qp_obj.via.int64;
    }

    return (qp_schema.via.int64 == SIRIDB_SCHEMA) ? 0 : qp_schema.via.int64;
}

/*
 * Get a siridb object by name.
 */
siridb_t * siridb_get(llist_t * siridb_list, const char * dbname)
{
    llist_node_t * node = siridb_list->first;
    siridb_t * siridb;

    while (node != NULL)
    {
        siridb = (siridb_t *) node->data;
        if (strcmp(siridb->dbname, dbname) == 0)
        {
            return siridb;
        }
        node = node->next;
    }

    return NULL;
}

/*
 * Sometimes we need a callback function and cannot use a macro expansion.
 */
void siridb_decref_cb(siridb_t * siridb, void * args __attribute__((unused)))
{
    siridb_decref(siridb);
}

/*
 * Typedef: sirinet_clserver_get_file
 *
 * Returns the length of the content for a file and set buffer with the file
 * content. Note that malloc is used to allocate memory for the buffer.
 *
 * In case of an error -1 is returned and buffer will be set to NULL.
 */
ssize_t siridb_get_file(char ** buffer, siridb_t * siridb)
{
    /* get servers file name */
    SIRIDB_GET_FN(fn, siridb->dbpath, "database.dat")

    return xpath_get_content(buffer, fn);
}

/*
 * Returns the number of open files by the given database.
 * (includes both the database and buffer path)
 */
int siridb_open_files(siridb_t * siridb)
{
    int open_files = procinfo_open_files(siridb->dbpath);

    if (    siridb->buffer_path != siridb->dbpath &&
            strncmp(
                siridb->dbpath,
                siridb->buffer_path,
                strlen(siridb->dbpath)))
    {
        open_files += procinfo_open_files(siridb->buffer_path);
    }

    return open_files;
}

/*
 * Returns 0 if successful or -1 in case of an error.
 */
int siridb_save(siridb_t * siridb)
{
    char buffer[SIRI_PATH_MAX];
    snprintf(buffer,
            SIRI_PATH_MAX,
            "%sdatabase.dat",
            siridb->dbpath);

    qp_fpacker_t * fpacker;

    if ((fpacker = qp_open(buffer, "w")) == NULL)
    {
        return -1;
    }

    return (qp_fadd_type(fpacker, QP_ARRAY_OPEN) ||
            qp_fadd_int8(fpacker, SIRIDB_SCHEMA) ||
            qp_fadd_raw(fpacker, (const unsigned char *) siridb->uuid, 16) ||
            qp_fadd_string(fpacker, siridb->dbname) ||
            qp_fadd_int8(fpacker, siridb->time->precision) ||
            qp_fadd_int64(fpacker, siridb->buffer_size) ||
            qp_fadd_int64(fpacker, siridb->duration_num) ||
            qp_fadd_int64(fpacker, siridb->duration_log) ||
            qp_fadd_string(fpacker, iso8601_tzname(siridb->tz)) ||
            qp_fadd_double(fpacker, siridb->drop_threshold) ||
            qp_fadd_int64(fpacker, siridb->select_points_limit) ||
            qp_fadd_int64(fpacker, siridb->list_limit) ||
            qp_fadd_type(fpacker, QP_ARRAY_CLOSE) ||
            qp_close(fpacker));
}

/*
 * Destroy SiriDB object.
 *
 * Never call this function but rather call siridb_decref.
 */
void siridb__free(siridb_t * siridb)
{
#if DEBUG
    log_debug("Free database: '%s'", siridb->dbname);
#endif

    /* first we should close all open files */
    if (siridb->buffer_fp != NULL)
    {
        fclose(siridb->buffer_fp);
    }

    if (siridb->dropped_fp != NULL)
    {
        fclose(siridb->dropped_fp);
    }

    if (siridb->store != NULL)
    {
        qp_close(siridb->store);
    }

    /* free users */
    if (siridb->users != NULL)
    {
        siridb_users_free(siridb->users);
    }

    /* free buffer positions */
    slist_free(siridb->empty_buffers);

    /* we do not need to free server and replica since they exist in
     * this list and therefore will be freed.
     */
    if (siridb->servers != NULL)
    {
        siridb_servers_free(siridb->servers);
    }

    /*
     * Destroy replicate before fifo but after servers so open promises are
     * closed which might depend on siridb->replicate
     *
     * siridb->replicate must be closed, see 'SIRI_set_closing_state'
     */
    if (siridb->replicate != NULL)
    {
        siridb_replicate_free(&siridb->replicate);
    }

    if (siridb->reindex != NULL)
    {
        siridb_reindex_free(&siridb->reindex);
    }

    /* free fifo (in case we have a replica) */
    if (siridb->fifo != NULL)
    {
        siridb_fifo_free(siridb->fifo);
    }

    /* free pools */
    if (siridb->pools != NULL)
    {
        siridb_pools_free(siridb->pools);
    }

    /* free imap (series) */
    if (siridb->series_map != NULL)
    {
        imap_free(siridb->series_map, NULL);
    }

    /* free c-tree lookup and series */
    if (siridb->series != NULL)
    {
        ct_free(siridb->series, (ct_free_cb) &siridb__series_decref);
    }

    /* free shards using imap walk an free the imap */
    if (siridb->shards != NULL)
    {
        imap_free(siridb->shards, (imap_free_cb) &siridb__shard_decref);
    }

    /* only free buffer path when not equal to db_path */
    if (siridb->buffer_path != siridb->dbpath)
    {
        free(siridb->buffer_path);
    }

    if (siridb->groups != NULL)
    {
        siridb_groups_decref(siridb->groups);
    }

    /* unlock the database in case no siri_err occurred */
    if (!siri_err)
    {
        lock_t lock_rc = lock_unlock(siridb->dbpath);
        if (lock_rc != LOCK_REMOVED)
        {
            log_error("%s", lock_str(lock_rc));
        }
    }

    free(siridb->dbpath);
    free(siridb->dbname);
    free(siridb->time);

    uv_mutex_destroy(&siridb->series_mutex);
    uv_mutex_destroy(&siridb->shards_mutex);

    free(siridb);
}

/*
 * Returns NULL and raises a SIGNAL in case an error has occurred.
 */
static siridb_t * SIRIDB_new(void)
{
    siridb_t * siridb = (siridb_t *) malloc(sizeof(siridb_t));
    if (siridb == NULL)
    {
        ERR_ALLOC
    }
    else
    {
        siridb->series = ct_new();
        if (siridb->series == NULL)
        {
            ERR_ALLOC
            free(siridb);
            siridb = NULL;
        }
        else
        {
            siridb->series_map = imap_new();
            if (siridb->series_map == NULL)
            {
                ct_free(siridb->series, NULL);
                free(siridb);
                siridb = NULL;
                ERR_ALLOC
            }
            else
            {
                siridb->shards = imap_new();
                if (siridb->shards == NULL)
                {
                    imap_free(siridb->series_map, NULL);
                    ct_free(siridb->series, NULL);
                    free(siridb);
                    siridb = NULL;
                    ERR_ALLOC

                }
                else
                {
                    /* allocate a list for buffer positions */
                    siridb->empty_buffers = slist_new(SLIST_DEFAULT_SIZE);
                    if (siridb->empty_buffers == NULL)
                    {
                        imap_free(siridb->shards, NULL);
                        imap_free(siridb->series_map, NULL);
                        ct_free(siridb->series, NULL);
                        free(siridb);
                        siridb = NULL;
                        ERR_ALLOC
                    }
                    else
                    {
                        siridb->dbname = NULL;
                        siridb->dbpath = NULL;
                        siridb->ref = 1;
                        siridb->active_tasks = 0;
                        siridb->insert_tasks = 0;
                        siridb->flags = 0;
                        siridb->buffer_path = NULL;
                        siridb->time = NULL;
                        siridb->users = NULL;
                        siridb->servers = NULL;
                        siridb->pools = NULL;
                        siridb->max_series_id = 0;
                        siridb->received_points = 0;
                        siridb->selected_points = 0;
                        siridb->drop_threshold = DEF_DROP_THRESHOLD;
                        siridb->select_points_limit = DEF_SELECT_POINTS_LIMIT;
                        siridb->list_limit = DEF_LIST_LIMIT;
                        siridb->buffer_size = -1;
                        siridb->tz = -1;
                        siridb->server = NULL;
                        siridb->replica = NULL;
                        siridb->fifo = NULL;
                        siridb->replicate = NULL;
                        siridb->reindex = NULL;
                        siridb->groups = NULL;

                        /* make file pointers are NULL when file is closed */
                        siridb->buffer_fp = NULL;
                        siridb->dropped_fp = NULL;
                        siridb->store = NULL;

                        uv_mutex_init(&siridb->series_mutex);
                        uv_mutex_init(&siridb->shards_mutex);
                    }
                }
            }
        }
    }
    return siridb;
}




