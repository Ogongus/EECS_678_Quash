/**
 * @file execute.c
 *
 * @brief Implements interface functions between Quash and the environment and
 * functions that interpret and execute commands.
 */

#include "execute.h"
#include "quash.h"
#include "command.h"
#include "deque.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>

/***************************************************************************
 * Deque type definitions for job/pid management
 ***************************************************************************/

IMPLEMENT_DEQUE_STRUCT(PidList, pid_t);
IMPLEMENT_DEQUE(PidList, pid_t);

/**
 * @brief Represents a background job tracked by Quash
 */
typedef struct Job {
  int   job_id;    /**< Unique job identifier */
  pid_t first_pid; /**< PID of the first process in the job */
  char* cmd;       /**< malloc'd copy of the command string */
} Job;

IMPLEMENT_DEQUE_STRUCT(JobList, Job);
IMPLEMENT_DEQUE(JobList, Job);

/***************************************************************************
 * Global state
 ***************************************************************************/

/** Background job queue */
static JobList job_list;
static bool    job_list_initialized = false;
static int     next_job_id = 1; /**< Monotonically increasing job ID */

/** Per-script execution state (reset in run_script, used by create_process) */
static int     pipe_read_fd = -1; /**< Read-end of the previous pipe */
static PidList current_pids;      /**< PIDs spawned in the current script */

/***************************************************************************
 * Interface Functions
 ***************************************************************************/

// Return a string containing the current working directory.
char* get_current_directory(bool* should_free) {
  static char buf[4096];
  if (getcwd(buf, sizeof(buf)) != NULL) {
    *should_free = false;
    return buf;
  }
  *should_free = false;
  return "/";
}

// Returns the value of an environment variable env_var
const char* lookup_env(const char* env_var) {
  if (env_var == NULL) return "";

  const char* val = getenv(env_var);

  // Special fallback for HOSTNAME if unset in environment
  if (strcmp(env_var, "HOSTNAME") == 0) {
    if (val == NULL || *val == '\0') {
      static char hostname[256];
      if (gethostname(hostname, sizeof(hostname)) == 0)
        return hostname;
      return "localhost";
    }
  }

  return val ? val : "";
}

// Set an environment variable
void write_env(const char* env_var, const char* val) {
  if (env_var == NULL) return;
  if (val == NULL || *val == '\0')
    unsetenv(env_var);
  else
    setenv(env_var, val, 1);
}

// Check the status of background jobs; remove and announce any that finished
void check_jobs_bg_status() {
  if (!job_list_initialized) return;

  size_t len = length_JobList(&job_list);

  for (size_t i = 0; i < len; i++) {
    Job job = pop_front_JobList(&job_list);

    int status;
    pid_t result = waitpid(job.first_pid, &status, WNOHANG);

    if (result == 0) {
      // Still running - put it back at the end of the queue
      push_back_JobList(&job_list, job);
    } else {
      // Completed (result > 0) or already gone (result == -1 / ECHILD)
      print_job_bg_complete(job.job_id, job.first_pid, job.cmd);
      // Reap any other children that may belong to this job
      while (waitpid(-1, NULL, WNOHANG) > 0);
      free(job.cmd);
    }
  }
}

// Prints the job id number, the process id of the first process belonging to
// the Job, and the command string associated with this job
void print_job(int job_id, pid_t pid, const char* cmd) {
  printf("[%d]\t%8d\t%s\n", job_id, pid, cmd);
  fflush(stdout);
}

// Prints a start up message for background processes
void print_job_bg_start(int job_id, pid_t pid, const char* cmd) {
  printf("Background job started: ");
  print_job(job_id, pid, cmd);
}

// Prints a completion message followed by the print job
void print_job_bg_complete(int job_id, pid_t pid, const char* cmd) {
  printf("Completed: \t");
  print_job(job_id, pid, cmd);
}

/***************************************************************************
 * Functions to process commands
 ***************************************************************************/

// Run a program reachable by PATH, relative path, or absolute path
void run_generic(GenericCommand cmd) {
  char*  exec = cmd.args[0];
  char** args = cmd.args;

  execvp(exec, args);
  perror("ERROR: Failed to execute program");
  exit(EXIT_FAILURE);
}

