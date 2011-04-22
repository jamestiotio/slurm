/*****************************************************************************\
 *  select_linear.c - node selection plugin for simple one-dimensional
 *  address space. Selects nodes for a job so as to minimize the number
 *  of sets of consecutive nodes using a best-fit algorithm.
 *****************************************************************************
 *  Copyright (C) 2004-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Portions Copyright (C) 2010 SchedMD <http://www.schedmd.com>.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#  if HAVE_STDINT_H
#    include <stdint.h>
#  endif
#  if HAVE_INTTYPES_H
#    include <inttypes.h>
#  endif
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/slurm_xlator.h"	/* Must be first */
#include "src/common/gres.h"
#include "src/common/job_resources.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/node_select.h"
#include "src/common/parse_time.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"

#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/preempt.h"
#include "src/slurmctld/proc_req.h"
#include "src/plugins/select/linear/select_linear.h"

#define NO_SHARE_LIMIT	0xfffe
#define NODEINFO_MAGIC	0x82ad
#define RUN_JOB_INCR	16
#define SELECT_DEBUG	0

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
slurm_ctl_conf_t slurmctld_conf __attribute__((weak_import));
struct node_record *node_record_table_ptr __attribute__((weak_import));
List part_list __attribute__((weak_import));
List job_list __attribute__((weak_import));
int node_record_count __attribute__((weak_import));
time_t last_node_update __attribute__((weak_import));
struct switch_record *switch_record_table __attribute__((weak_import));
int switch_record_cnt __attribute__((weak_import));
#else
slurm_ctl_conf_t slurmctld_conf;
struct node_record *node_record_table_ptr;
List part_list;
List job_list;
int node_record_count;
time_t last_node_update;
struct switch_record *switch_record_table;
int switch_record_cnt;
#endif

struct select_nodeinfo {
	uint16_t magic;		/* magic number */
	uint16_t alloc_cpus;
};

static int  _add_job_to_nodes(struct cr_record *cr_ptr,
			      struct job_record *job_ptr, char *pre_err,
			      int suspended);
static void _add_run_job(struct cr_record *cr_ptr, uint32_t job_id);
static void _add_tot_job(struct cr_record *cr_ptr, uint32_t job_id);
static void _build_select_struct(struct job_record *job_ptr, bitstr_t *bitmap);
static int  _cr_job_list_sort(void *x, void *y);
static job_resources_t *_create_job_resources(int node_cnt);
static void _dump_node_cr(struct cr_record *cr_ptr);
static struct cr_record *_dup_cr(struct cr_record *cr_ptr);
static int  _find_job_mate(struct job_record *job_ptr, bitstr_t *bitmap,
			   uint32_t min_nodes, uint32_t max_nodes,
			   uint32_t req_nodes);
static void _free_cr(struct cr_record *cr_ptr);
static uint16_t _get_avail_cpus(struct job_record *job_ptr, int index);
static uint16_t _get_total_cpus(int index);
static void _init_node_cr(void);
static int _job_count_bitmap(struct cr_record *cr_ptr,
			     struct job_record *job_ptr,
			     bitstr_t * bitmap, bitstr_t * jobmap,
			     int run_job_cnt, int tot_job_cnt, uint16_t mode);
static int _job_expand(struct job_record *from_job_ptr,
		       struct job_record *to_job_ptr);
static int _job_test(struct job_record *job_ptr, bitstr_t *bitmap,
		     uint32_t min_nodes, uint32_t max_nodes,
		     uint32_t req_nodes);
static int _job_test_topo(struct job_record *job_ptr, bitstr_t *bitmap,
			  uint32_t min_nodes, uint32_t max_nodes,
			  uint32_t req_nodes);
static bool _rem_run_job(struct cr_record *cr_ptr, uint32_t job_id);
static bool _rem_tot_job(struct cr_record *cr_ptr, uint32_t job_id);
static int _rm_job_from_nodes(struct cr_record *cr_ptr,
			      struct job_record *job_ptr, char *pre_err,
			      bool remove_all);
static int _rm_job_from_one_node(struct job_record *job_ptr,
				 struct node_record *node_ptr, char *pre_err);
static int _run_now(struct job_record *job_ptr, bitstr_t *bitmap,
		    uint32_t min_nodes, uint32_t max_nodes,
		    int max_share, uint32_t req_nodes,
		    List preemptee_candidates,
		    List *preemptee_job_list);
static bool _test_run_job(struct cr_record *cr_ptr, uint32_t job_id);
static bool _test_tot_job(struct cr_record *cr_ptr, uint32_t job_id);
static int _test_only(struct job_record *job_ptr, bitstr_t *bitmap,
		      uint32_t min_nodes, uint32_t max_nodes,
		      uint32_t req_nodes, int max_share);
static int _will_run_test(struct job_record *job_ptr, bitstr_t *bitmap,
			  uint32_t min_nodes, uint32_t max_nodes,
			  int max_share, uint32_t req_nodes,
			  List preemptee_candidates,
			  List *preemptee_job_list);

extern select_nodeinfo_t *select_p_select_nodeinfo_alloc(uint32_t size);
extern int select_p_select_nodeinfo_free(select_nodeinfo_t *nodeinfo);

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "select" for SLURM node selection) and <method>
 * is a description of how this plugin satisfies that application.  SLURM will
 * only load select plugins if the plugin_type string has a
 * prefix of "select/".
 *
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum version for their plugins as the node selection API matures.
 */
const char plugin_name[]       	= "Linear node selection plugin";
const char plugin_type[]       	= "select/linear";
const uint32_t plugin_id	= 102;
const uint32_t plugin_version	= 100;

static struct node_record *select_node_ptr = NULL;
static int select_node_cnt = 0;
static uint16_t select_fast_schedule;
static uint16_t cr_type;

/* Record of resources consumed on each node including job details */
static struct cr_record *cr_ptr = NULL;
static pthread_mutex_t cr_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef HAVE_XCPU
#define XCPU_POLL_TIME 120
static pthread_t xcpu_thread = 0;
static pthread_mutex_t thread_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static int agent_fini = 0;

static void *xcpu_agent(void *args)
{
	int i;
	static time_t last_xcpu_test;
	char clone_path[128], down_node_list[512];
	struct stat buf;
	time_t now;

	last_xcpu_test = time(NULL) + XCPU_POLL_TIME;
	while (!agent_fini) {
		now = time(NULL);

		if (difftime(now, last_xcpu_test) >= XCPU_POLL_TIME) {
			debug3("Running XCPU node state test");
			down_node_list[0] = '\0';

			for (i=0; i<select_node_cnt; i++) {
				snprintf(clone_path, sizeof(clone_path),
					 "%s/%s/xcpu/clone", XCPU_DIR,
					 select_node_ptr[i].name);
				if (stat(clone_path, &buf) == 0)
					continue;
				error("stat %s: %m", clone_path);
				if ((strlen(select_node_ptr[i].name) +
				     strlen(down_node_list) + 2) <
				    sizeof(down_node_list)) {
					if (down_node_list[0] != '\0')
						strcat(down_node_list,",");
					strcat(down_node_list,
					       select_node_ptr[i].name);
				} else
					error("down_node_list overflow");
			}
			if (down_node_list[0]) {
				slurm_drain_nodes(
					down_node_list,
					"select_linear: Can not stat XCPU ",
					slurm_get_slurm_user_id());
			}
			last_xcpu_test = now;
		}

		sleep(1);
	}
	return NULL;
}

static int _init_status_pthread(void)
{
	pthread_attr_t attr;

	slurm_mutex_lock( &thread_flag_mutex );
	if ( xcpu_thread ) {
		debug2("XCPU thread already running, not starting another");
		slurm_mutex_unlock( &thread_flag_mutex );
		return SLURM_ERROR;
	}

	slurm_attr_init( &attr );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
	pthread_create( &xcpu_thread, &attr, xcpu_agent, NULL);
	slurm_mutex_unlock( &thread_flag_mutex );
	slurm_attr_destroy( &attr );

	return SLURM_SUCCESS;
}

static int _fini_status_pthread(void)
{
	int i, rc = SLURM_SUCCESS;

	slurm_mutex_lock( &thread_flag_mutex );
	if ( xcpu_thread ) {
		agent_fini = 1;
		for (i=0; i<4; i++) {
			sleep(1);
			if (pthread_kill(xcpu_thread, 0)) {
				xcpu_thread = 0;
				break;
			}
		}
		if ( xcpu_thread ) {
			error("could not kill XCPU agent thread");
			rc = SLURM_ERROR;
		}
	}
	slurm_mutex_unlock( &thread_flag_mutex );
	return rc;
}
#endif

/* Add job id to record of jobs running on this node */
static void _add_run_job(struct cr_record *cr_ptr, uint32_t job_id)
{
	int i;

	if (cr_ptr->run_job_ids == NULL) {	/* create new array */
		cr_ptr->run_job_len = RUN_JOB_INCR;
		cr_ptr->run_job_ids = xmalloc(sizeof(uint32_t) *
					      cr_ptr->run_job_len);
		cr_ptr->run_job_ids[0] = job_id;
		return;
	}

	for (i=0; i<cr_ptr->run_job_len; i++) {
		if (cr_ptr->run_job_ids[i])
			continue;
		/* fill in hole */
		cr_ptr->run_job_ids[i] = job_id;
		return;
	}

	/* expand array and add to end */
	cr_ptr->run_job_len += RUN_JOB_INCR;
	xrealloc(cr_ptr->run_job_ids, sizeof(uint32_t) * cr_ptr->run_job_len);
	cr_ptr->run_job_ids[i] = job_id;
}

/* Add job id to record of jobs running or suspended on this node */
static void _add_tot_job(struct cr_record *cr_ptr, uint32_t job_id)
{
	int i;

	if (cr_ptr->tot_job_ids == NULL) {	/* create new array */
		cr_ptr->tot_job_len = RUN_JOB_INCR;
		cr_ptr->tot_job_ids = xmalloc(sizeof(uint32_t) *
					      cr_ptr->tot_job_len);
		cr_ptr->tot_job_ids[0] = job_id;
		return;
	}

	for (i=0; i<cr_ptr->tot_job_len; i++) {
		if (cr_ptr->tot_job_ids[i])
			continue;
		/* fill in hole */
		cr_ptr->tot_job_ids[i] = job_id;
		return;
	}

	/* expand array and add to end */
	cr_ptr->tot_job_len += RUN_JOB_INCR;
	xrealloc(cr_ptr->tot_job_ids, sizeof(uint32_t) * cr_ptr->tot_job_len);
	cr_ptr->tot_job_ids[i] = job_id;
}

static bool _ck_run_job(struct cr_record *cr_ptr, uint32_t job_id,
			bool clear_it)
{
	int i;
	bool rc = false;

	if ((cr_ptr->run_job_ids == NULL) || (cr_ptr->run_job_len == 0))
		return rc;

	for (i=0; i<cr_ptr->run_job_len; i++) {
		if (cr_ptr->run_job_ids[i] != job_id)
			continue;
		if (clear_it)
			cr_ptr->run_job_ids[i] = 0;
		rc = true;
	}
	return rc;
}

/* Remove job id from record of jobs running,
 * RET true if successful, false if the job was not running */
static bool _rem_run_job(struct cr_record *cr_ptr, uint32_t job_id)
{
	return _ck_run_job(cr_ptr, job_id, true);
}

/* Test for job id in record of jobs running,
 * RET true if successful, false if the job was not running */
static bool _test_run_job(struct cr_record *cr_ptr, uint32_t job_id)
{
	return _ck_run_job(cr_ptr, job_id, false);
}

static bool _ck_tot_job(struct cr_record *cr_ptr, uint32_t job_id,
			bool clear_it)
{
	int i;
	bool rc = false;

	if ((cr_ptr->tot_job_ids == NULL) || (cr_ptr->tot_job_len == 0))
		return rc;

	for (i=0; i<cr_ptr->tot_job_len; i++) {
		if (cr_ptr->tot_job_ids[i] != job_id)
			continue;
		if (clear_it)
			cr_ptr->tot_job_ids[i] = 0;
		rc = true;
	}
	return rc;
}
/* Remove job id from record of jobs running or suspended,
 * RET true if successful, false if the job was not found */
static bool _rem_tot_job(struct cr_record *cr_ptr, uint32_t job_id)
{
	return _ck_tot_job(cr_ptr, job_id, true);
}

/* Test for job id in record of jobs running or suspended,
 * RET true if successful, false if the job was not found */
static bool _test_tot_job(struct cr_record *cr_ptr, uint32_t job_id)
{
	return _ck_tot_job(cr_ptr, job_id, false);
}

static bool _enough_nodes(int avail_nodes, int rem_nodes,
			  uint32_t min_nodes, uint32_t req_nodes)
{
	int needed_nodes;

	if (req_nodes > min_nodes)
		needed_nodes = rem_nodes + min_nodes - req_nodes;
	else
		needed_nodes = rem_nodes;

	return(avail_nodes >= needed_nodes);
}

/*
 * _get_avail_cpus - Get the number of "available" cpus on a node
 *	given this number given the number of cpus_per_task and
 *	maximum sockets, cores, threads.  Note that the value of
 *	cpus is the lowest-level logical processor (LLLP).
 * IN job_ptr - pointer to job being scheduled
 * IN index - index of node's configuration information in select_node_ptr
 */
static uint16_t _get_avail_cpus(struct job_record *job_ptr, int index)
{
	struct node_record *node_ptr;
	uint16_t avail_cpus;
	uint16_t cpus, sockets, cores, threads;
	uint16_t cpus_per_task = 1;
	uint16_t ntasks_per_node = 0, ntasks_per_socket, ntasks_per_core;
	uint16_t min_sockets, min_cores, min_threads;
	multi_core_data_t *mc_ptr = NULL;

	if (job_ptr->details == NULL)
		return (uint16_t) 0;

	if (job_ptr->details->cpus_per_task)
		cpus_per_task = job_ptr->details->cpus_per_task;
	if (job_ptr->details->ntasks_per_node)
		ntasks_per_node = job_ptr->details->ntasks_per_node;
	if ((mc_ptr = job_ptr->details->mc_ptr)) {
		ntasks_per_socket = mc_ptr->ntasks_per_socket;
		ntasks_per_core   = mc_ptr->ntasks_per_core;
		min_sockets       = mc_ptr->sockets_per_node;
		min_cores         = mc_ptr->cores_per_socket;
		min_threads       = mc_ptr->threads_per_core;
	} else {
		ntasks_per_socket = 0;
		ntasks_per_core   = 0;
		min_sockets       = (uint16_t) NO_VAL;
		min_cores         = (uint16_t) NO_VAL;
		min_threads       = (uint16_t) NO_VAL;
	}

	node_ptr = select_node_ptr + index;
	if (select_fast_schedule) { /* don't bother checking each node */
		cpus    = node_ptr->config_ptr->cpus;
		sockets = node_ptr->config_ptr->sockets;
		cores   = node_ptr->config_ptr->cores;
		threads = node_ptr->config_ptr->threads;
	} else {
		cpus    = node_ptr->cpus;
		sockets = node_ptr->sockets;
		cores   = node_ptr->cores;
		threads = node_ptr->threads;
	}

#if SELECT_DEBUG
	info("host %s HW_ cpus %u sockets %u cores %u threads %u ",
	     node_ptr->name, cpus, sockets, cores, threads);
#endif

	avail_cpus = slurm_get_avail_procs(
		min_sockets, min_cores, min_threads, cpus_per_task,
		ntasks_per_node, ntasks_per_socket, ntasks_per_core,
		&cpus, &sockets, &cores, &threads, NULL,
		CR_CPU, job_ptr->job_id, node_ptr->name);

#if SELECT_DEBUG
	debug("avail_cpus index %d = %d (out of %d %d %d %d)",
	      index, avail_cpus, cpus, sockets, cores, threads);
#endif
	return(avail_cpus);
}

