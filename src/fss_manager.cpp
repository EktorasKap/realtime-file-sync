#include <iostream>
#include <string.h>
#include <sys/stat.h>   // For named-pipes

#include "server_manager.h"

/*  Signal handler flag
    1: an available worker slot exists
    0: no available slots                */
volatile sig_atomic_t empty_spot = 1; 
int worker_limit = 5;   // Global variable with default value = 5

#define FIFO1 "/tmp/fifo.1" 
#define FIFO2 "/tmp/fifo.2" 
#define PERMS 0666

// Configures sigaction
void set_signal();

// Checks if a new FIFO (named-pipe) can be created
void check_fifo_error(const int check_value);

// Closes and unlinks the named pipes
void closeFIFOs(const char* named_pipe);

int main(int argc, char **argv) {
    if(argc != 7) {
        fprintf(stderr, "\n\033[31mUsage: ./fss_manager -l <manager_logfile> -c <config_file> -n <worker_limit> \033[0m\n\n");
        exit(EXIT_FAILURE);
    }

    char *manager_logfile = NULL;
    char *config_file = NULL;
    int set_worker_limit = -1;

    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "-l") == 0) manager_logfile = argv[i + 1];
        else if (strcmp(argv[i], "-c") == 0) config_file = argv[i + 1];
        else if (strcmp(argv[i], "-n") == 0) set_worker_limit = atoi(argv[i + 1]);
    }

    // If a custom worker limit > 5 is provided, we apply it;
    // otherwise, we keep the default limit of 5.
    if(set_worker_limit != -1) {
        if(set_worker_limit > worker_limit)
            worker_limit = set_worker_limit;
    }

    // Open the manager logfile
    int log_file_fd = open(manager_logfile, O_WRONLY | O_CREAT | O_TRUNC, PERMS); 
    if(log_file_fd == -1) { perror("Failed to open manager_logfile"); exit(EXIT_FAILURE); }

    closeFIFOs(FIFO1);   // If named-pipe fifo.1 already exists, remove it
    closeFIFOs(FIFO2);   // If named-pipe fifo.2 already exists, remove it

    // Create the FIFOs (named-pipes) and open one for reading and the other for writing
    int fss_in, fss_out;    // (read, write)
    check_fifo_error(mkfifo(FIFO1, PERMS));
    check_fifo_error(mkfifo(FIFO2, PERMS));

    if((fss_in = open(FIFO1, O_RDONLY | O_NONBLOCK)) < 0) { 
        perror("fss_manager: can't open read fifo");
        closeFIFOs(FIFO1); closeFIFOs(FIFO2);
        exit(EXIT_FAILURE);
    }

    if((fss_out = open(FIFO2, O_WRONLY)) < 0) { 
        perror("fss_manager: can't open write fifo");
        closeFIFOs(FIFO1); closeFIFOs(FIFO2);
        exit(EXIT_FAILURE);
    }

    // Handle SIGCHLD
    set_signal();   
    int active_workers = 0;

    // Read the config_file and store the source_dir -> target_dir pairs in a map
    map<string, SyncInfo> file_pairs = load_config_file(config_file, log_file_fd);
    queue<Operation> worker_queue;          // Queue for tasks waiting if the worker limit is reached
    map<int, SyncInfo> watch_descr;         // Monitored directory pairs mapped to their watch descriptors
    map<string, string> console_operations; // Commands arriving from fss_console

    //~~~~~~~~~~~~ Store the initial fds to monitor ~~~~~~~~~~~~
    int total_fds = worker_limit + 2;   // +1: fss_in, +1: inotify
    struct pollfd fds[total_fds];

    // Store the inotify fd
    if((fds[worker_limit + 1].fd = inotify_init()) < 0) { perror("inotify_init failed"); exit(EXIT_FAILURE); }
    fds[worker_limit + 1].events = POLLIN;

    // Store the named-pipe fd
    fds[worker_limit].fd = fss_in;
    fds[worker_limit].events = POLLIN;

    /*  Initial synchronization of directories from the config file.
        Process as many pairs as the worker limit allows, 
        and push the rest into a waiting queue.     */
    for(auto& pair : file_pairs) {
        // watch
        int watch_d = inotify_add_watch(fds[total_fds - 1].fd, pair.first.c_str(), IN_CREATE | IN_MODIFY | IN_DELETE);   // IN_CLOSE_WRITE
        if(watch_d == -1) {
            close(fds[total_fds - 1].fd);
            perror("Watch failed");
            exit(EXIT_FAILURE);
        } else {
            char mes[MAXBUFF];
            sprintf(mes, "[%s] Added directory: %s -> %s\n", current_timestamp().c_str(), pair.first.c_str(), pair.second.target_dir.c_str());
            printf("%s", mes);  // Print to stdout
            write(log_file_fd, mes, strlen(mes));   // Write to manager_logfile
            sleep(0.001);

            watch_descr[watch_d] = pair.second;
            memset(mes, 0, sizeof(mes));
            sprintf(mes, "[%s] Monitoring started for %s\n", current_timestamp().c_str(), pair.first.c_str());
            printf("%s", mes);
            write(log_file_fd, mes, strlen(mes));
        }

        if(active_workers < worker_limit) {
            pair.second.status = "Active";  // Set status to monitored 
            pair.second.active = true;      // Synchronization starts 
            file_pairs[pair.first] = pair.second;   // Save changes to the map

            int worker_pipe[2]; // Create the worker's pipe
            if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }

            fds[active_workers].fd = worker_pipe[0];    // Read end
            fds[active_workers].events = POLLIN;
            active_workers++;

            worker_process(pair.second, "ALL", "FULL", worker_pipe);
        } else {    // Directory pairs that cannot be serviced immediately wait in the queue
            // No available slots to spawn a new process
            empty_spot = 0; // Will be set to 1 when a process finishes (via signal_handler)
            Operation oper = {
                pair.second,
                "ALL",
                "FULL"
            };
            worker_queue.push(oper);
        }
    }
    // Initialize unused file descriptor slots to -1
    for(int i = active_workers; i < worker_limit; i++) {
        fds[i].fd = -1;
        fds[i].events = 0;
        fds[i].revents = 0;
    }

    // Monitor fds using poll
    sync_fds(file_pairs, worker_queue, watch_descr, console_operations, fds, total_fds, active_workers, fss_out, log_file_fd);

    char mes[MAXBUFF];
    sprintf(mes, "[%s] Manager shutdown complete.", current_timestamp().c_str());
    printf("%s\n", mes); // Print to manager
    strcat(mes, "$\n");
    write(fss_out, mes, strlen(mes));   // Pass to fss_console

    file_pairs.clear();
    watch_descr.clear();
    console_operations.clear();
    while(!worker_queue.empty())
        worker_queue.pop();

    close(fss_in);
    close(fss_out);
    close(log_file_fd);
    return 0;
}

// Configures sigaction; handler is defined in server_manager.h
void set_signal() {
    // Σετάρουμε το signal
    static struct sigaction act;
    act.sa_handler = signal_handler;

    sigemptyset(&act.sa_mask);         // Create empty mask
    sigaddset(&act.sa_mask, SIGCHLD);  // Add SIGCHLD signal
    sigaction(SIGCHLD, &act, NULL);
}

// Closes and unlinks the named pipes
void closeFIFOs(const char* named_pipe) {
    struct stat st;
    
    // Αν υπάρχει το κάνουμε unlink
    if(stat(named_pipe, &st) == 0) {
        if(unlink(named_pipe) < 0) {
            perror("fss_console: can't unlink\n");
            exit(EXIT_FAILURE);
        }
    }
}

// Checks if a new FIFO (named-pipe) can be created successfully
void check_fifo_error(const int check_value) { 
    if((check_value < 0) && (errno != EEXIST)) {
        perror("Can't create fifo");
        exit(EXIT_FAILURE);
    }
}
