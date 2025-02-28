/*-------------------------------------------------------------------------
 *
 * syslogger.c
 *
 * The system logger (syslogger) appeared in Postgres 8.0. It catches all
 * stderr output from the postmaster, backends, and other subprocesses
 * by redirecting to a pipe, and writes it to a set of logfiles.
 * It's possible to have size and age limits for the logfile configured
 * in postgresql.conf. If these limits are reached or passed, the
 * current logfile is closed and a new one is created (rotated).
 * The logfiles are stored in a subdirectory (configurable in
 * postgresql.conf), using a user-selectable naming scheme.
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Copyright (c) 2004-2022, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/syslogger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "common/file_perm.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtime.h"
#include "port/pg_bitutils.h"
#include "postmaster/fork_process.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/timestamp.h"

/* POLAR */
#include "storage/fd.h"
#include "utils/builtins.h"

/*
 * We read() into a temp buffer twice as big as a chunk, so that any fragment
 * left after processing can be moved down to the front and we'll still have
 * room to read a full chunk.
 */
/* POLAR: enlarge this */
#define READ_BUF_SIZE (200 * PIPE_CHUNK_SIZE)

/* Log rotation signal file path, relative to $PGDATA */
#define LOGROTATE_SIGNAL_FILE	"logrotate"


/*
 * GUC parameters.  Logging_collector cannot be changed after postmaster
 * start, but the rest can change at SIGHUP.
 */
bool		Logging_collector = false;
int			Log_RotationAge = HOURS_PER_DAY * MINS_PER_HOUR;
int			Log_RotationSize = 10 * 1024;
char	   *Log_directory = NULL;
char	   *Log_filename = NULL;
bool		Log_truncate_on_rotation = false;
int			Log_file_mode = S_IRUSR | S_IWUSR;

/*
 * Globally visible state (used by elog.c)
 */
bool		am_syslogger = false;

/* POLAR: public */
int			MyLoggerIndex = 0;
bool		polar_enable_multi_syslogger = DEFAULT_MULTI_SYSLOGGER_FLAG;
bool		polar_enable_syslog_pipe_buffer = true;
bool		polar_enable_syslog_file_buffer = false;
bool		polar_enable_error_to_audit_log = false;
int			polar_syslogger_num = DEFAULT_SYSLOGGER_NUM;
#define GET_LOG_CHANNEL_FD_WITH_INDEX(index, end) \
	index ? syslogChannels[index][end] : syslogPipe[end]
#define GET_LOG_CHANNEL_FD(end) \
	GET_LOG_CHANNEL_FD_WITH_INDEX(MyLoggerIndex, end)
#define SET_LOG_CHANNEL_FD_WITH_INDEX(index, end, fd)  	\
	do {												\
		if (index == 0)									\
			syslogPipe[end] = fd;						\
		else											\
			syslogChannels[index][end] = fd;	\
	} while (0)
#define SET_LOG_CHANNEL_FD(end, fd) 	\
	SET_LOG_CHANNEL_FD_WITH_INDEX(MyLoggerIndex, end, fd)
#define FILE_BUF_MODE(file_type) \
	((file_type == LOG_DESTINATION_POLAR_AUDITLOG && polar_enable_syslog_file_buffer) ? PG_IOFBF : PG_IOLBF)
/* POLAR end */

extern bool redirection_done;

/*
 * Private state
 */
static pg_time_t next_rotation_time;
static bool pipe_eof_seen = false;
static bool rotation_disabled = false;
static FILE *syslogFile = NULL;
static FILE *csvlogFile = NULL;
static FILE *jsonlogFile = NULL;
NON_EXEC_STATIC pg_time_t first_syslogger_file_time = 0;
static char *last_sys_file_name = NULL;
static char *last_csv_file_name = NULL;
static char *last_json_file_name = NULL;

/* POLAR: private */
static FILE *auditlogFile = NULL;
static char *polar_last_audit_file_name = NULL;
static FILE *slowlogFile = NULL;
static char *polar_last_slowlog_file_name = NULL;

/* POLAR end */

/*
 * Buffers for saving partial messages from different backends.
 *
 * Keep NBUFFER_LISTS lists of these, with the entry for a given source pid
 * being in the list numbered (pid % NBUFFER_LISTS), so as to cut down on
 * the number of entries we have to examine for any one incoming message.
 * There must never be more than one entry for the same source pid.
 *
 * An inactive buffer is not removed from its list, just held for re-use.
 * An inactive buffer has pid == 0 and undefined contents of data.
 */
typedef struct
{
	int32		pid;			/* PID of source process */
	StringInfoData data;		/* accumulated data, as a StringInfo */
} save_buffer;

#define NBUFFER_LISTS 256
static List *buffer_lists[NBUFFER_LISTS];

/* These must be exported for EXEC_BACKEND case ... annoying */
#ifndef WIN32
int			syslogPipe[2] = {-1, -1};
#else
HANDLE		syslogPipe[2] = {0, 0};
#endif

/* POLAR */
#ifndef WIN32
bool		polar_syslog_channel_is_inited = false;
int			syslogChannels[MAX_SYSLOGGER_NUM][2];
#else
HANDLE		syslogChannels[MAX_SYSLOGGER_NUM][2] = {0};
#endif
/* POLAR end */

#ifdef WIN32
static HANDLE threadHandle = 0;
static CRITICAL_SECTION sysloggerSection;
#endif

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t rotation_requested = false;


/* Local subroutines */
#ifdef EXEC_BACKEND
static int	syslogger_fdget(FILE *file);
static FILE *syslogger_fdopen(int fd);
static pid_t syslogger_forkexec(void);
static void syslogger_parseArgs(int argc, char *argv[]);
#endif
NON_EXEC_STATIC void SysLoggerMain(int argc, char *argv[]) pg_attribute_noreturn();
static void process_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static void flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static FILE *logfile_open(const char *filename, const char *mode,
						  bool allow_errors);

#ifdef WIN32
static unsigned int __stdcall pipeThread(void *arg);
#endif
static void logfile_rotate(bool time_based_rotation, int size_rotation_for);
static bool logfile_rotate_dest(bool time_based_rotation,
								int size_rotation_for, pg_time_t fntime,
								int target_dest, char **last_file_name,
								FILE **logFile);
static char *logfile_getname(pg_time_t timestamp, const char *suffix);
static void set_next_rotation_time(void);
static void sigUsr1Handler(SIGNAL_ARGS);
static void update_metainfo_datafile(void);

/* POLAR */
static FILE *get_logfile_from_dest(int destination);
static FILE *logfile_open_with_buffer_mode(const char *filename, const char *mode,
										   bool allow_errors, int buffer_mode);
static void polar_drop_log_page_cache(const char *filename);
static void polar_remove_log_file(const char *path);
static void polar_remove_old_syslog_files(void);


/*
 * Main entry point for syslogger process
 * argc/argv parameters are valid only in EXEC_BACKEND case.
 */