/*
 * _get_total_cpus - Get the total number of cpus on a node
 *	Note that the value of cpus is the lowest-level logical
 *	processor (LLLP).
 * IN index - index of node's configuration information in select_node_ptr
 */
static uint16_t _get_total_cpus(int index)
{
	struct node_record *node_ptr = &(select_node_ptr[index]);
	if (select_fast_schedule)
		return node_ptr->config_ptr->cpus;
	else
		return node_ptr->cpus;
}

static job_resources_t *_create_job_resources(int node_cnt)
{
	job_resources_t *job_resrcs_ptr;

	job_resrcs_ptr = create_job_resources();
	job_resrcs_ptr->cpu_array_reps = xmalloc(sizeof(uint32_t) * node_cnt);
	job_resrcs_ptr->cpu_array_value = xmalloc(sizeof(uint16_t) * node_cnt);
	job_resrcs_ptr->cpus = xmalloc(sizeof(uint16_t) * node_cnt);
	job_resrcs_ptr->cpus_used = xmalloc(sizeof(uint16_t) * node_cnt);
	job_resrcs_ptr->memory_allocated = xmalloc(sizeof(uint32_t) * node_cnt);
	job_resrcs_ptr->memory_used = xmalloc(sizeof(uint32_t) * node_cnt);
	job_resrcs_ptr->nhosts = node_cnt;
	return job_resrcs_ptr;
}

/* Build the full job_resources_t *structure for a job based upon the nodes
 *	allocated to it (the bitmap) and the job's memory requirement */
static void _build_select_struct(struct job_record *job_ptr, bitstr_t *bitmap)
{
	int i, j, k;
	int first_bit, last_bit;
	uint32_t node_cpus, total_cpus = 0, node_cnt;
	struct node_record *node_ptr;
	uint32_t job_memory_cpu = 0, job_memory_node = 0;
	job_resources_t *job_resrcs_ptr;

	if (job_ptr->details->pn_min_memory  && (cr_type == CR_MEMORY)) {
		if (job_ptr->details->pn_min_memory & MEM_PER_CPU)
			job_memory_cpu = job_ptr->details->pn_min_memory &
				(~MEM_PER_CPU);
		else
			job_memory_node = job_ptr->details->pn_min_memory;
	}

	if (job_ptr->job_resrcs)	/* Old struct due to job requeue */
		free_job_resources(&job_ptr->job_resrcs);

	node_cnt = bit_set_count(bitmap);
	job_ptr->job_resrcs = job_resrcs_ptr = _create_job_resources(node_cnt);
	job_resrcs_ptr->node_bitmap = bit_copy(bitmap);
	job_resrcs_ptr->nodes = bitmap2node_name(bitmap);
	if (job_resrcs_ptr->node_bitmap == NULL)
		fatal("bit_copy malloc failure");
	job_resrcs_ptr->ncpus = job_ptr->total_cpus;
	if (build_job_resources(job_resrcs_ptr, (void *)select_node_ptr,
				select_fast_schedule))
		error("_build_select_struct: build_job_resources: %m");

	first_bit = bit_ffs(bitmap);
	last_bit  = bit_fls(bitmap);
	if (last_bit == -1)
		last_bit = -2;	/* no bits set */
	for (i=first_bit, j=0, k=-1; i<=last_bit; i++) {
		if (!bit_test(bitmap, i))
			continue;
		node_ptr = &(select_node_ptr[i]);
		if (select_fast_schedule)
			node_cpus = node_ptr->config_ptr->cpus;
		else
			node_cpus = node_ptr->cpus;
		job_resrcs_ptr->cpus[j] = node_cpus;
		if ((k == -1) ||
		    (job_resrcs_ptr->cpu_array_value[k] != node_cpus)) {
			job_resrcs_ptr->cpu_array_cnt++;
			job_resrcs_ptr->cpu_array_reps[++k] = 1;
			job_resrcs_ptr->cpu_array_value[k] = node_cpus;
		} else
			job_resrcs_ptr->cpu_array_reps[k]++;
		total_cpus += node_cpus;

		if (job_memory_node) {
			job_resrcs_ptr->memory_allocated[j] = job_memory_node;
		} else if (job_memory_cpu) {
			job_resrcs_ptr->memory_allocated[j] =
				job_memory_cpu * node_cpus;
		}

		if (set_job_resources_node(job_resrcs_ptr, j)) {
			error("_build_select_struct: set_job_resources_node: "
			      "%m");
		}
		j++;
	}
	if (job_resrcs_ptr->ncpus != total_cpus) {
		error("_build_select_struct: ncpus mismatch %u != %u",
		      job_resrcs_ptr->ncpus, total_cpus);
	}
}

/*
 * Set the bits in 'jobmap' that correspond to bits in the 'bitmap'
 * that are running 'run_job_cnt' jobs or less, and clear the rest.
 */
static int _job_count_bitmap(struct cr_record *cr_ptr,
			     struct job_record *job_ptr,
			     bitstr_t * bitmap, bitstr_t * jobmap,
			     int run_job_cnt, int tot_job_cnt, uint16_t mode)
{
	int i, i_first, i_last;
	int count = 0, total_jobs, total_run_jobs;
	struct part_cr_record *part_cr_ptr;
	struct node_record *node_ptr;
	uint32_t job_memory_cpu = 0, job_memory_node = 0;
	uint32_t alloc_mem = 0, job_mem = 0, avail_mem = 0;
	uint32_t cpu_cnt, gres_cpus;
	List gres_list;
	bool use_total_gres = true;

	xassert(cr_ptr);
	xassert(cr_ptr->nodes);
	if (mode != SELECT_MODE_TEST_ONLY) {
		use_total_gres = false;
		if (job_ptr->details->pn_min_memory  &&
		    (cr_type == CR_MEMORY)) {
			if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
				job_memory_cpu = job_ptr->details->pn_min_memory
					& (~MEM_PER_CPU);
			} else {
				job_memory_node = job_ptr->details->
					pn_min_memory;
			}
		}
	}

	i_first = bit_ffs(bitmap);
	i_last  = bit_fls(bitmap);
	if (i_first == -1)	/* job has no nodes */
		i_last = -2;
	for (i = i_first; i <= i_last; i++) {
		if (!bit_test(bitmap, i)) {
			bit_clear(jobmap, i);
			continue;
		}

		node_ptr = node_record_table_ptr + i;
		if (select_fast_schedule)
			cpu_cnt = node_ptr->config_ptr->cpus;
		else
			cpu_cnt = node_ptr->cpus;

		if (cr_ptr->nodes[i].gres_list)
			gres_list = cr_ptr->nodes[i].gres_list;
		else
			gres_list = node_ptr->gres_list;
		gres_cpus = gres_plugin_job_test(job_ptr->gres_list,
						 gres_list, use_total_gres,
						 NULL, 0, 0, job_ptr->job_id,
						 node_ptr->name);
		if ((gres_cpus != NO_VAL) && (gres_cpus < cpu_cnt)) {
			bit_clear(jobmap, i);
			continue;
		}

		if (mode == SELECT_MODE_TEST_ONLY) {
			bit_set(jobmap, i);
			count++;
			continue;	/* No need to test other resources */
		}

		if (job_memory_cpu || job_memory_node) {
			alloc_mem = cr_ptr->nodes[i].alloc_memory;
			if (select_fast_schedule) {
				avail_mem = node_ptr->config_ptr->real_memory;
				if (job_memory_cpu)
					job_mem = job_memory_cpu * cpu_cnt;
				else
					job_mem = job_memory_node;
			} else {
				avail_mem = node_ptr->real_memory;
				if (job_memory_cpu)
					job_mem = job_memory_cpu * cpu_cnt;
				else
					job_mem = job_memory_node;
			}
			if ((alloc_mem + job_mem) > avail_mem) {
				bit_clear(jobmap, i);
				continue;
			}
		}

		if ((mode != SELECT_MODE_TEST_ONLY) &&
		    (cr_ptr->nodes[i].exclusive_cnt != 0)) {
			/* already reserved by some exclusive job */
			bit_clear(jobmap, i);
			continue;
		}

		total_jobs = 0;
		total_run_jobs = 0;
		part_cr_ptr = cr_ptr->nodes[i].parts;
		while (part_cr_ptr) {
			total_run_jobs += part_cr_ptr->run_job_cnt;
			total_jobs     += part_cr_ptr->tot_job_cnt;
			part_cr_ptr = part_cr_ptr->next;
		}
		if ((total_run_jobs <= run_job_cnt) &&
		    (total_jobs     <= tot_job_cnt)) {
			bit_set(jobmap, i);
			count++;
		} else {
			bit_clear(jobmap, i);
		}

	}
	return count;
}

/* _find_job_mate - does most of the real work for select_p_job_test(),
 *	in trying to find a suitable job to mate this one with. This is
 *	a pretty simple algorithm now, but could try to match the job
 *	with multiple jobs that add up to the proper size or a single
 *	job plus a few idle nodes. */
static int _find_job_mate(struct job_record *job_ptr, bitstr_t *bitmap,
			  uint32_t min_nodes, uint32_t max_nodes,
			  uint32_t req_nodes)
{
	ListIterator job_iterator;
	struct job_record *job_scan_ptr;
	int rc = EINVAL;

	job_iterator = list_iterator_create(job_list);
	while ((job_scan_ptr = (struct job_record *) list_next(job_iterator))) {
		if ((!IS_JOB_RUNNING(job_scan_ptr))			||
		    (job_scan_ptr->node_cnt   != req_nodes)		||
		    (job_scan_ptr->total_cpus <
		     job_ptr->details->min_cpus)			||
		    (!bit_super_set(job_scan_ptr->node_bitmap, bitmap)))
			continue;
		if (job_scan_ptr->details && job_ptr->details &&
		    (job_scan_ptr->details->contiguous !=
		     job_ptr->details->contiguous))
			continue;

		if (job_ptr->details->req_node_bitmap &&
		    (!bit_super_set(job_ptr->details->req_node_bitmap,
				    job_scan_ptr->node_bitmap)))
			continue;	/* Required nodes missing from job */

		if (job_ptr->details->exc_node_bitmap &&
		    (bit_overlap(job_ptr->details->exc_node_bitmap,
				 job_scan_ptr->node_bitmap) != 0))
			continue;	/* Excluded nodes in this job */

		bit_and(bitmap, job_scan_ptr->node_bitmap);
		job_ptr->total_cpus = job_scan_ptr->total_cpus;
		rc = SLURM_SUCCESS;
		break;
	}
	list_iterator_destroy(job_iterator);
	return rc;
}

/* _job_test - does most of the real work for select_p_job_test(), which
 *	pretty much just handles load-leveling and max_share logic */
