/******************************************************************************
 *
 * mod_gearman - distribute checks with gearman
 *
 * Copyright (c) 2010 Sven Nierlein - sven.nierlein@consol.de
 *
 * This file is part of mod_gearman.
 *
 *  mod_gearman is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  mod_gearman is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with mod_gearman.  If not, see <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/


/* include header */
#include "result_thread.h"
#include "utils.h"
#include "mod_gearman.h"
#include "logger.h"
#include "gearman.h"

/* cleanup and exit this thread */
static void cancel_worker_thread (void * data) {

    gearman_worker_st *worker = (gearman_worker_st*) data;

    gearman_worker_unregister_all(worker);
    gearman_worker_remove_servers(worker);
    gearman_worker_free(worker);

    logger( GM_LOG_DEBUG, "worker thread finished\n" );

    return;
}

/* callback for task completed */
void *result_worker( void * data ) {
    gearman_worker_st worker;
    int *worker_num = (int*)data;

    logger( GM_LOG_TRACE, "worker %d started\n", *worker_num );

    pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    set_worker(&worker);

    pthread_cleanup_push ( cancel_worker_thread, (void*) &worker);

    while ( 1 ) {
        gearman_return_t ret;
        ret = gearman_worker_work( &worker );
        if ( ret != GEARMAN_SUCCESS ) {
            logger( GM_LOG_ERROR, "worker error: %s\n", gearman_worker_error( &worker ) );
            gearman_job_free_all( &worker );
            gearman_worker_free( &worker );

            sleep(1);
            set_worker(&worker );
        }
    }

    pthread_cleanup_pop(0);
    return NULL;
}

