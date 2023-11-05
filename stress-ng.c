/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2021-2023 Colin Ian King
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-affinity.h"
#include "core-bitops.h"
#include "core-builtin.h"
#include "core-clocksource.h"
#include "core-cpuidle.h"
#include "core-config-check.h"
#include "core-ftrace.h"
#include "core-hash.h"
#include "core-ignite-cpu.h"
#include "core-interrupts.h"
#include "core-io-priority.h"
#include "core-job.h"
#include "core-klog.h"
#include "core-limit.h"
#include "core-mlock.h"
#include "core-numa.h"
#include "core-opts.h"
#include "core-out-of-memory.h"
#include "core-perf.h"
#include "core-pragma.h"
#include "core-shared-heap.h"
#include "core-smart.h"
#include "core-stressors.h"
#include "core-syslog.h"
#include "core-thermal-zone.h"
#include "core-thrash.h"
#include "core-vmstat.h"

#if defined(HAVE_SYS_UTSNAME_H)
#include <sys/utsname.h>
#endif

#if defined(HAVE_SYSLOG_H)
#include <syslog.h>
#endif

#if defined(HAVE_LINUX_FS_H)
#include <linux/fs.h>
#endif

#include <float.h>

#define MIN_SEQUENTIAL		(0)
#define MAX_SEQUENTIAL		(1000000)
#define DEFAULT_SEQUENTIAL	(0)	/* Disabled */
#define DEFAULT_PARALLEL	(0)	/* Disabled */
#define DEFAULT_TIMEOUT		(60 * 60 * 24)
#define DEFAULT_BACKOFF		(0)
#define DEFAULT_CACHE_LEVEL     (3)

/* stress_stressor_info ignore value. 2 bits */
#define STRESS_STRESSOR_NOT_IGNORED		(0)
#define STRESS_STRESSOR_UNSUPPORTED		(1)
#define STRESS_STRESSOR_EXCLUDED		(2)

/* Stress test classes */
typedef struct {
	const stress_class_t class;	/* Class type bit mask */
	const char *name;		/* Name of class */
} stress_class_info_t;

typedef struct {
	const int opt;			/* optarg option */
	const uint64_t opt_flag;	/* global options flag bit setting */
} stress_opt_flag_t;

/* Per stressor information */
static stress_stressor_t *stressors_head, *stressors_tail;

/* Various option settings and flags */
static volatile bool wait_flag = true;		/* false = exit run wait loop */
static int terminate_signum;			/* signal sent to process */
static pid_t main_pid;				/* stress-ng main pid */
static bool *sigalarmed = NULL;			/* pointer to stressor stats->sigalarmed */

/* Globals */
stress_stressor_t *g_stressor_current;		/* current stressor being invoked */
int32_t g_opt_sequential = DEFAULT_SEQUENTIAL;	/* # of sequential stressors */
int32_t g_opt_parallel = DEFAULT_PARALLEL;	/* # of parallel stressors */
int32_t g_opt_permute = DEFAULT_PARALLEL;	/* # of permuted stressors */
uint64_t g_opt_timeout = TIMEOUT_NOT_SET;	/* timeout in seconds */
uint64_t g_opt_flags = OPT_FLAGS_PR_ERROR |	/* default option flags */
		       OPT_FLAGS_PR_INFO |
		       OPT_FLAGS_MMAP_MADVISE;
volatile bool g_stress_continue_flag = true;	/* false to exit stressor */
const char g_app_name[] = "stress-ng";		/* Name of application */
stress_shared_t *g_shared;			/* shared memory */
jmp_buf g_error_env;				/* parsing error env */
stress_put_val_t g_put_val;			/* sync data to somewhere */

#if defined(SA_SIGINFO)
typedef struct {
	int	code;
	pid_t	pid;
	uid_t	uid;
	struct timeval when;
	bool 	triggered;
} stress_sigalrm_info_t;

stress_sigalrm_info_t sigalrm_info;
#endif

/*
 *  optarg option to global setting option flags
 */
static const stress_opt_flag_t opt_flags[] = {
	{ OPT_abort,		OPT_FLAGS_ABORT },
	{ OPT_aggressive,	OPT_FLAGS_AGGRESSIVE_MASK },
	{ OPT_change_cpu,	OPT_FLAGS_CHANGE_CPU },
	{ OPT_dry_run,		OPT_FLAGS_DRY_RUN },
	{ OPT_ftrace,		OPT_FLAGS_FTRACE },
	{ OPT_ignite_cpu,	OPT_FLAGS_IGNITE_CPU },
	{ OPT_interrupts,	OPT_FLAGS_INTERRUPTS },
	{ OPT_keep_files, 	OPT_FLAGS_KEEP_FILES },
	{ OPT_keep_name, 	OPT_FLAGS_KEEP_NAME },
	{ OPT_klog_check,	OPT_FLAGS_KLOG_CHECK },
	{ OPT_ksm,		OPT_FLAGS_KSM },
	{ OPT_log_brief,	OPT_FLAGS_LOG_BRIEF },
	{ OPT_log_lockless,	OPT_FLAGS_LOG_LOCKLESS },
	{ OPT_maximize,		OPT_FLAGS_MAXIMIZE },
	{ OPT_metrics,		OPT_FLAGS_METRICS | OPT_FLAGS_PR_METRICS },
	{ OPT_metrics_brief,	OPT_FLAGS_METRICS_BRIEF | OPT_FLAGS_METRICS | OPT_FLAGS_PR_METRICS },
	{ OPT_minimize,		OPT_FLAGS_MINIMIZE },
	{ OPT_no_oom_adjust,	OPT_FLAGS_NO_OOM_ADJUST },
	{ OPT_no_rand_seed,	OPT_FLAGS_NO_RAND_SEED },
	{ OPT_oomable,		OPT_FLAGS_OOMABLE },
	{ OPT_oom_avoid,	OPT_FLAGS_OOM_AVOID },
	{ OPT_page_in,		OPT_FLAGS_MMAP_MINCORE },
	{ OPT_pathological,	OPT_FLAGS_PATHOLOGICAL },
#if defined(STRESS_PERF_STATS) && 	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	{ OPT_perf_stats,	OPT_FLAGS_PERF_STATS },
#endif
	{ OPT_settings,		OPT_FLAGS_SETTINGS },
	{ OPT_skip_silent,	OPT_FLAGS_SKIP_SILENT },
	{ OPT_smart,		OPT_FLAGS_SMART },
	{ OPT_sn,		OPT_FLAGS_SN },
	{ OPT_sock_nodelay,	OPT_FLAGS_SOCKET_NODELAY },
	{ OPT_stderr,		OPT_FLAGS_STDERR },
	{ OPT_stdout,		OPT_FLAGS_STDOUT },
#if defined(HAVE_SYSLOG_H)
	{ OPT_syslog,		OPT_FLAGS_SYSLOG },
#endif
	{ OPT_thrash, 		OPT_FLAGS_THRASH },
	{ OPT_times,		OPT_FLAGS_TIMES },
	{ OPT_timestamp,	OPT_FLAGS_TIMESTAMP },
	{ OPT_thermal_zones,	OPT_FLAGS_THERMAL_ZONES | OPT_FLAGS_TZ_INFO },
	{ OPT_verbose,		OPT_FLAGS_PR_ALL },
	{ OPT_verify,		OPT_FLAGS_VERIFY | OPT_FLAGS_PR_FAIL },
};

/*
 *  Attempt to catch a range of signals so
 *  we can clean up rather than leave
 *  cruft everywhere.
 */
static const int stress_terminate_signals[] = {
	/* POSIX.1-1990 */
#if defined(SIGHUP)
	SIGHUP,
#endif
#if defined(SIGINT)
	SIGINT,
#endif
#if defined(SIGILL)
	SIGILL,
#endif
#if defined(SIGQUIT)
	SIGQUIT,
#endif
#if defined(SIGABRT)
	SIGABRT,
#endif
#if defined(SIGFPE)
	SIGFPE,
#endif
#if defined(SIGTERM)
	SIGTERM,
#endif
#if defined(SIGXCPU)
	SIGXCPU,
#endif
#if defined(SIGXFSZ)
	SIGXFSZ,
#endif
	/* Linux various */
#if defined(SIGIOT)
	SIGIOT,
#endif
#if defined(SIGSTKFLT)
	SIGSTKFLT,
#endif
#if defined(SIGPWR)
	SIGPWR,
#endif
#if defined(SIGINFO)
	SIGINFO,
#endif
#if defined(SIGVTALRM)
	SIGVTALRM,
#endif
};

static const int stress_ignore_signals[] = {
#if defined(SIGUSR1)
	SIGUSR1,
#endif
#if defined(SIGUSR2)
	SIGUSR2,
#endif
#if defined(SIGTTOU)
	SIGTTOU,
#endif
#if defined(SIGTTIN)
	SIGTTIN,
#endif
#if defined(SIGWINCH)
	SIGWINCH,
#endif
};

/* Stressor id values */
enum {
	STRESS_START = -1,
	STRESSORS(STRESSOR_ENUM)
};

/* Stressor extern info structs */
STRESSORS(STRESSOR_INFO)

/*
 *  Human readable stress test names.
 */
static const stress_t stressors[] = {
	STRESSORS(STRESSOR_ELEM)
};

/*
 *  Different stress classes
 */
static const stress_class_info_t stress_classes[] = {
	{ CLASS_CPU_CACHE,	"cpu-cache" },
	{ CLASS_CPU,		"cpu" },
	{ CLASS_DEV,		"device" },
	{ CLASS_FILESYSTEM,	"filesystem" },
	{ CLASS_GPU,		"gpu" },
	{ CLASS_INTERRUPT,	"interrupt" },
	{ CLASS_IO,		"io" },
	{ CLASS_MEMORY,		"memory" },
	{ CLASS_NETWORK,	"network" },
	{ CLASS_OS,		"os" },
	{ CLASS_PIPE_IO,	"pipe" },
	{ CLASS_SCHEDULER,	"scheduler" },
	{ CLASS_SECURITY,	"security" },
	{ CLASS_VM,		"vm" },
};

/*
 *  Generic help options
 */
static const stress_help_t help_generic[] = {
	{ NULL,		"abort",		"abort all stressors if any stressor fails" },
	{ NULL,		"aggressive",		"enable all aggressive options" },
	{ "a N",	"all N",		"start N workers of each stress test" },
	{ "b N",	"backoff N",		"wait of N microseconds before work starts" },
	{ NULL,		"change cpu",		"force child processes to use different CPU to that of parent" },
	{ NULL,		"class name",		"specify a class of stressors, use with --sequential" },
	{ "n",		"dry-run",		"do not run" },
	{ NULL,		"ftrace",		"enable kernel function call tracing" },
	{ "h",		"help",			"show help" },
	{ NULL,		"ignite-cpu",		"alter kernel controls to make CPU run hot" },
	{ NULL,		"interrupts",		"check for error interrupts" },
	{ NULL,		"ionice-class C",	"specify ionice class (idle, besteffort, realtime)" },
	{ NULL,		"ionice-level L",	"specify ionice level (0 max, 7 min)" },
	{ NULL,		"iostate S",		"show I/O statistics every S seconds" },
	{ "j",		"job jobfile",		"run the named jobfile" },
	{ NULL,		"keep-files",		"do not remove files or directories" },
	{ "k",		"keep-name",		"keep stress worker names to be 'stress-ng'" },
	{ NULL,		"klog-check",		"check kernel message log for errors" },
	{ NULL,		"ksm",			"enable kernel samepage merging" },
	{ NULL,		"log-brief",		"less verbose log messages" },
	{ NULL,		"log-file filename",	"log messages to a log file" },
	{ NULL,		"log-lockless",		"log messages without message locking" },
	{ NULL,		"maximize",		"enable maximum stress options" },
	{ NULL,		"max-fd N",		"set maximum file descriptor limit" },
	{ NULL,		"mbind",		"set NUMA memory binding to specific nodes" },
	{ "M",		"metrics",		"print pseudo metrics of activity" },
	{ NULL,		"metrics-brief",	"enable metrics and only show non-zero results" },
	{ NULL,		"minimize",		"enable minimal stress options" },
	{ NULL,		"no-madvise",		"don't use random madvise options for each mmap" },
	{ NULL,		"no-oom-adjust",	"disable all forms of out-of-memory score adjustments" },
	{ NULL,		"no-rand-seed",		"seed random numbers with the same constant" },
	{ NULL,		"oom-avoid",		"Try to avoid stressors from being OOM'd" },
	{ NULL,		"oom-avoid-bytes N",	"Number of bytes free to stop futher memory allocations" },
	{ NULL,		"oomable",		"Do not respawn a stressor if it gets OOM'd" },
	{ NULL,		"page-in",		"touch allocated pages that are not in core" },
	{ NULL,		"parallel N",		"synonym for 'all N'" },
	{ NULL,		"pathological",		"enable stressors that are known to hang a machine" },
#if defined(STRESS_PERF_STATS) &&	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	{ NULL,		"perf",			"display perf statistics" },
#endif
	{ NULL,		"permute N",		"run permutations of stressors with N stressors per permutation" },
	{ "q",		"quiet",		"quiet output" },
	{ "r",		"random N",		"start N random workers" },
	{ NULL,		"sched type",		"set scheduler type" },
	{ NULL,		"sched-prio N",		"set scheduler priority level N" },
	{ NULL,		"sched-period N",	"set period for SCHED_DEADLINE to N nanosecs (Linux only)" },
	{ NULL,		"sched-runtime N",	"set runtime for SCHED_DEADLINE to N nanosecs (Linux only)" },
	{ NULL,		"sched-deadline N",	"set deadline for SCHED_DEADLINE to N nanosecs (Linux only)" },
	{ NULL,		"sched-reclaim",        "set reclaim cpu bandwidth for deadline scheduler (Linux only)" },
	{ NULL,		"seed N",		"set the random number generator seed with a 64 bit value" },
	{ NULL,		"sequential N",		"run all stressors one by one, invoking N of them" },
	{ NULL,		"skip-silent",		"silently skip unimplemented stressors" },
	{ NULL,		"smart",		"show changes in S.M.A.R.T. data" },
	{ NULL,		"sn",			"use scientific notation for metrics" },
	{ NULL,		"status S",		"show stress-ng progress status every S seconds" },
	{ NULL,		"stderr",		"all output to stderr" },
	{ NULL,		"stdout",		"all output to stdout (now the default)" },
	{ NULL,		"stressors",		"show available stress tests" },
#if defined(HAVE_SYSLOG_H)
	{ NULL,		"syslog",		"log messages to the syslog" },
#endif
	{ NULL,		"taskset",		"use specific CPUs (set CPU affinity)" },
	{ NULL,		"temp-path path",	"specify path for temporary directories and files" },
	{ NULL,		"thermalstat S",	"show CPU and thermal load stats every S seconds" },
	{ NULL,		"thrash",		"force all pages in causing swap thrashing" },
	{ "t N",	"timeout T",		"timeout after T seconds" },
	{ NULL,		"timer-slack N",	"set slack slack to N nanoseconds, 0 for default" },
	{ NULL,		"times",		"show run time summary at end of the run" },
	{ NULL,		"timestamp",		"timestamp log output " },
#if defined(STRESS_THERMAL_ZONES)
	{ NULL,		"tz",			"collect temperatures from thermal zones (Linux only)" },
#endif
	{ "v",		"verbose",		"verbose output" },
	{ NULL,		"verify",		"verify results (not available on all tests)" },
	{ NULL,		"verifiable",		"show stressors that enable verification via --verify" },
	{ "V",		"version",		"show version" },
	{ NULL,		"vmstat S",		"show memory and process statistics every S seconds" },
	{ "x",		"exclude list",		"list of stressors to exclude (not run)" },
	{ NULL,		"with list",		"list of stressors to invoke (use with --seq or --all)" },
	{ "Y",		"yaml file",		"output results to YAML formatted file" },
	{ NULL,		NULL,			NULL }
};