static int _job_test(struct job_record *job_ptr, bitstr_t *bitmap,
		     uint32_t min_nodes, uint32_t max_nodes,
		     uint32_t req_nodes)
{
	int i, index, error_code = EINVAL, sufficient;
	int *consec_nodes;	/* how many nodes we can add from this
				 * consecutive set of nodes */
	int *consec_cpus;	/* how many nodes we can add from this
				 * consecutive set of nodes */
	int *consec_start;	/* where this consecutive set starts (index) */
	int *consec_end;	/* where this consecutive set ends (index) */
	int *consec_req;	/* are nodes from this set required
				 * (in req_bitmap) */
	int consec_index, consec_size;
	int rem_cpus, rem_nodes;	/* remaining resources desired */
	int best_fit_nodes, best_fit_cpus, best_fit_req;
	int best_fit_location = 0, best_fit_sufficient;
	int avail_cpus, alloc_cpus = 0, total_cpus = 0;

	if (bit_set_count(bitmap) < min_nodes)
		return error_code;

	if ((job_ptr->details->req_node_bitmap) &&
	    (!bit_super_set(job_ptr->details->req_node_bitmap, bitmap)))
		return error_code;

	if (switch_record_cnt && switch_record_table) {
		/* Perform optimized resource selection based upon topology */
		return _job_test_topo(job_ptr, bitmap,
				      min_nodes, max_nodes, req_nodes);
	}

	consec_index = 0;
	consec_size  = 50;	/* start allocation for 50 sets of
				 * consecutive nodes */
	consec_cpus  = xmalloc(sizeof(int) * consec_size);
	consec_nodes = xmalloc(sizeof(int) * consec_size);
	consec_start = xmalloc(sizeof(int) * consec_size);
	consec_end   = xmalloc(sizeof(int) * consec_size);
	consec_req   = xmalloc(sizeof(int) * consec_size);


	/* Build table with information about sets of consecutive nodes */
	consec_cpus[consec_index] = consec_nodes[consec_index] = 0;
	consec_req[consec_index] = -1;	/* no required nodes here by default */
	rem_cpus = job_ptr->details->min_cpus;
	if (req_nodes > min_nodes)
		rem_nodes = req_nodes;
	else
		rem_nodes = min_nodes;

	for (index = 0; index < select_node_cnt; index++) {
		if (bit_test(bitmap, index)) {
			if (consec_nodes[consec_index] == 0)
				consec_start[consec_index] = index;
			avail_cpus = _get_avail_cpus(job_ptr, index);
			if (job_ptr->details->req_node_bitmap	&&
			    (max_nodes > 0)			&&
			    bit_test(job_ptr->details->req_node_bitmap,index)){
				if (consec_req[consec_index] == -1) {
					/* first required node in set */
					consec_req[consec_index] = index;
				}
				rem_nodes--;
				max_nodes--;
				rem_cpus   -= avail_cpus;
				alloc_cpus += avail_cpus;
				total_cpus += _get_total_cpus(index);
			} else {	 /* node not required (yet) */
				bit_clear(bitmap, index);
				consec_cpus[consec_index] += avail_cpus;
				consec_nodes[consec_index]++;
			}
		} else if (consec_nodes[consec_index] == 0) {
			consec_req[consec_index] = -1;
			/* already picked up any required nodes */
			/* re-use this record */
		} else {
			consec_end[consec_index] = index - 1;
			if (++consec_index >= consec_size) {
				consec_size *= 2;
				xrealloc(consec_cpus,
					 sizeof(int) * consec_size);
				xrealloc(consec_nodes,
					 sizeof(int) * consec_size);
				xrealloc(consec_start,
					 sizeof(int) * consec_size);
				xrealloc(consec_end,
					 sizeof(int) * consec_size);
				xrealloc(consec_req,
					 sizeof(int) * consec_size);
			}
			consec_cpus[consec_index] = 0;
			consec_nodes[consec_index] = 0;
			consec_req[consec_index] = -1;
		}
	}
	if (consec_nodes[consec_index] != 0)
		consec_end[consec_index++] = index - 1;

#if SELECT_DEBUG
	/* don't compile this, it slows things down too much */
	debug3("rem_cpus=%d, rem_nodes=%d", rem_cpus, rem_nodes);
	for (i = 0; i < consec_index; i++) {
		if (consec_req[i] != -1)
			debug3("start=%s, end=%s, nodes=%d, cpus=%d, req=%s",
			       select_node_ptr[consec_start[i]].name,
			       select_node_ptr[consec_end[i]].name,
			       consec_nodes[i], consec_cpus[i],
			       select_node_ptr[consec_req[i]].name);
		else
			debug3("start=%s, end=%s, nodes=%d, cpus=%d",
			       select_node_ptr[consec_start[i]].name,
			       select_node_ptr[consec_end[i]].name,
			       consec_nodes[i], consec_cpus[i]);
	}
#endif

	/* accumulate nodes from these sets of consecutive nodes until */
	/*   sufficient resources have been accumulated */
	while (consec_index && (max_nodes > 0)) {
		best_fit_cpus = best_fit_nodes = best_fit_sufficient = 0;
		best_fit_req = -1;	/* first required node, -1 if none */
		for (i = 0; i < consec_index; i++) {
			if (consec_nodes[i] == 0)
				continue;	/* no usable nodes here */

			if (job_ptr->details->contiguous &&
			    job_ptr->details->req_node_bitmap &&
			    (consec_req[i] == -1))
				continue;	/* no required nodes here */

			sufficient = (consec_cpus[i] >= rem_cpus) &&
				_enough_nodes(consec_nodes[i], rem_nodes,
					      min_nodes, req_nodes);

			/* if first possibility OR */
			/* contains required nodes OR */
			/* first set large enough for request OR */
			/* tightest fit (less resource waste) OR */
			/* nothing yet large enough, but this is biggest */
			if ((best_fit_nodes == 0) ||
			    ((best_fit_req == -1) && (consec_req[i] != -1))  ||
			    (sufficient && (best_fit_sufficient == 0))       ||
			    (sufficient && (consec_cpus[i] < best_fit_cpus)) ||
			    ((sufficient == 0) &&
			     (consec_cpus[i] > best_fit_cpus))) {
				best_fit_cpus = consec_cpus[i];
				best_fit_nodes = consec_nodes[i];
				best_fit_location = i;
				best_fit_req = consec_req[i];
				best_fit_sufficient = sufficient;
			}

			if (job_ptr->details->contiguous &&
			    job_ptr->details->req_node_bitmap) {
				/* Must wait for all required nodes to be
				 * in a single consecutive block */
				int j, other_blocks = 0;
				for (j = (i+1); j < consec_index; j++) {
					if (consec_req[j] != -1) {
						other_blocks = 1;
						break;
					}
				}
				if (other_blocks) {
					best_fit_nodes = 0;
					break;
				}
			}
		}
		if (best_fit_nodes == 0)
			break;
		if (job_ptr->details->contiguous &&
		    ((best_fit_cpus < rem_cpus) ||
		     (!_enough_nodes(best_fit_nodes, rem_nodes,
				     min_nodes, req_nodes))))
			break;	/* no hole large enough */
		if (best_fit_req != -1) {
			/* This collection of nodes includes required ones
			 * select nodes from this set, first working up
			 * then down from the required nodes */
			for (i = best_fit_req;
			     i <= consec_end[best_fit_location]; i++) {
				if ((max_nodes <= 0) ||
				    ((rem_nodes <= 0) && (rem_cpus <= 0)))
					break;
				if (bit_test(bitmap, i))
					continue;
				bit_set(bitmap, i);
				rem_nodes--;
				max_nodes--;
				avail_cpus = _get_avail_cpus(job_ptr, i);
				rem_cpus   -= avail_cpus;
				alloc_cpus += avail_cpus;
				total_cpus += _get_total_cpus(i);
			}
			for (i = (best_fit_req - 1);
			     i >= consec_start[best_fit_location]; i--) {
				if ((max_nodes <= 0) ||
				    ((rem_nodes <= 0) && (rem_cpus <= 0)))
					break;
				if (bit_test(bitmap, i))
					continue;
				bit_set(bitmap, i);
				rem_nodes--;
				max_nodes--;
				avail_cpus = _get_avail_cpus(job_ptr, i);
				rem_cpus   -= avail_cpus;
				alloc_cpus += avail_cpus;
				total_cpus += _get_total_cpus(i);
			}
		} else {
			for (i = consec_start[best_fit_location];
			     i <= consec_end[best_fit_location]; i++) {
				if ((max_nodes <= 0) ||
				    ((rem_nodes <= 0) && (rem_cpus <= 0)))
					break;
				if (bit_test(bitmap, i))
					continue;
				bit_set(bitmap, i);
				rem_nodes--;
				max_nodes--;
				avail_cpus = _get_avail_cpus(job_ptr, i);
				rem_cpus   -= avail_cpus;
				alloc_cpus += avail_cpus;
				total_cpus += _get_total_cpus(i);
			}
		}
		if (job_ptr->details->contiguous ||
		    ((rem_nodes <= 0) && (rem_cpus <= 0))) {
			error_code = SLURM_SUCCESS;
			break;
		}
		consec_cpus[best_fit_location] = 0;
		consec_nodes[best_fit_location] = 0;
	}

	if (error_code && (rem_cpus <= 0) &&
	    _enough_nodes(0, rem_nodes, min_nodes, req_nodes)) {
		error_code = SLURM_SUCCESS;
	}
	if (error_code == SLURM_SUCCESS) {
		/* job's total_cpus is needed for SELECT_MODE_WILL_RUN */
		job_ptr->total_cpus = total_cpus;
	}

	xfree(consec_cpus);
	xfree(consec_nodes);
	xfree(consec_start);
	xfree(consec_end);
	xfree(consec_req);
	return error_code;
}

/*
 * _job_test_topo - A topology aware version of _job_test()
 * NOTE: The logic here is almost identical to that of _eval_nodes_topo() in
 *       select/cons_res/job_test.c. Any bug found here is probably also there.
 */
static int _job_test_topo(struct job_record *job_ptr, bitstr_t *bitmap,
			  uint32_t min_nodes, uint32_t max_nodes,
			  uint32_t req_nodes)
{
	bitstr_t **switches_bitmap;		/* nodes on this switch */
	int       *switches_cpu_cnt;		/* total CPUs on switch */
	int       *switches_node_cnt;		/* total nodes on switch */
	int       *switches_required;		/* set if has required node */

	bitstr_t  *avail_nodes_bitmap = NULL;	/* nodes on any switch */
	bitstr_t  *req_nodes_bitmap   = NULL;
	int rem_cpus, rem_nodes;	/* remaining resources desired */
	int avail_cpus, alloc_cpus = 0, total_cpus = 0;
	int i, j, rc = SLURM_SUCCESS;
	int best_fit_inx, first, last;
	int best_fit_nodes, best_fit_cpus;
	int best_fit_location = 0, best_fit_sufficient;
	bool sufficient;

	rem_cpus = job_ptr->details->min_cpus;
	if (req_nodes > min_nodes)
		rem_nodes = req_nodes;
	else
		rem_nodes = min_nodes;

	if (job_ptr->details->req_node_bitmap) {
		req_nodes_bitmap = bit_copy(job_ptr->details->req_node_bitmap);
		i = bit_set_count(req_nodes_bitmap);
		if (i > max_nodes) {
			info("job %u requires more nodes than currently "
			     "available (%u>%u)",
			     job_ptr->job_id, i, max_nodes);
			rc = EINVAL;
			goto fini;
		}
	}

	/* Construct a set of switch array entries,
	 * use the same indexes as switch_record_table in slurmctld */
	switches_bitmap   = xmalloc(sizeof(bitstr_t *) * switch_record_cnt);
	switches_cpu_cnt  = xmalloc(sizeof(int)        * switch_record_cnt);
	switches_node_cnt = xmalloc(sizeof(int)        * switch_record_cnt);
	switches_required = xmalloc(sizeof(int)        * switch_record_cnt);
	avail_nodes_bitmap = bit_alloc(node_record_count);
	for (i=0; i<switch_record_cnt; i++) {
		switches_bitmap[i] = bit_copy(switch_record_table[i].
					      node_bitmap);
		bit_and(switches_bitmap[i], bitmap);
		bit_or(avail_nodes_bitmap, switches_bitmap[i]);
		switches_node_cnt[i] = bit_set_count(switches_bitmap[i]);
		if (req_nodes_bitmap &&
		    bit_overlap(req_nodes_bitmap, switches_bitmap[i])) {
			switches_required[i] = 1;
		}
	}
	bit_nclear(bitmap, 0, node_record_count - 1);

#if SELECT_DEBUG
	/* Don't compile this, it slows things down too much */
	for (i=0; i<switch_record_cnt; i++) {
		char *node_names = NULL;
		if (switches_node_cnt[i])
			node_names = bitmap2node_name(switches_bitmap[i]);
		debug("switch=%s nodes=%u:%s required:%u speed=%u",
		      switch_record_table[i].name,
		      switches_node_cnt[i], node_names,
		      switches_required[i],
		      switch_record_table[i].link_speed);
		xfree(node_names);
	}
#endif

	if (req_nodes_bitmap &&
	    (!bit_super_set(req_nodes_bitmap, avail_nodes_bitmap))) {
		info("job %u requires nodes not available on any switch",
		     job_ptr->job_id);
		rc = EINVAL;
		goto fini;
	}

	if (req_nodes_bitmap) {
		/* Accumulate specific required resources, if any */
		first = bit_ffs(req_nodes_bitmap);
		last  = bit_fls(req_nodes_bitmap);
		for (i=first; ((i<=last) && (first>=0)); i++) {
			if (!bit_test(req_nodes_bitmap, i))
				continue;
			if (max_nodes <= 0) {
				info("job %u requires nodes than allowed",
				     job_ptr->job_id);
				rc = EINVAL;
				goto fini;
			}
			bit_set(bitmap, i);
			bit_clear(avail_nodes_bitmap, i);
			rem_nodes--;
			max_nodes--;
			avail_cpus = _get_avail_cpus(job_ptr, i);
			rem_cpus   -= avail_cpus;
			alloc_cpus += avail_cpus;
			total_cpus += _get_total_cpus(i);
			for (j=0; j<switch_record_cnt; j++) {
				if (!bit_test(switches_bitmap[j], i))
					continue;
				bit_clear(switches_bitmap[j], i);
				switches_node_cnt[j]--;
			}
		}
		if ((rem_nodes <= 0) && (rem_cpus <= 0))
			goto fini;

		/* Accumulate additional resources from leafs that
		 * contain required nodes */
		for (j=0; j<switch_record_cnt; j++) {
			if ((switch_record_table[j].level != 0) ||
			    (switches_node_cnt[j] == 0) ||
			    (switches_required[j] == 0)) {
				continue;
			}
			while ((max_nodes > 0) &&
			       ((rem_nodes > 0) || (rem_cpus > 0))) {
				i = bit_ffs(switches_bitmap[j]);
				if (i == -1)
					break;
				bit_clear(switches_bitmap[j], i);
				switches_node_cnt[j]--;
				if (bit_test(bitmap, i)) {
					/* node on multiple leaf switches
					 * and already selected */
					continue;
				}
				bit_set(bitmap, i);
				bit_clear(avail_nodes_bitmap, i);
				rem_nodes--;
				max_nodes--;
				avail_cpus = _get_avail_cpus(job_ptr, i);
				rem_cpus   -= avail_cpus;
				alloc_cpus += avail_cpus;
				total_cpus += _get_total_cpus(i);
			}
		}
		if ((rem_nodes <= 0) && (rem_cpus <= 0))
			goto fini;

		/* Update bitmaps and node counts for higher-level switches */
		for (j=0; j<switch_record_cnt; j++) {
			if (switches_node_cnt[j] == 0)
				continue;
			first = bit_ffs(switches_bitmap[j]);
			if (first < 0)
				continue;
			last  = bit_fls(switches_bitmap[j]);
			for (i=first; i<=last; i++) {
				if (!bit_test(switches_bitmap[j], i))
					continue;
				if (!bit_test(avail_nodes_bitmap, i)) {
					/* cleared from lower level */
					bit_clear(switches_bitmap[j], i);
					switches_node_cnt[j]--;
				} else {
					switches_cpu_cnt[j] +=
						_get_avail_cpus(job_ptr, i);
				}
			}
		}
	} else {
		/* No specific required nodes, calculate CPU counts */
		for (j=0; j<switch_record_cnt; j++) {
			first = bit_ffs(switches_bitmap[j]);
			if (first < 0)
				continue;
			last  = bit_fls(switches_bitmap[j]);
			for (i=first; i<=last; i++) {
				if (!bit_test(switches_bitmap[j], i))
					continue;
				switches_cpu_cnt[j] +=
					_get_avail_cpus(job_ptr, i);
			}
		}
	}

	/* Determine lowest level switch satifying request with best fit */
	best_fit_inx = -1;
	for (j=0; j<switch_record_cnt; j++) {
		if ((switches_cpu_cnt[j]  < rem_cpus) ||
		    (!_enough_nodes(switches_node_cnt[j], rem_nodes,
				    min_nodes, req_nodes)))
			continue;
		if ((best_fit_inx == -1) ||
		    (switch_record_table[j].level <
		     switch_record_table[best_fit_inx].level) ||
		    ((switch_record_table[j].level ==
		      switch_record_table[best_fit_inx].level) &&
		     (switches_node_cnt[j] < switches_node_cnt[best_fit_inx])))
			best_fit_inx = j;
	}
	if (best_fit_inx == -1) {
		debug("_job_test_topo: could not find resources for job %u",
		      job_ptr->job_id);
		rc = EINVAL;
		goto fini;
	}
	bit_and(avail_nodes_bitmap, switches_bitmap[best_fit_inx]);

	/* Identify usable leafs (within higher switch having best fit) */
	for (j=0; j<switch_record_cnt; j++) {
		if ((switch_record_table[j].level != 0) ||
		    (!bit_super_set(switches_bitmap[j],
				    switches_bitmap[best_fit_inx]))) {
			switches_node_cnt[j] = 0;
		}
	}

	/* Select resources from these leafs on a best-fit basis */
	while ((max_nodes > 0) && ((rem_nodes > 0) || (rem_cpus > 0))) {
		best_fit_cpus = best_fit_nodes = best_fit_sufficient = 0;
		for (j=0; j<switch_record_cnt; j++) {
			if (switches_node_cnt[j] == 0)
				continue;
			sufficient = (switches_cpu_cnt[j] >= rem_cpus) &&
				_enough_nodes(switches_node_cnt[j],
					      rem_nodes, min_nodes,
					      req_nodes);
			/* If first possibility OR */
			/* first set large enough for request OR */
			/* tightest fit (less resource waste) OR */
			/* nothing yet large enough, but this is biggest */
			if ((best_fit_nodes == 0) ||
			    (sufficient && (best_fit_sufficient == 0)) ||
			    (sufficient &&
			     (switches_cpu_cnt[j] < best_fit_cpus)) ||
			    ((sufficient == 0) &&
			     (switches_cpu_cnt[j] > best_fit_cpus))) {
				best_fit_cpus =  switches_cpu_cnt[j];
				best_fit_nodes = switches_node_cnt[j];
				best_fit_location = j;
				best_fit_sufficient = sufficient;
			}
		}
		if (best_fit_nodes == 0)
			break;
		/* Use select nodes from this leaf */
		first = bit_ffs(switches_bitmap[best_fit_location]);
		last  = bit_fls(switches_bitmap[best_fit_location]);
		for (i=first; ((i<=last) && (first>=0)); i++) {
			if (!bit_test(switches_bitmap[best_fit_location], i))
				continue;

			bit_clear(switches_bitmap[best_fit_location], i);
			switches_node_cnt[best_fit_location]--;
			avail_cpus = _get_avail_cpus(job_ptr, i);
			switches_cpu_cnt[best_fit_location] -= avail_cpus;

			if (bit_test(bitmap, i)) {
				/* node on multiple leaf switches
				 * and already selected */
				continue;
			}

			bit_set(bitmap, i);
			rem_nodes--;
			max_nodes--;
			rem_cpus   -= avail_cpus;
			alloc_cpus += avail_cpus;
			total_cpus += _get_total_cpus(i);
			if ((max_nodes <= 0) ||
			    ((rem_nodes <= 0) && (rem_cpus <= 0)))
				break;
		}
		switches_node_cnt[best_fit_location] = 0;
	}
	if ((rem_cpus <= 0) &&
	    _enough_nodes(0, rem_nodes, min_nodes, req_nodes)) {
		rc = SLURM_SUCCESS;
	} else
		rc = EINVAL;

fini:	if (rc == SLURM_SUCCESS) {
		/* Job's total_cpus is needed for SELECT_MODE_WILL_RUN */
		job_ptr->total_cpus = total_cpus;
	}
	FREE_NULL_BITMAP(avail_nodes_bitmap);
	FREE_NULL_BITMAP(req_nodes_bitmap);
	for (i=0; i<switch_record_cnt; i++)
		FREE_NULL_BITMAP(switches_bitmap[i]);
	xfree(switches_bitmap);
	xfree(switches_cpu_cnt);
	xfree(switches_node_cnt);
	xfree(switches_required);

	return rc;
}