NON_EXEC_STATIC void
SysLoggerMain(int argc, char *argv[])
{
#ifndef WIN32
	/* POLAR: allocate it on heap because we enlarge the READ_BUF_SIZE */
	char	   *logbuffer = malloc(READ_BUF_SIZE);
	int			bytes_in_logbuffer = 0;
#endif
	char	   *currentLogDir;
	char	   *currentLogFilename;
	int			currentLogRotationAge;
	pg_time_t	now;

	/* POLAR */
	int			i = 0;
	int			channel_fd;
	int			wait_event_id = 0;
	WaitEvent	event;
	WaitEventSet *set = NULL;
	char		procTitle[15] = "logger ";
	char		loggerIndexStr[5];

	MyLoggerIndex = 0;
	if (argc == 1)
		MyLoggerIndex = *(int *) argv;
	pg_ltoa(MyLoggerIndex, loggerIndexStr);
	pg_ltoa(MyLoggerIndex, &procTitle[strlen(procTitle)]);
	/* POLAR end */

	now = MyStartTime;

#ifdef EXEC_BACKEND
	syslogger_parseArgs(argc, argv);
#endif							/* EXEC_BACKEND */

	MyBackendType = B_LOGGER;

	/* POLAR: open audit log file first */
	init_ps_display(procTitle);
	if (Log_destination & LOG_DESTINATION_POLAR_AUDITLOG)
	{
		char	   *filename = logfile_getname(first_syslogger_file_time, AUDITLOG_SUFFIX);

		auditlogFile = logfile_open_with_buffer_mode(filename, "a", false,
													 FILE_BUF_MODE(LOG_DESTINATION_POLAR_AUDITLOG));
		pfree(filename);
	}
	/* POLAR end */

	/*
	 * If we restarted, our stderr is already redirected into our own input
	 * pipe.  This is of course pretty useless, not to mention that it
	 * interferes with detecting pipe EOF.  Point stderr to /dev/null. This
	 * assumes that all interesting messages generated in the syslogger will
	 * come through elog.c and will be sent to write_syslogger_file.
	 */
	if (redirection_done)
	{
		int			fd = open(DEVNULL, O_WRONLY, 0);

		/*
		 * The closes might look redundant, but they are not: we want to be
		 * darn sure the pipe gets closed even if the open failed.  We can
		 * survive running with stderr pointing nowhere, but we can't afford
		 * to have extra pipe input descriptors hanging around.
		 *
		 * As we're just trying to reset these to go to DEVNULL, there's not
		 * much point in checking for failure from the close/dup2 calls here,
		 * if they fail then presumably the file descriptors are closed and
		 * any writes will go into the bitbucket anyway.
		 */
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		if (fd != -1)
		{
			(void) dup2(fd, STDOUT_FILENO);
			(void) dup2(fd, STDERR_FILENO);
			close(fd);
		}
	}

	/*
	 * Syslogger's own stderr can't be the syslogPipe, so set it back to text
	 * mode if we didn't just close it. (It was set to binary in
	 * SubPostmasterMain).
	 */
#ifdef WIN32
	else
		_setmode(STDERR_FILENO, _O_TEXT);
#endif

	/*
	 * Also close our copy of the write end of the pipe.  This is needed to
	 * ensure we can detect pipe EOF correctly.  (But note that in the restart
	 * case, the postmaster already did this.)
	 */
	/* POLAR */
#ifndef WIN32
	channel_fd = GET_LOG_CHANNEL_FD(1);
	if (channel_fd >= 0)
		close(channel_fd);
	SET_LOG_CHANNEL_FD(1, -1);

	/*
	 * POLAR: here we close all other syslogger fds that do not need, because
	 * we need catch file EOF when normal exit.
	 *
	 * If two processes have this socket open, one can close it but the socket
	 * isn't considered closed by the operating system because the other still
	 * has it open. Until the other process closes the socket, the process
	 * reading from the socket won't get an end-of-file. This can lead to
	 * confusion and deadlock.
	 */
	for (i = 0; i < polar_syslogger_num; i++)
	{
		if (i != MyLoggerIndex)
		{
			channel_fd = GET_LOG_CHANNEL_FD_WITH_INDEX(i, 0);
			if (channel_fd >= 0)
				close(channel_fd);
			SET_LOG_CHANNEL_FD_WITH_INDEX(i, 0, -1);
			channel_fd = GET_LOG_CHANNEL_FD_WITH_INDEX(i, 1);
			if (channel_fd >= 0)
				close(channel_fd);
			SET_LOG_CHANNEL_FD_WITH_INDEX(i, 1, -1);
		}
	}
#else
	channel_fd = GET_LOG_CHANNEL_FD(1);
	if (channel_fd >= 0)
		CloseHandle(channel_fd);
	SET_LOG_CHANNEL_FD(1, 0);
#endif

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we ignore all termination signals, and instead exit only when all
	 * upstream processes are gone, to ensure we don't miss any dying gasps of
	 * broken backends...
	 */

	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, sigUsr1Handler);	/* request log rotation */
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	PG_SETMASK(&UnBlockSig);

#ifdef WIN32
	/* Fire up separate data transfer thread */
	InitializeCriticalSection(&sysloggerSection);
	EnterCriticalSection(&sysloggerSection);

	threadHandle = (HANDLE) _beginthreadex(NULL, 0, pipeThread, NULL, 0, NULL);
	if (threadHandle == 0)
		elog(FATAL, "could not create syslogger data transfer thread: %m");
