/**
 * \file output_manager.c
 * \author Michal Kozubik <kozubik.michal@gmail.com>
 * \brief Distribution of IPFIX message to Data Managers
 *
 * Copyright (C) 2014 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */


#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <libxml/tree.h>
#include <sys/prctl.h>

#include <ipfixcol.h>
#include <signal.h>
#include <errno.h>
#include "configurator.h"
#include "data_manager.h"
#include "output_manager.h"

#include <dirent.h>
#include <inttypes.h>

/* MSG_ macros identifiers */
static const char *msg_module = "output manager";
static const char *stat_module = "stat";

/* Output Manager's configuration - singleton */
struct output_manager_config *conf = NULL;

/**
 * \brief Search for Data manager handling specified Observation Domain ID
 *
 * \todo: improve search e.g. by some kind of sorting data_managers
 *
 * @param[in] id Observation domain ID of wanted Data manager.
 * @return Desired Data manager's configuration structure if exists, NULL if
 * there is no Data manager for specified Observation domain ID
 */
static struct data_manager_config *get_data_mngmt_config (uint32_t id, struct data_manager_config *data_mngmts)
{
	struct data_manager_config *aux_cfg = data_mngmts;

	for (aux_cfg = data_mngmts; aux_cfg; aux_cfg = aux_cfg->next) {
		if (aux_cfg->observation_domain_id == id) {
			break;
		}
	}

	return aux_cfg;
}

/**
 * \brief Insert new Data manager into list
 * 
 * \param[in] output_manager Output Manager structure
 * \param[in] new_manager New Data manager
 */
void output_manager_insert(struct output_manager_config *output_manager, struct data_manager_config *new_manager)
{
	new_manager->next = NULL;
	if (output_manager->last == NULL) {
		output_manager->data_managers = new_manager;
	} else {
		output_manager->last->next = new_manager;
	}
	output_manager->last = new_manager;
}

/**
 * \brief Remove data manager from list, close it and free templates
 * 
 * \param[in] output_manager Output Manager structure
 * \param[in] old_manager Data Manager to remove and close
 */
void output_manager_remove(struct output_manager_config *output_manager, struct data_manager_config *old_manager)
{
	struct data_manager_config *aux_conf = output_manager->data_managers;

	if (aux_conf == old_manager) {
		output_manager->data_managers = old_manager->next;
	}

	while (aux_conf->next) {
		if (aux_conf->next == old_manager) {
			aux_conf->next = old_manager->next;
			if (output_manager->last == old_manager) {
				output_manager->last = aux_conf;
			}
			break;
		} else {
			aux_conf = aux_conf->next;
		}
	}

	if (output_manager->data_managers == NULL) {
		output_manager->last = NULL;
	}
	uint32_t odid = old_manager->observation_domain_id;
	data_manager_close(&old_manager);
	tm_remove_all_odid_templates(template_mgr, odid);
}

/**
 * \brief Get input queue
 */
inline struct ring_buffer *output_manager_get_in_queue()
{
	return conf->in_queue;
}

/**
 * \brief Set new input queue
 */
void output_manager_set_in_queue(struct ring_buffer *in_queue)
{
	if (conf->in_queue == in_queue) {
		return;
	}
	
	if (conf->running) {
		/* If already running, control message must be sent */
		pthread_mutex_lock(&conf->in_q_mutex);
		
		conf->new_in = in_queue;
		rbuffer_write(conf->in_queue, NULL, 1);
		
		/* Wait for change */
		while (conf->in_queue != in_queue) {
			pthread_cond_wait(&conf->in_q_cond, &conf->in_q_mutex);
		}
		
		pthread_mutex_unlock(&conf->in_q_mutex);
	} else {
		conf->in_queue = in_queue;
	}
}

/**
 * \brief Add new storage plugin
 */