/*
 *  stress_hash_checksum()
 *	generate a hash of the checksum data
 */
static inline void stress_hash_checksum(stress_checksum_t *checksum)
{
	checksum->hash = stress_hash_jenkin((uint8_t *)&checksum->data,
				sizeof(checksum->data));
}

/*
 *  stressor_find_by_name()
 *  	Find index into stressors by name
 */
static size_t stressor_find_by_name(const char *name)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		if (!stress_strcmp_munged(name, stressors[i].name))
			break;
	}
	return i;
}

/*
 *  stressor_find_by_id()
 *	Find stressor by id, return index
 */
static size_t stressor_find_by_id(const unsigned int id)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		if (id == stressors[i].id)
			break;
	}
	return i;
}

/*
 *  stress_ignore_stressor()
 *	remove stressor from stressor list
 */
static inline void stress_ignore_stressor(stress_stressor_t *ss, uint8_t reason)
{
	ss->ignore.run = reason;
}

/*
 *  stress_get_class_id()
 *	find the class id of a given class name
 */
static uint32_t stress_get_class_id(const char *const str)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stress_classes); i++) {
		if (!strcmp(stress_classes[i].name, str))
			return stress_classes[i].class;
	}
	return 0;
}

/*
 *  stress_get_class()
 *	parse for allowed class types, return bit mask of types, 0 if error
 */
static int stress_get_class(char *const class_str, uint32_t *class)
{
	char *str, *token;
	int ret = 0;

	*class = 0;
	for (str = class_str; (token = strtok(str, ",")) != NULL; str = NULL) {
		uint32_t cl = stress_get_class_id(token);

		if (!cl) {
			size_t i;
			const size_t len = strlen(token);

			if ((len > 1) && (token[len - 1] == '?')) {
				token[len - 1] = '\0';

				cl = stress_get_class_id(token);
				if (cl) {
					size_t j;

					(void)printf("class '%s' stressors:",
						token);
					for (j = 0; j < SIZEOF_ARRAY(stressors); j++) {
						if (stressors[j].info->class & cl) {
							char munged[64];

							(void)stress_munge_underscore(munged, stressors[j].name, sizeof(munged));
							(void)printf(" %s", munged);
						}
					}
					(void)printf("\n");
					return 1;
				}
			}
			(void)fprintf(stderr, "Unknown class: '%s', "
				"available classes:", token);
			for (i = 0; i < SIZEOF_ARRAY(stress_classes); i++)
				(void)fprintf(stderr, " %s", stress_classes[i].name);
			(void)fprintf(stderr, "\n\n");
			return -1;
		}
		*class |= cl;
	}
	return ret;
}

/*
 *  stress_exclude()
 *  	parse -x --exlude exclude list
 */
static int stress_exclude(void)
{
	char *str, *token, *opt_exclude;

	if (!stress_get_setting("exclude", &opt_exclude))
		return 0;

	for (str = opt_exclude; (token = strtok(str, ",")) != NULL; str = NULL) {
		unsigned int id;
		stress_stressor_t *ss = stressors_head;
		const size_t i = stressor_find_by_name(token);

		if (i >= SIZEOF_ARRAY(stressors)) {
			(void)fprintf(stderr, "Unknown stressor: '%s', "
				"invalid exclude option\n", token);
			return -1;
		}
		id = stressors[i].id;

		while (ss) {
			stress_stressor_t *next = ss->next;

			if (ss->stressor->id == id)
				stress_ignore_stressor(ss, STRESS_STRESSOR_EXCLUDED);
			ss = next;
		}
	}
	return 0;
}

/*
 *  stress_kill_stressors()
 * 	kill stressor tasks using signal sig
 */
static void stress_kill_stressors(const int sig, const bool force_sigkill)
{
	int signum = sig;
	stress_stressor_t *ss;

	if (force_sigkill) {
		static int count = 0;

		/* multiple calls will always fallback to SIGKILL */
		count++;
		if (count > 5) {
			pr_dbg("killing processes with SIGKILL\n");
			signum = SIGKILL;
		}
	}

	for (ss = stressors_head; ss; ss = ss->next) {
		int32_t i;

		if (ss->ignore.run)
			continue;

		for (i = 0; i < ss->num_instances; i++) {
			stress_stats_t *const stats = ss->stats[i];
			const pid_t pid = stats->pid;

			if (pid && !stats->signalled) {
				(void)shim_kill(pid, signum);
				stats->signalled = true;
			}
		}
	}
}

/*
 *  stress_sigint_handler()
 *	catch signals and set flag to break out of stress loops
 */
static void MLOCKED_TEXT stress_sigint_handler(int signum)
{
	(void)signum;
	if (g_shared)
		g_shared->caught_sigint = true;
	stress_continue_set_flag(false);
	wait_flag = false;

	/* Send alarm to all stressors */
	stress_kill_stressors(SIGALRM, true);
}

/*
 *  stress_sigalrm_handler()
 *	handle signal in parent process, don't block on waits
 */
static void MLOCKED_TEXT stress_sigalrm_handler(int signum)
{
	if (g_shared) {
		g_shared->caught_sigint = true;
		if (sigalarmed) {
			if (!*sigalarmed) {
				g_shared->instance_count.alarmed++;
				*sigalarmed = true;
			}
		}
	}
	if (getpid() == main_pid) {
		/* Parent */
		wait_flag = false;
		stress_kill_stressors(SIGALRM, false);
	} else {
		/* Child */
		stress_handle_stop_stressing(signum);
	}
}

/*
 *  stress_block_signals()
 *	block signals
 */
static void stress_block_signals(void)
{
	sigset_t set;

	(void)sigfillset(&set);
	(void)sigprocmask(SIG_SETMASK, &set, NULL);
}

#if defined(SA_SIGINFO)
static void MLOCKED_TEXT stress_sigalrm_action_handler(
	int signum,
	siginfo_t *info,
	void *ucontext)
{
	(void)ucontext;

	if (g_shared && 			/* shared mem initialized */
	    !g_shared->caught_sigint &&		/* and SIGINT not already handled */
	    info && 				/* and info is valid */
	    (info->si_code == SI_USER) &&	/* and not from kernel SIGALRM */
	    (!sigalrm_info.triggered)) {	/* and not already handled */
		sigalrm_info.code = info->si_code;
		sigalrm_info.pid = info->si_pid;
		sigalrm_info.uid = info->si_uid;
		(void)gettimeofday(&sigalrm_info.when, NULL);
		sigalrm_info.triggered = true;
	}
	stress_sigalrm_handler(signum);
}
#endif

#if defined(SIGUSR2)
/*
 *  stress_stats_handler()
 *	dump current system stats
 */
static void MLOCKED_TEXT stress_stats_handler(int signum)
{
	static char buffer[80];
	char *ptr = buffer;
	double min1, min5, min15;
	size_t shmall, freemem, totalmem, freeswap, totalswap;

	(void)signum;

	*ptr = '\0';

	if (stress_get_load_avg(&min1, &min5, &min15) == 0) {
		int ret;

		ret = snprintf(ptr, sizeof(buffer),
			"Load Avg: %.2f %.2f %.2f, ",
			min1, min5, min15);
		if (ret > 0)
			ptr += ret;
	}
	stress_get_memlimits(&shmall, &freemem, &totalmem, &freeswap, &totalswap);

	(void)snprintf(ptr, (size_t)(buffer - ptr),
		"MemFree: %zu MB, MemTotal: %zu MB",
		freemem / (size_t)MB, totalmem / (size_t)MB);
	/* Really shouldn't do this in a signal handler */
	(void)fprintf(stdout, "%s\n", buffer);
	(void)fflush(stdout);
}
#endif

/*
 *  stress_set_handler()
 *	set signal handler to catch SIGINT, SIGALRM, SIGHUP
 */
static int stress_set_handler(const char *stress, const bool child)
{
#if defined(SA_SIGINFO)
	struct sigaction sa;
#endif
	if (stress_sighandler(stress, SIGINT, stress_sigint_handler, NULL) < 0)
		return -1;
	if (stress_sighandler(stress, SIGHUP, stress_sigint_handler, NULL) < 0)
		return -1;
#if defined(SIGUSR2)
	if (!child) {
		if (stress_sighandler(stress, SIGUSR2,
			stress_stats_handler, NULL) < 0) {
			return -1;
		}
	}
#endif
#if defined(SA_SIGINFO)
	(void)shim_memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = stress_sigalrm_action_handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGALRM, &sa, NULL) < 0) {
		pr_fail("%s: sigaction SIGALRM: errno=%d (%s)\n",
                        stress, errno, strerror(errno));
	}
#else
	if (stress_sighandler(stress, SIGALRM, stress_sigalrm_handler, NULL) < 0)
		return -1;
#endif
	return 0;
}

/*
 *  stress_version()
 *	print program version info
 */
static void stress_version(void)
{
	(void)printf("%s, version " VERSION " (%s, %s)%s\n",
		g_app_name, stress_get_compiler(), stress_get_uname_info(),
		stress_is_dev_tty(STDOUT_FILENO) ? "" : " \U0001F4BB\U0001F525");
}

/*
 *  stress_usage_help()
 *	show generic help information
 */
static void stress_usage_help(const stress_help_t help_info[])
{
	size_t i;
	const int cols = stress_get_tty_width();

	for (i = 0; help_info[i].description; i++) {
		char opt_s[10] = "";
		int wd = 0;
		bool first = true;
		const char *ptr, *space = NULL;
		const char *start = help_info[i].description;

		if (help_info[i].opt_s)
			(void)snprintf(opt_s, sizeof(opt_s), "-%s,",
					help_info[i].opt_s);
		(void)printf("%-6s--%-22s", opt_s, help_info[i].opt_l);

		for (ptr = start; *ptr; ptr++) {
			if (*ptr == ' ')
				space = ptr;
			wd++;
			if (wd >= cols - 30) {
				const size_t n = (size_t)(space - start);

				if (!first)
					(void)printf("%-30s", "");
				first = false;
				(void)printf("%*.*s\n", (int)n, (int)n, start);
				start = space + 1;
				wd = 0;
			}
		}
		if (start != ptr) {
			const int n = (int)(ptr - start);
			if (!first)
				(void)printf("%-30s", "");
			(void)printf("%*.*s\n", n, n, start);
		}
	}
}

/*
 *  stress_verfiable_mode()
 *	show the stressors that are verified by their verify mode
 */
static void stress_verifiable_mode(const stress_verify_t mode)
{
	size_t i;
	bool space = false;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		if (stressors[i].info->verify == mode) {
			char munged[64];

			(void)stress_munge_underscore(munged, stressors[i].name, sizeof(munged));
			(void)printf("%s%s", space ? " " : "", munged);
			space = true;
		}
	}
	(void)putchar('\n');
}

/*
 *  stress_verfiable()
 *	show the stressors that have --verify ability
 */
static void stress_verifiable(void)
{
	(void)printf("Verification always enabled:\n");
	stress_verifiable_mode(VERIFY_ALWAYS);
	(void)printf("\nVerification enabled by --verify option:\n");
	stress_verifiable_mode(VERIFY_OPTIONAL);
	(void)printf("\nVerification not implemented:\n");
	stress_verifiable_mode(VERIFY_NONE);
}

/*
 *  stress_usage_help_stressors()
 *	show per stressor help information
 */
static void stress_usage_help_stressors(void)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		if (stressors[i].info->help)
			stress_usage_help(stressors[i].info->help);
	}
}

/*
 *  stress_show_stressor_names()
 *	show stressor names
 */
static inline void stress_show_stressor_names(void)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		char munged[64];

		(void)stress_munge_underscore(munged, stressors[i].name, sizeof(munged));
		(void)printf("%s%s", i ? " " : "", munged);
	}
	(void)putchar('\n');
}

/*
 *  stress_usage()
 *	print some help
 */
static void NORETURN stress_usage(void)
{
	stress_version();
	(void)printf("\nUsage: %s [OPTION [ARG]]\n", g_app_name);
	(void)printf("\nGeneral control options:\n");
	stress_usage_help(help_generic);
	(void)printf("\nStressor specific options:\n");
	stress_usage_help_stressors();
	(void)printf("\nExample: %s --cpu 8 --iomix 4 --vm 2 --vm-bytes 128M "
		"--fork 4 --timeout 10s\n\n"
		"Note: sizes can be suffixed with B, K, M, G and times with "
		"s, m, h, d, y\n", g_app_name);
	stress_settings_free();
	stress_temp_path_free();
	exit(EXIT_SUCCESS);
}

/*
 *  stress_opt_name()
 *	find name associated with an option value
 */
static const char *stress_opt_name(const int opt_val)
{
	size_t i;

	for (i = 0; stress_long_options[i].name; i++)
		if (stress_long_options[i].val == opt_val)
			return stress_long_options[i].name;

	return "unknown";
}

/*
 *  stress_get_processors()
 *	get number of processors, set count if <=0 as:
 *		count = 0 -> number of CPUs in system
 *		count < 0 -> number of CPUs online
 */
static void stress_get_processors(int32_t *count)
{
	if (*count == 0)
		*count = stress_get_processors_configured();
	else if (*count < 0)
		*count = stress_get_processors_online();
}

/*
 *  stress_stressor_finished()
 *	mark a stressor process as complete
 */
static inline void stress_stressor_finished(pid_t *pid)
{
	*pid = 0;
	g_shared->instance_count.reaped++;
}

/*
 *  stress_exit_status_to_string()
 *	map stress-ng exit status returns into text
 */
static const char *stress_exit_status_to_string(const int status)
{
	typedef struct {
		const int status;
		const char *description;
	} stress_exit_status_map_t;

	static const stress_exit_status_map_t stress_exit_status_map[] = {
		{ EXIT_SUCCESS,			"success" },
		{ EXIT_FAILURE,			"stress-ng core failure " },
		{ EXIT_NOT_SUCCESS,		"stressor failed" },
		{ EXIT_NO_RESOURCE,		"no resources" },
		{ EXIT_NOT_IMPLEMENTED,		"not implemented" },
		{ EXIT_SIGNALED,		"killed by signal" },
		{ EXIT_BY_SYS_EXIT,		"stressor terminated using _exit()" },
		{ EXIT_METRICS_UNTRUSTWORTHY,	"metrics may be untrustworthy" },
	};
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stress_exit_status_map); i++) {
		if (status == stress_exit_status_map[i].status)
			return stress_exit_status_map[i].description;
	}
	return "unknown";
}

#if defined(HAVE_SCHED_GETAFFINITY) &&	\
    NEED_GLIBC(2,3,0)
/*
 *  stress_wait_aggressive()
 *	while waiting for stressors to complete add some aggressive
 *	CPU affinity changing to exercise the scheduler placement
 */
