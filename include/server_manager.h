#ifndef SERVER_MANAGER
#define SERVER_MANAGER

#include <iostream>
#include <string.h>
#include <ctime>
#include <iomanip>      // For stringstream in current_timestamp
#include <fcntl.h>      // For O_RDONLY, O_WRONLY, etc.
#include <unistd.h>     // For close, read, write, etc.
#include <sys/wait.h>
#include <poll.h>
#include <map>
#include <queue>
#include <sys/inotify.h>
using namespace std;

#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"
#define RESET   "\033[0m"

#define MAXBUFF 1024
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

extern int worker_limit;
extern volatile sig_atomic_t empty_spot;

// Worker execution report
struct Report {
    char timestamp[64];
    char source_dir[256];
    char target_dir[256];
    char filename[256];
    pid_t worker_pid;
    char operation[64];
    char result[64];
    char details[256];
    int errors;
};

// Information about a monitored source_dir
struct SyncInfo {
    string source_dir;
    string target_dir;
    string status;      // "Active" or "Not active" (whether the directory is currently monitored)
    string last_sync_time;
    bool active;        // (.active ? "Yes" : "No") whether a sync operation is currently in progress
    int eror_count;
};

// Task stored in the waiting queue when no worker processes are available
struct Operation {
    SyncInfo pair;
    string filename;    // Target filename for the worker
    string operation;   // Operation command for the worker
};

using SyncMap = map<string, SyncInfo>;
using WatchMap = map<int, SyncInfo>;

// Monitors worker pipes, the fss_console input named pipe, and inotify events using poll()
void sync_fds(SyncMap file_pairs, queue<Operation> worker_queue, map<int, SyncInfo> watch_descr, map<string, string> console_operations, 
    struct pollfd fds[], const int total_fds, int active_workers, const int fss_out, const int log_file_fd);

// Checks monitored directories for inotify events
void check_watch(SyncMap& file_pairs, WatchMap watch_descr, queue<Operation>& worker_queue, struct pollfd fds[], const int total_fds, int &active_workers);

// Checks if any worker has finished and reads its execution report
void check_worker_pipes(SyncMap& file_pairs, queue<Operation>& worker_queue, map<string, string>& console_operations, const int fss_out, 
    struct pollfd fds[], int &active_workers, const int log_file_fd);

// Checks for incoming commands from fss_console via the named pipe
int check_named_pipes(SyncMap& file_pairs, queue<Operation>& worker_queue, WatchMap& watch_descr, map<string, string>& console_operations, 
    struct pollfd fds[], const int fss_out, int &active_workers, const int log_file_fd);

// Checks if a task exists in the queue (true: exists, false: does not exist)
bool find_in_queue(queue<Operation> queue, SyncInfo info);

// Formats and returns the status message for a directory
char* print_status(char* mes, SyncInfo info);

// Splits a string by spaces (" ") into 'num_parts' tokens
char **split_word(const char* word, const int num_parts);

// Frees memory allocated by split_word()
void free_split(char** parts, const int num_parts);

// Spawns a new worker process using fork() and exec()
void worker_process(SyncInfo info, const string filename, const string operation, const int worker_pipe[2]);

// Reads directory pairs from the config file and stores them in a map
map<string, SyncInfo> load_config_file(const char* filename, const int log_file_fd);

// Generates the current timestamp
string current_timestamp();

/* Signal handler:
   Sets the flag to true when a worker terminates, 
   indicating that a slot is available to spawn a new worker. */
void signal_handler(int signum);

#endif // SERVER_MANAGER