int output_manager_add_plugin(struct storage *plugin)
{
	int i;
	struct data_manager_config *data_mgr = NULL;
	
	/* Find a place for plugin in array */
	for (i = 0; conf->storage_plugins[i]; ++i) {}
	conf->storage_plugins[i] = plugin;
	
	if (plugin->xml_conf->observation_domain_id) {
		/* Plugin for one specific ODID */
		data_mgr = get_data_mngmt_config(atol(plugin->xml_conf->observation_domain_id), conf->data_managers);
		
		if (data_mgr) {
			/* Update existing Data Manager */
			data_manager_add_plugin(data_mgr, plugin);
		}
	} else {
		/* Update all existing Data Managers */
		for (data_mgr = conf->data_managers; data_mgr; data_mgr = data_mgr->next) {
			/* Add plugin to the Data Manager */
			data_manager_add_plugin(data_mgr, plugin);
		}
	}
	
	return 0;
}

/**
 * \brief Remove storage plugin
 */
int output_manager_remove_plugin(int id)
{
	int i;
	struct data_manager_config *data_mgr = NULL;
	struct storage *plugin = NULL;
	
	/* Find plugin with given id */
	for (i = 0; conf->storage_plugins[i]; ++i) {
		if (conf->storage_plugins[i]->id == id) {
			/* Remove it from array */
			plugin = conf->storage_plugins[i];
			conf->storage_plugins[i] = NULL;
			break;
		}
	}
	
	if (!plugin) {
		/* Plugin not found */
		return 0;
	}
	
	/* Kill all it's instances */
	if (plugin->xml_conf->observation_domain_id) {
		/* Has ODID - max. 1 instance */
		data_mgr = get_data_mngmt_config(atol(plugin->xml_conf->observation_domain_id), conf->data_managers);
		
		if (data_mgr) {
			/* Kill plugin */
			data_manager_remove_plugin(data_mgr, id);
		}
	} else {
		/* Multiple instances */
		for (data_mgr = conf->data_managers; data_mgr; data_mgr = data_mgr->next) {
			/* Kill plugin */
			data_manager_remove_plugin(data_mgr, id);
		}
	}
	
	return 0;
}

/**
 * \brief Output Managers thread
 *
 * @param[in] config configuration structure
 * @return NULL
 */
static void *output_manager_plugin_thread(void* config)
{
	struct data_manager_config *data_config = NULL;
	struct ipfix_message* msg = NULL;
	unsigned int index;

	conf = (struct output_manager_config *) config;
	index = conf->in_queue->read_offset;

	/* set the thread name to reflect the configuration */
	prctl(PR_SET_NAME, "ipfixcol OM", 0, 0, 0);      /* output managers' manager */

	/* loop will break upon receiving NULL from buffer */
	while (1) {
		/* get next data */
		index = -1;
		
		msg = rbuffer_read(conf->in_queue, &index);

		if (!msg) {
			rbuffer_remove_reference(conf->in_queue, index, 1);
			if (conf->new_in) {
				/* Set new input queue */
				conf->in_queue = (struct ring_buffer *) conf->new_in;
				conf->new_in = NULL;
				pthread_cond_signal(&conf->in_q_cond);
				continue;
			}
			
			/* End manager */
			break;
		}

		/* get appropriate data manager's config according to Observation domain ID */
		data_config = get_data_mngmt_config (msg->input_info->odid, conf->data_managers);
		if (data_config == NULL) {
			/*
			 * no data manager config for this observation domain ID found -
			 * we have a new observation domain ID, so create new data manager for
			 * it
			 */
			data_config = data_manager_create(msg->input_info->odid, conf->storage_plugins);
			if (data_config == NULL) {
				MSG_WARNING(msg_module, "[%u] Unable to create Data Manager; skipping data...",
						msg->input_info->odid);
				rbuffer_remove_reference(conf->in_queue, index, 1);
				continue;
			}

			/* add config to data_mngmts structure */
			output_manager_insert(conf, data_config);

			MSG_NOTICE(msg_module, "[%u] Data Manager created", msg->input_info->odid);
		}

		if (msg->source_status == SOURCE_STATUS_NEW) {
			/* New source, increment reference counter */
			MSG_DEBUG(msg_module, "[%u] New source", data_config->observation_domain_id);
			data_config->references++;
		} else if (msg->source_status == SOURCE_STATUS_CLOSED) {
			/* Source closed, decrement reference counter */
			MSG_DEBUG(msg_module, "[%u] Closed source", data_config->observation_domain_id);
			data_config->references--;

			if (data_config->references == 0) {
				/* No reference for this ODID, close DM */
				MSG_DEBUG(msg_module, "[%u] No source; releasing templates...", data_config->observation_domain_id);
				output_manager_remove(conf, data_config);
			}
			rbuffer_remove_reference(conf->in_queue, index, 1);
			continue;

		}
		
		__sync_fetch_and_add(&(conf->packets), 1);
		__sync_fetch_and_add(&(conf->data_records), msg->data_records_count);
		
		/* Write data into input queue of Storage Plugins */
		if (rbuffer_write(data_config->store_queue, msg, data_config->plugins_count) != 0) {
			MSG_WARNING(msg_module, "[%u] Unable to write into Data Manager's input queue; skipping data...", data_config->observation_domain_id);
			rbuffer_remove_reference(conf->in_queue, index, 1);
			free(msg);
			continue;
		}
		
		/* Remove data from queue (without deallocating) */
		rbuffer_remove_reference(conf->in_queue, index, 0);
	}

	MSG_NOTICE(msg_module, "Closing Output Manager thread");

	return (void *) 0;
}