static void stress_wait_aggressive(
	const int32_t ticks_per_sec,
	stress_stressor_t *stressors_list)
{
	stress_stressor_t *ss;
	cpu_set_t proc_mask;
	const useconds_t usec_sleep =
		ticks_per_sec ? 1000000 / ((useconds_t)5 * ticks_per_sec) : 1000000 / 250;

	while (wait_flag) {
		const int32_t cpus = stress_get_processors_configured();
		bool procs_alive = false;

		/*
		 *  If we can't get the mask, then don't do
		 *  any affinity twiddling
		 */
		if (sched_getaffinity(0, sizeof(proc_mask), &proc_mask) < 0)
			return;
		if (!CPU_COUNT(&proc_mask))	/* Highly unlikely */
			return;

		(void)shim_usleep(usec_sleep);

		for (ss = stressors_list; ss; ss = ss->next) {
			int32_t j;

			for (j = 0; j < ss->num_instances; j++) {
				const stress_stats_t *const stats = ss->stats[j];
				const pid_t pid = stats->pid;

				if (pid) {
					cpu_set_t mask;
					int32_t cpu_num;
					int status, ret;

					ret = waitpid(pid, &status, WNOHANG);
					if ((ret < 0) && (errno == ESRCH))
						continue;
					procs_alive = true;

					do {
						cpu_num = (int32_t)stress_mwc32modn(cpus);
					} while (!(CPU_ISSET(cpu_num, &proc_mask)));

					CPU_ZERO(&mask);
					CPU_SET(cpu_num, &mask);
					if (sched_setaffinity(pid, sizeof(mask), &mask) < 0)
						return;
				}
			}
		}
		if (!procs_alive)
			break;
	}
}
#endif

/*
 *   stress_wait_pid()
 *	wait for a stressor by their given pid
 */
static void stress_wait_pid(
	stress_stressor_t *ss,
	const pid_t pid,
	const char *stressor_name,
	stress_stats_t *stats,
	bool *success,
	bool *resource_success,
	bool *metrics_success)
{
	int status, ret;
	bool do_abort = false;

redo:
	ret = shim_waitpid(pid, &status, 0);
	if (ret > 0) {
		int wexit_status = WEXITSTATUS(status);

		if (WIFSIGNALED(status)) {
#if defined(WTERMSIG)
			const int wterm_signal = WTERMSIG(status);

			if (wterm_signal != SIGALRM) {
#if NEED_GLIBC(2,1,0)
				const char *signame = strsignal(wterm_signal);

				pr_dbg("%s: [%d] terminated on signal: %d (%s)\n",
					stressor_name, ret, wterm_signal, signame);
#else
				pr_dbg("%s: [%d] terminated on signal: %d\n",
					stressor_name, ret, wterm_signal);
#endif
			}
#else
			pr_dbg("%s [%d] terminated on signal\n",
				stressor_name, ret);
#endif
			/*
			 *  If the stressor got killed by OOM or SIGKILL
			 *  then somebody outside of our control nuked it
			 *  so don't necessarily flag that up as a direct
			 *  failure.
			 */
			if (stress_process_oomed(ret)) {
				pr_dbg("%s: [%d] killed by the OOM killer\n",
					stressor_name, ret);
			} else if (wterm_signal == SIGKILL) {
				pr_dbg("%s: [%d] possibly killed by the OOM killer\n",
					stressor_name, ret);
			} else if (wterm_signal != SIGALRM) {
				*success = false;
			}
		}
		switch (wexit_status) {
		case EXIT_SUCCESS:
			ss->status[STRESS_STRESSOR_STATUS_PASSED]++;
			break;
		case EXIT_NO_RESOURCE:
			ss->status[STRESS_STRESSOR_STATUS_SKIPPED]++;
			pr_warn_skip("%s: [%d] aborted early, out of system resources\n",
				stressor_name, ret);
			*resource_success = false;
			do_abort = true;
			break;
		case EXIT_NOT_IMPLEMENTED:
			ss->status[STRESS_STRESSOR_STATUS_SKIPPED]++;
			do_abort = true;
			break;
			case EXIT_SIGNALED:
			do_abort = true;
#if defined(STRESS_REPORT_EXIT_SIGNALED)
			pr_dbg("%s: [%d] aborted via a termination signal\n",
				stressor_name, ret);
#endif
			break;
		case EXIT_BY_SYS_EXIT:
			ss->status[STRESS_STRESSOR_STATUS_FAILED]++;
			pr_dbg("%s: [%d] aborted via exit() which was not expected\n",
				stressor_name, ret);
			do_abort = true;
			break;
		case EXIT_METRICS_UNTRUSTWORTHY:
			ss->status[STRESS_STRESSOR_STATUS_BAD_METRICS]++;
			*metrics_success = false;
			break;
		case EXIT_FAILURE:
			ss->status[STRESS_STRESSOR_STATUS_FAILED]++;
			/*
			 *  Stressors should really return EXIT_NOT_SUCCESS
			 *  as EXIT_FAILURE should indicate a core stress-ng
			 *  problem.
			 */
			wexit_status = EXIT_NOT_SUCCESS;
			goto wexit_status_default;
		default:
wexit_status_default:
			pr_err("%s: [%d] terminated with an error, exit status=%d (%s)\n",
				stressor_name, ret, wexit_status,
				stress_exit_status_to_string(wexit_status));
			*success = false;
			do_abort = true;
			break;
		}
		if ((g_opt_flags & OPT_FLAGS_ABORT) && do_abort) {
			stress_continue_set_flag(false);
			wait_flag = false;
			stress_kill_stressors(SIGALRM, true);
		}

		stress_stressor_finished(&stats->pid);
		pr_dbg("%s: [%d] terminated (%s)\n",
			stressor_name, ret,
			stress_exit_status_to_string(wexit_status));
	} else if (ret == -1) {
		/* Somebody interrupted the wait */
		if (errno == EINTR)
			goto redo;
		/* This child did not exist, mark it done anyhow */
		if (errno == ECHILD)
			stress_stressor_finished(&stats->pid);
	}
}

/*
 *  stress_wait_stressors()
 * 	wait for stressor child processes
 */
static void stress_wait_stressors(
	const int32_t ticks_per_sec,
	stress_stressor_t *stressors_list,
	bool *success,
	bool *resource_success,
	bool *metrics_success)
{
	stress_stressor_t *ss;

#if defined(HAVE_SCHED_GETAFFINITY) &&	\
    NEED_GLIBC(2,3,0)
	/*
	 *  On systems that support changing CPU affinity
	 *  we keep on moving processes between processors
	 *  to impact on memory locality (e.g. NUMA) to
	 *  try to thrash the system when in aggressive mode
	 */
	if (g_opt_flags & OPT_FLAGS_AGGRESSIVE)
		stress_wait_aggressive(ticks_per_sec, stressors_list);
#else
	(void)ticks_per_sec;
#endif
	for (ss = stressors_list; ss; ss = ss->next) {
		int32_t j;

		if (ss->ignore.run || ss->ignore.permute)
			continue;

		for (j = 0; j < ss->num_instances; j++) {
			stress_stats_t *const stats = ss->stats[j];
			const pid_t pid = stats->pid;

			if (pid) {
				char munged[64];

				(void)stress_munge_underscore(munged, ss->stressor->name, sizeof(munged));
				stress_wait_pid(ss, pid, munged, stats, success, resource_success, metrics_success);
				stress_clean_dir(munged, pid, (uint32_t)j);
			}
		}
	}
	if (g_opt_flags & OPT_FLAGS_IGNITE_CPU)
		stress_ignite_cpu_stop();
}

/*
 *  stress_handle_terminate()
 *	catch terminating signals
 */
static void MLOCKED_TEXT stress_handle_terminate(int signum)
{
	static char buf[128];
	const int fd = fileno(stderr);
	terminate_signum = signum;
	stress_continue_set_flag(false);

	switch (signum) {
	case SIGILL:
	case SIGSEGV:
	case SIGFPE:
	case SIGBUS:
	case SIGABRT:
		/*
		 *  Critical failure, report and die ASAP
		 */
		(void)snprintf(buf, sizeof(buf), "%s: info:  [%d] stressor terminated with unexpected signal %s\n",
			g_app_name, (int)getpid(), stress_strsignal(signum));
		VOID_RET(ssize_t, write(fd, buf, strlen(buf)));
		stress_kill_stressors(SIGALRM, true);
		_exit(EXIT_SIGNALED);
	default:
		/*
		 *  Kill stressors
		 */
		stress_kill_stressors(SIGALRM, true);
		break;
	}
}

/*
 *  stress_get_nth_stressor()
 *	return nth stressor from list
 */
static stress_stressor_t *stress_get_nth_stressor(const uint32_t n)
{
	stress_stressor_t *ss = stressors_head;
	uint32_t i = 0;

	while (ss && (i < n)) {
		if (!ss->ignore.run)
			i++;
		ss = ss->next;
	}
	return ss;
}

/*
 *  stress_get_num_stressors()
 *	return number of stressors in stressor list
 */
static uint32_t stress_get_num_stressors(void)
{
	uint32_t n = 0;
	stress_stressor_t *ss;

	for (ss = stressors_head; ss; ss = ss->next)
		if (!ss->ignore.run)
			n++;

	return n;
}

/*
 *  stress_stressors_free()
 *	free stressor info from stressor list
 */
static void stress_stressors_free(void)
{
	stress_stressor_t *ss = stressors_head;

	while (ss) {
		stress_stressor_t *next = ss->next;

		free(ss->stats);
		free(ss);
		ss = next;
	}

	stressors_head = NULL;
	stressors_tail = NULL;
}

/*
 *  stress_get_total_num_instances()
 *	deterimine number of runnable stressors from list
 */
static int32_t stress_get_total_num_instances(stress_stressor_t *stressors_list)
{
	int32_t total_num_instances = 0;
	stress_stressor_t *ss;

	for (ss = stressors_list; ss; ss = ss->next)
		total_num_instances += ss->num_instances;

	return total_num_instances;
}

/*
 *  stress_child_atexit(void)
 *	handle unexpected exit() call in child stressor
 */
static void NORETURN stress_child_atexit(void)
{
	_exit(EXIT_BY_SYS_EXIT);
}

/*
 *  stress_metrics_set_const_check()
 *	set metrics with given description a value. If const_description is
 *	true then the description is a literal string and does not need
 *	to be dup'd from the shared memory heap, otherwise it's a stack
 *	based string and needs to be dup'd so it does not go out of scope.
 *
 *	Note that stress_shared_heap_dup_const will dup a string using
 *	special reserved shared heap that all stressors can access. The
 *	returned string must not be written to. It may even be a cached
 *	copy of another dup by another stressor process (to save memory).
 */
void stress_metrics_set_const_check(
	const stress_args_t *args,
	const size_t idx,
	char *description,
	const bool const_description,
	const double value)
{
	stress_metrics_data_t *metrics;

	if (idx >= STRESS_MISC_METRICS_MAX)
		return;
	if (!args)
		return;
	metrics = args->metrics;
	if (!metrics)
		return;

	metrics[idx].description = const_description ?
		description :
		stress_shared_heap_dup_const(description);
	if (metrics[idx].description)
		metrics[idx].value = value;
}

#if defined(HAVE_GETRUSAGE)
/*
 *  stress_getrusage()
 *	accumulate rusgage stats
 */
static void stress_getrusage(const int who, stress_stats_t *stats)
{
	struct rusage usage;

	if (shim_getrusage(who, &usage) == 0) {
		stats->rusage_utime +=
			(double)usage.ru_utime.tv_sec +
			((double)usage.ru_utime.tv_usec) / STRESS_DBL_MICROSECOND;
		stats->rusage_stime +=
			(double)usage.ru_stime.tv_sec +
			((double)usage.ru_stime.tv_usec) / STRESS_DBL_MICROSECOND;
#if defined(HAVE_RUSAGE_RU_MAXRSS)
		if (stats->rusage_maxrss < usage.ru_maxrss)
			stats->rusage_maxrss = usage.ru_maxrss;
#else
		stats->rusage_maxrss = 0;	/* Not available */
#endif
	}
}
#endif

static void stress_get_usage_stats(const int32_t ticks_per_sec, stress_stats_t *stats)
{
#if defined(HAVE_GETRUSAGE)
	(void)ticks_per_sec;

	stats->rusage_utime = 0.0;
	stats->rusage_stime = 0.0;
	stress_getrusage(RUSAGE_SELF, stats);
	stress_getrusage(RUSAGE_CHILDREN, stats);
#else
	struct tms t;

	stats->rusage_utime = 0.0;
	stats->rusage_stime = 0.0;
	(void)shim_memset(&t, 0, sizeof(t));
	if ((ticks_per_sec > 0) && (times(&t) != (clock_t)-1)) {
		stats->rusage_utime =
			(double)(t.tms_utime + t.tms_cutime) / (double)ticks_per_sec;
		stats->rusage_stime =
			(double)(t.tms_stime + t.tms_cstime) / (double)ticks_per_sec;
	}
#endif
	stats->rusage_utime_total += stats->rusage_utime;
	stats->rusage_stime_total += stats->rusage_stime;
}

/*
 *  stress_run_child()
 *	invoke a stressor in a child process
 */