// Print strings separated by spaces followed by a newline
void run_echo(EchoCommand cmd) {
  char** str = cmd.args;

  for (int i = 0; str[i] != NULL; i++) {
    if (i > 0) printf(" ");
    printf("%s", str[i]);
  }
  printf("\n");

  fflush(stdout);
}

// Sets an environment variable
void run_export(ExportCommand cmd) {
  write_env(cmd.env_var, cmd.val);
}

// Changes the current working directory and updates PWD
void run_cd(CDCommand cmd) {
  const char* dir = cmd.dir;

  if (dir == NULL) {
    perror("ERROR: Failed to resolve path");
    return;
  }

  char resolved[PATH_MAX];
  if (realpath(dir, resolved) == NULL) {
    perror("ERROR: Failed to resolve path");
    return;
  }

  if (chdir(resolved) != 0) {
    perror("ERROR: Failed to change directory");
    return;
  }

  write_env("PWD", resolved);
}

// Sends a signal to the first process of the given background job
void run_kill(KillCommand cmd) {
  int signal = cmd.sig;
  int job_id = cmd.job;

  if (!job_list_initialized) {
    return;
  }

  size_t len = length_JobList(&job_list);
  for (size_t i = 0; i < len; i++) {
    Job job = pop_front_JobList(&job_list);
    if (job.job_id == job_id)
      kill(job.first_pid, signal);
    push_back_JobList(&job_list, job);
  }
}

// Prints the current working directory to stdout
void run_pwd() {
  bool  should_free = false;
  char* cwd = get_current_directory(&should_free);
  printf("%s\n", cwd);
  if (should_free) free(cwd);
  fflush(stdout);
}

// Prints all background jobs currently in the job list to stdout
void run_jobs() {
  if (!job_list_initialized) {
    fflush(stdout);
    return;
  }

  size_t len = length_JobList(&job_list);
  for (size_t i = 0; i < len; i++) {
    Job job = pop_front_JobList(&job_list);
    print_job(job.job_id, job.first_pid, job.cmd);
    push_back_JobList(&job_list, job);
  }

  fflush(stdout);
}

/***************************************************************************
 * Functions for command resolution and process setup
 ***************************************************************************/

/**
 * @brief Dispatch table for commands that run inside a child process
 */
void child_run_command(Command cmd) {
  CommandType type = get_command_type(cmd);

  switch (type) {
  case GENERIC:
    run_generic(cmd.generic);
    break;
  case ECHO:
    run_echo(cmd.echo);
    break;
  case PWD:
    run_pwd();
    break;
  case JOBS:
    run_jobs();
    break;
  case EXPORT:
  case CD:
  case KILL:
  case EXIT:
  case EOC:
    break;
  default:
    fprintf(stderr, "Unknown command type: %d\n", type);
  }
}

/**
 * @brief Dispatch table for commands that must run in the parent (quash) process
 */
void parent_run_command(Command cmd) {
  CommandType type = get_command_type(cmd);

  switch (type) {
  case EXPORT:
    run_export(cmd.export);
    break;
  case CD:
    run_cd(cmd.cd);
    break;
  case KILL:
    run_kill(cmd.kill);
    break;
  case GENERIC:
  case ECHO:
  case PWD:
  case JOBS:
  case EXIT:
  case EOC:
    break;
  default:
    fprintf(stderr, "Unknown command type: %d\n", type);
  }
}

/**
 * @brief Fork a child process, wire up any pipes / redirects, and execute
 *        the command. Updates global pipe_read_fd and current_pids.
 */