#endif							/* WIN32 */

	/*
	 * Remember active logfiles' name(s).  We recompute 'em from the reference
	 * time because passing down just the pg_time_t is a lot cheaper than
	 * passing a whole file path in the EXEC_BACKEND case.
	 */
	last_sys_file_name = logfile_getname(first_syslogger_file_time, SYSLOG_SUFFIX); /* POLAR */
	if (csvlogFile != NULL)
		last_csv_file_name = logfile_getname(first_syslogger_file_time, ".csv");
	if (jsonlogFile != NULL)
		last_json_file_name = logfile_getname(first_syslogger_file_time, ".json");
	/* POLAR */
	if (auditlogFile != NULL)
		polar_last_audit_file_name = logfile_getname(first_syslogger_file_time, AUDITLOG_SUFFIX);
	if (slowlogFile != NULL)
		polar_last_slowlog_file_name = logfile_getname(first_syslogger_file_time, SLOWLOG_SUFFIX);
	/* POLAR end */

	/* remember active logfile parameters */
	currentLogDir = pstrdup(Log_directory);
	currentLogFilename = pstrdup(Log_filename);
	currentLogRotationAge = Log_RotationAge;
	/* set next planned rotation time */
	set_next_rotation_time();
	update_metainfo_datafile();

	/*
	 * Reset whereToSendOutput, as the postmaster will do (but hasn't yet, at
	 * the point where we forked).  This prevents duplicate output of messages
	 * from syslogger itself.
	 */
	whereToSendOutput = DestNone;

	/*
	 * Set up a reusable WaitEventSet object we'll use to wait for our latch,
	 * and (except on Windows) our socket.
	 *
	 * Unlike all other postmaster child processes, we'll ignore postmaster
	 * death because we want to collect final log output from all backends and
	 * then exit last.  We'll do that by running until we see EOF on the
	 * syslog pipe, which implies that all other backends have exited
	 * (including the postmaster).
	 */

	/* POLAR: move create wait event out of loop begin */
	set = CreateWaitEventSet(CurrentMemoryContext, 3);
	AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET,
					  MyLatch, NULL);
	wait_event_id = AddWaitEventToSet(set, WL_SOCKET_READABLE,
									  GET_LOG_CHANNEL_FD(0), NULL, NULL);
	/* POLAR end */

	/* main worker loop */
	for (;;)
	{
		bool		time_based_rotation = false;
		int			size_rotation_for = 0;
		long		cur_timeout;
		int			cur_flags;

#ifndef WIN32
		int			rc;
		int			ret;
#endif

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/*
		 * Process any requests or signals received recently.
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * Check if the log directory or filename pattern changed in
			 * postgresql.conf. If so, force rotation to make sure we're
			 * writing the logfiles in the right place.
			 */
			if (strcmp(Log_directory, currentLogDir) != 0)
			{
				pfree(currentLogDir);
				currentLogDir = pstrdup(Log_directory);
				rotation_requested = true;

				/*
				 * Also, create new directory if not present; ignore errors
				 */
				(void) MakePGDirectory(Log_directory);
			}
			if (strcmp(Log_filename, currentLogFilename) != 0)
			{
				pfree(currentLogFilename);
				currentLogFilename = pstrdup(Log_filename);
				rotation_requested = true;
			}

			/*
			 * Force a rotation if CSVLOG output was just turned on or off and
			 * we need to open or close csvlogFile accordingly.
			 */
			if (((Log_destination & LOG_DESTINATION_CSVLOG) != 0) !=
				(csvlogFile != NULL))
				rotation_requested = true;

			/* POLAR */
			if (((Log_destination & LOG_DESTINATION_POLAR_AUDITLOG) != 0) !=
				(auditlogFile != NULL))
				rotation_requested = true;
			if (((Log_destination & LOG_DESTINATION_POLAR_SLOWLOG) != 0) !=
				(slowlogFile != NULL))
				rotation_requested = true;
			/* POLAR end */

			/*
			 * Force a rotation if JSONLOG output was just turned on or off
			 * and we need to open or close jsonlogFile accordingly.
			 */
			if (((Log_destination & LOG_DESTINATION_JSONLOG) != 0) !=
				(jsonlogFile != NULL))
				rotation_requested = true;

			/*
			 * If rotation time parameter changed, reset next rotation time,
			 * but don't immediately force a rotation.
			 */
			if (currentLogRotationAge != Log_RotationAge)
			{
				currentLogRotationAge = Log_RotationAge;
				set_next_rotation_time();
			}

			/*
			 * If we had a rotation-disabling failure, re-enable rotation
			 * attempts after SIGHUP, and force one immediately.
			 */
			if (rotation_disabled)
			{
				rotation_disabled = false;
				rotation_requested = true;
			}

			/*
			 * Force rewriting last log filename when reloading configuration.
			 * Even if rotation_requested is false, log_destination may have
			 * been changed and we don't want to wait the next file rotation.
			 */
			update_metainfo_datafile();
		}

		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			/* Do a logfile rotation if it's time */
			now = (pg_time_t) time(NULL);
			if (now >= next_rotation_time)
				rotation_requested = time_based_rotation = true;
		}

		if (!rotation_requested && Log_RotationSize > 0 && !rotation_disabled)
		{
			/* Do a rotation if file is too big */
			if (ftell(syslogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_STDERR;
			}
			if (csvlogFile != NULL &&
				ftell(csvlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_CSVLOG;
			}
			if (jsonlogFile != NULL &&
				ftell(jsonlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_JSONLOG;
			}
			/* POLAR */
			if (auditlogFile != NULL &&
				ftell(auditlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_POLAR_AUDITLOG;
			}
			if (slowlogFile != NULL &&
				ftell(slowlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_POLAR_SLOWLOG;
			}
			/* POLAR end */
		}

		if (rotation_requested)
		{
			/*
			 * TODO(wormhole.gl): need to choose the proper logger carefully
			 * to do these works
			 */
			/*
			 * POLAR: remove oldest log file, now every syslogger will do this
			 * work
			 */
			polar_remove_old_syslog_files();
			/* POLAR end */

			/*
			 * Force rotation when both values are zero. It means the request
			 * was sent by pg_rotate_logfile() or "pg_ctl logrotate".
			 */
			if (!time_based_rotation && size_rotation_for == 0)
				size_rotation_for = LOG_DESTINATION_STDERR |
					LOG_DESTINATION_CSVLOG |
					LOG_DESTINATION_JSONLOG |
					LOG_DESTINATION_POLAR_AUDITLOG | LOG_DESTINATION_POLAR_SLOWLOG; /* POLAR */
			logfile_rotate(time_based_rotation, size_rotation_for);
		}

		/*
		 * Calculate time till next time-based rotation, so that we don't
		 * sleep longer than that.  We assume the value of "now" obtained
		 * above is still close enough.  Note we can't make this calculation
		 * until after calling logfile_rotate(), since it will advance
		 * next_rotation_time.
		 *
		 * Also note that we need to beware of overflow in calculation of the
		 * timeout: with large settings of Log_RotationAge, next_rotation_time
		 * could be more than INT_MAX msec in the future.  In that case we'll
		 * wait no more than INT_MAX msec, and try again.
		 */
		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			pg_time_t	delay;

			delay = next_rotation_time - now;
			if (delay > 0)
			{
				if (delay > INT_MAX / 1000)
					delay = INT_MAX / 1000;
				cur_timeout = delay * 1000L;	/* msec */
			}
			else
				cur_timeout = 0;
			cur_flags = WL_TIMEOUT; /* POLAR */
		}
		else
		{
			cur_timeout = -1L;
			cur_flags = 0;
		}

		/* POLAR optimized */
		ModifyWaitEvent(set, wait_event_id, WL_SOCKET_READABLE | cur_flags, NULL);

		/*
		 * Sleep until there's something to do
		 */
#ifndef WIN32
		/* POLAR */
		ret = WaitEventSetWait(set, cur_timeout, &event, 1, WAIT_EVENT_SYSLOGGER_MAIN);

		if (ret == 0)
			rc = WL_TIMEOUT;
		else
			rc = event.events & (WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_MASK);

		if (rc & WL_SOCKET_READABLE)
		{
			int			bytesRead;

			bytesRead = read(GET_LOG_CHANNEL_FD(0),
							 logbuffer + bytes_in_logbuffer,
							 READ_BUF_SIZE - bytes_in_logbuffer);
			if (bytesRead < 0)
			{
				if (errno != EINTR)
					ereport(LOG,
							(errcode_for_socket_access(),
							 errmsg("could not read from logger pipe: %m")));
			}
			else if (bytesRead > 0)
			{
				bytes_in_logbuffer += bytesRead;
				process_pipe_input(logbuffer, &bytes_in_logbuffer);
				continue;
			}
			else
			{
				/*
				 * Zero bytes read when select() is saying read-ready means
				 * EOF on the pipe: that is, there are no longer any processes
				 * with the pipe write end open.  Therefore, the postmaster
				 * and all backends are shut down, and we are done.
				 */
				pipe_eof_seen = true;

				/* if there's any data left then force it out now */
				flush_pipe_input(logbuffer, &bytes_in_logbuffer);
			}
		}
		else if (rc & WL_TIMEOUT)	/* POLAR */
		{
			/* if there's any data left then force it out now */
			flush_syslogger_file(LOG_DESTINATION_POLAR_AUDITLOG);
		}
#else							/* WIN32 */

		/*
		 * On Windows we leave it to a separate thread to transfer data and
		 * detect pipe EOF.  The main thread just wakes up to handle SIGHUP
		 * and rotation conditions.
		 *
		 * Server code isn't generally thread-safe, so we ensure that only one
		 * of the threads is active at a time by entering the critical section
		 * whenever we're not sleeping.
		 */
		LeaveCriticalSection(&sysloggerSection);

		(void) WaitEventSetWait(wes, cur_timeout, &event, 1,
								WAIT_EVENT_SYSLOGGER_MAIN);

		EnterCriticalSection(&sysloggerSection);
#endif							/* WIN32 */

		if (pipe_eof_seen)
		{
			/*
			 * seeing this message on the real stderr is annoying - so we make
			 * it DEBUG1 to suppress in normal use.
			 */
			ereport(DEBUG1,
					(errmsg_internal("logger shutting down")));

			/*
			 * Normal exit from the syslogger is here.  Note that we
			 * deliberately do not close syslogFile before exiting; this is to
			 * allow for the possibility of elog messages being generated
			 * inside proc_exit.  Regular exit() will take care of flushing
			 * and closing stdio channels.
			 */
			proc_exit(0);
		}
	}

	/* POLAR */
	FreeWaitEventSet(set);
}

/*
 * Postmaster subroutine to start a syslogger subprocess.
 */
int
SysLogger_Start(int loggerIndex)
{
	pid_t		sysloggerPid;
	char	   *filename;

	if (!Logging_collector)
		return 0;

	/*
	 * If first time through, create the pipe which will receive stderr
	 * output.
	 *
	 * If the syslogger crashes and needs to be restarted, we continue to use
	 * the same pipe (indeed must do so, since extant backends will be writing
	 * into that pipe).
	 *
	 * This means the postmaster must continue to hold the read end of the
	 * pipe open, so we can pass it down to the reincarnated syslogger. This
	 * is a bit klugy but we have little choice.
	 *
	 * Also note that we don't bother counting the pipe FDs by calling
	 * Reserve/ReleaseExternalFD.  There's no real need to account for them
	 * accurately in the postmaster or syslogger process, and both ends of the
	 * pipe will wind up closed in all other postmaster children.
	 */
#ifndef WIN32
	/* POLAR */
	if (!polar_syslog_channel_is_inited)
	{
		memset(syslogChannels, -1, MAX_SYSLOGGER_NUM * 2);
		polar_syslog_channel_is_inited = true;
	}
	if (loggerIndex == 0)
	{
		if (syslogPipe[0] < 0)
		{
			if (pipe(syslogPipe) < 0)
				/* if (socketpair(AF_UNIX, SOCK_STREAM, 0, syslogPipe) < 0) */
				ereport(FATAL,
						(errcode_for_socket_access(),
						 (errmsg("could not create pipe for syslog: %m"))));
		}
	}
	else
	{
		if (syslogChannels[loggerIndex][0] < 0)
		{
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, syslogChannels[loggerIndex]) < 0)
				ereport(FATAL,
						(errcode_for_socket_access(),
						 (errmsg("could not create channels for syslog: %m"))));
		}
	}
#else
	if (!syslogPipe[0])
	{
		SECURITY_ATTRIBUTES sa;

		memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = TRUE;

		if (!CreatePipe(&syslogPipe[0], &syslogPipe[1], &sa, 32768))
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not create pipe for syslog: %m")));
	}
#endif

	/*
	 * Create log directory if not present; ignore errors
	 */
	(void) MakePGDirectory(Log_directory);

	/*
	 * The initial logfile is created right in the postmaster, to verify that
	 * the Log_directory is writable.  We save the reference time so that the
	 * syslogger child process can recompute this file name.
	 *
	 * It might look a bit strange to re-do this during a syslogger restart,
	 * but we must do so since the postmaster closed syslogFile after the
	 * previous fork (and remembering that old file wouldn't be right anyway).
	 * Note we always append here, we won't overwrite any existing file.  This
	 * is consistent with the normal rules, because by definition this is not
	 * a time-based rotation.
	 */
	first_syslogger_file_time = time(NULL);

	filename = logfile_getname(first_syslogger_file_time, SYSLOG_SUFFIX);

	syslogFile = logfile_open(filename, "a", false);

	pfree(filename);

	/*
	 * Likewise for the initial CSV log file, if that's enabled.  (Note that
	 * we open syslogFile even when only CSV output is nominally enabled,
	 * since some code paths will write to syslogFile anyway.)
	 */
	if (Log_destination & LOG_DESTINATION_CSVLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".csv");

		csvlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

	/*
	 * Likewise for the initial JSON log file, if that's enabled.  (Note that
	 * we open syslogFile even when only JSON output is nominally enabled,
	 * since some code paths will write to syslogFile anyway.)
	 */
	if (Log_destination & LOG_DESTINATION_JSONLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".json");

		jsonlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

	/* POLAR */
	if (Log_destination & LOG_DESTINATION_POLAR_SLOWLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, SLOWLOG_SUFFIX);
		slowlogFile = logfile_open(filename, "a", false);
		pfree(filename);
	}
	/* POLAR end */