static int MLOCKED_TEXT stress_run_child(
	stress_checksum_t **checksum,
	stress_stats_t *const stats,
	const double fork_time_start,
	const int64_t backoff,
	const int32_t ticks_per_sec,
	const int32_t ionice_class,
	const int32_t ionice_level,
	const int32_t instance,
	const int32_t started_instances,
	const size_t page_size)
{
	pid_t child_pid;
	char name[64];
	int rc = EXIT_SUCCESS;
	bool ok;
	double finish, run_duration;

	sigalarmed = &stats->sigalarmed;
	child_pid = getpid();

	(void)stress_munge_underscore(name, g_stressor_current->stressor->name, sizeof(name));
	stress_set_proc_state(name, STRESS_STATE_START);
	g_shared->instance_count.started++;

	(void)sched_settings_apply(true);
	(void)atexit(stress_child_atexit);
	if (stress_set_handler(name, true) < 0) {
		rc = EXIT_FAILURE;
		stress_block_signals();
		goto child_exit;
	}
	stress_parent_died_alarm();
	stress_process_dumpable(false);
	stress_set_timer_slack();

	if (g_opt_flags & OPT_FLAGS_KSM)
		stress_ksm_memory_merge(1);

	stress_set_proc_state(name, STRESS_STATE_INIT);
	stress_mwc_reseed();
	stress_set_max_limits();
	stress_set_iopriority(ionice_class, ionice_level);
	(void)umask(0077);

	pr_dbg("%s: [%d] started (instance %" PRIu32 " on CPU %u)\n",
		name, (int)child_pid, instance, stress_get_cpu());

	if (g_opt_flags & OPT_FLAGS_INTERRUPTS)
		stress_interrupts_start(stats->interrupts);
#if defined(STRESS_PERF_STATS) &&	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	if (g_opt_flags & OPT_FLAGS_PERF_STATS)
		(void)stress_perf_open(&stats->sp);
#endif
	(void)shim_usleep((useconds_t)(backoff * started_instances));
#if defined(STRESS_PERF_STATS) &&	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	if (g_opt_flags & OPT_FLAGS_PERF_STATS)
		(void)stress_perf_enable(&stats->sp);
#endif
	stress_yield_sleep_ms();
	stats->start = stress_time_now();
	if (g_opt_timeout)
		(void)alarm((unsigned int)g_opt_timeout);
	if (stress_continue_flag() && !(g_opt_flags & OPT_FLAGS_DRY_RUN)) {
		const stress_args_t args = {
			.ci = &stats->ci,
			.name = name,
			.max_ops = g_stressor_current->bogo_ops,
			.instance = (uint32_t)instance,
			.num_instances = (uint32_t)g_stressor_current->num_instances,
			.pid = child_pid,
			.page_size = page_size,
			.time_end = stress_time_now() + (double)g_opt_timeout,
			.mapped = &g_shared->mapped,
			.metrics = stats->metrics,
			.info = g_stressor_current->stressor->info
		};
		stress_set_oom_adjustment(&args, false);
		(void)shim_memset(*checksum, 0, sizeof(**checksum));
		stats->start = stress_time_now();
		rc = g_stressor_current->stressor->info->stressor(&args);
		stress_block_signals();
		(void)alarm(0);
		if (g_opt_flags & OPT_FLAGS_INTERRUPTS) {
			stress_interrupts_stop(stats->interrupts);
			stress_interrupts_check_failure(name, stats->interrupts, instance, &rc);
		}
		pr_fail_check(&rc);
#if defined(SA_SIGINFO) &&	\
    defined(SI_USER)
		/*
		 *  Sanity check if process was killed by
		 *  an external SIGALRM source
		 */
		if (sigalrm_info.triggered && (sigalrm_info.code == SI_USER)) {
			time_t t = sigalrm_info.when.tv_sec;
			const struct tm *tm = localtime(&t);

			if (tm) {
				pr_dbg("%s: terminated by SIGALRM externally at %2.2d:%2.2d:%2.2d.%2.2ld by user %d\n",
					name,
					tm->tm_hour, tm->tm_min, tm->tm_sec,
					(long)sigalrm_info.when.tv_usec / 10000,
					sigalrm_info.uid);
			} else {
				pr_dbg("%s: terminated by SIGALRM externally by user %d\n",
					name, sigalrm_info.uid);
			}
		}
#endif
		stats->completed = true;
		ok = (rc == EXIT_SUCCESS);
		stats->ci.run_ok = ok;
		(*checksum)->data.ci.run_ok = ok;
		/* Ensure reserved padding is zero to not confuse checksum */
		(void)shim_memset((*checksum)->data.pad, 0, sizeof((*checksum)->data.pad));

		stress_set_proc_state(name, STRESS_STATE_STOP);
		/*
		 *  Bogo ops counter should be OK for reading,
		 *  if not then flag up that the counter may
		 *  be untrustyworthy
		 */
		if ((!stats->ci.counter_ready) && (!stats->ci.force_killed)) {
			pr_warn("%s: WARNING: bogo-ops counter in non-ready state, "
				"metrics are untrustworthy (process may have been "
				"terminated prematurely)\n",
				name);
			rc = EXIT_METRICS_UNTRUSTWORTHY;
		}
		(*checksum)->data.ci.counter = args.ci->counter;
		stress_hash_checksum(*checksum);
	}
#if defined(STRESS_PERF_STATS) &&	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	if (g_opt_flags & OPT_FLAGS_PERF_STATS) {
		(void)stress_perf_disable(&stats->sp);
		(void)stress_perf_close(&stats->sp);
	}
#endif
#if defined(STRESS_THERMAL_ZONES)
	if (g_opt_flags & OPT_FLAGS_THERMAL_ZONES)
		(void)stress_tz_get_temperatures(&g_shared->tz_info, &stats->tz);
#endif
	finish = stress_time_now();
	stats->duration = finish - stats->start;
	stats->counter_total += stats->ci.counter;
	stats->duration_total += stats->duration;

	stress_get_usage_stats(ticks_per_sec, stats);
	pr_dbg("%s: [%d] exited (instance %" PRIu32 " on CPU %d)\n",
		name, (int)child_pid, instance, stress_get_cpu());

	/* Allow for some slops of ~0.5 secs */
	run_duration = (finish - fork_time_start) + 0.5;

	/*
	 * Apparently succeeded but terminated early?
	 * Could be a bug, so report a warning
	 */
	if (stats->ci.run_ok &&
	    (g_shared && !g_shared->caught_sigint) &&
	    (run_duration < (double)g_opt_timeout) &&
	    (!(g_stressor_current->bogo_ops && stats->ci.counter >= g_stressor_current->bogo_ops))) {

		pr_warn("%s: WARNING: finished prematurely after just %s\n",
			name, stress_duration_to_str(run_duration, true));
	}
child_exit:
	/*
	 *  We used to free allocations on the heap, but
	 *  the child is going to _exit() soon so it's
	 *  faster to just free the heap objects on _exit()
	 */
	if ((rc != 0) && (g_opt_flags & OPT_FLAGS_ABORT)) {
		stress_continue_set_flag(false);
		wait_flag = false;
		(void)shim_kill(getppid(), SIGALRM);
	}
	stress_set_proc_state(name, STRESS_STATE_EXIT);
	if (terminate_signum)
		rc = EXIT_SIGNALED;
	g_shared->instance_count.exited++;
	g_shared->instance_count.started--;
	if (rc == EXIT_FAILURE)
		g_shared->instance_count.failed++;

	return rc;
}

/*
 *  stress_run()
 *	kick off and run stressors
 */
static void MLOCKED_TEXT stress_run(
	const int32_t ticks_per_sec,
	stress_stressor_t *stressors_list,
	double *duration,
	bool *success,
	bool *resource_success,
	bool *metrics_success,
	stress_checksum_t **checksum)
{
	double time_start, time_finish;
	int32_t started_instances = 0;
	const size_t page_size = stress_get_page_size();
	int64_t backoff = DEFAULT_BACKOFF;
	int32_t ionice_class = UNDEFINED;
	int32_t ionice_level = UNDEFINED;
	bool handler_set = false;

	wait_flag = true;
	time_start = stress_time_now();
	pr_dbg("starting stressors\n");

	(void)stress_get_setting("backoff", &backoff);
	(void)stress_get_setting("ionice-class", &ionice_class);
	(void)stress_get_setting("ionice-level", &ionice_level);

	/*
	 *  Work through the list of stressors to run
	 */
	for (g_stressor_current = stressors_list; g_stressor_current; g_stressor_current = g_stressor_current->next) {
		int32_t j;

		if (g_stressor_current->ignore.run || g_stressor_current->ignore.permute)
			continue;

		/*
		 *  Each stressor has 1 or more instances to run
		 */
		for (j = 0; j < g_stressor_current->num_instances; j++, (*checksum)++) {
			double fork_time_start;
			pid_t pid;
			int rc;
			stress_stats_t *const stats = g_stressor_current->stats[j];

			if (g_opt_timeout && (stress_time_now() - time_start > (double)g_opt_timeout))
				goto abort;

			stats->pid = -1;
			stats->ci.counter_ready = true;
			stats->ci.counter = 0;
			stats->checksum = *checksum;
again:
			if (!stress_continue_flag())
				break;
			fork_time_start = stress_time_now();
			pid = fork();
			switch (pid) {
			case -1:
				if (errno == EAGAIN) {
					(void)shim_usleep(100000);
					goto again;
				}
				pr_err("Cannot fork: errno=%d (%s)\n",
					errno, strerror(errno));
				stress_kill_stressors(SIGALRM, false);
				goto wait_for_stressors;
			case 0:
				/* Child */
				rc = stress_run_child(checksum,
						stats, fork_time_start,
						backoff, ticks_per_sec,
						ionice_class, ionice_level,
						j, started_instances,
						page_size);
				_exit(rc);
			default:
				if (pid > -1) {
					stats->pid = pid;
					stats->signalled = false;
					started_instances++;
					stress_ftrace_add_pid(pid);
				}

				/* Forced early abort during startup? */
				if (!stress_continue_flag()) {
					pr_dbg("abort signal during startup, cleaning up\n");
					stress_kill_stressors(SIGALRM, true);
					goto wait_for_stressors;
				}
				break;
			}
		}
	}
	if (!handler_set) {
		(void)stress_set_handler("stress-ng", false);
		handler_set = true;
	}
abort:
	pr_dbg("%d stressor%s started\n", started_instances,
		 started_instances == 1 ? "" : "s");

wait_for_stressors:
	if (!handler_set)
		(void)stress_set_handler("stress-ng", false);
	if (g_opt_flags & OPT_FLAGS_IGNITE_CPU)
		stress_ignite_cpu_start();
#if STRESS_FORCE_TIMEOUT_ALL
	if (g_opt_timeout)
		(void)alarm((unsigned int)g_opt_timeout);
#endif
	stress_wait_stressors(ticks_per_sec, stressors_list, success, resource_success, metrics_success);
	time_finish = stress_time_now();

	*duration += time_finish - time_start;
}

/*
 *  stress_show_stressors()
 *	show names of stressors that are going to be run
 */
static int stress_show_stressors(void)
{
	char *newstr, *str = NULL;
	ssize_t len = 0;
	char buffer[64];
	bool previous = false;
	stress_stressor_t *ss;

	for (ss = stressors_head; ss; ss = ss->next) {
		const int32_t n = ss->num_instances;

		if (ss->ignore.run)
			continue;

		if (n) {
			char munged[64];
			ssize_t buffer_len;

			(void)stress_munge_underscore(munged, ss->stressor->name, sizeof(munged));
			buffer_len = snprintf(buffer, sizeof(buffer),
					"%s %" PRId32 " %s", previous ? "," : "", n, munged);
			previous = true;
			if (buffer_len >= 0) {
				newstr = realloc(str, (size_t)(len + buffer_len + 1));
				if (!newstr) {
					pr_err("Cannot allocate temporary buffer\n");
					free(str);
					return -1;
				}
				str = newstr;
				(void)shim_strlcpy(str + len, buffer, (size_t)(buffer_len + 1));
			}
			len += buffer_len;
		}
	}
	pr_inf("dispatching hogs:%s\n", str ? str : "");
	free(str);

	return 0;
}

/*
 *  stress_exit_status_type()
 *	report exit status of all instances of a given status type
 */
static void stress_exit_status_type(const char *name, const size_t type)
{
	stress_stressor_t *ss;

	char *str;
	size_t str_len = 1;
	uint32_t n = 0;

	str = malloc(1);
	if (!str)
		return;
	*str = '\0';

	for (ss = stressors_head; ss; ss = ss->next) {
		uint32_t count = ss->status[type];

		if ((ss->ignore.run) && (type == STRESS_STRESSOR_STATUS_SKIPPED)) {
			count = ss->num_instances;
		}
		if (count > 0) {
			char buf[80], munged[64];
			char *new_str;
			size_t buf_len;

			(void)stress_munge_underscore(munged, ss->stressor->name, sizeof(munged));
			(void)snprintf(buf, sizeof(buf), " %s (%" PRIu32")", munged, count);
			buf_len = strlen(buf);
			new_str = realloc(str, str_len + buf_len);
			if (!new_str) {
				free(str);
				return;
			}
			str = new_str;
			str_len += buf_len;
			(void)shim_strlcat(str, buf, str_len);
			n += count;
		}
	}
	if (n) {
		pr_inf("%s: %" PRIu32 ":%s\n", name, n, str);
	} else  {
		pr_inf("%s: 0\n", name);
	}
	free(str);
}

/*
 *  stress_exit_status_summary()
 *	provide summary of exit status of all instances
 */
static void stress_exit_status_summary(void)
{
	stress_exit_status_type("skipped", STRESS_STRESSOR_STATUS_SKIPPED);
	stress_exit_status_type("passed", STRESS_STRESSOR_STATUS_PASSED);
	stress_exit_status_type("failed", STRESS_STRESSOR_STATUS_FAILED);
	stress_exit_status_type("metrics untrustworthy", STRESS_STRESSOR_STATUS_BAD_METRICS);
}

/*
 *  stress_metrics_check()
 *	as per ELISA request, sanity check bogo ops and run flag
 *	to see if corruption occurred and print failure messages
 *	and set *success to false if hash and data is dubious.
 */
static void stress_metrics_check(bool *success)
{
	stress_stressor_t *ss;
	bool ok = true;
	uint64_t counter_check = 0;
	double min_run_time = DBL_MAX;

	for (ss = stressors_head; ss; ss = ss->next) {
		int32_t j;

		if (ss->ignore.run)
			continue;

		for (j = 0; j < ss->num_instances; j++) {
			const stress_stats_t *const stats = ss->stats[j];
			const stress_checksum_t *checksum = stats->checksum;
			stress_checksum_t stats_checksum;

			if (!stats->completed)
				continue;

			counter_check |= stats->ci.counter;
			if (stats->duration < min_run_time)
				min_run_time = stats->duration;

			if (checksum == NULL) {
				pr_fail("%s instance %d unexpected null checksum data\n",
					ss->stressor->name, j);
				ok = false;
				continue;
			}

			(void)shim_memset(&stats_checksum, 0, sizeof(stats_checksum));
			stats_checksum.data.ci.counter = stats->ci.counter;
			stats_checksum.data.ci.run_ok = stats->ci.run_ok;
			stress_hash_checksum(&stats_checksum);

			if (stats->ci.counter != checksum->data.ci.counter) {
				pr_fail("%s instance %d corrupted bogo-ops counter, %" PRIu64 " vs %" PRIu64 "\n",
					ss->stressor->name, j,
					stats->ci.counter, checksum->data.ci.counter);
				ok = false;
			}
			if (stats->ci.run_ok != checksum->data.ci.run_ok) {
				pr_fail("%s instance %d corrupted run flag, %d vs %d\n",
					ss->stressor->name, j,
					stats->ci.run_ok, checksum->data.ci.run_ok);
				ok = false;
			}
			if (stats_checksum.hash != checksum->hash) {
				pr_fail("%s instance %d hash error in bogo-ops counter and run flag, %" PRIu32 " vs %" PRIu32 "\n",
					ss->stressor->name, j,
					stats_checksum.hash, checksum->hash);
				ok = false;
			}
		}
	}

	/*
	 *  Bogo ops counter should be not zero for the majority of
	 *  stressors after 30 seconds of run time
	 */
	if (!counter_check && (min_run_time > 30.0))
		pr_warn("metrics-check: all bogo-op counters are zero, data may be incorrect\n");

	if (ok) {
		pr_dbg("metrics-check: all stressor metrics validated and sane\n");
	} else {
		pr_fail("metrics-check: stressor metrics corrupted, data is compromised\n");
		*success = false;
	}
}

static char *stess_description_yamlify(const char *description)
{
	static char yamlified[40];
	char *dst;
	const char *src, *end = yamlified + sizeof(yamlified);

	for (dst = yamlified, src = description; *src; src++) {
		register int ch = (int)*src;

		if (isalpha(ch)) {
			*(dst++) = (char)tolower(ch);
		} else if (isdigit(ch)) {
			*(dst++) = (char)ch;
		} else if (ch == ' ') {
			*(dst++) = '-';
		}
		if (dst >= end - 1)
			break;
	}
	*dst = '\0';

	return yamlified;
}

/*
 *  stress_metrics_dump()
 *	output metrics
 */