/**
 * \brief Get total cpu time
 * 
 * @return total cpu time
 */
uint64_t statistics_total_cpu()
{
	FILE *stat = fopen("/proc/stat", "r");
	if (!stat) {
		MSG_WARNING(stat_module, "Cannot open file '%s'", "/proc/stat");
		return 0;
	}
	
	uint32_t user = 0, nice = 0, sys = 0, idle = 0;
	if (fscanf(stat, "%*s %" SCNu32 "%" SCNu32 "%" SCNu32 "%" SCNu32, &user, &nice, &sys, &idle) == EOF) {
		MSG_ERROR(stat_module, "Error while reading /proc/stat: %s", strerror(errno));
	}
	
	fclose(stat);
	
	return ((uint64_t) user + (uint64_t) nice + (uint64_t) sys + (uint64_t) idle);
}

/**
 * \brief Search thread statistics
 * 
 * @param conf statiscics conf
 * @param tid thread id
 * @return thread
 */
struct stat_thread *statistics_get_thread(struct stat_conf *conf, unsigned long tid)
{
	struct stat_thread *aux_thread;
	for (aux_thread = conf->threads; aux_thread; aux_thread = aux_thread->next) {
		if (aux_thread->tid == tid) {
			break;
		}
	}
	
	return aux_thread;
}

/**
 * \brief Add thread to list
 * 
 * @param conf statistics conf
 * @param tid thread id
 * @return thread
 */
struct stat_thread *statistics_add_thread(struct stat_conf *conf, long tid)
{
	struct stat_thread *thread = calloc(1, sizeof(struct stat_thread));
	if (!thread) {
		MSG_ERROR(stat_module, "Memory allocation failed (%s:%d)", __FILE__, __LINE__);
		return NULL;
	}
	
	
	thread->tid = tid;
	thread->next = conf->threads;
	conf->threads = thread;
	
	return thread;
}

/**
 * \brief Print cpu usage for each collector's thread
 * 
 * @param conf statistics conf
 */