/*
 * deallocate resources that were assigned to this job
 *
 * if remove_all = false: the job has been suspended, so just deallocate CPUs
 * if remove_all = true: deallocate all resources
 */
static int _rm_job_from_nodes(struct cr_record *cr_ptr,
			      struct job_record *job_ptr, char *pre_err,
			      bool remove_all)
{
	int i, i_first, i_last, node_offset, rc = SLURM_SUCCESS;
	struct part_cr_record *part_cr_ptr;
	job_resources_t *job_resrcs_ptr;
	uint32_t job_memory, job_memory_cpu = 0, job_memory_node = 0;
	bool exclusive, is_job_running;
	uint16_t cpu_cnt;
	struct node_record *node_ptr;
	List gres_list;

	if (cr_ptr == NULL) {
		error("%s: cr_ptr not initialized", pre_err);
		return SLURM_ERROR;
	}

	if (_rem_tot_job(cr_ptr, job_ptr->job_id) == 0) {
		info("select/linear: job %u has no resources allocated",
		     job_ptr->job_id);
		return SLURM_ERROR;
	}

	if (remove_all && job_ptr->details &&
	    job_ptr->details->pn_min_memory && (cr_type == CR_MEMORY)) {
		if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
			job_memory_cpu = job_ptr->details->pn_min_memory &
				(~MEM_PER_CPU);
		} else
			job_memory_node = job_ptr->details->pn_min_memory;
	}

	if ((job_resrcs_ptr = job_ptr->job_resrcs) == NULL) {
		error("job %u lacks a job_resources struct", job_ptr->job_id);
		return SLURM_ERROR;
	}

	is_job_running = _rem_run_job(cr_ptr, job_ptr->job_id);
	exclusive = (job_ptr->details->shared == 0);
	i_first = bit_ffs(job_resrcs_ptr->node_bitmap);
	i_last  = bit_fls(job_resrcs_ptr->node_bitmap);
	if (i_first == -1)	/* job has no nodes */
		i_last = -2;
	node_offset = -1;
	for (i = i_first; i <= i_last; i++) {
		if (!bit_test(job_resrcs_ptr->node_bitmap, i))
			continue;
		node_offset++;
		if (!bit_test(job_ptr->node_bitmap, i))
			continue;

		node_ptr = node_record_table_ptr + i;
		if (select_fast_schedule)
			cpu_cnt = node_ptr->config_ptr->cpus;
		else
			cpu_cnt = node_ptr->cpus;
		if (job_memory_cpu)
			job_memory = job_memory_cpu * cpu_cnt;
		else
			job_memory = job_memory_node;
		if (cr_ptr->nodes[i].alloc_memory >= job_memory)
			cr_ptr->nodes[i].alloc_memory -= job_memory;
		else {
			/* This can be the result of FastSchedule=0 and
			 * the node being configured with fewer CPUs than
			 * actually exist. The job allocation set when
			 * slurmctld restarts may be based upon a lower CPU
			 * count than when the job gets deallocated. */
			if (select_fast_schedule ||
			    (node_ptr->config_ptr->cpus == node_ptr->cpus)) {
				error("%s: memory underflow for node %s",
				      pre_err, node_ptr->name);
			} else {
				debug("%s: memory underflow for node %s",
				      pre_err, node_ptr->name);
			}
			cr_ptr->nodes[i].alloc_memory = 0;
		}

		if (remove_all) {
			if (cr_ptr->nodes[i].gres_list)
				gres_list = cr_ptr->nodes[i].gres_list;
			else
				gres_list = node_ptr->gres_list;
			gres_plugin_job_dealloc(job_ptr->gres_list, gres_list,
						node_offset, job_ptr->job_id,
						node_ptr->name);
			gres_plugin_node_state_log(gres_list, node_ptr->name);
		}

		if (exclusive) {
			if (cr_ptr->nodes[i].exclusive_cnt)
				cr_ptr->nodes[i].exclusive_cnt--;
			else {
				error("%s: exclusive_cnt underflow for "
				      "node %s", pre_err, node_ptr->name);
			}
		}

		part_cr_ptr = cr_ptr->nodes[i].parts;
		while (part_cr_ptr) {
			if (part_cr_ptr->part_ptr != job_ptr->part_ptr) {
				part_cr_ptr = part_cr_ptr->next;
				continue;
			}
			if (!is_job_running)
				/* cancelled job already suspended */;
			else if (part_cr_ptr->run_job_cnt > 0)
				part_cr_ptr->run_job_cnt--;
			else {
				error("%s: run_job_cnt underflow for node %s",
				      pre_err, node_ptr->name);
			}
			if (remove_all) {
				if (part_cr_ptr->tot_job_cnt > 0)
					part_cr_ptr->tot_job_cnt--;
				else {
					error("%s: tot_job_cnt underflow "
					      "for node %s",
					      pre_err, node_ptr->name);
				}
				if ((part_cr_ptr->tot_job_cnt == 0) &&
				    (part_cr_ptr->run_job_cnt)) {
					part_cr_ptr->run_job_cnt = 0;
					error("%s: run_job_cnt out of sync "
					      "for node %s",
					      pre_err, node_ptr->name);
				}
			}
			break;
		}
		if (part_cr_ptr == NULL) {
			if (job_ptr->part_nodes_missing) {
				;
			} else if (job_ptr->part_ptr) {
				info("%s: job %u and its partition %s "
				     "no longer contain node %s",
				     pre_err, job_ptr->job_id,
				     job_ptr->partition, node_ptr->name);
			} else {
				info("%s: job %u has no pointer to partition "
				     "%s and node %s",
				     pre_err, job_ptr->job_id,
				     job_ptr->partition, node_ptr->name);
			}
			job_ptr->part_nodes_missing = true;
			rc = SLURM_ERROR;
		}
	}

	return rc;
}

/* Move all resources from one job to another */
static int _job_expand(struct job_record *from_job_ptr,
		       struct job_record *to_job_ptr)
{
	int i, node_cnt, rc = SLURM_SUCCESS;
	struct node_record *node_ptr;
	job_resources_t *from_job_resrcs_ptr, *to_job_resrcs_ptr,
		        *new_job_resrcs_ptr;
	bool from_node_used, to_node_used;
	int from_node_offset, to_node_offset, new_node_offset;
	uint32_t from_job_memory_cpu  = 0, to_job_memory_cpu  = 0;
	uint32_t from_job_memory_node = 0, to_job_memory_node = 0;
	int first_bit, last_bit;

	xassert(from_job_ptr);
	xassert(to_job_ptr);
	if (cr_ptr == NULL) {
		error("select/linear: cr_ptr not initialized");
		return SLURM_ERROR;
	}

	if (from_job_ptr->job_id == to_job_ptr->job_id) {
		error("select/linear: attempt to merge job %u with self",
		      from_job_ptr->job_id);
		return SLURM_ERROR;
	}
	if (_test_tot_job(cr_ptr, from_job_ptr->job_id) == 0) {
		info("select/linear: job %u has no resources allocated",
		     from_job_ptr->job_id);
		return SLURM_ERROR;
	}
	if (_test_tot_job(cr_ptr, to_job_ptr->job_id) == 0) {
		info("select/linear: job %u has no resources allocated",
		     to_job_ptr->job_id);
		return SLURM_ERROR;
	}

	if (from_job_ptr->gres_list || to_job_ptr->gres_list) {
		/* This is possible to add, but very complex and fragile */
		info("select/linear: attempt to merge job %u with GRES",
		     from_job_ptr->job_id);
		return ESLURM_EXPAND_GRES;
	}

	from_job_resrcs_ptr = from_job_ptr->job_resrcs;
	if ((from_job_resrcs_ptr == NULL) ||
	    (from_job_resrcs_ptr->cpus == NULL) ||
	    (from_job_resrcs_ptr->node_bitmap == NULL)) {
		error("select/linear: job %u lacks a job_resources struct",
		      from_job_ptr->job_id);
		return SLURM_ERROR;
	}
	to_job_resrcs_ptr = to_job_ptr->job_resrcs;
	if ((to_job_resrcs_ptr == NULL) ||
	    (to_job_resrcs_ptr->cpus == NULL) ||
	    (to_job_resrcs_ptr->node_bitmap == NULL)) {
		error("select/linear: job %u lacks a job_resources struct",
		      to_job_ptr->job_id);
		return SLURM_ERROR;
	}

	if (to_job_resrcs_ptr->core_bitmap_used) {
		i = bit_size(to_job_resrcs_ptr->core_bitmap_used);
		bit_nclear(to_job_resrcs_ptr->core_bitmap_used, 0, i-1);
	}

	if (from_job_ptr->details &&
	    from_job_ptr->details->pn_min_memory && (cr_type == CR_MEMORY)) {
		if (from_job_ptr->details->pn_min_memory & MEM_PER_CPU) {
			from_job_memory_cpu = from_job_ptr->details->
					      pn_min_memory & (~MEM_PER_CPU);
		} else {
			from_job_memory_node = from_job_ptr->details->
					       pn_min_memory;
		}
	}
	if (to_job_ptr->details &&
	    to_job_ptr->details->pn_min_memory && (cr_type == CR_MEMORY)) {
		if (to_job_ptr->details->pn_min_memory & MEM_PER_CPU) {
			to_job_memory_cpu = to_job_ptr->details->
					    pn_min_memory & (~MEM_PER_CPU);
		} else {
			to_job_memory_node = to_job_ptr->details->
					     pn_min_memory;
		}
	}

	node_cnt = bit_set_count(from_job_resrcs_ptr->node_bitmap) +
		   bit_set_count(to_job_resrcs_ptr->node_bitmap);
	new_job_resrcs_ptr = _create_job_resources(node_cnt);
	new_job_resrcs_ptr->ncpus = from_job_resrcs_ptr->ncpus +
				    to_job_resrcs_ptr->ncpus;
	new_job_resrcs_ptr->node_req = to_job_resrcs_ptr->node_req;
	new_job_resrcs_ptr->node_bitmap = bit_copy(to_job_resrcs_ptr->
						   node_bitmap);
	bit_or(new_job_resrcs_ptr->node_bitmap,
	       from_job_resrcs_ptr->node_bitmap);
	new_job_resrcs_ptr->nodes = bitmap2node_name(new_job_resrcs_ptr->
						     node_bitmap);
	build_job_resources(new_job_resrcs_ptr, node_record_table_ptr,
			    select_fast_schedule);
	xfree(to_job_ptr->node_addr);
	to_job_ptr->node_addr = xmalloc(sizeof(slurm_addr_t) * node_cnt);
	first_bit = MIN(bit_ffs(from_job_resrcs_ptr->node_bitmap),
			bit_ffs(to_job_resrcs_ptr->node_bitmap));
	last_bit  = MAX(bit_fls(from_job_resrcs_ptr->node_bitmap),
			bit_fls(to_job_resrcs_ptr->node_bitmap));
	from_node_offset = to_node_offset = new_node_offset = -1;
	for (i = first_bit; i <= last_bit; i++) {
		from_node_used = to_node_used = false;
		if (bit_test(from_job_resrcs_ptr->node_bitmap, i)) {
			from_node_used = true;
			from_node_offset++;
		}
		if (bit_test(to_job_resrcs_ptr->node_bitmap, i)) {
			to_node_used = true;
			to_node_offset++;
		}
		if (!from_node_used && !to_node_used)
			continue;
		new_node_offset++;
		node_ptr = node_record_table_ptr + i;
		memcpy(&to_job_ptr->node_addr[new_node_offset],
                       &node_ptr->slurm_addr, sizeof(slurm_addr_t));
		if (from_node_used) {
			/* Merge alloc info from both "from" and "to" jobs,
			 * leave "from" job with no allocated CPUs or memory */
			new_job_resrcs_ptr->cpus[new_node_offset] +=
				from_job_resrcs_ptr->cpus[from_node_offset];
			from_job_resrcs_ptr->cpus[from_node_offset] = 0;
			/* new_job_resrcs_ptr->cpus_used[new_node_offset] +=
				from_job_resrcs_ptr->
				cpus_used[from_node_offset]; */
			new_job_resrcs_ptr->memory_allocated[new_node_offset]+=
				from_job_resrcs_ptr->
				memory_allocated[from_node_offset];
			from_job_resrcs_ptr->
				memory_allocated[from_node_offset] = 0;
			/* new_job_resrcs_ptr->memory_used[new_node_offset] +=
				from_job_resrcs_ptr->
				memory_used[from_node_offset]; */
			if (to_node_used &&
			    (to_job_ptr->details->shared == 0)) {
				if (cr_ptr->nodes[i].exclusive_cnt)
					cr_ptr->nodes[i].exclusive_cnt--;
				else {
					error("select/linear: exclusive_cnt "
					      "underflow for node %s",
					      node_ptr->name);
				}
			}
			job_resources_bits_copy(new_job_resrcs_ptr,
						new_node_offset,
						from_job_resrcs_ptr,
						from_node_offset);
		}
		if (to_node_used) {
			/* Merge alloc info from both "from" and "to" jobs */
			new_job_resrcs_ptr->cpus[new_node_offset] +=
				to_job_resrcs_ptr->cpus[to_node_offset];
			new_job_resrcs_ptr->cpus_used[new_node_offset] +=
				to_job_resrcs_ptr->cpus_used[to_node_offset];
			if (!from_node_used ||
			    (from_job_ptr->details->pn_min_memory &
			     MEM_PER_CPU)) {
				/* node allocated by one job or allocating 
				 * memory by CPU, add mem allocations */
				new_job_resrcs_ptr->
				memory_allocated[new_node_offset] +=
					to_job_resrcs_ptr->
					memory_allocated[to_node_offset];
			} else if (from_node_used) {
				/* mem allocated by node and both jobs have
				 * allocations on the same node */
				if (cr_ptr->nodes[i].alloc_memory >=
				    to_job_resrcs_ptr->
				    memory_allocated[to_node_offset]) {
					cr_ptr->nodes[i].alloc_memory -=
						to_job_resrcs_ptr->
						memory_allocated[to_node_offset];
				} else {
					cr_ptr->nodes[i].alloc_memory = 0;
					error("select/linear: memory underflow"
					      " for node %s",
					      node_ptr->name);
				}
			}
			new_job_resrcs_ptr->memory_used[new_node_offset] +=
				to_job_resrcs_ptr->memory_used[to_node_offset];
			job_resources_bits_copy(new_job_resrcs_ptr,
						new_node_offset,
						to_job_resrcs_ptr,
						to_node_offset);
		}
	}
	build_job_resources_cpu_array(new_job_resrcs_ptr);