static void stress_metrics_dump(FILE *yaml)
{
	stress_stressor_t *ss;
	bool misc_metrics = false;

	pr_block_begin();
	if (g_opt_flags & OPT_FLAGS_METRICS_BRIEF) {
		pr_metrics("%-13s %9.9s %9.9s %9.9s %9.9s %12s %14s\n",
			   "stressor", "bogo ops", "real time", "usr time",
			   "sys time", "bogo ops/s", "bogo ops/s");
		pr_metrics("%-13s %9.9s %9.9s %9.9s %9.9s %12s %14s\n",
			   "", "", "(secs) ", "(secs) ", "(secs) ", "(real time)",
			   "(usr+sys time)");
	} else {
		pr_metrics("%-13s %9.9s %9.9s %9.9s %9.9s %12s %14s %12.12s %13.13s\n",
			   "stressor", "bogo ops", "real time", "usr time",
			   "sys time", "bogo ops/s", "bogo ops/s", "CPU used per",
			   "RSS Max");
		pr_metrics("%-13s %9.9s %9.9s %9.9s %9.9s %12s %14s %12.12s %13.13s\n",
			   "", "", "(secs) ", "(secs) ", "(secs) ", "(real time)",
			   "(usr+sys time)","instance (%)", "(KB)");
	}
	pr_yaml(yaml, "metrics:\n");

	for (ss = stressors_head; ss; ss = ss->next) {
		uint64_t c_total = 0;
		double   r_total = 0.0, u_total = 0.0, s_total = 0.0;
		long int maxrss = 0;
		int32_t  j;
		size_t i;
		char munged[64];
		double u_time, s_time, t_time, bogo_rate_r_time, bogo_rate, cpu_usage;
		bool run_ok = false;

		if (ss->ignore.run || ss->ignore.permute)
			continue;
		if (!ss->stats)
			continue;

		(void)stress_munge_underscore(munged, ss->stressor->name, sizeof(munged));

		for (j = 0; j < ss->num_instances; j++)
			ss->completed_instances = 0;

		for (j = 0; j < ss->num_instances; j++) {
			const stress_stats_t *const stats = ss->stats[j];

			if (stats->completed)
				ss->completed_instances++;

			run_ok  |= stats->ci.run_ok;
			c_total += stats->counter_total;
			u_total += stats->rusage_utime_total;
			s_total += stats->rusage_stime_total;
#if defined(HAVE_RUSAGE_RU_MAXRSS)
			if (maxrss < stats->rusage_maxrss)
				maxrss = stats->rusage_maxrss;
#endif
			r_total += stats->duration_total;
		}
		/* Real time in terms of average wall clock time of all procs */
		r_total = ss->completed_instances ?
			r_total / (double)ss->completed_instances : 0.0;

		if ((g_opt_flags & OPT_FLAGS_METRICS_BRIEF) &&
		    (c_total == 0) && (!run_ok))
			continue;

		u_time = u_total;
		s_time = s_total;
		t_time = u_time + s_time;

		/* Total usr + sys time of all procs */
		bogo_rate_r_time = (r_total > 0.0) ? (double)c_total / r_total : 0.0;
		{
			double us_total = u_time + s_time;

			bogo_rate = (us_total > 0.0) ? (double)c_total / us_total : 0.0;
		}

		cpu_usage = (r_total > 0) ? 100.0 * t_time / r_total : 0.0;
		cpu_usage = ss->completed_instances ? cpu_usage / ss->completed_instances : 0.0;

		if (g_opt_flags & OPT_FLAGS_METRICS_BRIEF) {
			if (g_opt_flags & OPT_FLAGS_SN) {
				pr_metrics("%-13s %9" PRIu64 " %9.3e %9.3e %9.3e %12.5e %14.5e\n",
					munged,		/* stress test name */
					c_total,	/* op count */
					r_total,	/* average real (wall) clock time */
					u_time, 	/* actual user time */
					s_time,		/* actual system time */
					bogo_rate_r_time, /* bogo ops on wall clock time */
					bogo_rate);	/* bogo ops per second */
			} else {
				pr_metrics("%-13s %9" PRIu64 " %9.2f %9.2f %9.2f %12.2f %14.2f\n",
					munged,		/* stress test name */
					c_total,	/* op count */
					r_total,	/* average real (wall) clock time */
					u_time, 	/* actual user time */
					s_time,		/* actual system time */
					bogo_rate_r_time, /* bogo ops on wall clock time */
					bogo_rate);	/* bogo ops per second */
			}
		} else {
			/* extended metrics */
			if (g_opt_flags & OPT_FLAGS_SN) {
				pr_metrics("%-13s %9" PRIu64 " %9.3e %9.3e %9.3e %12.5e %14.5e %15.4e %13ld\n",
					munged,		/* stress test name */
					c_total,	/* op count */
					r_total,	/* average real (wall) clock time */
					u_time, 	/* actual user time */
					s_time,		/* actual system time */
					bogo_rate_r_time, /* bogo ops on wall clock time */
					bogo_rate,	/* bogo ops per second */
					cpu_usage,	/* % cpu usage */
					maxrss);	/* maximum RSS in KB */
			} else {
				pr_metrics("%-13s %9" PRIu64 " %9.2f %9.2f %9.2f %12.2f %14.2f %12.2f %13ld\n",
					munged,		/* stress test name */
					c_total,	/* op count */
					r_total,	/* average real (wall) clock time */
					u_time, 	/* actual user time */
					s_time,		/* actual system time */
					bogo_rate_r_time, /* bogo ops on wall clock time */
					bogo_rate,	/* bogo ops per second */
					cpu_usage,	/* % cpu usage */
					maxrss);	/* maximum RSS in KB */
			}
		}

		if (g_opt_flags & OPT_FLAGS_SN) {
			pr_yaml(yaml, "    - stressor: %s\n", munged);
			pr_yaml(yaml, "      bogo-ops: %" PRIu64 "\n", c_total);
			pr_yaml(yaml, "      bogo-ops-per-second-usr-sys-time: %e\n", bogo_rate);
			pr_yaml(yaml, "      bogo-ops-per-second-real-time: %e\n", bogo_rate_r_time);
			pr_yaml(yaml, "      wall-clock-time: %e\n", r_total);
			pr_yaml(yaml, "      user-time: %e\n", u_time);
			pr_yaml(yaml, "      system-time: %e\n", s_time);
			pr_yaml(yaml, "      cpu-usage-per-instance: %e\n", cpu_usage);
			pr_yaml(yaml, "      max-rss: %ld\n", maxrss);
		} else {
			pr_yaml(yaml, "    - stressor: %s\n", munged);
			pr_yaml(yaml, "      bogo-ops: %" PRIu64 "\n", c_total);
			pr_yaml(yaml, "      bogo-ops-per-second-usr-sys-time: %f\n", bogo_rate);
			pr_yaml(yaml, "      bogo-ops-per-second-real-time: %f\n", bogo_rate_r_time);
			pr_yaml(yaml, "      wall-clock-time: %f\n", r_total);
			pr_yaml(yaml, "      user-time: %f\n", u_time);
			pr_yaml(yaml, "      system-time: %f\n", s_time);
			pr_yaml(yaml, "      cpu-usage-per-instance: %f\n", cpu_usage);
			pr_yaml(yaml, "      max-rss: %ld\n", maxrss);
		}

		for (i = 0; i < SIZEOF_ARRAY(ss->stats[0]->metrics); i++) {
			const char *description = ss->stats[0]->metrics[i].description;

			if (description) {
				double metric, total = 0.0;

				misc_metrics = true;
				for (j = 0; j < ss->num_instances; j++) {
					const stress_stats_t *const stats = ss->stats[j];

					total += stats->metrics[i].value;
				}
				metric = ss->completed_instances ? total / ss->completed_instances : 0.0;
				if (g_opt_flags & OPT_FLAGS_SN) {
					pr_yaml(yaml, "      %s: %e\n", stess_description_yamlify(description), metric);
				} else {
					pr_yaml(yaml, "      %s: %f\n", stess_description_yamlify(description), metric);
				}
			}
		}
		pr_yaml(yaml, "\n");
	}

	if (misc_metrics && !(g_opt_flags & OPT_FLAGS_METRICS_BRIEF)) {
		pr_metrics("miscellaneous metrics:\n");
		for (ss = stressors_head; ss; ss = ss->next) {
			size_t i;
			int32_t j;
			char munged[64];

			if (ss->ignore.run)
				continue;
			if (!ss->stats)
				continue;

			(void)stress_munge_underscore(munged, ss->stressor->name, sizeof(munged));

			for (i = 0; i < SIZEOF_ARRAY(ss->stats[0]->metrics); i++) {
				const char *description = ss->stats[0]->metrics[i].description;

				if (description) {
					int64_t exponent = 0;
					double geomean, mantissa = 1.0;
					double n = 0.0;

					for (j = 0; j < ss->num_instances; j++) {
						int e;
						const stress_stats_t *const stats = ss->stats[j];

						if (stats->metrics[i].value > 0.0) {
							const double f = frexp(stats->metrics[i].value, &e);

							mantissa *= f;
							exponent += e;
							n += 1.0;
						}
					}
					if (n > 0.0) {
						const double inverse_n = 1.0 / (double)n;

						geomean = pow(mantissa, inverse_n) * pow(2.0, (double)exponent * inverse_n);
					} else {
						geomean = 0.0;
					}
					if (g_opt_flags & OPT_FLAGS_SN) {
						pr_metrics("%-13s %13.2e %s (geometric mean of %" PRIu32 " instances)\n",
							   munged, geomean, description, ss->completed_instances);
					} else {
						pr_metrics("%-13s %13.2f %s (geometric mean of %" PRIu32 " instances)\n",
							   munged, geomean, description, ss->completed_instances);
					}
				}
			}
		}
	}
	pr_block_end();
}

/*
 *  stress_times_dump()
 *	output the run times
 */
static void stress_times_dump(
	FILE *yaml,
	const int32_t ticks_per_sec,
	const double duration)
{
	struct tms buf;
	double total_cpu_time = stress_get_processors_configured() * duration;
	double u_time, s_time, t_time, u_pc, s_pc, t_pc;
	double min1, min5, min15;
	int rc;

	if (!(g_opt_flags & OPT_FLAGS_TIMES))
		return;

	if (times(&buf) == (clock_t)-1) {
		pr_err("cannot get run time information: errno=%d (%s)\n",
			errno, strerror(errno));
		return;
	}
	rc = stress_get_load_avg(&min1, &min5, &min15);

	u_time = (double)buf.tms_cutime / (double)ticks_per_sec;
	s_time = (double)buf.tms_cstime / (double)ticks_per_sec;
	t_time = ((double)buf.tms_cutime + (double)buf.tms_cstime) / (double)ticks_per_sec;
	u_pc = (total_cpu_time > 0.0) ? 100.0 * u_time / total_cpu_time : 0.0;
	s_pc = (total_cpu_time > 0.0) ? 100.0 * s_time / total_cpu_time : 0.0;
	t_pc = (total_cpu_time > 0.0) ? 100.0 * t_time / total_cpu_time : 0.0;

	pr_inf("for a %.2fs run time:\n", duration);
	pr_inf("  %8.2fs available CPU time\n",
		total_cpu_time);
	pr_inf("  %8.2fs user time   (%6.2f%%)\n", u_time, u_pc);
	pr_inf("  %8.2fs system time (%6.2f%%)\n", s_time, s_pc);
	pr_inf("  %8.2fs total time  (%6.2f%%)\n", t_time, t_pc);
	if (!rc) {
		pr_inf("load average: %.2f %.2f %.2f\n",
			min1, min5, min15);
	}

	pr_yaml(yaml, "times:\n");
	pr_yaml(yaml, "      run-time: %f\n", duration);
	pr_yaml(yaml, "      available-cpu-time: %f\n", total_cpu_time);
	pr_yaml(yaml, "      user-time: %f\n", u_time);
	pr_yaml(yaml, "      system-time: %f\n", s_time);
	pr_yaml(yaml, "      total-time: %f\n", t_time);
	pr_yaml(yaml, "      user-time-percent: %f\n", u_pc);
	pr_yaml(yaml, "      system-time-percent: %f\n", s_pc);
	pr_yaml(yaml, "      total-time-percent: %f\n", t_pc);
	if (!rc) {
		pr_yaml(yaml, "      load-average-1-minute: %f\n", min1);
		pr_yaml(yaml, "      load-average-5-minute: %f\n", min5);
		pr_yaml(yaml, "      load-average-15-minute: %f\n", min15);
	}
}

/*
 *  stress_log_args()
 *	dump to syslog argv[]
 */
static void stress_log_args(int argc, char **argv)
{
	size_t i, len, buflen, *arglen;
	char *buf;
	const char *user = shim_getlogin();
	const uid_t uid = getuid();

	arglen = calloc((size_t)argc, sizeof(*arglen));
	if (!arglen)
		return;

	for (buflen = 0, i = 0; i < (size_t)argc; i++) {
		arglen[i] = strlen(argv[i]);
		buflen += arglen[i] + 1;
	}

	buf = calloc(buflen, sizeof(*buf));
	if (!buf) {
		free(arglen);
		return;
	}

	for (len = 0, i = 0; i < (size_t)argc; i++) {
		if (i) {
			(void)shim_strlcat(buf + len, " ", buflen - len);
			len++;
		}
		(void)shim_strlcat(buf + len, argv[i], buflen - len);
		len += arglen[i];
	}
	if (user) {
		shim_syslog(LOG_INFO, "invoked with '%s' by user %d '%s'\n", buf, uid, user);
		pr_dbg("invoked with '%s' by user %d '%s'\n", buf, uid, user);
	} else {
		shim_syslog(LOG_INFO, "invoked with '%s' by user %d\n", buf, uid);
		pr_dbg("invoked with '%s' by user %d\n", buf, uid);
	}
	free(buf);
	free(arglen);
}

/*
 *  stress_log_system_mem_info()
 *	dump system memory info
 */
void stress_log_system_mem_info(void)
{
#if defined(HAVE_SYS_SYSINFO_H) && \
    defined(HAVE_SYSINFO) && \
    defined(HAVE_SYSLOG_H)
	struct sysinfo info;

	(void)shim_memset(&info, 0, sizeof(info));
	if (sysinfo(&info) == 0) {
		shim_syslog(LOG_INFO, "memory (MB): total %.2f, "
			"free %.2f, "
			"shared %.2f, "
			"buffer %.2f, "
			"swap %.2f, "
			"free swap %.2f\n",
			(double)(info.totalram * info.mem_unit) / MB,
			(double)(info.freeram * info.mem_unit) / MB,
			(double)(info.sharedram * info.mem_unit) / MB,
			(double)(info.bufferram * info.mem_unit) / MB,
			(double)(info.totalswap * info.mem_unit) / MB,
			(double)(info.freeswap * info.mem_unit) / MB);
	}
#endif
}

/*
 *  stress_log_system_info()
 *	dump system info
 */
static void stress_log_system_info(void)
{
#if defined(HAVE_UNAME) && 		\
    defined(HAVE_SYS_UTSNAME_H) &&	\
    defined(HAVE_SYSLOG_H)
	struct utsname buf;

	if (uname(&buf) >= 0) {
		shim_syslog(LOG_INFO, "system: '%s' %s %s %s %s\n",
			buf.nodename,
			buf.sysname,
			buf.release,
			buf.version,
			buf.machine);
	}
#endif
}