#ifdef EXEC_BACKEND
	switch ((sysloggerPid = syslogger_forkexec()))
#else
	switch ((sysloggerPid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork system logger: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			InitPostmasterChild();

			/* Close the postmaster's sockets */
			ClosePostmasterPorts(true);

			/* Drop our connection to postmaster's shared memory, as well */
			dsm_detach_all();
			PGSharedMemoryDetach();

			/* do the work */
			/* POLAR add one argv */
			GCC_IGNORE_BEGIN("-Wstringop-overflow");
			SysLoggerMain(1, (char **) &loggerIndex);
			GCC_IGNORE_END();
			/* POLAR end */
			break;
#endif

		default:
			/* success, in postmaster */

			/* now we redirect stderr, if not done already */
			if (!redirection_done)
			{
#ifdef WIN32
				int			fd;
#endif

				/*
				 * Leave a breadcrumb trail when redirecting, in case the user
				 * forgets that redirection is active and looks only at the
				 * original stderr target file.
				 */
				ereport(LOG,
						(errmsg("redirecting log output to logging collector process"),
						 errhint("Future log output will appear in directory \"%s\".",
								 Log_directory)));

#ifndef WIN32
				if (loggerIndex == 0)
				{
					fflush(stdout);
					if (dup2(syslogPipe[1], fileno(stdout)) < 0)
						ereport(FATAL,
								(errcode_for_file_access(),
								 errmsg("could not redirect stdout: %m")));
					fflush(stderr);
					if (dup2(syslogPipe[1], fileno(stderr)) < 0)
						ereport(FATAL,
								(errcode_for_file_access(),
								 errmsg("could not redirect stderr: %m")));
					/* Now we are done with the write end of the pipe. */
					close(syslogPipe[1]);
					syslogPipe[1] = -1;
				}
				else
				{
					/* POLAR close index 1 */
					close(syslogChannels[loggerIndex][1]);
					syslogChannels[loggerIndex][1] = -1;
					/* POLAR end */
				}
#else

				/*
				 * open the pipe in binary mode and make sure stderr is binary
				 * after it's been dup'ed into, to avoid disturbing the pipe
				 * chunking protocol.
				 */
				fflush(stderr);
				fd = _open_osfhandle((intptr_t) syslogPipe[1],
									 _O_APPEND | _O_BINARY);
				if (dup2(fd, STDERR_FILENO) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stderr: %m")));
				close(fd);
				_setmode(STDERR_FILENO, _O_BINARY);

				/*
				 * Now we are done with the write end of the pipe.
				 * CloseHandle() must not be called because the preceding
				 * close() closes the underlying handle.
				 */
				syslogPipe[1] = 0;
#endif
				redirection_done = true;
			}

			/* postmaster will never write the file(s); close 'em */
			fclose(syslogFile);
			syslogFile = NULL;
			if (csvlogFile != NULL)
			{
				fclose(csvlogFile);
				csvlogFile = NULL;
			}
			if (jsonlogFile != NULL)
			{
				fclose(jsonlogFile);
				jsonlogFile = NULL;
			}
			/* POLAR */
			if (auditlogFile != NULL)
			{
				fclose(auditlogFile);
				auditlogFile = NULL;
			}
			if (slowlogFile != NULL)
			{
				fclose(slowlogFile);
				slowlogFile = NULL;
			}
			/* POLAR end */
			return (int) sysloggerPid;
	}

	/* we should never reach here */
	return 0;
}


#ifdef EXEC_BACKEND

/*
 * syslogger_fdget() -
 *
 * Utility wrapper to grab the file descriptor of an opened error output
 * file.  Used when building the command to fork the logging collector.
 */
static int
syslogger_fdget(FILE *file)
{
#ifndef WIN32
	if (file != NULL)
		return fileno(file);
	else
		return -1;
#else
	if (file != NULL)
		return (int) _get_osfhandle(_fileno(file));
	else
		return 0;
#endif							/* WIN32 */
}

/*
 * syslogger_fdopen() -
 *
 * Utility wrapper to re-open an error output file, using the given file
 * descriptor.  Used when parsing arguments in a forked logging collector.
 */
static FILE *
syslogger_fdopen(int fd)
{
	FILE	   *file = NULL;

#ifndef WIN32
	if (fd != -1)
	{
		file = fdopen(fd, "a");
		setvbuf(file, NULL, PG_IOLBF, 0);
	}
#else							/* WIN32 */
	if (fd != 0)
	{
		fd = _open_osfhandle(fd, _O_APPEND | _O_TEXT);
		if (fd > 0)
		{
			file = fdopen(fd, "a");
			setvbuf(file, NULL, PG_IOLBF, 0);
		}
	}
#endif							/* WIN32 */

	return file;
}

/*
 * syslogger_forkexec() -
 *
 * Format up the arglist for, then fork and exec, a syslogger process
 */
static pid_t
syslogger_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;
	char		filenobuf[32];
	char		csvfilenobuf[32];
	char		jsonfilenobuf[32];

	av[ac++] = "postgres";
	av[ac++] = "--forklog";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */

	/* static variables (those not passed by write_backend_variables) */
	snprintf(filenobuf, sizeof(filenobuf), "%d",
			 syslogger_fdget(syslogFile));
	av[ac++] = filenobuf;
	snprintf(csvfilenobuf, sizeof(csvfilenobuf), "%d",
			 syslogger_fdget(csvlogFile));
	av[ac++] = csvfilenobuf;
	snprintf(jsonfilenobuf, sizeof(jsonfilenobuf), "%d",
			 syslogger_fdget(jsonlogFile));
	av[ac++] = jsonfilenobuf;

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * syslogger_parseArgs() -
 *
 * Extract data from the arglist for exec'ed syslogger process
 */
static void
syslogger_parseArgs(int argc, char *argv[])
{
	int			fd;

	Assert(argc == 6);
	argv += 3;

	/*
	 * Re-open the error output files that were opened by SysLogger_Start().
	 *
	 * We expect this will always succeed, which is too optimistic, but if it
	 * fails there's not a lot we can do to report the problem anyway.  As
	 * coded, we'll just crash on a null pointer dereference after failure...
	 */
	fd = atoi(*argv++);
	syslogFile = syslogger_fdopen(fd);
	fd = atoi(*argv++);
	csvlogFile = syslogger_fdopen(fd);
	fd = atoi(*argv++);
	jsonlogFile = syslogger_fdopen(fd);
}
#endif							/* EXEC_BACKEND */


/* --------------------------------
 *		pipe protocol handling
 * --------------------------------
 */

/*
 * Process data received through the syslogger pipe.
 *
 * This routine interprets the log pipe protocol which sends log messages as
 * (hopefully atomic) chunks - such chunks are detected and reassembled here.
 *
 * The protocol has a header that starts with two nul bytes, then has a 16 bit
 * length, the pid of the sending process, and a flag to indicate if it is
 * the last chunk in a message. Incomplete chunks are saved until we read some
 * more, and non-final chunks are accumulated until we get the final chunk.
 *
 * All of this is to avoid 2 problems:
 * . partial messages being written to logfiles (messes rotation), and
 * . messages from different backends being interleaved (messages garbled).
 *
 * Any non-protocol messages are written out directly. These should only come
 * from non-PostgreSQL sources, however (e.g. third party libraries writing to
 * stderr).
 *
 * logbuffer is the data input buffer, and *bytes_in_logbuffer is the number
 * of bytes present.  On exit, any not-yet-eaten data is left-justified in
 * logbuffer, and *bytes_in_logbuffer is updated.
 */
static void
process_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	char	   *cursor = logbuffer;
	int			count = *bytes_in_logbuffer;
	int			dest = LOG_DESTINATION_STDERR;

	/* While we have enough for a header, process data... */
	while (count >= (int) (offsetof(PipeProtoHeader, data) + 1))
	{
		PipeProtoHeader p;
		int			chunklen;
		bits8		dest_flags;

		/* Do we have a valid header? */
		memcpy(&p, cursor, offsetof(PipeProtoHeader, data));
		dest_flags = p.flags & (PIPE_PROTO_DEST_STDERR |
								PIPE_PROTO_DEST_CSVLOG |
								PIPE_PROTO_DEST_JSONLOG |
								POLAR_PIPE_PROTO_DEST_AUDITLOG |	/* POLAR: audit and slow
																	 * log */
								POLAR_PIPE_PROTO_DEST_SLOWLOG);
		if (p.nuls[0] == '\0' && p.nuls[1] == '\0' &&
			p.len > 0 && p.pid != 0 &&
			pg_popcount((char *) &dest_flags, 1) == 1)
		{
			List	   *buffer_list;
			ListCell   *cell;
			save_buffer *existing_slot = NULL,
					   *free_slot = NULL;
			StringInfo	str;

			chunklen = PIPE_HEADER_SIZE + p.len;

			/* Fall out of loop if we don't have the whole chunk yet */
			if (count < chunklen)
				break;

			if ((p.flags & PIPE_PROTO_DEST_STDERR) != 0)
				dest = LOG_DESTINATION_STDERR;
			else if ((p.flags & PIPE_PROTO_DEST_CSVLOG) != 0)
				dest = LOG_DESTINATION_CSVLOG;
			else if ((p.flags & PIPE_PROTO_DEST_JSONLOG) != 0)
				dest = LOG_DESTINATION_JSONLOG;
			else if ((p.flags & POLAR_PIPE_PROTO_DEST_AUDITLOG) != 0)
				dest = LOG_DESTINATION_POLAR_AUDITLOG;
			else if ((p.flags & POLAR_PIPE_PROTO_DEST_SLOWLOG) != 0)
				dest = LOG_DESTINATION_POLAR_SLOWLOG;
			else
			{
				/* this should never happen as of the header validation */
				Assert(false);
			}

			/* Locate any existing buffer for this source pid */
			buffer_list = buffer_lists[p.pid % NBUFFER_LISTS];
			foreach(cell, buffer_list)
			{
				save_buffer *buf = (save_buffer *) lfirst(cell);

				if (buf->pid == p.pid)
				{
					existing_slot = buf;
					break;
				}
				if (buf->pid == 0 && free_slot == NULL)
					free_slot = buf;
			}

			if ((p.flags & PIPE_PROTO_IS_LAST) == 0)
			{
				/*
				 * Save a complete non-final chunk in a per-pid buffer
				 */
				if (existing_slot != NULL)
				{
					/* Add chunk to data from preceding chunks */
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
				else
				{
					/* First chunk of message, save in a new buffer */
					if (free_slot == NULL)
					{
						/*
						 * Need a free slot, but there isn't one in the list,
						 * so create a new one and extend the list with it.
						 */
						free_slot = palloc(sizeof(save_buffer));
						buffer_list = lappend(buffer_list, free_slot);
						buffer_lists[p.pid % NBUFFER_LISTS] = buffer_list;
					}
					free_slot->pid = p.pid;
					str = &(free_slot->data);
					initStringInfo(str);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
			}
			else
			{
				/*
				 * Final chunk --- add it to anything saved for that pid, and
				 * either way write the whole thing out.
				 */
				if (existing_slot != NULL)
				{
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
					write_syslogger_file(str->data, str->len, dest);
					/* Mark the buffer unused, and reclaim string storage */
					existing_slot->pid = 0;
					pfree(str->data);
				}
				else
				{
					/* The whole message was one chunk, evidently. */
					write_syslogger_file(cursor + PIPE_HEADER_SIZE, p.len,
										 dest);
				}
			}

			/* Finished processing this chunk */
			cursor += chunklen;
			count -= chunklen;
		}
		else
		{
			/* Process non-protocol data */

			/*
			 * Look for the start of a protocol header.  If found, dump data
			 * up to there and repeat the loop.  Otherwise, dump it all and
			 * fall out of the loop.  (Note: we want to dump it all if at all
			 * possible, so as to avoid dividing non-protocol messages across
			 * logfiles.  We expect that in many scenarios, a non-protocol
			 * message will arrive all in one read(), and we want to respect
			 * the read() boundary if possible.)
			 */
			for (chunklen = 1; chunklen < count; chunklen++)
			{
				if (cursor[chunklen] == '\0')
					break;
			}
			/* fall back on the stderr log as the destination */
			write_syslogger_file(cursor, chunklen, LOG_DESTINATION_STDERR);
			cursor += chunklen;
			count -= chunklen;
		}
	}

	/* We don't have a full chunk, so left-align what remains in the buffer */
	if (count > 0 && cursor != logbuffer)
		memmove(logbuffer, cursor, count);
	*bytes_in_logbuffer = count;
}

/*
 * Force out any buffered data
 *
 * This is currently used only at syslogger shutdown, but could perhaps be
 * useful at other times, so it is careful to leave things in a clean state.
 */
static void
flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	int			i;

	/* POLAR audit log: notice that audit log is never larger than one chunk */
	if (MyLoggerIndex != 0)
	{
		if (*bytes_in_logbuffer > 0)
			write_syslogger_file(logbuffer, *bytes_in_logbuffer,
								 LOG_DESTINATION_POLAR_AUDITLOG);
		flush_syslogger_file(LOG_DESTINATION_POLAR_AUDITLOG);
		*bytes_in_logbuffer = 0;
		return;
	}

	/* Dump any incomplete protocol messages */
	for (i = 0; i < NBUFFER_LISTS; i++)
	{
		List	   *list = buffer_lists[i];
		ListCell   *cell;

		foreach(cell, list)
		{
			save_buffer *buf = (save_buffer *) lfirst(cell);

			if (buf->pid != 0)
			{
				StringInfo	str = &(buf->data);

				write_syslogger_file(str->data, str->len,
									 LOG_DESTINATION_STDERR);
				/* Mark the buffer unused, and reclaim string storage */
				buf->pid = 0;
				pfree(str->data);
			}
		}
	}

	/*
	 * Force out any remaining pipe data as-is; we don't bother trying to
	 * remove any protocol headers that may exist in it.
	 */
	if (*bytes_in_logbuffer > 0)
		write_syslogger_file(logbuffer, *bytes_in_logbuffer,
							 LOG_DESTINATION_STDERR);
	flush_syslogger_file(LOG_DESTINATION_STDERR);	/* POLAR */
	*bytes_in_logbuffer = 0;
}


/* --------------------------------
 *		logfile routines
 * --------------------------------
 */

/*
 * Write text to the currently open logfile
 *
 * This is exported so that elog.c can call it when MyBackendType is B_LOGGER.
 * This allows the syslogger process to record elog messages of its own,
 * even though its stderr does not point at the syslog pipe.
 */
void
write_syslogger_file(const char *buffer, int count, int destination)
{
	int			rc;
	FILE	   *logfile;

	/*
	 * If we're told to write to a structured log file, but it's not open,
	 * dump the data to syslogFile (which is always open) instead.  This can
	 * happen if structured output is enabled after postmaster start and we've
	 * been unable to open logFile.  There are also race conditions during a
	 * parameter change whereby backends might send us structured output
	 * before we open the logFile or after we close it.  Writing formatted
	 * output to the regular log file isn't great, but it beats dropping log
	 * output on the floor.
	 *
	 * Think not to improve this by trying to open logFile on-the-fly.  Any
	 * failure in that would lead to recursion.
	 */
	logfile = get_logfile_from_dest(destination);

	rc = fwrite(buffer, 1, count, logfile);

	/*
	 * Try to report any failure.  We mustn't use ereport because it would
	 * just recurse right back here, but write_stderr is OK: it will write
	 * either to the postmaster's original stderr, or to /dev/null, but never
	 * to our input pipe which would result in a different sort of looping.
	 */
	if (rc != count)
		write_stderr("could not write to log file: %s\n", strerror(errno));
}

#ifdef WIN32

/*
 * Worker thread to transfer data from the pipe to the current logfile.
 *
 * We need this because on Windows, WaitForMultipleObjects does not work on
 * unnamed pipes: it always reports "signaled", so the blocking ReadFile won't
 * allow for SIGHUP; and select is for sockets only.
 */
static unsigned int __stdcall
pipeThread(void *arg)
{
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;

	for (;;)
	{
		DWORD		bytesRead;
		BOOL		result;

		result = ReadFile(syslogPipe[0],
						  logbuffer + bytes_in_logbuffer,
						  sizeof(logbuffer) - bytes_in_logbuffer,
						  &bytesRead, 0);

		/*
		 * Enter critical section before doing anything that might touch
		 * global state shared by the main thread. Anything that uses
		 * palloc()/pfree() in particular are not safe outside the critical
		 * section.
		 */
		EnterCriticalSection(&sysloggerSection);
		if (!result)
		{
			DWORD		error = GetLastError();

			if (error == ERROR_HANDLE_EOF ||
				error == ERROR_BROKEN_PIPE)
				break;
			_dosmaperr(error);
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read from logger pipe: %m")));
		}
		else if (bytesRead > 0)
		{
			bytes_in_logbuffer += bytesRead;
			process_pipe_input(logbuffer, &bytes_in_logbuffer);
		}

		/*
		 * If we've filled the current logfile, nudge the main thread to do a
		 * log rotation.
		 */
		if (Log_RotationSize > 0)
		{
			if (ftell(syslogFile) >= Log_RotationSize * 1024L ||
				(csvlogFile != NULL && ftell(csvlogFile) >= Log_RotationSize * 1024L) ||
				(jsonlogFile != NULL && ftell(jsonlogFile) >= Log_RotationSize * 1024L))
				SetLatch(MyLatch);
		}
		LeaveCriticalSection(&sysloggerSection);
	}

	/* We exit the above loop only upon detecting pipe EOF */
	pipe_eof_seen = true;

	/* if there's any data left then force it out now */
	flush_pipe_input(logbuffer, &bytes_in_logbuffer);

	/* set the latch to waken the main thread, which will quit */
	SetLatch(MyLatch);

	LeaveCriticalSection(&sysloggerSection);
	_endthread();
	return 0;
}
#endif							/* WIN32 */

/*
 * Open a new logfile with proper permissions and buffering options.
 *
 * If allow_errors is true, we just log any open failure and return NULL
 * (with errno still correct for the fopen failure).
 * Otherwise, errors are treated as fatal.
 */
static FILE *
logfile_open(const char *filename, const char *mode, bool allow_errors)
{
	FILE	   *fh;
	mode_t		oumask;

	/*
	 * Note we do not let Log_file_mode disable IWUSR, since we certainly want
	 * to be able to write the files ourselves.
	 */
	oumask = umask((mode_t) ((~(Log_file_mode | S_IWUSR)) & (S_IRWXU | S_IRWXG | S_IRWXO)));
	fh = fopen(filename, mode);
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* use CRLF line endings on Windows */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		int			save_errno = errno;

		ereport(allow_errors ? LOG : FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open log file \"%s\": %m",
						filename)));
		errno = save_errno;
	}

	return fh;
}