	/* Now swap data: "new" -> "to" and clear "from" */
	free_job_resources(&to_job_ptr->job_resrcs);
	to_job_ptr->job_resrcs = new_job_resrcs_ptr;

	to_job_ptr->total_cpus   += from_job_ptr->total_cpus;
	to_job_ptr->cpu_cnt      += from_job_ptr->cpu_cnt;
	if (to_job_ptr->details) {
		to_job_ptr->details->min_cpus = to_job_ptr->total_cpus;
		to_job_ptr->details->max_cpus = to_job_ptr->total_cpus;
	}
	from_job_ptr->total_cpus   = 0;
	from_job_resrcs_ptr->ncpus = 0;
	if (from_job_ptr->details) {
		from_job_ptr->details->min_cpus = 0;
		from_job_ptr->details->max_cpus = 0;
	}

	from_job_ptr->total_nodes   = 0;
	from_job_resrcs_ptr->nhosts = 0;
	from_job_ptr->node_cnt      = 0;
	if (from_job_ptr->details)
		from_job_ptr->details->min_nodes = 0;
	to_job_ptr->total_nodes     = new_job_resrcs_ptr->nhosts;
	to_job_ptr->node_cnt        = new_job_resrcs_ptr->nhosts;

	bit_or(to_job_ptr->node_bitmap, from_job_ptr->node_bitmap);
	bit_nclear(from_job_ptr->node_bitmap, 0, (node_record_count - 1));
	bit_nclear(from_job_resrcs_ptr->node_bitmap, 0,
		  (node_record_count - 1));

	xfree(to_job_ptr->nodes);
	to_job_ptr->nodes = xstrdup(new_job_resrcs_ptr->nodes);
	xfree(from_job_ptr->nodes);
	from_job_ptr->nodes = xstrdup("");
	xfree(from_job_resrcs_ptr->nodes);
	from_job_resrcs_ptr->nodes = xstrdup("");

	return rc;
}

/*
 * deallocate resources that were assigned to this job on one node
 */
static int _rm_job_from_one_node(struct job_record *job_ptr,
				 struct node_record *node_ptr, char *pre_err)
{
	int i, node_inx, node_offset, rc = SLURM_SUCCESS;
	struct part_cr_record *part_cr_ptr;
	job_resources_t *job_resrcs_ptr;
	uint32_t job_memory, job_memory_cpu = 0, job_memory_node = 0;
	bool exclusive, is_job_running;
	int first_bit, last_bit;
	uint16_t cpu_cnt;
	List gres_list;

	if (cr_ptr == NULL) {
		error("%s: cr_ptr not initialized", pre_err);
		return SLURM_ERROR;
	}

	if (_test_tot_job(cr_ptr, job_ptr->job_id) == 0) {
		info("select/linear: job %u has no resources allocated",
		     job_ptr->job_id);
		return SLURM_ERROR;
	}

	if (job_ptr->details &&
	    job_ptr->details->pn_min_memory && (cr_type == CR_MEMORY)) {
		if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
			job_memory_cpu = job_ptr->details->pn_min_memory &
				(~MEM_PER_CPU);
		} else
			job_memory_node = job_ptr->details->pn_min_memory;
	}
	if ((job_ptr->job_resrcs == NULL) ||
	    (job_ptr->job_resrcs->cpus == NULL)) {
		error("job %u lacks a job_resources struct", job_ptr->job_id);
		return SLURM_ERROR;
	}
	job_resrcs_ptr = job_ptr->job_resrcs;
	node_inx = node_ptr - node_record_table_ptr;
	if (!bit_test(job_resrcs_ptr->node_bitmap, node_inx)) {
		error("job %u allocated nodes (%s) which have been removed "
		      "from slurm.conf",
		      job_ptr->job_id, node_ptr->name);
		return SLURM_ERROR;
	}
	first_bit = bit_ffs(job_resrcs_ptr->node_bitmap);
	last_bit  = node_inx;
	node_offset = -1;
	for (i = first_bit; i <= node_inx; i++) {
		if (!bit_test(job_resrcs_ptr->node_bitmap, i))
			continue;
		node_offset++;
	}
	if (job_resrcs_ptr->cpus[node_offset] == 0) {
		error("duplicate relinquish of node %s by job %u",
		      node_ptr->name, job_ptr->job_id);
		return SLURM_ERROR;
	}
	job_resrcs_ptr->cpus[node_offset] = 0;
	build_job_resources_cpu_array(job_resrcs_ptr);

	is_job_running = _test_run_job(cr_ptr, job_ptr->job_id);
	if (select_fast_schedule)
		cpu_cnt = node_ptr->config_ptr->cpus;
	else
		cpu_cnt = node_ptr->cpus;
	if (job_memory_cpu)
		job_memory = job_memory_cpu * cpu_cnt;
	else
		job_memory = job_memory_node;
	if (cr_ptr->nodes[node_inx].alloc_memory >= job_memory)
		cr_ptr->nodes[node_inx].alloc_memory -= job_memory;
	else {
		cr_ptr->nodes[node_inx].alloc_memory = 0;
		error("%s: memory underflow for node %s",
		      pre_err, node_ptr->name);
	}

	if (cr_ptr->nodes[i].gres_list)
		gres_list = cr_ptr->nodes[i].gres_list;
	else
		gres_list = node_ptr->gres_list;
	gres_plugin_job_dealloc(job_ptr->gres_list, gres_list, node_offset,
				job_ptr->job_id, node_ptr->name);
	gres_plugin_node_state_log(gres_list, node_ptr->name);

	exclusive = (job_ptr->details->shared == 0);
	if (exclusive) {
		if (cr_ptr->nodes[node_inx].exclusive_cnt)
			cr_ptr->nodes[node_inx].exclusive_cnt--;
		else {
			error("%s: exclusive_cnt underflow for node %s",
			      pre_err, node_ptr->name);
		}
	}
	part_cr_ptr = cr_ptr->nodes[node_inx].parts;
	while (part_cr_ptr) {
		if (part_cr_ptr->part_ptr != job_ptr->part_ptr) {
			part_cr_ptr = part_cr_ptr->next;
			continue;
		}
		if (!is_job_running)
			/* cancelled job already suspended */;
		else if (part_cr_ptr->run_job_cnt > 0)
			part_cr_ptr->run_job_cnt--;
		else {
			error("%s: run_job_cnt underflow for node %s",
			      pre_err, node_ptr->name);
		}
		if (part_cr_ptr->tot_job_cnt > 0)
			part_cr_ptr->tot_job_cnt--;
		else {
			error("%s: tot_job_cnt underflow for node %s",
			      pre_err, node_ptr->name);
		}
		if ((part_cr_ptr->tot_job_cnt == 0) &&
		    (part_cr_ptr->run_job_cnt)) {
			part_cr_ptr->run_job_cnt = 0;
			error("%s: run_job_cnt out of sync for node %s",
			      pre_err, node_ptr->name);
		}
		break;
	}
	if (part_cr_ptr == NULL) {
		if (job_ptr->part_ptr) {
			error("%s: Could not find partition %s for node %s",
			      pre_err, job_ptr->part_ptr->name, node_ptr->name);
		} else {
			error("%s: no partition ptr given for job %u and node %s",
			      pre_err, job_ptr->job_id, node_ptr->name);
		}
		rc = SLURM_ERROR;
	}

	return rc;
}

/*
 * allocate resources to the given job
 *
 * if alloc_all = 0: the job has been suspended, so just re-allocate CPUs
 * if alloc_all = 1: allocate all resources (CPUs and memory)
 */
static int _add_job_to_nodes(struct cr_record *cr_ptr,
			     struct job_record *job_ptr, char *pre_err,
			     int alloc_all)
{
	int i, i_first, i_last, node_cnt, node_offset, rc = SLURM_SUCCESS;
	bool exclusive;
	struct part_cr_record *part_cr_ptr;
	job_resources_t *job_resrcs_ptr;
	uint32_t job_memory_cpu = 0, job_memory_node = 0;
	uint16_t cpu_cnt;
	struct node_record *node_ptr;
	List gres_list;

	if (cr_ptr == NULL) {
		error("%s: cr_ptr not initialized", pre_err);
		return SLURM_ERROR;
	}

	if (alloc_all && job_ptr->details &&
	    job_ptr->details->pn_min_memory && (cr_type == CR_MEMORY)) {
		if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
			job_memory_cpu = job_ptr->details->pn_min_memory &
				(~MEM_PER_CPU);
		} else
			job_memory_node = job_ptr->details->pn_min_memory;
	}
	if ((job_resrcs_ptr = job_ptr->job_resrcs) == NULL) {
		error("job %u lacks a job_resources struct", job_ptr->job_id);
		return SLURM_ERROR;
	}

	exclusive = (job_ptr->details->shared == 0);
	if (alloc_all)
		_add_run_job(cr_ptr, job_ptr->job_id);
	_add_tot_job(cr_ptr, job_ptr->job_id);

	i_first = bit_ffs(job_resrcs_ptr->node_bitmap);
	i_last  = bit_fls(job_resrcs_ptr->node_bitmap);
	node_cnt = bit_set_count(job_resrcs_ptr->node_bitmap);
	if (i_first == -1)	/* job has no nodes */
		i_last = -2;
	node_offset = -1;
	for (i = i_first; i <= i_last; i++) {
		if (!bit_test(job_resrcs_ptr->node_bitmap, i))
			continue;
		node_offset++;
		if (!bit_test(job_ptr->node_bitmap, i))
			continue;

		node_ptr = node_record_table_ptr + i;
		if (select_fast_schedule)
			cpu_cnt = node_ptr->config_ptr->cpus;
		else
			cpu_cnt = node_ptr->cpus;

		if (job_memory_cpu) {
			cr_ptr->nodes[i].alloc_memory += job_memory_cpu *
				cpu_cnt;
		} else
			cr_ptr->nodes[i].alloc_memory += job_memory_node;

		if (alloc_all) {
			if (cr_ptr->nodes[i].gres_list)
				gres_list = cr_ptr->nodes[i].gres_list;
			else
				gres_list = node_ptr->gres_list;
			gres_plugin_job_alloc(job_ptr->gres_list, gres_list,
					      node_cnt, node_offset, cpu_cnt,
					      job_ptr->job_id, node_ptr->name);
			gres_plugin_node_state_log(gres_list, node_ptr->name);
		}

		if (exclusive)
			cr_ptr->nodes[i].exclusive_cnt++;

		part_cr_ptr = cr_ptr->nodes[i].parts;
		while (part_cr_ptr) {
			if (part_cr_ptr->part_ptr != job_ptr->part_ptr) {
				part_cr_ptr = part_cr_ptr->next;
				continue;
			}
			if (alloc_all)
				part_cr_ptr->run_job_cnt++;
			part_cr_ptr->tot_job_cnt++;
			break;
		}
		if (part_cr_ptr == NULL) {
			info("%s: job %u could not find partition %s for "
			     "node %s",
			     pre_err, job_ptr->job_id, job_ptr->partition,
			     node_ptr->name);
			job_ptr->part_nodes_missing = true;
			rc = SLURM_ERROR;
		}
	}

	return rc;
}

static void _free_cr(struct cr_record *cr_ptr)
{
	int i;
	struct part_cr_record *part_cr_ptr1, *part_cr_ptr2;

	if (cr_ptr == NULL)
		return;

	for (i = 0; i < select_node_cnt; i++) {
		part_cr_ptr1 = cr_ptr->nodes[i].parts;
		while (part_cr_ptr1) {
			part_cr_ptr2 = part_cr_ptr1->next;
			xfree(part_cr_ptr1);
			part_cr_ptr1 = part_cr_ptr2;
		}
		if (cr_ptr->nodes[i].gres_list)
			list_destroy(cr_ptr->nodes[i].gres_list);
	}
	xfree(cr_ptr->nodes);
	xfree(cr_ptr->run_job_ids);
	xfree(cr_ptr->tot_job_ids);
	xfree(cr_ptr);
}