static void statistics_print_cpu(struct stat_conf *conf)
{
	DIR *dir = opendir(conf->tasks_dir);
	if (!dir) {
		MSG_WARNING(stat_module, "Cannot open directory '%s'", conf->tasks_dir);
		return;
	}
	
	FILE *stat;
	struct dirent *entry;
	char stat_path[MAX_DIR_LEN], thread_name[MAX_DIR_LEN], state;
	int tid = 0;
	unsigned long utime, systime;
	uint64_t proc_time;
	uint64_t total_cpu = statistics_total_cpu();
	float usage;
	
	MSG_INFO(stat_module, "");
	MSG_INFO(stat_module, "%10s %7s %10s %15s", "TID", "state", "cpu usage", "thread name");
	while ((entry = readdir(dir)) != NULL) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		
		/* parse stat file */
		snprintf(stat_path, MAX_DIR_LEN, "%s/%s/stat", conf->tasks_dir, entry->d_name);
		stat = fopen(stat_path, "r");
		if (!stat) {
			MSG_WARNING(stat_module, "Cannot open file '%s'", stat_path);
			continue;
		}
		
		/* read thread info */
		uint8_t format_str_len = 62 + sizeof(MAX_DIR_LEN) + 1; // 62 is size of the raw format string, without MAX_DIR_LEN; +1 is null-terminating char
		char format_str[format_str_len];
		snprintf(format_str, format_str_len, "%%d (%%%d[^)]) %%c %%*d %%*d %%*d %%*d %%*d %%*u %%*u %%*u %%*u %%*u %%lu %%lu", MAX_DIR_LEN);
		if (fscanf(stat, format_str, &tid, thread_name, &state, &utime, &systime) == EOF) {
			MSG_ERROR(stat_module, "Error while reading %s: %s", stat_path, strerror(errno));
		}
		fclose(stat);
		
		/* Count thread cpu time */
		proc_time = utime + systime;
		
		struct stat_thread *thread = statistics_get_thread(conf, tid);
		if (!thread) {
			thread = statistics_add_thread(conf, tid);
			if (!thread) {
				continue;
			}
		}
		
		/* Count thread cpu usage (%) */
		if (thread->proc_time && total_cpu - conf->total_cpu > 0) {
			usage = conf->cpus * (proc_time - thread->proc_time) * 100 / (float) (total_cpu - conf->total_cpu);
		} else {
			usage = 0.0;
		}
		
		/* print statistics */
		MSG_INFO(stat_module, "%10d %7c %8.2f %% %15s", tid, state, usage, thread_name);
		
		/* update stats */
		thread->proc_time = proc_time;
	}
	MSG_INFO(stat_module, "");
	closedir(dir);
	
	/* update stats */
	conf->total_cpu = total_cpu;
}

/**
 * \brief Dummy SIGUSR1 signal handler
 */
void sig_handler(int s) 
{
	(void) s;
}

/**
 * \brief Print queues usage
 * 
 * @param conf output manager's config
 */
void statistics_print_buffers(struct output_manager_config *conf)
{	
	/* Print info about preprocessor's output queue */
	MSG_INFO(stat_module, "queues usage:");
	
	struct ring_buffer *prep_buffer = get_preprocessor_output_queue();
	MSG_INFO(stat_module, "preprocessor's output queue: %u / %u", prep_buffer->count, prep_buffer->size);

	/* Print info about Output Manager's queues */
	struct data_manager_config *dm = conf->data_managers;	
	if (dm) {
		MSG_INFO(stat_module, "output manager's output queues:");
		MSG_INFO(stat_module, "%.4s | %.10s / %.10s", "ODID", "waiting" ,"total size");
		
		while (dm) {
			MSG_INFO(stat_module, "[%u] %10u / %u", dm->observation_domain_id, dm->store_queue->count, dm->store_queue->size);
			dm = dm->next;
		}
	}
}

/**
 * \brief Periodically prints statistics about proccessing speed
 * 
 * @param config output manager's configuration
 * @return 0;
 */