/*
 * Do logfile rotation for a single destination, as specified by target_dest.
 * The information stored in *last_file_name and *logFile is updated on a
 * successful file rotation.
 *
 * Returns false if the rotation has been stopped, or true to move on to
 * the processing of other formats.
 */
static bool
logfile_rotate_dest(bool time_based_rotation, int size_rotation_for,
					pg_time_t fntime, int target_dest,
					char **last_file_name, FILE **logFile)
{
	char	   *logFileExt = NULL;
	char	   *filename;
	FILE	   *fh;

	/*
	 * If the target destination was just turned off, close the previous file
	 * and unregister its data.  This cannot happen for stderr as syslogFile
	 * is assumed to be always opened even if stderr is disabled in
	 * log_destination.
	 */
	if ((Log_destination & target_dest) == 0 &&
		target_dest != LOG_DESTINATION_STDERR)
	{
		if (*logFile != NULL)
			fclose(*logFile);
		*logFile = NULL;
		if (*last_file_name != NULL)
			pfree(*last_file_name);
		*last_file_name = NULL;
		return true;
	}

	/*
	 * Leave if it is not time for a rotation or if the target destination has
	 * no need to do a rotation based on the size of its file.
	 */
	if (!time_based_rotation && (size_rotation_for & target_dest) == 0)
		return true;

	/* file extension depends on the destination type */
	if (target_dest == LOG_DESTINATION_STDERR)
		logFileExt = SYSLOG_SUFFIX; /* POLAR */
	else if (target_dest == LOG_DESTINATION_CSVLOG)
		logFileExt = ".csv";
	else if (target_dest == LOG_DESTINATION_JSONLOG)
		logFileExt = ".json";
	else if (target_dest == LOG_DESTINATION_POLAR_AUDITLOG)
		logFileExt = AUDITLOG_SUFFIX;
	else if (target_dest == LOG_DESTINATION_POLAR_SLOWLOG)
		logFileExt = SLOWLOG_SUFFIX;
	else
	{
		/* cannot happen */
		Assert(false);
	}

	/* build the new file name */
	filename = logfile_getname(fntime, logFileExt);

	/*
	 * Decide whether to overwrite or append.  We can overwrite if (a)
	 * Log_truncate_on_rotation is set, (b) the rotation was triggered by
	 * elapsed time and not something else, and (c) the computed file name is
	 * different from what we were previously logging into.
	 */
	if (Log_truncate_on_rotation && time_based_rotation &&
		*last_file_name != NULL &&
		strcmp(filename, *last_file_name) != 0)
	{
		polar_drop_log_page_cache(*last_file_name); /* POLAR */
		fh = logfile_open(filename, "w", true);
	}
	else
		fh = logfile_open(filename, "a", true);

	if (!fh)
	{
		/*
		 * ENFILE/EMFILE are not too surprising on a busy system; just keep
		 * using the old file till we manage to get a new one.  Otherwise,
		 * assume something's wrong with Log_directory and stop trying to
		 * create files.
		 */
		if (errno != ENFILE && errno != EMFILE)
		{
			ereport(LOG,
					(errmsg("disabling automatic rotation (use SIGHUP to re-enable)")));
			rotation_disabled = true;
		}

		if (filename)
			pfree(filename);
		return false;
	}

	/* fill in the new information */
	if (*logFile != NULL)
		fclose(*logFile);
	*logFile = fh;

	/* instead of pfree'ing filename, remember it for next time */
	if (*last_file_name != NULL)
		pfree(*last_file_name);
	*last_file_name = filename;

	return true;
}