static void _dump_node_cr(struct cr_record *cr_ptr)
{
#if SELECT_DEBUG
	int i;
	struct part_cr_record *part_cr_ptr;
	struct node_record *node_ptr;
	List gres_list;

	if ((cr_ptr == NULL) || (cr_ptr->nodes == NULL))
		return;

	for (i = 0; i < cr_ptr->run_job_len; i++) {
		if (cr_ptr->run_job_ids[i])
			info("Running job:%u", cr_ptr->run_job_ids[i]);
	}
	for (i = 0; i < cr_ptr->tot_job_len; i++) {
		if (cr_ptr->tot_job_ids[i])
			info("Alloc job:%u", cr_ptr->tot_job_ids[i]);
	}

	for (i = 0; i < select_node_cnt; i++) {
		node_ptr = node_record_table_ptr + i;
		info("Node:%s exclusive_cnt:%u alloc_mem:%u",
		     node_ptr->name, cr_ptr->nodes[i].exclusive_cnt,
		     cr_ptr->nodes[i].alloc_memory);

		part_cr_ptr = cr_ptr->nodes[i].parts;
		while (part_cr_ptr) {
			info("  Part:%s run:%u tot:%u",
			     part_cr_ptr->part_ptr->name,
			     part_cr_ptr->run_job_cnt,
			     part_cr_ptr->tot_job_cnt);
			part_cr_ptr = part_cr_ptr->next;
		}

		if (cr_ptr->nodes[i].gres_list)
			gres_list = cr_ptr->nodes[i].gres_list;
		else
			gres_list = node_ptr->gres_list;
		if (gres_list)
			gres_plugin_node_state_log(gres_list, node_ptr->name);
	}
#endif
}

static struct cr_record *_dup_cr(struct cr_record *cr_ptr)
{
	int i;
	struct cr_record *new_cr_ptr;
	struct part_cr_record *part_cr_ptr, *new_part_cr_ptr;
	struct node_record *node_ptr;
	List gres_list;

	if (cr_ptr == NULL)
		return NULL;

	new_cr_ptr = xmalloc(sizeof(struct cr_record));
	new_cr_ptr->run_job_len = cr_ptr->run_job_len;
	i = sizeof(uint32_t) * cr_ptr->run_job_len;
	new_cr_ptr->run_job_ids = xmalloc(i);
	memcpy(new_cr_ptr->run_job_ids, cr_ptr->run_job_ids, i);
	new_cr_ptr->tot_job_len = cr_ptr->tot_job_len;
	i = sizeof(uint32_t) * cr_ptr->tot_job_len;
	new_cr_ptr->tot_job_ids = xmalloc(i);
	memcpy(new_cr_ptr->tot_job_ids, cr_ptr->tot_job_ids, i);

	new_cr_ptr->nodes = xmalloc(select_node_cnt *
				    sizeof(struct node_cr_record));
	for (i = 0; i < select_node_cnt; i++) {
		node_ptr = node_record_table_ptr + i;
		new_cr_ptr->nodes[i].alloc_memory = cr_ptr->nodes[i].
			alloc_memory;
		new_cr_ptr->nodes[i].exclusive_cnt = cr_ptr->nodes[i].
			exclusive_cnt;

		part_cr_ptr = cr_ptr->nodes[i].parts;
		while (part_cr_ptr) {
			new_part_cr_ptr =
				xmalloc(sizeof(struct part_cr_record));
			new_part_cr_ptr->part_ptr    = part_cr_ptr->part_ptr;
			new_part_cr_ptr->run_job_cnt = part_cr_ptr->run_job_cnt;
			new_part_cr_ptr->tot_job_cnt = part_cr_ptr->tot_job_cnt;
			new_part_cr_ptr->next 	     = new_cr_ptr->nodes[i].
				parts;
			new_cr_ptr->nodes[i].parts   = new_part_cr_ptr;
			part_cr_ptr = part_cr_ptr->next;
		}

		if (cr_ptr->nodes[i].gres_list)
			gres_list = cr_ptr->nodes[i].gres_list;
		else
			gres_list = node_ptr->gres_list;
		new_cr_ptr->nodes[i].gres_list =
			gres_plugin_node_state_dup(gres_list);
	}
	return new_cr_ptr;
}

static void _init_node_cr(void)
{
	struct part_record *part_ptr;
	struct part_cr_record *part_cr_ptr;
	job_resources_t *job_resrcs_ptr;
	struct node_record *node_ptr;
	ListIterator part_iterator;
	struct job_record *job_ptr;
	ListIterator job_iterator;
	uint32_t job_memory_cpu, job_memory_node;
	int exclusive, i, i_first, i_last, node_offset;

	if (cr_ptr)
		return;

	cr_ptr = xmalloc(sizeof(struct cr_record));
	cr_ptr->nodes = xmalloc(select_node_cnt
				* sizeof(struct node_cr_record));

	/* build partition records */
	part_iterator = list_iterator_create(part_list);
	while ((part_ptr = (struct part_record *) list_next(part_iterator))) {
		for (i = 0; i < select_node_cnt; i++) {
			if (part_ptr->node_bitmap == NULL)
				break;
			if (!bit_test(part_ptr->node_bitmap, i))
				continue;
			part_cr_ptr = xmalloc(sizeof(struct part_cr_record));
			part_cr_ptr->next = cr_ptr->nodes[i].parts;
			part_cr_ptr->part_ptr = part_ptr;
			cr_ptr->nodes[i].parts = part_cr_ptr;
		}

	}
	list_iterator_destroy(part_iterator);

	/* Clear existing node Gres allocations */
	for (i = 0, node_ptr = node_record_table_ptr; i < node_record_count;
	     i++, node_ptr++) {
		gres_plugin_node_state_dealloc_all(node_ptr->gres_list);
	}

	/* record running and suspended jobs in node_cr_records */
	job_iterator = list_iterator_create(job_list);
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (!IS_JOB_RUNNING(job_ptr) && !IS_JOB_SUSPENDED(job_ptr))
			continue;
		if ((job_resrcs_ptr = job_ptr->job_resrcs) == NULL) {
			error("job %u lacks a job_resources struct",
			      job_ptr->job_id);
			continue;
		}
		if (IS_JOB_RUNNING(job_ptr) ||
		    (IS_JOB_SUSPENDED(job_ptr) && (job_ptr->priority != 0)))
			_add_run_job(cr_ptr, job_ptr->job_id);
		_add_tot_job(cr_ptr, job_ptr->job_id);

		job_memory_cpu  = 0;
		job_memory_node = 0;
		if (job_ptr->details && job_ptr->details->pn_min_memory &&
		    (cr_type == CR_MEMORY)) {
			if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
				job_memory_cpu = job_ptr->details->
					pn_min_memory &
					(~MEM_PER_CPU);
			} else {
				job_memory_node = job_ptr->details->
					pn_min_memory;
			}
		}

		/* Use job_resrcs_ptr->node_bitmap rather than
		 * job_ptr->node_bitmap which can have DOWN nodes
		 * cleared from the bitmap */
		if (job_resrcs_ptr->node_bitmap == NULL)
			continue;

		exclusive = (job_ptr->details->shared == 0);
		node_offset = -1;
		i_first = bit_ffs(job_resrcs_ptr->node_bitmap);
		i_last  = bit_fls(job_resrcs_ptr->node_bitmap);
		if (i_first == -1)
			i_last = -2;
		for (i = i_first; i <= i_last; i++) {
			if (!bit_test(job_resrcs_ptr->node_bitmap, i))
				continue;
			node_offset++;
			node_ptr = node_record_table_ptr + i;
			if (exclusive)
				cr_ptr->nodes[i].exclusive_cnt++;
			if (job_memory_cpu == 0) {
				cr_ptr->nodes[i].alloc_memory +=
					job_memory_node;
			} else if (select_fast_schedule) {
				cr_ptr->nodes[i].alloc_memory +=
					job_memory_cpu *
					node_record_table_ptr[i].
					config_ptr->cpus;
			} else {
				cr_ptr->nodes[i].alloc_memory +=
					job_memory_cpu *
					node_record_table_ptr[i].cpus;
			}

			if (bit_test(job_ptr->node_bitmap, i)) {
				gres_plugin_job_alloc(job_ptr->gres_list,
						      node_ptr->gres_list,
						      job_resrcs_ptr->nhosts,
						      node_offset,
						      job_resrcs_ptr->
						      cpus[node_offset],
						      job_ptr->job_id,
						      node_ptr->name);
			}

			part_cr_ptr = cr_ptr->nodes[i].parts;
			while (part_cr_ptr) {
				if (part_cr_ptr->part_ptr !=
				    job_ptr->part_ptr) {
					part_cr_ptr = part_cr_ptr->next;
					continue;
				}
				if (IS_JOB_RUNNING(job_ptr) ||
				    (IS_JOB_SUSPENDED(job_ptr) &&
				     (job_ptr->priority != 0))) {
					/* Running or being gang scheduled */
					part_cr_ptr->run_job_cnt++;
				}
				part_cr_ptr->tot_job_cnt++;
				break;
			}
			if (part_cr_ptr == NULL) {
				info("_init_node_cr: job %u could not find "
				     "partition %s for node %s",
				     job_ptr->job_id, job_ptr->partition,
				     node_ptr->name);
				job_ptr->part_nodes_missing = true;
			}
		}
	}
	list_iterator_destroy(job_iterator);
	_dump_node_cr(cr_ptr);
}

static int _find_job (void *x, void *key)
{
	struct job_record *job_ptr = (struct job_record *) x;
	if (job_ptr == (struct job_record *) key)
		return 1;
	return 0;
}

static bool _is_preemptable(struct job_record *job_ptr,
			    List preemptee_candidates)
{
	if (!preemptee_candidates)
		return false;
	if (list_find_first(preemptee_candidates, _find_job, job_ptr))
		return true;
	return false;
}

/* Determine if a job can ever run */
static int _test_only(struct job_record *job_ptr, bitstr_t *bitmap,
		      uint32_t min_nodes, uint32_t max_nodes,
		      uint32_t req_nodes, int max_share)
{
	bitstr_t *orig_map;
	int i, rc = SLURM_ERROR;
	uint32_t save_mem;

	orig_map = bit_copy(bitmap);
	if (!orig_map)
		fatal("bit_copy: malloc failure");

	/* Try to run with currently available nodes */
	i = _job_count_bitmap(cr_ptr, job_ptr, orig_map, bitmap,
			      NO_SHARE_LIMIT, NO_SHARE_LIMIT,
			      SELECT_MODE_TEST_ONLY);
	if (i >= min_nodes) {
		save_mem = job_ptr->details->pn_min_memory;
		job_ptr->details->pn_min_memory = 0;
		rc = _job_test(job_ptr, bitmap, min_nodes,
			       max_nodes, req_nodes);
		job_ptr->details->pn_min_memory = save_mem;
	}
	FREE_NULL_BITMAP(orig_map);

	return rc;
}

/* Allocate resources for a job now, if possible */
static int _run_now(struct job_record *job_ptr, bitstr_t *bitmap,
		    uint32_t min_nodes, uint32_t max_nodes,
		    int max_share, uint32_t req_nodes,
		    List preemptee_candidates,
		    List *preemptee_job_list)
{

	bitstr_t *orig_map;
	int max_run_job, j, sus_jobs, rc = EINVAL, prev_cnt = -1;
	struct job_record *tmp_job_ptr;
	ListIterator job_iterator, preemptee_iterator;
	struct cr_record *exp_cr;

	orig_map = bit_copy(bitmap);
	if (!orig_map)
		fatal("bit_copy: malloc failure");

	for (max_run_job=0; ((max_run_job<max_share) && (rc != SLURM_SUCCESS));
	     max_run_job++) {
		bool last_iteration = (max_run_job == (max_share - 1));
		for (sus_jobs=0; ((sus_jobs<5) && (rc != SLURM_SUCCESS));
		     sus_jobs+=4) {
			if (last_iteration)
				sus_jobs = NO_SHARE_LIMIT;
			j = _job_count_bitmap(cr_ptr, job_ptr,
					      orig_map, bitmap,
					      max_run_job,
					      max_run_job + sus_jobs,
					      SELECT_MODE_RUN_NOW);
#if SELECT_DEBUG
			{
				char *node_list = bitmap2node_name(bitmap);
				info("_run_job %u iter:%d cnt:%d nodes:%s",
				     job_ptr->job_id, max_run_job, j,
				     node_list);
				xfree(node_list);
			}
#endif
			if ((j == prev_cnt) || (j < min_nodes))
				continue;
			prev_cnt = j;
			if (max_run_job > 0) {
				/* We need to share. Try to find
				 * suitable job to share nodes with */
				rc = _find_job_mate(job_ptr, bitmap,
						    min_nodes,
						    max_nodes, req_nodes);
				if (rc == SLURM_SUCCESS)
					break;
			}
			rc = _job_test(job_ptr, bitmap, min_nodes, max_nodes,
				       req_nodes);
		}
	}

	if ((rc != SLURM_SUCCESS) && preemptee_candidates &&
	    (exp_cr = _dup_cr(cr_ptr))) {
		/* Remove all preemptable jobs from simulated environment */
		job_iterator = list_iterator_create(job_list);
		while ((tmp_job_ptr = (struct job_record *)
			list_next(job_iterator))) {
			if (!IS_JOB_RUNNING(tmp_job_ptr) &&
			    !IS_JOB_SUSPENDED(tmp_job_ptr))
				continue;
			if (_is_preemptable(tmp_job_ptr,
					    preemptee_candidates)) {
				bool remove_all = false;
				uint16_t mode;
				mode = slurm_job_preempt_mode(tmp_job_ptr);
				if ((mode == PREEMPT_MODE_REQUEUE)    ||
				    (mode == PREEMPT_MODE_CHECKPOINT) ||
				    (mode == PREEMPT_MODE_CANCEL))
					remove_all = true;
				/* Remove preemptable job now */
				_rm_job_from_nodes(exp_cr, tmp_job_ptr,
						   "_run_now",
						   remove_all);
				j = _job_count_bitmap(exp_cr, job_ptr,
						      orig_map, bitmap,
						      (max_share - 1),
						      NO_SHARE_LIMIT,
						      SELECT_MODE_RUN_NOW);
				if (j < min_nodes)
					continue;
				rc = _job_test(job_ptr, bitmap, min_nodes,
					       max_nodes, req_nodes);
				if (rc == SLURM_SUCCESS)
					break;
			}
		}
		list_iterator_destroy(job_iterator);

		if ((rc == SLURM_SUCCESS) && preemptee_job_list &&
		    preemptee_candidates) {
			/* Build list of preemptee jobs whose resources are
			 * actually used */
			if (*preemptee_job_list == NULL) {
				*preemptee_job_list = list_create(NULL);
				if (*preemptee_job_list == NULL)
					fatal("list_create malloc failure");
			}
			preemptee_iterator = list_iterator_create(
				preemptee_candidates);
			while ((tmp_job_ptr = (struct job_record *)
				list_next(preemptee_iterator))) {
				if (bit_overlap(bitmap,
						tmp_job_ptr->node_bitmap) == 0)
					continue;
				list_append(*preemptee_job_list,
					    tmp_job_ptr);
			}
			list_iterator_destroy(preemptee_iterator);
		}
		_free_cr(exp_cr);
	}
	if (rc == SLURM_SUCCESS)
		_build_select_struct(job_ptr, bitmap);
	FREE_NULL_BITMAP(orig_map);

	return rc;
}

/* Determine where and when the job at job_ptr can begin execution by updating
 * a scratch cr_record structure to reflect each job terminating at the
 * end of its time limit and use this to show where and when the job at job_ptr
 * will begin execution. Used by SLURM's sched/backfill plugin and Moab. */
