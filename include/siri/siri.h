/*
 * siri.h - global methods for SiriDB.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 08-03-2016
 *
 */
#pragma once

#include <uv.h>
#include <siri/grammar/grammar.h>
#include <siri/db/db.h>
#include <siri/file/handler.h>
#include <stdbool.h>
#include <siri/optimize.h>
#include <siri/heartbeat.h>
#include <siri/cfg/cfg.h>
#include <siri/args/args.h>
#include <llist/llist.h>

typedef struct cleri_grammar_s cleri_grammar_t;
typedef struct siridb_list_s siridb_list_t;
typedef struct siri_fh_s siri_fh_t;
typedef struct siri_optimize_s siri_optimize_t;
typedef struct siri_heartbeat_s siri_heartbeat_t;
typedef struct siri_cfg_s siri_cfg_t;
typedef struct siri_args_s siri_args_t;
typedef struct llist_s llist_t;

typedef enum
{
    SIRI_STATUS_LOADING,
    SIRI_STATUS_RUNNING,
    SIRI_STATUS_CLOSING
} siri_status_t;

typedef enum
{
    SIRI_ERR_PATH_MISSING_TAIL_SLASH,
    SIRI_ERR_PATH_NOT_FOUND,
    SIRI_ERR_READING_CONF,
    SIRI_ERR_READING_DAT,
    SIRI_ERR_MEM_ALLOC
} siri_err_t;

typedef struct siri_s
{
    siri_status_t status;
    uv_loop_t * loop;
    cleri_grammar_t * grammar;
    llist_t * siridb_list;
    siri_fh_t * fh;
    siri_optimize_t * optimize;
    uv_timer_t * heartbeat;
    siri_cfg_t * cfg;
    siri_args_t * args;
    uv_mutex_t siridb_mutex;
    uint32_t startup_time;
} siri_t;

typedef struct siri_async_handle_s
{
    uv_close_cb free_cb;    /* must be on top */
} siri_async_handle_t;

void siri_setup_logger(void);
int siri_start(void);
void siri_free(void);

extern siri_t siri;