void create_process(CommandHolder holder) {
  bool p_in  = holder.flags & PIPE_IN;
  bool p_out = holder.flags & PIPE_OUT;
  bool r_in  = holder.flags & REDIRECT_IN;
  bool r_app = holder.flags & REDIRECT_APPEND;
  bool r_out = (holder.flags & REDIRECT_OUT) || r_app;

  int new_pipe[2] = {-1, -1};

  // Create the pipe that this process will write into (if needed)
  if (p_out) {
    if (pipe(new_pipe) < 0) {
      perror("ERROR: Failed to create pipe");
      return;
    }
  }

  pid_t pid = fork();

  if (pid < 0) {
    perror("ERROR: Failed to fork");
    return;
  }

  if (pid == 0) {
    /* ---- Child process ---- */

    // Consume the read-end of the previous pipe as stdin
    if (p_in && pipe_read_fd != -1) {
      dup2(pipe_read_fd, STDIN_FILENO);
      close(pipe_read_fd);
    }

    // Wire the write-end of the new pipe to stdout
    if (p_out) {
      close(new_pipe[0]);          // child doesn't read from this pipe
      dup2(new_pipe[1], STDOUT_FILENO);
      close(new_pipe[1]);
    }

    // Redirect stdin from a file
    if (r_in) {
      int fd = open(holder.redirect_in, O_RDONLY);
      if (fd < 0) {
        perror("ERROR: Failed to open redirect input file");
        exit(EXIT_FAILURE);
      }
      dup2(fd, STDIN_FILENO);
      close(fd);
    }

    // Redirect stdout to a file (truncate or append)
    if (r_out) {
      int oflags = O_WRONLY | O_CREAT | (r_app ? O_APPEND : O_TRUNC);
      int fd = open(holder.redirect_out, oflags, 0644);
      if (fd < 0) {
        perror("ERROR: Failed to open redirect output file");
        exit(EXIT_FAILURE);
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }

    child_run_command(holder.cmd);
    exit(EXIT_SUCCESS);

  } else {
    /* ---- Parent process ---- */

    // Close the read-end of the previous pipe now that the child has it
    if (p_in && pipe_read_fd != -1) {
      close(pipe_read_fd);
      pipe_read_fd = -1;
    }

    // Hand the read-end of the new pipe to the next create_process call
    if (p_out) {
      close(new_pipe[1]);          // parent doesn't write into this pipe
      pipe_read_fd = new_pipe[0];
    }

    // Execute parent-only commands (cd, export, kill)
    parent_run_command(holder.cmd);

    push_back_PidList(&current_pids, pid);
  }
}

// Clean up the job list - called via atexit in quash.c
void destroy_job_list() {
  if (job_list_initialized) {
    while (!is_empty_JobList(&job_list)) {
      Job job = pop_front_JobList(&job_list);
      free(job.cmd);
    }
    destroy_JobList(&job_list);
    job_list_initialized = false;
  }
}

// Run a list of commands
void run_script(CommandHolder* holders) {
  if (holders == NULL)
    return;

  check_jobs_bg_status();

  if (get_command_holder_type(holders[0]) == EXIT &&
      get_command_holder_type(holders[1]) == EOC) {
    end_main_loop();
    return;
  }

  // Lazily initialise the job list once
  if (!job_list_initialized) {
    job_list = new_JobList(10);
    job_list_initialized = true;
  }

  // Reset per-script state
  current_pids = new_PidList(8);
  pipe_read_fd = -1;

  CommandType type;

  // Fork a process for every command in the holder array
  for (int i = 0; (type = get_command_holder_type(holders[i])) != EOC; ++i)
    create_process(holders[i]);

  // Close any dangling pipe read-end left over from the last command
  if (pipe_read_fd != -1) {
    close(pipe_read_fd);
    pipe_read_fd = -1;
  }

  if (!(holders[0].flags & BACKGROUND)) {
    /* ---- Foreground job: wait for every child ---- */
    while (!is_empty_PidList(&current_pids)) {
      pid_t pid = pop_front_PidList(&current_pids);
      int status;
      waitpid(pid, &status, 0);
    }
    destroy_PidList(&current_pids);

  } else {
    /* ---- Background job: register in the job list ---- */
    pid_t first_pid = peek_front_PidList(&current_pids);
    destroy_PidList(&current_pids);

    int job_id = next_job_id++;

    char* cmd = get_command_string(); // strdup'd - freed when job completes

    Job new_job = { job_id, first_pid, cmd };
    push_back_JobList(&job_list, new_job);

    print_job_bg_start(job_id, first_pid, cmd);
  }
}