static int _will_run_test(struct job_record *job_ptr, bitstr_t *bitmap,
			  uint32_t min_nodes, uint32_t max_nodes,
			  int max_share, uint32_t req_nodes,
			  List preemptee_candidates,
			  List *preemptee_job_list)
{
	struct cr_record *exp_cr;
	struct job_record *tmp_job_ptr;
	List cr_job_list;
	ListIterator job_iterator, preemptee_iterator;
	bitstr_t *orig_map;
	int i, max_run_jobs, rc = SLURM_ERROR;
	time_t now = time(NULL);

	max_run_jobs = MAX((max_share - 1), 1);	/* exclude this job */
	orig_map = bit_copy(bitmap);
	if (!orig_map)
		fatal("bit_copy: malloc failure");

	/* Try to run with currently available nodes */
	i = _job_count_bitmap(cr_ptr, job_ptr, orig_map, bitmap,
			      max_run_jobs, NO_SHARE_LIMIT,
			      SELECT_MODE_WILL_RUN);
	if (i >= min_nodes) {
		rc = _job_test(job_ptr, bitmap, min_nodes, max_nodes,
			       req_nodes);
		if (rc == SLURM_SUCCESS) {
			FREE_NULL_BITMAP(orig_map);
			job_ptr->start_time = time(NULL);
			return SLURM_SUCCESS;
		}
	}

	/* Job is still pending. Simulate termination of jobs one at a time
	 * to determine when and where the job can start. */
	exp_cr = _dup_cr(cr_ptr);
	if (exp_cr == NULL) {
		FREE_NULL_BITMAP(orig_map);
		return SLURM_ERROR;
	}

	/* Build list of running and suspended jobs */
	cr_job_list = list_create(NULL);
	if (!cr_job_list)
		fatal("list_create: memory allocation failure");
	job_iterator = list_iterator_create(job_list);
	while ((tmp_job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (!IS_JOB_RUNNING(tmp_job_ptr) &&
		    !IS_JOB_SUSPENDED(tmp_job_ptr))
			continue;
		if (tmp_job_ptr->end_time == 0) {
			error("Job %u has zero end_time", tmp_job_ptr->job_id);
			continue;
		}
		if (_is_preemptable(tmp_job_ptr, preemptee_candidates)) {
			uint16_t mode = slurm_job_preempt_mode(tmp_job_ptr);
			bool remove_all = false;
			if ((mode == PREEMPT_MODE_REQUEUE)    ||
			    (mode == PREEMPT_MODE_CHECKPOINT) ||
			    (mode == PREEMPT_MODE_CANCEL))
				remove_all = true;
			/* Remove preemptable job now */
			_rm_job_from_nodes(exp_cr, tmp_job_ptr,
					   "_will_run_test", remove_all);
		} else
			list_append(cr_job_list, tmp_job_ptr);

	}
	list_iterator_destroy(job_iterator);

	/* Test with all preemptable jobs gone */
	if (preemptee_candidates) {
		i = _job_count_bitmap(exp_cr, job_ptr, orig_map, bitmap,
				      max_run_jobs, NO_SHARE_LIMIT,
				      SELECT_MODE_RUN_NOW);
		if (i >= min_nodes) {
			rc = _job_test(job_ptr, bitmap, min_nodes, max_nodes,
				       req_nodes);
			if (rc == SLURM_SUCCESS)
				job_ptr->start_time = now + 1;
		}
	}

	/* Remove the running jobs one at a time from exp_node_cr and try
	 * scheduling the pending job after each one */
	if (rc != SLURM_SUCCESS) {
		list_sort(cr_job_list, _cr_job_list_sort);
		job_iterator = list_iterator_create(cr_job_list);
		while ((tmp_job_ptr = (struct job_record *)
			list_next(job_iterator))) {
			_rm_job_from_nodes(exp_cr, tmp_job_ptr,
					   "_will_run_test", true);
			i = _job_count_bitmap(exp_cr, job_ptr, orig_map,
					      bitmap, max_run_jobs,
					      NO_SHARE_LIMIT,
					      SELECT_MODE_RUN_NOW);
			if (i < min_nodes)
				continue;
			rc = _job_test(job_ptr, bitmap, min_nodes, max_nodes,
				       req_nodes);
			if (rc != SLURM_SUCCESS)
				continue;
			if (tmp_job_ptr->end_time <= now)
				job_ptr->start_time = now + 1;
			else
				job_ptr->start_time = tmp_job_ptr->end_time;
			break;
		}
		list_iterator_destroy(job_iterator);
	}

	if ((rc == SLURM_SUCCESS) && preemptee_job_list &&
	    preemptee_candidates) {
		/* Build list of preemptee jobs whose resources are
		 * actually used. List returned even if not killed
		 * in selected plugin, but by Moab or something else. */
		if (*preemptee_job_list == NULL) {
			*preemptee_job_list = list_create(NULL);
			if (*preemptee_job_list == NULL)
				fatal("list_create malloc failure");
		}
		preemptee_iterator =list_iterator_create(preemptee_candidates);
		while ((tmp_job_ptr = (struct job_record *)
			list_next(preemptee_iterator))) {
			if (bit_overlap(bitmap, tmp_job_ptr->node_bitmap) == 0)
				continue;

			list_append(*preemptee_job_list, tmp_job_ptr);
		}
		list_iterator_destroy(preemptee_iterator);
	}

	list_destroy(cr_job_list);
	_free_cr(exp_cr);
	FREE_NULL_BITMAP(orig_map);
	return rc;
}

static int  _cr_job_list_sort(void *x, void *y)
{
	struct job_record *job1_ptr = (struct job_record *) x;
	struct job_record *job2_ptr = (struct job_record *) y;
	return (int) difftime(job1_ptr->end_time, job2_ptr->end_time);
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init ( void )
{
	int rc = SLURM_SUCCESS;
#ifdef HAVE_XCPU
	rc = _init_status_pthread();
#endif
	cr_type = slurmctld_conf.select_type_param;
	return rc;
}

extern int fini ( void )
{
	int rc = SLURM_SUCCESS;
#ifdef HAVE_XCPU
	rc = _fini_status_pthread();
#endif
	slurm_mutex_lock(&cr_mutex);
	_free_cr(cr_ptr);
	cr_ptr = NULL;
	slurm_mutex_unlock(&cr_mutex);
	return rc;
}

/*
 * The remainder of this file implements the standard SLURM
 * node selection API.
 */

extern int select_p_state_save(char *dir_name)
{
	return SLURM_SUCCESS;
}

extern int select_p_state_restore(char *dir_name)
{
	return SLURM_SUCCESS;
}

extern int select_p_job_init(List job_list)
{
	return SLURM_SUCCESS;
}

extern bool select_p_node_ranking(struct node_record *node_ptr, int node_cnt)
{
	return false;
}

extern int select_p_node_init(struct node_record *node_ptr, int node_cnt)
{
	if (node_ptr == NULL) {
		error("select_p_node_init: node_ptr == NULL");
		return SLURM_ERROR;
	}

	if (node_cnt < 0) {
		error("select_p_node_init: node_cnt < 0");
		return SLURM_ERROR;
	}

	/* NOTE: We free the consumable resources info here, but
	 * can't rebuild it since the partition and node structures
	 * have not yet had node bitmaps reset. */
	slurm_mutex_lock(&cr_mutex);
	_free_cr(cr_ptr);
	cr_ptr = NULL;
	slurm_mutex_unlock(&cr_mutex);

	select_node_ptr = node_ptr;
	select_node_cnt = node_cnt;
	select_fast_schedule = slurm_get_fast_schedule();

	return SLURM_SUCCESS;
}

extern int select_p_block_init(List part_list)
{
	return SLURM_SUCCESS;
}

/*
 * select_p_job_test - Given a specification of scheduling requirements,
 *	identify the nodes which "best" satisfy the request.
 * 	"best" is defined as either single set of consecutive nodes satisfying
 *	the request and leaving the minimum number of unused nodes OR
 *	the fewest number of consecutive node sets
 * IN/OUT job_ptr - pointer to job being considered for initiation,
 *                  set's start_time when job expected to start
 * IN/OUT bitmap - usable nodes are set on input, nodes not required to
 *	satisfy the request are cleared, other left set
 * IN min_nodes - minimum count of nodes
 * IN req_nodes - requested (or desired) count of nodes
 * IN max_nodes - maximum count of nodes
 * IN mode - SELECT_MODE_RUN_NOW: try to schedule job now
 *           SELECT_MODE_TEST_ONLY: test if job can ever run
 *           SELECT_MODE_WILL_RUN: determine when and where job can run
 * IN preemptee_candidates - List of pointers to jobs which can be preempted.
 * IN/OUT preemptee_job_list - Pointer to list of job pointers. These are the
 *		jobs to be preempted to initiate the pending job. Not set
 *		if mode=SELECT_MODE_TEST_ONLY or input pointer is NULL.
 * RET zero on success, EINVAL otherwise
 * globals (passed via select_p_node_init):
 *	node_record_count - count of nodes configured
 *	node_record_table_ptr - pointer to global node table
 * NOTE: the job information that is considered for scheduling includes:
 *	req_node_bitmap: bitmap of specific nodes required by the job
 *	contiguous: allocated nodes must be sequentially located
 *	num_cpus: minimum number of processors required by the job
 * NOTE: bitmap must be a superset of the job's required at the time that
 *	select_p_job_test is called
 */
extern int select_p_job_test(struct job_record *job_ptr, bitstr_t *bitmap,
			     uint32_t min_nodes, uint32_t max_nodes,
			     uint32_t req_nodes, uint16_t mode,
			     List preemptee_candidates,
			     List *preemptee_job_list)
{
	int max_share = 0, rc = EINVAL;

	xassert(bitmap);
	if (job_ptr->details == NULL)
		return EINVAL;

	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL) {
		_init_node_cr();
		if (cr_ptr == NULL) {
			slurm_mutex_unlock(&cr_mutex);
			error("select_p_job_test: cr_ptr not initialized");
			return SLURM_ERROR;
		}
	}

	if (bit_set_count(bitmap) < min_nodes) {
		slurm_mutex_unlock(&cr_mutex);
		return EINVAL;
	}

	if (job_ptr->details->shared)
		max_share = job_ptr->part_ptr->max_share & ~SHARED_FORCE;
	else	/* ((shared == 0) || (shared == (uint16_t) NO_VAL)) */
		max_share = 1;

	if (mode == SELECT_MODE_WILL_RUN) {
		rc = _will_run_test(job_ptr, bitmap, min_nodes, max_nodes,
				    max_share, req_nodes,
				    preemptee_candidates, preemptee_job_list);
	} else if (mode == SELECT_MODE_TEST_ONLY) {
		rc = _test_only(job_ptr, bitmap, min_nodes, max_nodes,
				req_nodes, max_share);
	} else if (mode == SELECT_MODE_RUN_NOW) {
		rc = _run_now(job_ptr, bitmap, min_nodes, max_nodes,
			      max_share, req_nodes,
			      preemptee_candidates, preemptee_job_list);
	} else
		fatal("select_p_job_test: Mode %d is invalid", mode);

	slurm_mutex_unlock(&cr_mutex);

	return rc;
}

extern int select_p_job_begin(struct job_record *job_ptr)
{
	int rc = SLURM_SUCCESS;
#ifdef HAVE_XCPU
	int i;
	char clone_path[128];

	xassert(job_ptr);
	xassert(job_ptr->node_bitmap);

	for (i=0; i<select_node_cnt; i++) {
		if (bit_test(job_ptr->node_bitmap, i) == 0)
			continue;
		snprintf(clone_path, sizeof(clone_path),
			 "%s/%s/xcpu/clone", XCPU_DIR,
			 select_node_ptr[i].name);
		if (chown(clone_path, (uid_t)job_ptr->user_id,
			  (gid_t)job_ptr->group_id)) {
			error("chown %s: %m", clone_path);
			rc = SLURM_ERROR;
		} else {
			debug("chown %s to %u", clone_path,
			      job_ptr->user_id);
		}
	}
#endif
	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL)
		_init_node_cr();
	_add_job_to_nodes(cr_ptr, job_ptr, "select_p_job_begin", 1);
	gres_plugin_job_state_log(job_ptr->gres_list, job_ptr->job_id);
	slurm_mutex_unlock(&cr_mutex);
	return rc;
}

/* Determine if allocated nodes are usable (powered up) */
extern int select_p_job_ready(struct job_record *job_ptr)
{
	int i, i_first, i_last;
	struct node_record *node_ptr;

	if (!IS_JOB_RUNNING(job_ptr) && !IS_JOB_SUSPENDED(job_ptr)) {
		/* Gang scheduling might suspend job immediately */
		return 0;
	}

	if ((job_ptr->node_bitmap == NULL) ||
	    ((i_first = bit_ffs(job_ptr->node_bitmap)) == -1))
		return READY_NODE_STATE;
	i_last  = bit_fls(job_ptr->node_bitmap);

	for (i = i_first; i <= i_last; i++) {
		if (bit_test(job_ptr->node_bitmap, i) == 0)
			continue;
		node_ptr = node_record_table_ptr + i;
		if (IS_NODE_POWER_SAVE(node_ptr) || IS_NODE_POWER_UP(node_ptr))
			return 0;
	}

	return READY_NODE_STATE;
}


extern bool select_p_job_expand_allow(void)
{
	return true;
}

extern int select_p_job_expand(struct job_record *from_job_ptr,
			       struct job_record *to_job_ptr)
{
	int rc;

	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL)
		_init_node_cr();
	rc = _job_expand(from_job_ptr, to_job_ptr);
	slurm_mutex_unlock(&cr_mutex);
	return rc;
}

extern int select_p_job_resized(struct job_record *job_ptr,
				struct node_record *node_ptr)
{
	int rc = SLURM_SUCCESS;
#ifdef HAVE_XCPU
	int i = node_ptr - node_record_table_ptr;
	char clone_path[128];

	if (bit_test(job_ptr->node_bitmap, i) == 0)
		continue;
	snprintf(clone_path, sizeof(clone_path), "%s/%s/xcpu/clone", XCPU_DIR,
		 node_ptr->name);
	if (chown(clone_path, (uid_t)0, (gid_t)0)) {
		error("chown %s: %m", clone_path);
		rc = SLURM_ERROR;
	} else
		debug("chown %s to 0", clone_path);
#endif

	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL)
		_init_node_cr();
	_rm_job_from_one_node(job_ptr, node_ptr, "select_p_job_resized");
	slurm_mutex_unlock(&cr_mutex);
	return rc;
}

extern int select_p_job_signal(struct job_record *job_ptr, int signal)
{
	return SLURM_SUCCESS;
}