/* put back the result into the core */
void *get_results( gearman_job_st *job, void *context, size_t *result_size, gearman_return_t *ret_ptr ) {
    int wsize;
    char workload[GM_BUFFERSIZE];
    char *decrypted_data;
    char *decrypted_data_c;
    struct timeval now, core_start_time;
    check_result * chk_result;
    int active_check = TRUE;
    char *ptr;
    double now_f, core_starttime_f, starttime_f, finishtime_f, exec_time, latency;

    /* for calculating real latency */
    gettimeofday(&now,NULL);

    /* contect is unused */
    context = context;

    /* set size of result */
    *result_size = 0;

    /* set result pointer to success */
    *ret_ptr = GEARMAN_SUCCESS;

    /* get the data */
    wsize = gearman_job_workload_size(job);
    strncpy(workload, (const char*)gearman_job_workload(job), wsize);
    workload[wsize] = '\0';
    logger( GM_LOG_TRACE, "got result %s\n", gearman_job_handle( job ));
    logger( GM_LOG_TRACE, "%d +++>\n%s\n<+++\n", strlen(workload), workload );

    /* decrypt data */
    decrypted_data   = malloc(GM_BUFFERSIZE);
    decrypted_data_c = decrypted_data;
    mod_gm_decrypt(&decrypted_data, workload, mod_gm_opt->transportmode);

    if(decrypted_data == NULL) {
        *ret_ptr = GEARMAN_WORK_FAIL;
        return NULL;
    }
    logger( GM_LOG_TRACE, "%d --->\n%s\n<---\n", strlen(decrypted_data), decrypted_data );

    /*
     * save this result to a file, so when nagios crashes,
     * we have at least the crashed package
     */
    if(mod_gm_opt->debug_result == GM_ENABLED) {
        FILE * fd;
        fd = fopen( "/tmp/last_result_received.txt", "w+" );
        if(fd == NULL)
            perror("fopen");
        fputs( decrypted_data, fd );
        fclose( fd );
    }

    /* nagios will free it after processing */
    if ( ( chk_result = ( check_result * )malloc( sizeof *chk_result ) ) == 0 ) {
        *ret_ptr = GEARMAN_WORK_FAIL;
        return NULL;
    }

    chk_result->service_description = NULL;
    chk_result->host_name           = NULL;
    chk_result->output              = NULL;
    chk_result->return_code         = 0;
    chk_result->exited_ok           = 1;
    chk_result->early_timeout       = 0;
    chk_result->latency             = 0;
    chk_result->start_time.tv_sec   = 0;
    chk_result->start_time.tv_usec  = 0;
    chk_result->finish_time.tv_sec  = 0;
    chk_result->finish_time.tv_usec = 0;
    chk_result->scheduled_check     = TRUE;
    chk_result->reschedule_check    = TRUE;
    core_start_time.tv_sec          = 0;
    core_start_time.tv_usec         = 0;

    while ( (ptr = strsep(&decrypted_data, "\n" )) != NULL ) {
        char *key   = strsep( &ptr, "=" );
        char *value = strsep( &ptr, "\x0" );

        if ( key == NULL )
            continue;

        if ( !strcmp( key, "output" ) ) {
            if ( value == NULL ) {
                chk_result->output = strdup("(null)");
            }
            else {
                chk_result->output = strdup( value );
            }
        }

        if ( value == NULL ) {
            break;
        }

        if ( !strcmp( value, "") ) {
            continue;
        }

        if ( !strcmp( key, "host_name" ) ) {
            chk_result->host_name = strdup( value );
        } else if ( !strcmp( key, "service_description" ) ) {
            chk_result->service_description = strdup( value );
        } else if ( !strcmp( key, "check_options" ) ) {
            chk_result->check_options = atoi( value );
        } else if ( !strcmp( key, "scheduled_check" ) ) {
            chk_result->scheduled_check = atoi( value );
        } else if ( !strcmp( key, "type" ) && !strcmp( value, "passive" ) ) {
            active_check=FALSE;
        } else if ( !strcmp( key, "reschedule_check" ) ) {
            chk_result->reschedule_check = atoi( value );
        } else if ( !strcmp( key, "exited_ok" ) ) {
            chk_result->exited_ok = atoi( value );
        } else if ( !strcmp( key, "early_timeout" ) ) {
            chk_result->early_timeout = atoi( value );
        } else if ( !strcmp( key, "return_code" ) ) {
            chk_result->return_code = atoi( value );
        } else if ( !strcmp( key, "core_start_time" ) ) {
            string2timeval(value, &core_start_time);
        } else if ( !strcmp( key, "start_time" ) ) {
            string2timeval(value, &chk_result->start_time);
        } else if ( !strcmp( key, "finish_time" ) ) {
            string2timeval(value, &chk_result->finish_time);
        } else if ( !strcmp( key, "latency" ) ) {
            chk_result->latency = atof( value );
        }
    }

    if ( chk_result->host_name == NULL || chk_result->output == NULL ) {
        *ret_ptr= GEARMAN_WORK_FAIL;
        logger( GM_LOG_ERROR, "discarded invalid result\n" );
        return NULL;
    }

    if ( chk_result->service_description != NULL ) {
        chk_result->object_check_type    = SERVICE_CHECK;
        chk_result->check_type           = SERVICE_CHECK_ACTIVE;
        if(active_check == FALSE )
            chk_result->check_type       = SERVICE_CHECK_PASSIVE;
    } else {
        chk_result->object_check_type    = HOST_CHECK;
        chk_result->check_type           = HOST_CHECK_ACTIVE;
        if(active_check == FALSE )
            chk_result->check_type       = HOST_CHECK_PASSIVE;
    }

    /* fill some maybe missing options */
    if(chk_result->start_time.tv_sec  == 0) {
        chk_result->start_time.tv_sec = (unsigned long)time(NULL);
    }
    if(chk_result->finish_time.tv_sec  == 0) {
        chk_result->finish_time.tv_sec = (unsigned long)time(NULL);
    }
    if(core_start_time.tv_sec  == 0) {
        core_start_time.tv_sec = (unsigned long)time(NULL);
    }

    /* calculate real latency */
    now_f            = (double)now.tv_sec + (double)now.tv_usec / 1000000;
    core_starttime_f = (double)core_start_time.tv_sec + (double)core_start_time.tv_usec / 1000000;
    starttime_f      = (double)chk_result->start_time.tv_sec + (double)chk_result->start_time.tv_usec / 1000000;
    finishtime_f     = (double)chk_result->finish_time.tv_sec + (double)chk_result->finish_time.tv_usec / 1000000;
    exec_time        = finishtime_f - starttime_f;
    latency          = now_f - exec_time - core_starttime_f;
    chk_result->latency    += latency;

    /* this check is not a freshnes check */
    chk_result->check_options    = chk_result->check_options & ! CHECK_OPTION_FRESHNESS_CHECK;

    /* initialize and fill with result info */
    chk_result->output_file    = 0;
    chk_result->output_file_fd = -1;

    if ( chk_result->service_description != NULL ) {
        logger( GM_LOG_DEBUG, "service job completed: %s %s: %d\n", chk_result->host_name, chk_result->service_description, chk_result->return_code );
    } else {
        logger( GM_LOG_DEBUG, "host job completed: %s: %d\n", chk_result->host_name, chk_result->return_code );
    }

    /* nagios internal function */
    add_check_result_to_list( chk_result );

    /* reset pointer */
    chk_result = NULL;

    free(decrypted_data_c);

    return NULL;
}


/* get the worker */
int set_worker( gearman_worker_st *worker ) {

    create_worker( mod_gm_opt->server_list, worker );

    if ( mod_gm_opt->result_queue == NULL ) {
        logger( GM_LOG_ERROR, "got no result queue!\n" );
        return GM_ERROR;
    }
    logger( GM_LOG_DEBUG, "started result_worker thread for queue: %s\n", mod_gm_opt->result_queue );

    if(worker_add_function( worker, mod_gm_opt->result_queue, get_results ) != GM_OK) {
        return GM_ERROR;
    }

    /* add our dummy queue, gearman sometimes forgets the last added queue */
    worker_add_function( worker, "dummy", dummy);

    return GM_OK;
}