static void *stress_map_page(int prot, char *prot_str, size_t page_size)
{
	void *ptr;

	ptr = mmap(NULL, page_size, prot,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (ptr == MAP_FAILED) {
		pr_err("cannot mmap %s shared page, errno=%d (%s)\n",
			prot_str, errno, strerror(errno));
	}
	return ptr;
}

/*
 *  stress_shared_map()
 *	mmap shared region, with an extra page at the end
 *	that is marked read-only to stop accidental smashing
 *	from a run-away stack expansion
 */
static inline void stress_shared_map(const int32_t num_procs)
{
	const size_t page_size = stress_get_page_size();
	size_t len = sizeof(stress_shared_t) +
		     (sizeof(stress_stats_t) * (size_t)num_procs);
	size_t sz = (len + (page_size << 1)) & ~(page_size - 1);
#if defined(HAVE_MPROTECT)
	void *last_page;
#endif

	g_shared = (stress_shared_t *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANON, -1, 0);
	if (g_shared == MAP_FAILED) {
		pr_err("cannot mmap to shared memory region, errno=%d (%s)\n",
			errno, strerror(errno));
		stress_stressors_free();
		exit(EXIT_FAILURE);
	}

	/* Paraniod */
	(void)shim_memset(g_shared, 0, sz);
	g_shared->length = sz;
	g_shared->instance_count.started = 0;
	g_shared->instance_count.exited = 0;
	g_shared->instance_count.reaped = 0;
	g_shared->instance_count.failed = 0;
	g_shared->instance_count.alarmed = 0;
	g_shared->time_started = stress_time_now();

	/*
	 * libc on some systems warn that vfork is deprecated,
	 * we know this, force warnings off where possible
	 */
STRESS_PRAGMA_PUSH
STRESS_PRAGMA_WARN_OFF
	g_shared->vfork = vfork;
STRESS_PRAGMA_POP

#if defined(HAVE_MPROTECT)
	last_page = ((uint8_t *)g_shared) + sz - page_size;

	/* Make last page trigger a segfault if it is accessed */
	(void)mprotect(last_page, page_size, PROT_NONE);
#elif defined(HAVE_MREMAP) &&	\
      defined(MAP_FIXED)
	{
		void *new_last_page;

		/* Try to remap last page with PROT_NONE */
		(void)munmap(last_page, page_size);
		new_last_page = mmap(last_page, page_size, PROT_NONE,
			MAP_SHARED | MAP_ANON | MAP_FIXED, -1, 0);

		/* Failed, retry read-only */
		if (new_last_page == MAP_FAILED)
			new_last_page = mmap(last_page, page_size, PROT_READ,
				MAP_SHARED | MAP_ANON | MAP_FIXED, -1, 0);
		/* Can't remap, bump length down a page */
		if (new_last_page == MAP_FAILED)
			g_shared->length -= sz;
	}
#endif

	/*
	 *  copy of checksums and run data in a different shared
	 *  memory segment so that we can sanity check these for
	 *  any form of corruption
	 */
	len = sizeof(stress_checksum_t) * (size_t)num_procs;
	sz = (len + page_size) & ~(page_size - 1);
	g_shared->checksum.checksums = (stress_checksum_t *)mmap(NULL, sz,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (g_shared->checksum.checksums == MAP_FAILED) {
		pr_err("cannot mmap checksums, errno=%d (%s)\n",
			errno, strerror(errno));
		goto err_unmap_shared;
	}
	(void)shim_memset(g_shared->checksum.checksums, 0, sz);
	g_shared->checksum.length = sz;

	/*
	 *  mmap some pages for testing invalid arguments in
	 *  various stressors, get the allocations done early
	 *  to avoid later mmap failures on stressor child
	 *  processes
	 */
	g_shared->mapped.page_none = stress_map_page(PROT_NONE, "PROT_NONE", page_size);
	if (g_shared->mapped.page_none == MAP_FAILED)
		goto err_unmap_checksums;
	g_shared->mapped.page_ro = stress_map_page(PROT_READ, "PROT_READ", page_size);
	if (g_shared->mapped.page_ro == MAP_FAILED)
		goto err_unmap_page_none;
	g_shared->mapped.page_wo = stress_map_page(PROT_READ, "PROT_WRITE", page_size);
	if (g_shared->mapped.page_wo == MAP_FAILED)
		goto err_unmap_page_ro;
	return;

err_unmap_page_ro:
	(void)munmap((void *)g_shared->mapped.page_ro, page_size);
err_unmap_page_none:
	(void)munmap((void *)g_shared->mapped.page_none, page_size);
err_unmap_checksums:
	(void)munmap((void *)g_shared->checksum.checksums, g_shared->checksum.length);
err_unmap_shared:
	(void)munmap((void *)g_shared, g_shared->length);
	stress_stressors_free();
	exit(EXIT_FAILURE);

}

/*
 *  stress_shared_unmap()
 *	unmap shared region
 */
void stress_shared_unmap(void)
{
	const size_t page_size = stress_get_page_size();

	(void)munmap((void *)g_shared->mapped.page_wo, page_size);
	(void)munmap((void *)g_shared->mapped.page_ro, page_size);
	(void)munmap((void *)g_shared->mapped.page_none, page_size);
	(void)munmap((void *)g_shared->checksum.checksums, g_shared->checksum.length);
	(void)munmap((void *)g_shared, g_shared->length);
}

/*
 *  stress_exclude_unsupported()
 *	tag stressor proc count to be excluded
 */
static inline void stress_exclude_unsupported(bool *unsupported)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		if (stressors[i].info && stressors[i].info->supported) {
			stress_stressor_t *ss;
			unsigned int id = stressors[i].id;

			for (ss = stressors_head; ss; ss = ss->next) {
				if (ss->ignore.run)
					continue;
				if ((ss->stressor->id == id) &&
				    ss->num_instances &&
				    (stressors[i].info->supported(stressors[i].name) < 0)) {
					stress_ignore_stressor(ss, STRESS_STRESSOR_UNSUPPORTED);
					*unsupported = true;
				}
			}
		}
	}
}

/*
 *  stress_set_proc_limits()
 *	set maximum number of processes for specific stressors
 */
static void stress_set_proc_limits(void)
{
#if defined(RLIMIT_NPROC)
	stress_stressor_t *ss;
	struct rlimit limit;

	if (getrlimit(RLIMIT_NPROC, &limit) < 0)
		return;

	for (ss = stressors_head; ss; ss = ss->next) {
		size_t i;

		if (ss->ignore.run)
			continue;

		i = stressor_find_by_id(ss->stressor->id);
		if ((i < SIZEOF_ARRAY(stressors)) &&
		    stressors[i].info &&
		    stressors[i].info->set_limit &&
		    ss->num_instances) {
			const uint64_t max = (uint64_t)limit.rlim_cur / (uint64_t)ss->num_instances;
			stressors[i].info->set_limit(max);
		}
	}
#endif
}

static void stress_append_stressor(stress_stressor_t *ss)
{
	ss->prev = NULL;
	ss->next = NULL;

	/* Add to end of procs list */
	if (stressors_tail)
		stressors_tail->next = ss;
	else
		stressors_head = ss;
	ss->prev = stressors_tail;
	stressors_tail = ss;
}

/*
 *  stress_find_proc_info()
 *	find proc info that is associated with a specific
 *	stressor.  If it does not exist, create a new one
 *	and return that. Terminate if out of memory.
 */
static stress_stressor_t *stress_find_proc_info(const stress_t *stressor)
{
	stress_stressor_t *ss;

#if 0
	/* Scan backwards in time to find last matching stressor */
	for (ss = stressors_tail; ss; ss = ss->prev) {
		if (ss->stressor == stressor)
			return ss;
	}
#endif
	ss = calloc(1, sizeof(*ss));
	if (!ss) {
		(void)fprintf(stderr, "Cannot allocate stressor state info\n");
		exit(EXIT_FAILURE);
	}
	ss->stressor = stressor;
	ss->ignore.run = STRESS_STRESSOR_NOT_IGNORED;
	stress_append_stressor(ss);

	return ss;
}

/*
 *  stress_stressors_init()
 *	initialize any stressors that will be used
 */
static void stress_stressors_init(void)
{
	stress_stressor_t *ss;

	for (ss = stressors_head; ss; ss = ss->next) {
		size_t i;

		if (ss->ignore.run)
			continue;

		i = stressor_find_by_id(ss->stressor->id);
		if ((i < SIZEOF_ARRAY(stressors)) &&
		    stressors[i].info &&
		    stressors[i].info->init) {
			stressors[i].info->init();
		}
	}
}

/*
 *  stress_stressors_deinit()
 *	de-initialize any stressors that will be used
 */
static void stress_stressors_deinit(void)
{
	stress_stressor_t *ss;

	for (ss = stressors_head; ss; ss = ss->next) {
		size_t i;

		if (ss->ignore.run)
			continue;

		for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
			if (stressors[i].info &&
			    stressors[i].info->deinit &&
			    stressors[i].id == ss->stressor->id)
				stressors[i].info->deinit();
		}
	}
}


/*
 *  stessor_set_defaults()
 *	set up stressor default settings that can be overridden
 *	by user later on
 */
static inline void stressor_set_defaults(void)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		if (stressors[i].info && stressors[i].info->set_default)
			stressors[i].info->set_default();
	}
}

/*
 *  stress_exclude_pathological()
 *	Disable pathological stressors if user has not explicitly
 *	request them to be used. Let's play safe.
 */
static inline void stress_exclude_pathological(void)
{
	if (!(g_opt_flags & OPT_FLAGS_PATHOLOGICAL)) {
		stress_stressor_t *ss = stressors_head;

		while (ss) {
			stress_stressor_t *next = ss->next;

			if ((!ss->ignore.run) && (ss->stressor->info->class & CLASS_PATHOLOGICAL)) {
				if (ss->num_instances > 0) {
					char munged[64];

					(void)stress_munge_underscore(munged, ss->stressor->name, sizeof(munged));
					pr_inf("disabled '%s' as it "
						"may hang or reboot the machine "
						"(enable it with the "
						"--pathological option)\n", munged);
				}
				stress_ignore_stressor(ss, STRESS_STRESSOR_EXCLUDED);
			}
			ss = next;
		}
	}
}

/*
 *  stress_setup_stats_buffers()
 *	setup the stats data from the shared memory
 */
static inline void stress_setup_stats_buffers(void)
{
	stress_stressor_t *ss;
	stress_stats_t *stats = g_shared->stats;

	for (ss = stressors_head; ss; ss = ss->next) {
		int32_t i;

		if (ss->ignore.run)
			continue;

		for (i = 0; i < ss->num_instances; i++, stats++) {
			size_t j;

			ss->stats[i] = stats;
			for (j = 0; j < SIZEOF_ARRAY(stats->metrics); j++) {
				stats->metrics[j].value = -1.0;
				stats->metrics[j].description = NULL;
			}
		}
	}
}

/*
 *  stress_set_random_stressors()
 *	select stressors at random
 */
static inline void stress_set_random_stressors(void)
{
	int32_t opt_random = 0;

	(void)stress_get_setting("random", &opt_random);

	if (g_opt_flags & OPT_FLAGS_RANDOM) {
		int32_t n = opt_random;
		const uint32_t n_procs = stress_get_num_stressors();

		if (g_opt_flags & OPT_FLAGS_SET) {
			(void)fprintf(stderr, "Cannot specify random "
				"option with other stress processes "
				"selected\n");
			exit(EXIT_FAILURE);
		}

		if (!n_procs) {
			(void)fprintf(stderr,
				"No stressors are available, unable to continue\n");
			exit(EXIT_FAILURE);
		}

		/* create n randomly chosen stressors */
		while (n > 0) {
			const uint32_t i = stress_mwc32modn(n_procs);
			stress_stressor_t *ss = stress_get_nth_stressor(i);

			if (!ss)
				continue;

			ss->num_instances++;
			n--;
		}
	}
}

static void stress_with(const int32_t instances)
{
	char *opt_with = NULL, *str, *token;

	(void)stress_get_setting("with", &opt_with);

	for (str = opt_with; (token = strtok(str, ",")) != NULL; str = NULL) {
		stress_stressor_t *ss;
		const size_t i = stressor_find_by_name(token);

		if (i >= SIZEOF_ARRAY(stressors)) {
			(void)fprintf(stderr, "Unknown stressor: '%s', "
				"invalid --with option\n", token);
			exit(EXIT_FAILURE);
		}
		ss = stress_find_proc_info(&stressors[i]);
		if (!ss) {
			(void)fprintf(stderr, "Cannot allocate stressor state info\n");
			exit(EXIT_FAILURE);
		}
		ss->num_instances = instances;
	}
	return;
}

/*
 *  stress_enable_all_stressors()
 *	enable all the stressors
 */
static void stress_enable_all_stressors(const int32_t instances)
{
	size_t i;

	if (g_opt_flags & OPT_FLAGS_WITH) {
		stress_with(instances);
		return;
	}

	/* Don't enable all if some stressors are set */
	if (g_opt_flags & OPT_FLAGS_SET)
		return;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		stress_stressor_t *ss = stress_find_proc_info(&stressors[i]);

		if (!ss) {
			(void)fprintf(stderr, "Cannot allocate stressor state info\n");
			exit(EXIT_FAILURE);
		}
		ss->num_instances = instances;
	}
}

/*
 *  stress_enable_classes()
 *	enable stressors based on class
 */
static void stress_enable_classes(const uint32_t class)
{
	size_t i;

	if (!class)
		return;

	/* This indicates some stressors are set */
	g_opt_flags |= OPT_FLAGS_SET;

	for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
		if (stressors[i].info->class & class) {
			stress_stressor_t *ss = stress_find_proc_info(&stressors[i]);

			if (g_opt_flags & OPT_FLAGS_SEQUENTIAL)
				ss->num_instances = g_opt_sequential;
			else if (g_opt_flags & OPT_FLAGS_ALL)
				ss->num_instances = g_opt_parallel;
			else if (g_opt_flags & OPT_FLAGS_PERMUTE)
				ss->num_instances = g_opt_permute;
		}
	}
}


/*
 *  stress_parse_opts
 *	parse argv[] and set stress-ng options accordingly
 */