/*
 * perform logfile rotation
 */
static void
logfile_rotate(bool time_based_rotation, int size_rotation_for)
{
	pg_time_t	fntime;

	rotation_requested = false;

	/*
	 * When doing a time-based rotation, invent the new logfile name based on
	 * the planned rotation time, not current time, to avoid "slippage" in the
	 * file name when we don't do the rotation immediately.
	 */
	if (time_based_rotation)
		fntime = next_rotation_time;
	else
		fntime = time(NULL);

	/* file rotation for stderr */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_STDERR, &last_sys_file_name,
							 &syslogFile))
		return;

	/* file rotation for csvlog */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_CSVLOG, &last_csv_file_name,
							 &csvlogFile))
		return;

	/* file rotation for jsonlog */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_JSONLOG, &last_json_file_name,
							 &jsonlogFile))
		return;

	/* POLAR: file rotation for audit log */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_POLAR_AUDITLOG, &polar_last_audit_file_name,
							 &auditlogFile))
		return;

	/* POLAR: file rotation for slow log */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_POLAR_SLOWLOG, &polar_last_slowlog_file_name,
							 &slowlogFile))
		return;

	update_metainfo_datafile();

	set_next_rotation_time();
}


/*
 * construct logfile name using timestamp information
 *
 * If suffix isn't NULL, append it to the name, replacing any ".log"
 * that may be in the pattern.
 *
 * Result is palloc'd.
 */