extern int select_p_job_fini(struct job_record *job_ptr)
{
	int rc = SLURM_SUCCESS;
#ifdef HAVE_XCPU
	int i;
	char clone_path[128];

	for (i=0; i<select_node_cnt; i++) {
		if (bit_test(job_ptr->node_bitmap, i) == 0)
			continue;
		snprintf(clone_path, sizeof(clone_path), "%s/%s/xcpu/clone",
			 XCPU_DIR, select_node_ptr[i].name);
		if (chown(clone_path, (uid_t)0, (gid_t)0)) {
			error("chown %s: %m", clone_path);
			rc = SLURM_ERROR;
		} else {
			debug("chown %s to 0", clone_path);
		}
	}
#endif
	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL)
		_init_node_cr();
	_rm_job_from_nodes(cr_ptr, job_ptr, "select_p_job_fini", true);
	slurm_mutex_unlock(&cr_mutex);
	return rc;
}

extern int select_p_job_suspend(struct job_record *job_ptr)
{
	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL)
		_init_node_cr();
	_rm_job_from_nodes(cr_ptr, job_ptr, "select_p_job_suspend", false);
	slurm_mutex_unlock(&cr_mutex);
	return SLURM_SUCCESS;
}

extern int select_p_job_resume(struct job_record *job_ptr)
{
	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL)
		_init_node_cr();
	_add_job_to_nodes(cr_ptr, job_ptr, "select_p_job_resume", 0);
	slurm_mutex_unlock(&cr_mutex);
	return SLURM_SUCCESS;
}

extern bitstr_t *select_p_step_pick_nodes(struct job_record *job_ptr,
					  select_jobinfo_t *jobinfo,
					  uint32_t node_count)
{
	return NULL;
}

extern int select_p_step_finish(struct step_record *step_ptr)
{
	return SLURM_SUCCESS;
}

extern int select_p_pack_select_info(time_t last_query_time,
				     uint16_t show_flags, Buf *buffer_ptr,
				     uint16_t protocol_version)
{
	/* This function is always invalid on normal Linux clusters */
	return SLURM_ERROR;
}

extern int select_p_select_nodeinfo_pack(select_nodeinfo_t *nodeinfo,
					 Buf buffer,
					 uint16_t protocol_version)
{
	pack16(nodeinfo->alloc_cpus, buffer);

	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_unpack(select_nodeinfo_t **nodeinfo,
					   Buf buffer,
					   uint16_t protocol_version)
{
	select_nodeinfo_t *nodeinfo_ptr = NULL;

	nodeinfo_ptr = select_p_select_nodeinfo_alloc(NO_VAL);
	*nodeinfo = nodeinfo_ptr;

	safe_unpack16(&nodeinfo_ptr->alloc_cpus, buffer);

	return SLURM_SUCCESS;

unpack_error:
	error("select_nodeinfo_unpack: error unpacking here");
	select_p_select_nodeinfo_free(nodeinfo_ptr);
	*nodeinfo = NULL;

	return SLURM_ERROR;
}

extern select_nodeinfo_t *select_p_select_nodeinfo_alloc(uint32_t size)
{
	select_nodeinfo_t *nodeinfo = xmalloc(sizeof(struct select_nodeinfo));

	nodeinfo->magic = NODEINFO_MAGIC;

	return nodeinfo;
}

extern int select_p_select_nodeinfo_free(select_nodeinfo_t *nodeinfo)
{
	if(nodeinfo) {
		if (nodeinfo->magic != NODEINFO_MAGIC) {
			error("select_p_select_nodeinfo_free: "
			      "nodeinfo magic bad");
			return EINVAL;
		}
		nodeinfo->magic = 0;
		xfree(nodeinfo);
	}
	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_set_all(time_t last_query_time)
{
	struct node_record *node_ptr = NULL;
	int i=0;
	static time_t last_set_all = 0;

	/* only set this once when the last_node_update is newer than
	 * the last time we set things up. */
	if(last_set_all && (last_node_update < last_set_all)) {
		debug2("Node select info for set all hasn't "
		       "changed since %ld",
		       (long)last_set_all);
		return SLURM_NO_CHANGE_IN_DATA;
	}
	last_set_all = last_node_update;

	for (i=0; i<node_record_count; i++) {
		select_nodeinfo_t *nodeinfo = NULL;

		node_ptr = node_record_table_ptr + i;
		/* We have to use the '_g_' here to make sure we get
		   the correct data to work on.  i.e. cray calls this
		   plugin from within select/cray which has it's own
		   struct.
		*/
		select_g_select_nodeinfo_get(node_ptr->select_nodeinfo,
					     SELECT_NODEDATA_PTR, 0,
					     (void *)&nodeinfo);
		if(!nodeinfo) {
			error("no nodeinfo returned from structure");
			continue;
		}

		if ((node_ptr->node_state & NODE_STATE_COMPLETING) ||
		    (node_ptr->node_state == NODE_STATE_ALLOCATED)) {
			if (slurmctld_conf.fast_schedule)
				nodeinfo->alloc_cpus =
					node_ptr->config_ptr->cpus;
			else
				nodeinfo->alloc_cpus = node_ptr->cpus;
		} else
			nodeinfo->alloc_cpus = 0;
	}

	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_set(struct job_record *job_ptr)
{
	xassert(job_ptr);

	slurm_mutex_lock(&cr_mutex);
	if (cr_ptr == NULL)
		_init_node_cr();
	slurm_mutex_unlock(&cr_mutex);

	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_get(select_nodeinfo_t *nodeinfo,
					enum select_nodedata_type dinfo,
					enum node_states state,
					void *data)
{
	int rc = SLURM_SUCCESS;
	uint16_t *uint16 = (uint16_t *) data;
	select_nodeinfo_t **select_nodeinfo = (select_nodeinfo_t **) data;

	if (nodeinfo == NULL) {
		error("get_nodeinfo: nodeinfo not set");
		return SLURM_ERROR;
	}

	if (nodeinfo->magic != NODEINFO_MAGIC) {
		error("get_nodeinfo: nodeinfo magic bad");
		return SLURM_ERROR;
	}

	switch (dinfo) {
	case SELECT_NODEDATA_SUBGRP_SIZE:
		*uint16 = 0;
		break;
	case SELECT_NODEDATA_SUBCNT:
		if (state == NODE_STATE_ALLOCATED)
			*uint16 = nodeinfo->alloc_cpus;
		else
			*uint16 = 0;
		break;
	case SELECT_NODEDATA_PTR:
		*select_nodeinfo = nodeinfo;
		break;
	default:
		error("Unsupported option %d for get_nodeinfo.", dinfo);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

extern select_jobinfo_t *select_p_select_jobinfo_alloc(void)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_jobinfo_set(select_jobinfo_t *jobinfo,
				       enum select_jobdata_type data_type,
				       void *data)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_jobinfo_get (select_jobinfo_t *jobinfo,
					enum select_jobdata_type data_type,
					void *data)
{
	return SLURM_ERROR;
}

extern select_jobinfo_t *select_p_select_jobinfo_copy(
	select_jobinfo_t *jobinfo)
{
	return NULL;
}

extern int select_p_select_jobinfo_free  (select_jobinfo_t *jobinfo)
{
	return SLURM_SUCCESS;
}

extern int  select_p_select_jobinfo_pack(select_jobinfo_t *jobinfo, Buf buffer,
					 uint16_t protocol_version)
{
	return SLURM_SUCCESS;
}

extern int  select_p_select_jobinfo_unpack(select_jobinfo_t **jobinfo,
					   Buf buffer,
					   uint16_t protocol_version)
{
	return SLURM_SUCCESS;
}

extern char *select_p_select_jobinfo_sprint(select_jobinfo_t *jobinfo,
					    char *buf, size_t size, int mode)
{
	if (buf && size) {
		buf[0] = '\0';
		return buf;
	} else
		return NULL;
}

extern char *select_p_select_jobinfo_xstrdup(select_jobinfo_t *jobinfo,
					     int mode)
{
	return NULL;
}

extern int select_p_update_block (update_part_msg_t *part_desc_ptr)
{
	return SLURM_SUCCESS;
}

extern int select_p_update_sub_node (update_part_msg_t *part_desc_ptr)
{
	return SLURM_SUCCESS;
}

extern int select_p_get_info_from_plugin (enum select_jobdata_type info,
					  struct job_record *job_ptr,
					  void *data)
{
	return SLURM_SUCCESS;
}

extern int select_p_update_node_config (int index)
{
	return SLURM_SUCCESS;
}

extern int select_p_update_node_state (struct node_record *node_ptr)
{
	return SLURM_SUCCESS;
}

extern int select_p_alter_node_cnt(enum select_node_cnt type, void *data)
{
	return SLURM_SUCCESS;
}

extern int select_p_reconfigure(void)
{
	slurm_mutex_lock(&cr_mutex);
	_free_cr(cr_ptr);
	cr_ptr = NULL;
	_init_node_cr();
	slurm_mutex_unlock(&cr_mutex);

	return SLURM_SUCCESS;
}

/*
 * select_p_resv_test - Identify the nodes which "best" satisfy a reservation
 *	request. "best" is defined as either single set of consecutive nodes
 *	satisfying the request and leaving the minimum number of unused nodes
 *	OR the fewest number of consecutive node sets
 * IN avail_bitmap - nodes available for the reservation
 * IN node_cnt - count of required nodes
 * RET - nodes selected for use by the reservation
 */
extern bitstr_t * select_p_resv_test(bitstr_t *avail_bitmap, uint32_t node_cnt)
{
	bitstr_t **switches_bitmap;		/* nodes on this switch */
	int       *switches_cpu_cnt;		/* total CPUs on switch */
	int       *switches_node_cnt;		/* total nodes on switch */
	int       *switches_required;		/* set if has required node */

	bitstr_t  *avail_nodes_bitmap = NULL;	/* nodes on any switch */
	int rem_nodes;			/* remaining resources desired */
	int i, j;
	int best_fit_inx, first, last;
	int best_fit_nodes;
	int best_fit_location = 0, best_fit_sufficient;
	bool sufficient;

	xassert(avail_bitmap);
	if (!switch_record_cnt || !switch_record_table)
		return bit_pick_cnt(avail_bitmap, node_cnt);

	/* Use topology state information */
	if (bit_set_count(avail_bitmap) < node_cnt)
		return avail_nodes_bitmap;
	rem_nodes = node_cnt;

	/* Construct a set of switch array entries,
	 * use the same indexes as switch_record_table in slurmctld */
	switches_bitmap   = xmalloc(sizeof(bitstr_t *) * switch_record_cnt);
	switches_cpu_cnt  = xmalloc(sizeof(int)        * switch_record_cnt);
	switches_node_cnt = xmalloc(sizeof(int)        * switch_record_cnt);
	switches_required = xmalloc(sizeof(int)        * switch_record_cnt);
	for (i=0; i<switch_record_cnt; i++) {
		switches_bitmap[i] = bit_copy(switch_record_table[i].
					      node_bitmap);
		bit_and(switches_bitmap[i], avail_bitmap);
		switches_node_cnt[i] = bit_set_count(switches_bitmap[i]);
	}

#if SELECT_DEBUG
	/* Don't compile this, it slows things down too much */
	for (i=0; i<switch_record_cnt; i++) {
		char *node_names = NULL;
		if (switches_node_cnt[i])
			node_names = bitmap2node_name(switches_bitmap[i]);
		debug("switch=%s nodes=%u:%s required:%u speed=%u",
		      switch_record_table[i].name,
		      switches_node_cnt[i], node_names,
		      switches_required[i],
		      switch_record_table[i].link_speed);
		xfree(node_names);
	}
#endif

	/* Determine lowest level switch satifying request with best fit */
	best_fit_inx = -1;
	for (j=0; j<switch_record_cnt; j++) {
		if (switches_node_cnt[j] < rem_nodes)
			continue;
		if ((best_fit_inx == -1) ||
		    (switch_record_table[j].level <
		     switch_record_table[best_fit_inx].level) ||
		    ((switch_record_table[j].level ==
		      switch_record_table[best_fit_inx].level) &&
		     (switches_node_cnt[j] < switches_node_cnt[best_fit_inx])))
			best_fit_inx = j;
	}
	if (best_fit_inx == -1) {
		debug("select_p_resv_test: could not find resources for "
		      "reservation");
		goto fini;
	}

	/* Identify usable leafs (within higher switch having best fit) */
	for (j=0; j<switch_record_cnt; j++) {
		if ((switch_record_table[j].level != 0) ||
		    (!bit_super_set(switches_bitmap[j],
				    switches_bitmap[best_fit_inx]))) {
			switches_node_cnt[j] = 0;
		}
	}

	/* Select resources from these leafs on a best-fit basis */
	avail_nodes_bitmap = bit_alloc(node_record_count);
	while (rem_nodes > 0) {
		best_fit_nodes = best_fit_sufficient = 0;
		for (j=0; j<switch_record_cnt; j++) {
			if (switches_node_cnt[j] == 0)
				continue;
			sufficient = (switches_node_cnt[j] >= rem_nodes);
			/* If first possibility OR */
			/* first set large enough for request OR */
			/* tightest fit (less resource waste) OR */
			/* nothing yet large enough, but this is biggest */
			if ((best_fit_nodes == 0) ||
			    (sufficient && (best_fit_sufficient == 0)) ||
			    (sufficient &&
			     (switches_node_cnt[j] < best_fit_nodes)) ||
			    ((sufficient == 0) &&
			     (switches_node_cnt[j] > best_fit_nodes))) {
				best_fit_nodes = switches_node_cnt[j];
				best_fit_location = j;
				best_fit_sufficient = sufficient;
			}
		}
		if (best_fit_nodes == 0)
			break;
		/* Use select nodes from this leaf */
		first = bit_ffs(switches_bitmap[best_fit_location]);
		last  = bit_fls(switches_bitmap[best_fit_location]);
		for (i=first; ((i<=last) && (first>=0)); i++) {
			if (!bit_test(switches_bitmap[best_fit_location], i))
				continue;

			bit_clear(switches_bitmap[best_fit_location], i);
			switches_node_cnt[best_fit_location]--;

			if (bit_test(avail_nodes_bitmap, i)) {
				/* node on multiple leaf switches
				 * and already selected */
				continue;
			}

			bit_set(avail_nodes_bitmap, i);
			if (--rem_nodes <= 0)
				break;
		}
		switches_node_cnt[best_fit_location] = 0;
	}
	if (rem_nodes > 0)	/* insufficient resources */
		FREE_NULL_BITMAP(avail_nodes_bitmap);

fini:	for (i=0; i<switch_record_cnt; i++)
		FREE_NULL_BITMAP(switches_bitmap[i]);
	xfree(switches_bitmap);
	xfree(switches_cpu_cnt);
	xfree(switches_node_cnt);
	xfree(switches_required);

	return avail_nodes_bitmap;
}

extern void select_p_ba_init(void)
{
	return;
}
extern void select_p_ba_fini(void)
{
	return;
}

extern int *select_p_ba_get_dims(void)
{
	return NULL;
}

extern void select_p_ba_reset(bool track_down_nodes)
{
	return;
}

extern int select_p_ba_request_apply(select_ba_request_t *ba_request)
{
	return 1;
}

extern int select_p_ba_remove_block(List mps, bool is_small)
{
	return 1;
}