int stress_parse_opts(int argc, char **argv, const bool jobmode)
{
	optind = 0;

	for (;;) {
		int64_t i64;
		int32_t i32;
		uint32_t u32;
		uint64_t u64, max_fds;
		int16_t i16;
		int c, option_index, ret;
		size_t i;

		opterr = (!jobmode) ? opterr : 0;
next_opt:
		if ((c = getopt_long(argc, argv, "?khMVvqnt:b:c:i:j:m:d:f:s:l:p:P:C:S:a:y:F:D:T:u:o:r:B:R:Y:x:",
			stress_long_options, &option_index)) == -1) {
			break;
		}

		for (i = 0; i < SIZEOF_ARRAY(stressors); i++) {
			if (stressors[i].short_getopt == c) {
				const char *name = stress_opt_name(c);
				stress_stressor_t *ss = stress_find_proc_info(&stressors[i]);

				g_stressor_current = ss;
				g_opt_flags |= OPT_FLAGS_SET;
				ss->num_instances = stress_get_int32(optarg);
				stress_get_processors(&ss->num_instances);
				stress_check_max_stressors(name, ss->num_instances);
				goto next_opt;
			}
			if (stressors[i].op == (stress_op_t)c) {
				uint64_t bogo_ops;

				bogo_ops = stress_get_uint64(optarg);
				stress_check_range(stress_opt_name(c), bogo_ops, MIN_OPS, MAX_OPS);
				/* We don't need to set this, but it may be useful */
				stress_set_setting(stress_opt_name(c), TYPE_ID_UINT64, &bogo_ops);
				if (g_stressor_current)
					g_stressor_current->bogo_ops = bogo_ops;
				goto next_opt;
			}
			if (stressors[i].info->opt_set_funcs) {
				size_t j;
				const stressor_info_t *info = stressors[i].info;

				for (j = 0; info->opt_set_funcs[j].opt_set_func; j++) {
					if (info->opt_set_funcs[j].opt == c) {
						ret = info->opt_set_funcs[j].opt_set_func(optarg);
						if (ret < 0)
							return EXIT_FAILURE;
						goto next_opt;
					}
				}
			}
		}

		for (i = 0; i < SIZEOF_ARRAY(opt_flags); i++) {
			if (c == opt_flags[i].opt) {
				stress_set_setting_true(stress_opt_name(c), NULL);
				g_opt_flags |= opt_flags[i].opt_flag;
				goto next_opt;
			}
		}

		switch (c) {
		case OPT_all:
			g_opt_flags |= OPT_FLAGS_ALL;
			g_opt_parallel = stress_get_int32(optarg);
			stress_get_processors(&g_opt_parallel);
			stress_check_max_stressors("all", g_opt_parallel);
			break;
		case OPT_cache_size:
			/* 1K..4GB should be enough range  */
			u64 = stress_get_uint64_byte(optarg);
			stress_check_range_bytes("cache-size", u64, 1 * KB, 4 * GB);
			/* round down to 64 byte boundary */
			u64 &= ~(uint64_t)63;
			stress_set_setting("cache-size", TYPE_ID_UINT64, &u64);
			break;
		case OPT_backoff:
			i64 = (int64_t)stress_get_uint64(optarg);
			stress_set_setting_global("backoff", TYPE_ID_INT64, &i64);
			break;
		case OPT_cache_level:
			/*
			 * Note: Overly high values will be caught in the
			 * caching code.
			 */
			ret = atoi(optarg);
			if ((ret <= 0) || (ret > 3))
				ret = DEFAULT_CACHE_LEVEL;
			i16 = (int16_t)ret;
			stress_set_setting("cache-level", TYPE_ID_INT16, &i16);
			break;
		case OPT_cache_ways:
			u32 = stress_get_uint32(optarg);
			stress_set_setting("cache-ways", TYPE_ID_UINT32, &u32);
			break;
		case OPT_class:
			ret = stress_get_class(optarg, &u32);
			if (ret < 0)
				return EXIT_FAILURE;
			else if (ret > 0)
				exit(EXIT_SUCCESS);
			else {
				stress_set_setting("class", TYPE_ID_UINT32, &u32);
				stress_enable_classes(u32);
			}
			break;
		case OPT_config:
			printf("config:\n%s", stress_config);
			exit(EXIT_SUCCESS);
		case OPT_exclude:
			stress_set_setting_global("exclude", TYPE_ID_STR, (void *)optarg);
			break;
		case OPT_help:
			stress_usage();
			break;
		case OPT_ionice_class:
			i32 = stress_get_opt_ionice_class(optarg);
			stress_set_setting("ionice-class", TYPE_ID_INT32, &i32);
			break;
		case OPT_ionice_level:
			i32 = stress_get_int32(optarg);
			stress_set_setting("ionice-level", TYPE_ID_INT32, &i32);
			break;
		case OPT_job:
			stress_set_setting_global("job", TYPE_ID_STR, (void *)optarg);
			break;
		case OPT_log_file:
			stress_set_setting_global("log-file", TYPE_ID_STR, (void *)optarg);
			break;
		case OPT_max_fd:
			max_fds = (uint64_t)stress_get_file_limit();
			u64 = stress_get_uint64_percent(optarg, 1, max_fds,
				"Cannot determine maximum file descriptor limit");
			stress_check_range(optarg, u64, 8, max_fds);
			stress_set_setting_global("max-fd", TYPE_ID_UINT64, &u64);
			break;
		case OPT_mbind:
			if (stress_set_mbind(optarg) < 0)
				exit(EXIT_FAILURE);
			break;
		case OPT_no_madvise:
			g_opt_flags &= ~OPT_FLAGS_MMAP_MADVISE;
			break;
		case OPT_oom_avoid_bytes:
			{
				size_t shmall, freemem, totalmem, freeswap, totalswap, bytes;

				bytes = (size_t)stress_get_uint64_byte_memory(optarg, 1);
				stress_get_memlimits(&shmall, &freemem, &totalmem, &freeswap, &totalswap);
				if ((freemem > 0) && (bytes > freemem / 2)) {
					char buf[32];

					bytes = freemem / 2;
					pr_inf("option --oom-avoid-bytes too large, limiting to "
						"50%% (%s) of free memory\n",
						stress_uint64_to_str(buf, sizeof(buf), (uint64_t)bytes));
				}
				stress_set_setting("oom-avoid-bytes", TYPE_ID_SIZE_T, &bytes);
				g_opt_flags |= OPT_FLAGS_OOM_AVOID;
			}
			break;
		case OPT_query:
			if (!jobmode)
				(void)printf("Try '%s --help' for more information.\n", g_app_name);
			return EXIT_FAILURE;
		case OPT_quiet:
			g_opt_flags &= ~(OPT_FLAGS_PR_ALL);
			break;
		case OPT_random:
			g_opt_flags |= OPT_FLAGS_RANDOM;
			i32 = stress_get_int32(optarg);
			stress_get_processors(&i32);
			stress_check_max_stressors("random", i32);
			stress_set_setting("random", TYPE_ID_INT32, &i32);
			break;
		case OPT_sched:
			i32 = stress_get_opt_sched(optarg);
			stress_set_setting_global("sched", TYPE_ID_INT32, &i32);
			break;
		case OPT_sched_prio:
			i32 = stress_get_int32(optarg);
			stress_set_setting_global("sched-prio", TYPE_ID_INT32, &i32);
			break;
		case OPT_sched_period:
			u64 = stress_get_uint64(optarg);
			stress_set_setting_global("sched-period", TYPE_ID_UINT64, &u64);
			break;
		case OPT_sched_runtime:
			u64 = stress_get_uint64(optarg);
			stress_set_setting_global("sched-runtime", TYPE_ID_UINT64, &u64);
			break;
		case OPT_sched_deadline:
			u64 = stress_get_uint64(optarg);
			stress_set_setting_global("sched-deadline", TYPE_ID_UINT64, &u64);
			break;
		case OPT_sched_reclaim:
			g_opt_flags |= OPT_FLAGS_DEADLINE_GRUB;
			break;
		case OPT_seed:
			u64 = stress_get_uint64(optarg);
			g_opt_flags |= OPT_FLAGS_SEED;
			stress_set_setting_global("seed", TYPE_ID_UINT64, &u64);
			break;
		case OPT_sequential:
			g_opt_flags |= OPT_FLAGS_SEQUENTIAL;
			g_opt_sequential = stress_get_int32(optarg);
			stress_get_processors(&g_opt_sequential);
			stress_check_range("sequential", (uint64_t)g_opt_sequential,
				MIN_SEQUENTIAL, MAX_SEQUENTIAL);
			break;
		case OPT_permute:
			g_opt_flags |= OPT_FLAGS_PERMUTE;
			g_opt_permute = stress_get_int32(optarg);
			stress_get_processors(&g_opt_permute);
			stress_check_max_stressors("permute", g_opt_permute);
			break;
		case OPT_status:
			if (stress_set_status(optarg) < 0)
				exit(EXIT_FAILURE);
			break;
		case OPT_stressors:
			stress_show_stressor_names();
			exit(EXIT_SUCCESS);
		case OPT_taskset:
			if (stress_set_cpu_affinity(optarg) < 0)
				exit(EXIT_FAILURE);
			break;
		case OPT_temp_path:
			if (stress_set_temp_path(optarg) < 0)
				exit(EXIT_FAILURE);
			break;
		case OPT_timeout:
			g_opt_timeout = stress_get_uint64_time(optarg);
			break;
		case OPT_timer_slack:
			(void)stress_set_timer_slack_ns(optarg);
			break;
		case OPT_version:
			stress_version();
			exit(EXIT_SUCCESS);
		case OPT_verifiable:
			stress_verifiable();
			exit(EXIT_SUCCESS);
		case OPT_vmstat:
			if (stress_set_vmstat(optarg) < 0)
				exit(EXIT_FAILURE);
			break;
		case OPT_thermalstat:
			if (stress_set_thermalstat(optarg) < 0)
				exit(EXIT_FAILURE);
			break;
		case OPT_iostat:
			if (stress_set_iostat(optarg) < 0)
				exit(EXIT_FAILURE);
			break;
		case OPT_with:
			g_opt_flags |= (OPT_FLAGS_WITH | OPT_FLAGS_SET);
			stress_set_setting_global("with", TYPE_ID_STR, (void *)optarg);
			break;
		case OPT_yaml:
			stress_set_setting_global("yaml", TYPE_ID_STR, (void *)optarg);
			break;
		default:
			if (!jobmode)
				(void)printf("Unknown option (%d)\n",c);
			return EXIT_FAILURE;
		}
	}

	if (optind < argc) {
		bool unicode = false;

		(void)printf("Error: unrecognised option:");
		while (optind < argc) {
			(void)printf(" %s", argv[optind]);
			if (((argv[optind][0] & 0xff) == 0xe2) &&
			    ((argv[optind][1] & 0xff) == 0x88)) {
				unicode = true;
			}
			optind++;
		}
		(void)printf("\n");
		if (unicode)
			(void)printf("note: a Unicode minus sign was used instead of an ASCII '-' for an option\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

/*
 *  stress_alloc_proc_resources()
 *	allocate array of pids based on n pids required
 */
static void stress_alloc_proc_resources(
	stress_stats_t ***stats,
	const int32_t n)
{
	*stats = calloc((size_t)n, sizeof(stress_stats_t *));
	if (!*stats) {
		pr_err("cannot allocate stats array of %" PRIu32 " elements\n", n);
		stress_stressors_free();
		exit(EXIT_FAILURE);
	}
}

/*
 *  stress_set_default_timeout()
 *	set timeout to a default value if not already set
 */
static void stress_set_default_timeout(const uint64_t timeout)
{
	char *action;

	if (g_opt_timeout == TIMEOUT_NOT_SET) {
		g_opt_timeout = timeout;
		action = "defaulting";
	} else {
		action = "setting";
	}
	pr_inf("%s to a %s run per stressor\n",
		action, stress_duration_to_str((double)g_opt_timeout, false));
}

/*
 *  stress_setup_sequential()
 *	setup for sequential --seq mode stressors
 */
static void stress_setup_sequential(const uint32_t class, const int32_t instances)
{
	stress_stressor_t *ss;

	stress_set_default_timeout(60);

	for (ss = stressors_head; ss; ss = ss->next) {
		if (ss->stressor->info->class & class)
			ss->num_instances = instances;
		if (ss->ignore.run)
			continue;
		stress_alloc_proc_resources(&ss->stats, ss->num_instances);
	}
}

/*
 *  stress_setup_parallel()
 *	setup for parallel mode stressors
 */
static void stress_setup_parallel(const uint32_t class, const int32_t instances)
{
	stress_stressor_t *ss;

	stress_set_default_timeout(DEFAULT_TIMEOUT);

	for (ss = stressors_head; ss; ss = ss->next) {
		if (ss->stressor->info->class & class)
			ss->num_instances = instances;
		if (ss->ignore.run)
			continue;
		/*
		 * Share bogo ops between processes equally, rounding up
		 * if nonzero bogo_ops
		 */
		ss->bogo_ops = ss->num_instances ?
			(ss->bogo_ops + (ss->num_instances - 1)) / ss->num_instances : 0;
		if (ss->num_instances)
			stress_alloc_proc_resources(&ss->stats, ss->num_instances);
	}
}

/*
 *  stress_run_sequential()
 *	run stressors sequentially
 */
static inline void stress_run_sequential(
	const int32_t ticks_per_sec,
	double *duration,
	bool *success,
	bool *resource_success,
	bool *metrics_success)
{
	stress_stressor_t *ss;
	stress_checksum_t *checksum = g_shared->checksum.checksums;

	/*
	 *  Step through each stressor one by one
	 */
	for (ss = stressors_head; ss && stress_continue_flag(); ss = ss->next) {
		stress_stressor_t *next = ss->next;

		if (ss->ignore.run)
			continue;

		ss->next = NULL;
		stress_run(ticks_per_sec, ss, duration, success, resource_success,
			metrics_success, &checksum);
		ss->next = next;
	}
}

/*
 *  stress_run_parallel()
 *	run stressors in parallel
 */
static inline void stress_run_parallel(
	const int32_t ticks_per_sec,
	double *duration,
	bool *success,
	bool *resource_success,
	bool *metrics_success)
{
	stress_checksum_t *checksum = g_shared->checksum.checksums;

	/*
	 *  Run all stressors in parallel
	 */
	stress_run(ticks_per_sec, stressors_head, duration, success, resource_success,
			metrics_success, &checksum);
}

/*
 *  stress_run_permute()
 *	run stressors using permutations
 */
static inline void stress_run_permute(
	const int32_t ticks_per_sec,
	double *duration,
	bool *success,
	bool *resource_success,
	bool *metrics_success)
{
	stress_stressor_t *ss;
	size_t i, perms, num_perms;
	const size_t max_perms = 16;
	char str[4096];

	for (perms = 0, ss = stressors_head; ss; ss = ss->next) {
		ss->ignore.permute = true;
		if (!ss->ignore.run)
			perms++;
	}

	if (perms > max_perms) {
		pr_inf("permute: limiting to first %zu stressors\n", max_perms);
		perms = max_perms;
	}

	num_perms = 1U << perms;

	for (i = 1; stress_continue_flag() && (i < num_perms); i++) {
		size_t j;

		*str = '\0';
		for (j = 0, ss = stressors_head; (j < max_perms) && ss; ss = ss->next) {
			ss->ignore.permute = true;
			if (ss->ignore.run)
				continue;
			ss->ignore.permute = ((i & (1U << j)) == 0);
			if (!ss->ignore.permute) {
				if (*str)
					shim_strlcat(str, ", ", sizeof(str));
				shim_strlcat(str, ss->stressor->name, sizeof(str));
			}
			j++;
		}
		pr_inf("permute: %s\n", str);
		stress_run_parallel(ticks_per_sec, duration, success, resource_success, metrics_success);
		pr_inf("permute: %.2f%% complete\n", (double)i / (double)(num_perms - 1) * 100.0);
	}
	for (ss = stressors_head; ss; ss = ss->next) {
		ss->ignore.permute = false;
	}
}

/*
 *  stress_mlock_executable()
 *	try to mlock image into memory so it
 *	won't get swapped out
 */
static inline void stress_mlock_executable(void)
{
#if defined(MLOCKED_SECTION)
	extern void *__start_mlocked_text;
	extern void *__stop_mlocked_text;

	stress_mlock_region(&__start_mlocked_text, &__stop_mlocked_text);
#endif
}

/*
 *  stress_yaml_open()
 *	open YAML results file
 */
static FILE *stress_yaml_open(const char *yaml_filename)
{
	FILE *yaml = NULL;

	if (yaml_filename) {
		yaml = fopen(yaml_filename, "w");
		if (!yaml)
			pr_err("Cannot output YAML data to %s\n", yaml_filename);

		pr_yaml(yaml, "---\n");
		stress_yaml_runinfo(yaml);
	}
	return yaml;
}

/*
 *  stress_yaml_open()
 *	close YAML results file
 */
static void stress_yaml_close(FILE *yaml)
{
	if (yaml) {
		pr_yaml(yaml, "...\n");
		(void)fclose(yaml);
	}
}

int main(int argc, char **argv, char **envp)
{
	double duration = 0.0;			/* stressor run time in secs */
	bool success = true;
	bool resource_success = true;
	bool metrics_success = true;
	FILE *yaml;				/* YAML output file */
	char *yaml_filename = NULL;		/* YAML file name */
	char *log_filename;			/* log filename */
	char *job_filename = NULL;		/* job filename */
	int32_t ticks_per_sec;			/* clock ticks per second (jiffies) */
	int32_t ionice_class = UNDEFINED;	/* ionice class */
	int32_t ionice_level = UNDEFINED;	/* ionice level */
	size_t i;
	uint32_t class = 0;
	const uint32_t cpus_online = (uint32_t)stress_get_processors_online();
	const uint32_t cpus_configured = (uint32_t)stress_get_processors_configured();
	int ret;
	bool unsupported = false;		/* true if stressors are unsupported */

	main_pid = getpid();

	/* Enable stress-ng stack smashing message */
	stress_set_stack_smash_check_flag(true);

	if (stress_set_temp_path(".") < 0)
		exit(EXIT_FAILURE);
	stress_set_proc_name_init(argc, argv, envp);

	if (setjmp(g_error_env) == 1) {
		ret = EXIT_FAILURE;
		goto exit_temp_path_free;
	}

	yaml = NULL;

	/* --exec stressor uses this to exec itself and then exit early */
	if ((argc == 2) && !strcmp(argv[1], "--exec-exit")) {
		ret = EXIT_SUCCESS;
		goto exit_temp_path_free;
	}

	stressors_head = NULL;
	stressors_tail = NULL;
	stress_mwc_reseed();

	(void)stress_get_page_size();
	stressor_set_defaults();

	if (stress_get_processors_configured() < 0) {
		pr_err("sysconf failed, number of cpus configured "
			"unknown: errno=%d: (%s)\n",
			errno, strerror(errno));
		ret = EXIT_FAILURE;
		goto exit_settings_free;
	}
	ticks_per_sec = stress_get_ticks_per_second();
	if (ticks_per_sec < 0) {
		pr_err("sysconf failed, clock ticks per second "
			"unknown: errno=%d (%s)\n",
			errno, strerror(errno));
		ret = EXIT_FAILURE;
		goto exit_settings_free;
	}

	ret = stress_parse_opts(argc, argv, false);
	if (ret != EXIT_SUCCESS)
		goto exit_settings_free;

	if ((g_opt_flags & (OPT_FLAGS_STDERR | OPT_FLAGS_STDOUT)) ==
	    (OPT_FLAGS_STDERR | OPT_FLAGS_STDOUT)) {
		(void)fprintf(stderr, "stderr and stdout cannot "
			"be used together\n");
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}

	if (stress_check_temp_path() < 0) {
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}

	if (g_opt_flags & OPT_FLAGS_KSM)
		stress_ksm_memory_merge(1);

	/*
	 *  Load in job file options
	 */
	(void)stress_get_setting("job", &job_filename);
	if (stress_parse_jobfile(argc, argv, job_filename) < 0) {
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}

	/*
	 *  Sanity check minimize/maximize options
	 */
	if ((g_opt_flags & OPT_FLAGS_MINMAX_MASK) == OPT_FLAGS_MINMAX_MASK) {
		(void)fprintf(stderr, "maximize and minimize cannot "
			"be used together\n");
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}

	/*
	 *  Sanity check seq/all settings
	 */
	if (stress_popcount64(g_opt_flags & (OPT_FLAGS_RANDOM | OPT_FLAGS_SEQUENTIAL | OPT_FLAGS_ALL | OPT_FLAGS_PERMUTE)) > 1) {
		(void)fprintf(stderr, "cannot invoke --random, --sequential, --all or --permute "
			"options together\n");
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}
	(void)stress_get_setting("class", &class);

	if (class &&
	    !(g_opt_flags & (OPT_FLAGS_SEQUENTIAL | OPT_FLAGS_ALL | OPT_FLAGS_PERMUTE))) {
		(void)fprintf(stderr, "class option is only used with "
			"--sequential, --all or --permute options\n");
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}

	/*
	 *  Sanity check mutually exclusive random seed flags
	 */
	if ((g_opt_flags & (OPT_FLAGS_NO_RAND_SEED | OPT_FLAGS_SEED)) ==
	    (OPT_FLAGS_NO_RAND_SEED | OPT_FLAGS_SEED)) {
		(void)fprintf(stderr, "cannot invoke mutually exclusive "
			"--seed and --no-rand-seed options together\n");
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}

	/*
	 *  Sanity check --with option
	 */
	if ((g_opt_flags & OPT_FLAGS_WITH) &&
	    ((g_opt_flags & (OPT_FLAGS_SEQUENTIAL | OPT_FLAGS_ALL | OPT_FLAGS_PERMUTE)) == 0)) {
		(void)fprintf(stderr, "the --with option also requires the --seq, --all or --permute options\n");
		ret = EXIT_FAILURE;
		goto exit_stressors_free;
	}

	stress_cpuidle_init();

	/*
	 *  Setup logging
	 */
	if (stress_get_setting("log-file", &log_filename))
		pr_openlog(log_filename);
	shim_openlog("stress-ng", 0, LOG_USER);
	stress_log_args(argc, argv);
	stress_log_system_info();
	stress_log_system_mem_info();
	stress_runinfo();
	stress_cpuidle_log_info();
	pr_dbg("%" PRId32 " processor%s online, %" PRId32
		" processor%s configured\n",
		cpus_online, cpus_online == 1 ? "" : "s",
		cpus_configured, cpus_configured == 1 ? "" : "s");

	/*
	 *  For random mode the stressors must be available
	 */
	if (g_opt_flags & OPT_FLAGS_RANDOM)
		stress_enable_all_stressors(0);
	/*
	 *  These two options enable all the stressors
	 */
	if (g_opt_flags & OPT_FLAGS_SEQUENTIAL)
		stress_enable_all_stressors(g_opt_sequential);
	if (g_opt_flags & OPT_FLAGS_ALL)
		stress_enable_all_stressors(g_opt_parallel);
	if (g_opt_flags & OPT_FLAGS_PERMUTE)
		stress_enable_all_stressors(g_opt_permute);
	/*
	 *  Discard stressors that we can't run
	 */
	stress_exclude_unsupported(&unsupported);
	stress_exclude_pathological();
	/*
	 *  Throw away excluded stressors
	 */
	if (stress_exclude() < 0) {
		ret = EXIT_FAILURE;
		goto exit_logging_close;
	}

	/*
	 *  Setup random stressors if requested
	 */
	stress_set_random_stressors();

	(void)stress_ftrace_start();
#if defined(STRESS_PERF_STATS) &&	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	if (g_opt_flags & OPT_FLAGS_PERF_STATS)
		stress_perf_init();
#endif

	/*
	 *  Setup running environment
	 */
	stress_process_dumpable(false);
	stress_cwd_readwriteable();
	stress_set_oom_adjustment(NULL, false);

	/*
	 *  Get various user defined settings
	 */
	if (sched_settings_apply(false) < 0) {
		ret = EXIT_FAILURE;
		goto exit_logging_close;
	}
	(void)stress_get_setting("ionice-class", &ionice_class);
	(void)stress_get_setting("ionice-level", &ionice_level);
	stress_set_iopriority(ionice_class, ionice_level);
	(void)stress_get_setting("yaml", &yaml_filename);

	stress_mlock_executable();

	/*
	 *  Enable signal handers
	 */
	for (i = 0; i < SIZEOF_ARRAY(stress_terminate_signals); i++) {
		if (stress_sighandler("stress-ng", stress_terminate_signals[i],
					stress_handle_terminate, NULL) < 0) {
			ret = EXIT_FAILURE;
			goto exit_logging_close;
		}
	}
	/*
	 *  Ignore other signals
	 */
	for (i = 0; i < SIZEOF_ARRAY(stress_ignore_signals); i++) {
		VOID_RET(int, stress_sighandler("stress-ng", stress_ignore_signals[i],
						SIG_IGN, NULL));
	}

	/*
	 *  Setup stressor proc info
	 */
	if (g_opt_flags & OPT_FLAGS_SEQUENTIAL) {
		stress_setup_sequential(class, g_opt_sequential);
	} else if (g_opt_flags & OPT_FLAGS_PERMUTE) {
		stress_setup_sequential(class, g_opt_permute);
	} else {
		stress_setup_parallel(class, g_opt_parallel);
	}
	/*
	 *  Seq/parallel modes may have added in
	 *  excluded stressors, so exclude check again
	 */
	stress_exclude_unsupported(&unsupported);
	stress_exclude_pathological();

	stress_set_proc_limits();

	if (!stressors_head) {
		pr_err("No stress workers invoked%s\n",
			unsupported ? " (one or more were unsupported)" : "");
		/*
		 *  If some stressors were given but marked as
		 *  unsupported then this is not an error.
		 */
		ret = unsupported ? EXIT_SUCCESS : EXIT_FAILURE;
		goto exit_logging_close;
	}

	/*
	 *  Allocate shared memory segment for shared data
	 *  across all the child stressors
	 */
	stress_shared_map(stress_get_total_num_instances(stressors_head));

	/*
	 *  And now shared memory is created, initialize pr_* lock mechanism
	 */
	if (!stress_shared_heap_init()) {
		pr_err("failed to create shared heap \n");
		ret = EXIT_FAILURE;
		goto exit_shared_unmap;
	}

	/*
	 *  Initialize global locks
	 */
#if defined(STRESS_PERF_STATS) &&	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	g_shared->perf.lock = stress_lock_create();
	if (!g_shared->perf.lock) {
		pr_err("failed to create perf lock\n");
		ret = EXIT_FAILURE;
		goto exit_shared_unmap;
	}
#endif
	g_shared->warn_once.lock = stress_lock_create();
	if (!g_shared->warn_once.lock) {
		pr_err("failed to create warn_once lock\n");
		ret = EXIT_FAILURE;
		goto exit_destroy_perf_lock;
	}
	g_shared->net_port_map.lock = stress_lock_create();
	if (!g_shared->warn_once.lock) {
		pr_err("failed to create net_port_map lock\n");
		ret = EXIT_FAILURE;
		goto exit_destroy_warn_once_lock;
	}

	/*
	 *  Assign procs with shared stats memory
	 */
	stress_setup_stats_buffers();

	/*
	 *  Allocate shared cache memory
	 */
	g_shared->mem_cache.size = 0;
	(void)stress_get_setting("cache-size", &g_shared->mem_cache.size);
	g_shared->mem_cache.level = DEFAULT_CACHE_LEVEL;
	(void)stress_get_setting("cache-level", &g_shared->mem_cache.level);
	g_shared->mem_cache.ways = 0;
	(void)stress_get_setting("cache-ways", &g_shared->mem_cache.ways);
	if (stress_cache_alloc("cache allocate") < 0) {
		ret = EXIT_FAILURE;
		goto exit_shared_unmap;
	}

	/*
	 *  Show the stressors we're going to run
	 */
	if (stress_show_stressors() < 0) {
		ret = EXIT_FAILURE;
		goto exit_shared_unmap;
	}

#if defined(STRESS_THERMAL_ZONES)
	/*
	 *  Setup thermal zone data
	 */
	if (g_opt_flags & OPT_FLAGS_TZ_INFO)
		stress_tz_init(&g_shared->tz_info);
#endif

	stress_clear_warn_once();
	stress_stressors_init();

	/* Start thrasher process if required */
	if (g_opt_flags & OPT_FLAGS_THRASH)
		stress_thrash_start();

	stress_vmstat_start();
	stress_smart_start();
	stress_klog_start();
	stress_clocksource_check();

	if (g_opt_flags & OPT_FLAGS_METRICS)
		stress_config_check();

	if (g_opt_flags & OPT_FLAGS_SEQUENTIAL) {
		stress_run_sequential(ticks_per_sec, &duration, &success, &resource_success, &metrics_success);
	} else if (g_opt_flags & OPT_FLAGS_PERMUTE) {
		stress_run_permute(ticks_per_sec, &duration, &success, &resource_success, &metrics_success);
	} else {
		stress_run_parallel(ticks_per_sec, &duration, &success, &resource_success, &metrics_success);
	}

	stress_clocksource_check();

	/* Stop alarms */
	(void)alarm(0);

	/* Stop thasher process */
	if (g_opt_flags & OPT_FLAGS_THRASH)
		stress_thrash_stop();

	yaml = stress_yaml_open(yaml_filename);

	/*
	 *  Dump metrics
	 */
	if (g_opt_flags & OPT_FLAGS_METRICS)
		stress_metrics_dump(yaml);

	stress_metrics_check(&success);
	if (g_opt_flags & OPT_FLAGS_INTERRUPTS)
		stress_interrupts_dump(yaml, stressors_head);

#if defined(STRESS_PERF_STATS) &&	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	/*
	 *  Dump perf statistics
	 */
	if (g_opt_flags & OPT_FLAGS_PERF_STATS)
		stress_perf_stat_dump(yaml, stressors_head, duration);
#endif

#if defined(STRESS_THERMAL_ZONES)
	/*
	 *  Dump thermal zone measurements
	 */
	if (g_opt_flags & OPT_FLAGS_THERMAL_ZONES)
		stress_tz_dump(yaml, stressors_head);
	if (g_opt_flags & OPT_FLAGS_TZ_INFO)
		stress_tz_free(&g_shared->tz_info);
#endif
	/*
	 *  Dump run times
	 */
	stress_times_dump(yaml, ticks_per_sec, duration);
	stress_exit_status_summary();

	stress_klog_stop(&success);
	stress_smart_stop();
	stress_vmstat_stop();
	stress_ftrace_stop();
	stress_ftrace_free();

	pr_inf("%s run completed in %s\n",
		success ? "successful" : "unsuccessful",
		stress_duration_to_str(duration, true));

	if (g_opt_flags & OPT_FLAGS_SETTINGS)
		stress_settings_show();
	/*
	 *  Tidy up
	 */
	(void)stress_lock_destroy(g_shared->net_port_map.lock);
	(void)stress_lock_destroy(g_shared->warn_once.lock);
#if defined(STRESS_PERF_STATS) && 	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	(void)stress_lock_destroy(g_shared->perf.lock);
#endif

	stress_shared_heap_deinit();
	stress_stressors_deinit();
	stress_stressors_free();
	stress_cpuidle_free();
	stress_cache_free();
	stress_shared_unmap();
	stress_settings_free();
	stress_temp_path_free();

	/*
	 *  Close logs
	 */
	shim_closelog();
	pr_closelog();
	stress_yaml_close(yaml);

	/*
	 *  Done!
	 */
	if (!success)
		exit(EXIT_NOT_SUCCESS);
	if (!resource_success)
		exit(EXIT_NO_RESOURCE);
	if (!metrics_success)
		exit(EXIT_METRICS_UNTRUSTWORTHY);
	exit(EXIT_SUCCESS);

exit_destroy_warn_once_lock:
	(void)stress_lock_destroy(g_shared->warn_once.lock);

exit_destroy_perf_lock:
#if defined(STRESS_PERF_STATS) && 	\
    defined(HAVE_LINUX_PERF_EVENT_H)
	(void)stress_lock_destroy(g_shared->perf.lock);
#endif

exit_shared_unmap:
	stress_shared_unmap();

exit_logging_close:
	shim_closelog();
	pr_closelog();

exit_stressors_free:
	stress_stressors_free();

exit_settings_free:
	stress_settings_free();

exit_temp_path_free:
	stress_temp_path_free();
	exit(ret);
}