static char *
logfile_getname(pg_time_t timestamp, const char *suffix)
{
	char	   *filename;
	int			len;
	char		loggerIndexStr[10];

	filename = palloc(MAXPGPATH);

	snprintf(filename, MAXPGPATH, "%s/", Log_directory);

	len = strlen(filename);

	/* treat Log_filename as a strftime pattern */
	pg_strftime(filename + len, MAXPGPATH - len, Log_filename,
				pg_localtime(&timestamp, log_timezone));

	if (suffix != NULL)
	{
		len = strlen(filename);
		if (len > 4 && (strcmp(filename + (len - 4), ".log") == 0))
			len -= 4;

		/* POLAR: add log index to log file name */
		if (strcmp(suffix, AUDITLOG_SUFFIX) == 0)
		{
			snprintf(loggerIndexStr, 10, "_%d", MyLoggerIndex);
			strlcpy(filename + len, loggerIndexStr, MAXPGPATH - len);
			len = strlen(filename);
		}
		/* POLAR END */

		strlcpy(filename + len, suffix, MAXPGPATH - len);
	}

	/* POLAR */
	len = strlen(filename);
	filename[len] = '\0';

	return filename;
}

/*
 * Determine the next planned rotation time, and store in next_rotation_time.
 */
static void
set_next_rotation_time(void)
{
	pg_time_t	now;
	struct pg_tm *tm;
	int			rotinterval;

	/* nothing to do if time-based rotation is disabled */
	if (Log_RotationAge <= 0)
		return;

	/*
	 * The requirements here are to choose the next time > now that is a
	 * "multiple" of the log rotation interval.  "Multiple" can be interpreted
	 * fairly loosely.  In this version we align to log_timezone rather than
	 * GMT.
	 */
	rotinterval = Log_RotationAge * SECS_PER_MINUTE;	/* convert to seconds */
	now = (pg_time_t) time(NULL);
	tm = pg_localtime(&now, log_timezone);
	now += tm->tm_gmtoff;
	now -= now % rotinterval;
	now += rotinterval;
	now -= tm->tm_gmtoff;
	next_rotation_time = now;
}

/*
 * Store the name of the file(s) where the log collector, when enabled, writes
 * log messages.  Useful for finding the name(s) of the current log file(s)
 * when there is time-based logfile rotation.  Filenames are stored in a
 * temporary file and which is renamed into the final destination for
 * atomicity.  The file is opened with the same permissions as what gets
 * created in the data directory and has proper buffering options.
 */