static void *statistics_thread(void* config)
{
	struct output_manager_config *conf = (struct output_manager_config *) config;
	time_t begin = time(NULL), now, diff_time;
	uint64_t pkts, last_pkts, records, last_records, diff_pkts, diff_records;
	pkts = last_pkts = records = last_records = diff_pkts = diff_records = 0;
	
	/* create statistics config */
	conf->stats.total_cpu = 0;
	conf->stats.threads = NULL;
	conf->stats.cpus = sysconf(_SC_NPROCESSORS_ONLN);
	snprintf(conf->stats.tasks_dir, MAX_DIR_LEN, "/proc/%d/task/", getpid());
	
	/* Catch SIGUSR1 */
	signal(SIGUSR1, sig_handler);
	
	/* set thread name */
	prctl(PR_SET_NAME, "ipfixcol:stats", 0, 0, 0);
	
	while (conf->stat_interval) {
		sleep(conf->stat_interval);
		
		/* killed by output manager*/
		if (conf->stats.done) {
			break;
		}
		
		/* Compute time */
		now = time(NULL);
		diff_time = now - begin;
		
		/* Update and save packets counter */
		pkts = conf->packets;
		diff_pkts = pkts - last_pkts;
		last_pkts = pkts;
		
		/* Update and save data records counter */
		records = conf->data_records;
		diff_records = records - last_records;
		last_records = records;
		
		/* print info */
		MSG_INFO(stat_module, "now: %lu", now);
		MSG_INFO(stat_module, "%15s %15s %15s %15s %15s", "total time", "total packets", "tot. data rec.", "packets/s", "data records/s");
		MSG_INFO(stat_module, "%15lu %15lu %15lu %15lu %15lu", diff_time, pkts, records, diff_pkts/conf->stat_interval, diff_records/conf->stat_interval);
		
		/* print cpu usage by threads */
		statistics_print_cpu(&(conf->stats));
		
		/* print buffers usage */
		statistics_print_buffers(conf);
	}
	
	return NULL;
}

/**
 * \brief Creates new Output Manager
 *
 * @param[in] plugins_config plugins configurator
 * @param[in] stat_interval statistics printing interval
 * @param[out] config configuration structure
 * @return 0 on success, negative value otherwise
 */
int output_manager_create(configurator *plugins_config, int stat_interval, void **config)
{
	conf = calloc(1, sizeof(struct output_manager_config));
	if (!conf) {
		MSG_ERROR(msg_module, "Memory allocation failed (%s:%d)", __FILE__, __LINE__);
		return -1;
	}
	
	conf->stat_interval = stat_interval;
	conf->plugins_config = plugins_config;
	
	*config = conf;
	
	return 0;
}

/**
 * \brief Start Output Manager's thread(s)
 * 
 * @return 0 on success
 */
int output_manager_start() {

	int retval;

	/* Create Output Manager's thread */
	retval = pthread_create(&(conf->thread_id), NULL, &output_manager_plugin_thread, (void *) conf);
	if (retval != 0) {
		MSG_ERROR(msg_module, "Unable to create Output Manager thread");
		free(conf);
		return -1;
	}
	
	conf->running = 1;

	if (conf->stat_interval > 0) {
		retval = pthread_create(&(conf->stat_thread), NULL, &statistics_thread, (void *) conf);
		if (retval != 0) {
			MSG_ERROR(msg_module, "Unable to create statistics thread");
			free(conf);
			return -1;
		}
	}
	
	return 0;
}

/**
 * \brief Close Ouput Manager and all Data Managers
 */
void output_manager_close(void *config) {
	struct output_manager_config *manager = (struct output_manager_config *) config;
	struct data_manager_config *aux_config = NULL, *tmp = NULL;
	struct stat_thread *aux_thread = NULL, *tmp_thread = NULL;

	/* Stop Output Manager's thread and free input buffer */
	if (manager->running) {
		rbuffer_write(manager->in_queue, NULL, 1);
		pthread_join(manager->thread_id, NULL);
		rbuffer_free(manager->in_queue);

		/* Close statistics thread */
		if (manager->stat_interval > 0) {
			manager->stats.done = 1;
			pthread_kill(manager->stat_thread, SIGUSR1);
			pthread_join(manager->stat_thread, NULL);
		}

		aux_config = manager->data_managers;
		/* Close all data managers */
		while (aux_config) {
			tmp = aux_config;
			aux_config = aux_config->next;
			data_manager_close(&tmp);
		}

		/* Free all thread structures for statistics */
		aux_thread = manager->stats.threads;
		while (aux_thread) {
			tmp_thread = aux_thread;
			aux_thread = aux_thread->next;
			free(tmp_thread);
		}
	}
	
	free(manager);
}