static void
update_metainfo_datafile(void)
{
	FILE	   *fh;
	mode_t		oumask;

	if (!(Log_destination & LOG_DESTINATION_STDERR) &&
		!(Log_destination & LOG_DESTINATION_CSVLOG) &&
		!(Log_destination & LOG_DESTINATION_JSONLOG) &&
		!(Log_destination & LOG_DESTINATION_POLAR_AUDITLOG) &&
		!(Log_destination & LOG_DESTINATION_POLAR_SLOWLOG)
		)
	{
		if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							LOG_METAINFO_DATAFILE)));
		return;
	}

	/* use the same permissions as the data directory for the new file */
	oumask = umask(pg_mode_mask);
	fh = fopen(LOG_METAINFO_DATAFILE_TMP, "w");
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* use CRLF line endings on Windows */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP)));
		return;
	}

	if (last_sys_file_name && (Log_destination & LOG_DESTINATION_STDERR))
	{
		if (fprintf(fh, "stderr %s\n", last_sys_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_csv_file_name && (Log_destination & LOG_DESTINATION_CSVLOG))
	{
		if (fprintf(fh, "csvlog %s\n", last_csv_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_json_file_name && (Log_destination & LOG_DESTINATION_JSONLOG))
	{
		if (fprintf(fh, "jsonlog %s\n", last_json_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	/* POLAR */
	if (polar_last_audit_file_name && (Log_destination & LOG_DESTINATION_POLAR_AUDITLOG))
	{
		if (fprintf(fh, "auditlog %s\n", polar_last_audit_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (polar_last_slowlog_file_name && (Log_destination & LOG_DESTINATION_POLAR_SLOWLOG))
	{
		if (fprintf(fh, "slowlog %s\n", polar_last_slowlog_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}
	/* POLAR end */
	fclose(fh);

	if (rename(LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE)));
}

/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/*
 * Check to see if a log rotation request has arrived.  Should be
 * called by postmaster after receiving SIGUSR1.
 */
bool
CheckLogrotateSignal(void)
{
	struct stat stat_buf;

	if (stat(LOGROTATE_SIGNAL_FILE, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * Remove the file signaling a log rotation request.
 */
void
RemoveLogrotateSignalFiles(void)
{
	unlink(LOGROTATE_SIGNAL_FILE);
}

/* SIGUSR1: set flag to rotate logfile */
static void
sigUsr1Handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	rotation_requested = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * POLAR: remove oldest log file
 *
 * Remove oldest log file from log directory
 */
static void
polar_remove_old_syslog_files()
{
	DIR		   *logdir;
	struct dirent *logde;
	char		oldest_path[MAXPGPATH];
	char		oldest_auditlog_path[MAXPGPATH];
	char		oldest_slowlog_path[MAXPGPATH];
	char		path[MAXPGPATH];
	int			i = 0;
	int			num_log_files = 0;
	int			num_auditlog_files = 0;
	int			num_slowlog_files = 0;
	int			log_prefix_pos = 0;

	/* if polar_max_log_files is disabled, we should skip it */
	if (polar_max_log_files < 0 &&
		polar_max_auditlog_files < 0 && polar_max_slowlog_files < 0)
		return;
	logdir = AllocateDir(Log_directory);
	if (logdir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open error log directory \"%s\": %m",
						Log_directory)));
	/* Get first '%' postion in log_filename, NB:this is a single line */
	for (i = 0; Log_filename[i] != '\0' && Log_filename[i] != '%'; i++);
	/* save first '%' postion */
	log_prefix_pos = i;
	/* Init our result path, mark it invalid */
	oldest_path[0] = '\0';
	oldest_auditlog_path[0] = '\0';
	oldest_slowlog_path[0] = '\0';
	while ((logde = ReadDir(logdir, Log_directory)) != NULL)
	{
		/* Skip special stuff */
		if (strcmp(logde->d_name, ".") == 0 || strcmp(logde->d_name, "..") == 0)
			continue;

		/*
		 * skip not pg log prefix file and choose minimum log file name.
		 */
		if (strncmp(logde->d_name, Log_filename, log_prefix_pos) == 0)
		{
			/* increase log file num */
			if (strstr(logde->d_name, AUDITLOG_SUFFIX) != NULL)
			{
				/* audit log */
				num_auditlog_files++;
				if (oldest_auditlog_path[0] == '\0' ||
					strcmp(logde->d_name, oldest_auditlog_path) < 0)
				{
					/* save it */
					strcpy(oldest_auditlog_path, logde->d_name);
				}
			}
			else if (strstr(logde->d_name, SLOWLOG_SUFFIX) != NULL)
			{
				/* slow log */
				num_slowlog_files++;
				if (oldest_slowlog_path[0] == '\0' ||
					strcmp(logde->d_name, oldest_slowlog_path) < 0)
				{
					/* save it */
					strcpy(oldest_slowlog_path, logde->d_name);
				}
			}
			else
			{
				num_log_files++;
				if (oldest_path[0] == '\0' || strcmp(logde->d_name, oldest_path) < 0)
				{
					/* save it */
					strcpy(oldest_path, logde->d_name);
				}
			}
		}
	}
	if (num_auditlog_files <= polar_max_auditlog_files
		&& num_log_files <= polar_max_log_files
		&& num_slowlog_files <= polar_max_slowlog_files)
	{
		FreeDir(logdir);
		return;
	}
	if (polar_max_auditlog_files > 0 && num_auditlog_files > polar_max_auditlog_files)
	{
		memset(path, 0, MAXPGPATH);
		snprintf(path, MAXPGPATH, "%s/%s", Log_directory, oldest_auditlog_path);
		elog(DEBUG2, "attempting to remove oldest audit log file %s", path);
		polar_remove_log_file(path);
	}
	if (polar_max_slowlog_files > 0 && num_slowlog_files > polar_max_slowlog_files)
	{
		memset(path, 0, MAXPGPATH);
		snprintf(path, MAXPGPATH, "%s/%s", Log_directory, oldest_slowlog_path);
		elog(DEBUG2, "attempting to remove oldest slow log file %s", path);
		polar_remove_log_file(path);
	}
	if (polar_max_log_files > 0 && num_log_files > polar_max_log_files)
	{
		memset(path, 0, MAXPGPATH);
		snprintf(path, MAXPGPATH, "%s/%s", Log_directory, oldest_path);
		elog(DEBUG2, "attempting to remove oldest log file %s", path);
		polar_remove_log_file(path);
	}
	FreeDir(logdir);
	return;
}

/*
 * POLAR: remove the page cache of old log file.
 */
static void
polar_drop_log_page_cache(const char *filename)
{
	int			last_log_fd;
	int			ret;

	last_log_fd = open(filename, O_RDONLY);
	if (last_log_fd < 0)
	{
		ereport(LOG,
				(errmsg("the old log file doesn't exist")));
	}
	ret = posix_fadvise(last_log_fd, 0, 0, POSIX_FADV_DONTNEED);
	/* return zero means success */
	if (ret != 0)
	{
		ereport(LOG,
				(errmsg("try to drop the old log page cache fail. continue run")));
	}
	close(last_log_fd);
}

/*
 * POLAR: remove log file
 */
static void
polar_remove_log_file(const char *path)
{
	int			rc;
#ifdef WIN32
	char		newpath[MAXPGPATH];

	/*
	 * On Windows, if another process (e.g another backend) holds the file
	 * open in FILE_SHARE_DELETE mode, unlink will succeed, but the file will
	 * still show up in directory listing until the last handle is closed. To
	 * avoid confusing the lingering deleted file for a live WAL file that
	 * needs to be archived, rename it before deleting it.
	 *
	 * If another process holds the file open without FILE_SHARE_DELETE flag,
	 * rename will fail. We'll try again at the next checkpoint.
	 */
	snprintf(newpath, MAXPGPATH, "%s.deleted", path);
	if (rename(path, newpath) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename old error log file \"%s\": %m",
						path)));
		return;
	}
	rc = unlink(newpath);
#else
	rc = unlink(path);
#endif
	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove old error log file \"%s\": %m",
						path)));
	}
	return;
}

/* POLAR */
/*
 * flush log buffer
 */
void
flush_syslogger_file(int destination)
{
	FILE	   *logfile = get_logfile_from_dest(destination);

	fflush(logfile);
}

/* Just a copy of logfile_open, add buffer_mode parameter */
static FILE *
logfile_open_with_buffer_mode(const char *filename, const char *mode, bool allow_errors,
							  int buffer_mode)
{
	FILE	   *fh;
	mode_t		oumask;

	/*
	 * Note we do not let Log_file_mode disable IWUSR, since we certainly want
	 * to be able to write the files ourselves.
	 */
	oumask = umask((mode_t) ((~(Log_file_mode | S_IWUSR)) & (S_IRWXU | S_IRWXG | S_IRWXO)));
	fh = fopen(filename, mode);
	umask(oumask);
	if (fh)
	{
		setvbuf(fh, NULL, buffer_mode, 0);
#ifdef WIN32
		/* use CRLF line endings on Windows */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		int			save_errno = errno;

		ereport(allow_errors ? LOG : FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open log file \"%s\": %m",
						filename)));
		errno = save_errno;
	}
	return fh;
}

/*
 * get logfile handler from destinaction
 */
static inline FILE *
get_logfile_from_dest(int destination)
{
	if (destination == LOG_DESTINATION_CSVLOG && csvlogFile != NULL)
		return csvlogFile;
	if (destination == LOG_DESTINATION_JSONLOG && jsonlogFile != NULL)
		return jsonlogFile;
	if (destination == LOG_DESTINATION_POLAR_AUDITLOG && auditlogFile != NULL)
		return auditlogFile;
	if (destination == LOG_DESTINATION_POLAR_SLOWLOG && slowlogFile != NULL)
		return slowlogFile;
	return syslogFile;
}

/* POLAR end */
