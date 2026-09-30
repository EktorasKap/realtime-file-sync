#include "server_manager.h"

// Monitors worker pipes, the fss_console input named pipe, and inotify events using poll()
void sync_fds(SyncMap file_pairs, queue<Operation> worker_queue, map<int, SyncInfo> watch_descr, map<string, string> console_operations, struct pollfd fds[], const int total_fds, int active_workers, const int fss_out, const int log_file_fd) {
    bool shutdown = false;

    while(shutdown == false || active_workers != 0) {
        int poll_count = poll(fds, total_fds, -1);  // Poll file descriptors for events
        if(poll_count == -1) {
            if(errno == EINTR) {
                continue;
            } else {
                perror("poll failed");
                exit(EXIT_FAILURE);
            }
        }

        ////////////////////// Worker pipes //////////////////////
        check_worker_pipes(file_pairs, worker_queue, console_operations, fss_out, fds, active_workers, log_file_fd);
        //////////////////////////////////////////////////////////

        ///////////////////////////////// Watch - inotify /////////////////////////////////
        if(!shutdown) check_watch(file_pairs, watch_descr, worker_queue, fds, total_fds, active_workers);
        ///////////////////////////////////////////////////////////////////////////////////

        //////////////////////////// Named piped FIFO ////////////////////////////
        if(check_named_pipes(file_pairs, worker_queue, watch_descr, console_operations, fds, fss_out, active_workers, log_file_fd)) {
            char mes[MAXBUFF];
            sprintf(mes, "[%s] Waiting for all active workers to finish.\n", current_timestamp().c_str());
            printf("%s", mes);  // Print to manager stdout
            strcat(mes, "--");
            write(fss_out, mes, strlen(mes));   // Send to fss_console

            shutdown = true;
            sleep(0.001);
            memset(mes, 0, sizeof(mes));
            sprintf(mes, "[%s] Processing remaining queued tasks.\n", current_timestamp().c_str());
            printf("%s", mes);  // Print to manager stdout
            strcat(mes, "--");
            write(fss_out, mes, strlen(mes));   // Send to fss_console
        }
        //////////////////////////////////////////////////////////////////////////
    }

    // Close all file descriptors
    close(fds[worker_limit + 1].fd);
    for(int i = 0; i < worker_limit; i++) {
        if(fds[i].fd != -1)
            close(fds[i].fd);
    }
}

// Checks monitored directories for inotify events
void check_watch(SyncMap& file_pairs, WatchMap watch_descr, queue<Operation>& worker_queue, struct pollfd fds[], const int total_fds, int &active_workers) {
    if(fds[total_fds - 1].revents & POLLIN) {
        char buffer[EVENT_BUF_LEN];
        int length = read(fds[total_fds - 1].fd, buffer, EVENT_BUF_LEN);
        if(length < 0) { if(errno != EINTR) { perror("Read failed"); exit(EXIT_FAILURE); } }

        int i = 0;
        // bool create = false;
        while(i < length) {
            struct inotify_event *event = (struct inotify_event *) &buffer[i];

            if(event->len > 0) {
                string filename(event->name);
                string event_types;

                if(event->mask & IN_CREATE) {
                    // create = true;

                    SyncInfo tmp_info = watch_descr[event->wd];
                    SyncInfo info = file_pairs[tmp_info.source_dir];
                    if(active_workers < worker_limit) {
                        info.active = true;
                        watch_descr[event->wd] = info;
                        file_pairs[info.source_dir] = info;

                        int worker_pipe[2];
                        if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }
                        
                        int free_idx;
                        for(int i = 0; i < worker_limit; i++) {
                            if(fds[i].fd == -1) {
                                free_idx = i; 
                                break;
                            }
                        }
                        fds[free_idx].fd = worker_pipe[0];
                        fds[free_idx].events = POLLIN;
                        active_workers++;

                        worker_process(info, (char*)filename.c_str(), "ADDED", worker_pipe);
                    } else {
                        empty_spot = 0;
                        Operation oper = {
                            info, 
                            filename,
                            "ADDED"
                        };
                        worker_queue.push(oper);
                    }
                } else if(event->mask & IN_MODIFY) {  // && create == false
                    SyncInfo tmp_info = watch_descr[event->wd];
                    SyncInfo info = file_pairs[tmp_info.source_dir];
                    if(active_workers < worker_limit) {
                        info.active = true;
                        watch_descr[event->wd] = info;
                        file_pairs[info.source_dir] = info;

                        int worker_pipe[2];
                        if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }

                        int free_idx;
                        for(int i = 0; i < worker_limit; i++) {
                            if(fds[i].fd == -1) {
                                free_idx = i; 
                                break;
                            }
                        }
                        fds[free_idx].fd = worker_pipe[0];
                        fds[free_idx].events = POLLIN;
                        active_workers++;

                        worker_process(info, (char*)filename.c_str(), "MODIFY", worker_pipe);
                    } else {
                        empty_spot = 0;
                        Operation oper = {
                            info,
                            filename,
                            "MODIFY"
                        };
                        worker_queue.push(oper);
                    }
                } else if(event->mask & IN_DELETE) {
                    SyncInfo tmp_info = watch_descr[event->wd];
                    SyncInfo info = file_pairs[tmp_info.source_dir];
                    if(active_workers < worker_limit) {
                        info.active = true;
                        watch_descr[event->wd] = info;
                        file_pairs[info.source_dir] = info;

                        int worker_pipe[2];
                        if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }
                        
                        int free_idx;
                        for(int i = 0; i < worker_limit; i++) {
                            if(fds[i].fd == -1) {
                                free_idx = i; 
                                break;
                            }
                        }
                        fds[free_idx].fd = worker_pipe[0];
                        fds[free_idx].events = POLLIN;
                        active_workers++;

                        worker_process(info, (char*)filename.c_str(), "DELETE", worker_pipe);
                    } else {
                        // Add task to the waiting queue
                        empty_spot = 0;
                        Operation oper = {
                            info,
                            filename, 
                            "DELETE"
                        };
                        worker_queue.push(oper);
                    }
                }
            }
            i += EVENT_SIZE + event->len;
        }
        close(length);
    }
}

// Checks if any worker has finished and reads its execution report
void check_worker_pipes(SyncMap& file_pairs, queue<Operation>& worker_queue, map<string, string>& console_operations, const int fss_out, struct pollfd fds[], int &active_workers, const int log_file_fd) {
    if(empty_spot) {    // If a worker process terminated, the flag is set to true; read from its pipe
        // Read from pipe and spawn a new worker if a task is waiting in the queue
        for(int i = 0; i < worker_limit; i++) {
            if(fds[i].revents & POLLIN && fds[i].fd != -1) {
                active_workers--;   // A worker has finished

                // Read the report from the worker's pipe
                Report report;
                read(fds[i].fd, &report, sizeof(Report));

                // If triggered by an fss_console command, return the result to the console as well
                if(console_operations.find(report.source_dir) != console_operations.end()) {
                    if(console_operations[report.source_dir] == "sync") {
                        char mes[MAXBUFF];
                        sprintf(mes, "[%s] Sync completed: %s -> %s Errors: %d", current_timestamp().c_str(), report.source_dir, report.target_dir, report.errors);
                        printf("%s\n", mes);    // Print to stdout

                        string log_mes = mes;   // Prepare message for manager logfile
                        log_mes = log_mes + '\n';
                        write(log_file_fd, log_mes.c_str(), log_mes.length());  // Write to manager_logfile
                        
                        strcat(mes, "$\n");     // Format for fss_console
                        write(fss_out, mes, strlen(mes));   // Send to fss_console
                        console_operations.erase(report.source_dir);

                        // file_pairs[report.source_dir].last_sync_time = report.timestamp;
                        file_pairs[report.source_dir].eror_count = report.errors;
                    }
                } else if(!strcmp(report.operation, "FULL")) {    // Full synchronization
                    // file_pairs[report.source_dir].last_sync_time = report.timestamp;
                    file_pairs[report.source_dir].eror_count = report.errors;
                }

                // Write entry to manager log-file
                if(!strcmp(report.operation, "FULL")) {
                    char mes[MAXBUFF];
                    sprintf(mes, "[%s] [%s] [%s] [%d] [%s] [%s] [%s]\n", 
                        report.timestamp, report.source_dir, report.target_dir, report.worker_pid, report.operation, report.result, report.details);
                    write(log_file_fd, mes, strlen(mes));
                } else {
                    size_t len = strlen("File: ") + strlen(report.source_dir) + strlen(" - ") + strlen(report.details) + 1;
                    char *filename = (char*)malloc(sizeof(char) * len);
                    if(!filename) {
                        perror("malloc failed");
                        exit(EXIT_FAILURE);
                    }
                    strcpy(filename, "File: ");
                    strcat(filename, report.filename);

                    if(strlen(report.details) != 0) {
                        strcat(filename, " - ");
                        strcat(filename, report.details);
                    }

                    char mes[MAXBUFF];
                    sprintf(mes, "[%s] [%s] [%s] [%d] [%s] [%s] [%s]\n", 
                        report.timestamp, report.source_dir, report.target_dir, report.worker_pid, report.operation, report.result, filename);
                    write(log_file_fd, mes, strlen(mes));
                    free(filename);
                }
                // Synchronization completed for this directory
                file_pairs[report.source_dir].active = false;
                file_pairs[report.source_dir].last_sync_time = report.timestamp;
                
                // Close the read end of the pipe
                close(fds[i].fd);   
                fds[i].fd = -1;     // Mark slot as empty
                fds[i].events = 0;

                // If a task is waiting in the queue, spawn a new worker for it
                if(!worker_queue.empty()) {
                    Operation info = worker_queue.front();
                    worker_queue.pop();

                    info.pair.status = "Active";
                    info.pair.active = true;

                    int worker_pipe[2];
                    if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }

                    fds[i].fd = worker_pipe[0];
                    fds[i].events = POLLIN;
                    active_workers++;
                    if(active_workers == worker_limit)
                        empty_spot = false;

                    worker_process(info.pair, info.filename.c_str(), info.operation.c_str(), worker_pipe);
                }
            }
        }
    }
}

// Checks for incoming commands from fss_console via the named pipe
int check_named_pipes(SyncMap& file_pairs, queue<Operation>& worker_queue, WatchMap& watch_descr, map<string, string>& console_operations, struct pollfd fds[], const int fss_out, int &active_workers, const int log_file_fd) {
    if(fds[worker_limit].revents & POLLIN) {
        // Read the incoming command
        char buff[MAXBUFF];
        read(fds[worker_limit].fd, buff, MAXBUFF);
        buff[strcspn(buff, "$")] = '\0';

        char **operation = split_word(buff, 1);

        if(!strcmp(operation[0], "add")) {
            /*  source_dir: operation[1]
                target_dir: operation[2]    */
            operation = split_word(buff, 3);
            if(operation == NULL) { write(fss_out, "$\n", 2); return 0; } // Ignore invalid command 

            // If the <source, target> pair already exists, do not re-add it
            SyncMap::iterator info = file_pairs.find(operation[1]);
            if(info != file_pairs.end()) {  // source_dir exists
                if(info->second.target_dir == operation[2]) {   // The exact <source, target> pair already exists
                    // If the pair is not currently monitored, start monitoring and syncing it
                    if(info->second.status == "Not active") {
                        // Set status to monitored
                        info->second.status = "Active";
                        file_pairs[info->first] = info->second;

                        int watch_d = inotify_add_watch(fds[worker_limit + 1].fd, operation[1], IN_CREATE | IN_MODIFY | IN_DELETE);  
                        if(watch_d == -1){
                            write(fss_out, "$\n", 2);   // Ignore on failure
                            return 0;
                        } else {
                            char mes[MAXBUFF];
                            sprintf(mes, "[%s] Added directory: %s -> %s\n", current_timestamp().c_str(), operation[1], operation[2]);
                            printf("%s", mes); // Print to stdout
                            write(log_file_fd, mes, strlen(mes));  // Write to manager_logfile
            
                            strcat(mes, "--");     // Format for fss_console
                            write(fss_out, mes, strlen(mes));   // Send to fss_console
                            sleep(0.001);

                            watch_descr[watch_d] = info->second;
                            memset(mes, 0, sizeof(mes));
                            sprintf(mes,"[%s] Monitoring started for %s", current_timestamp().c_str(), operation[1]);
                            printf("%s\n", mes);

                            string log_mes = mes;   // Prepare message for manager logfile
                            log_mes = log_mes + '\n';
                            write(log_file_fd, log_mes.c_str(), log_mes.length());  // Write to manager_logfile

                            strcat(mes, "$\n");
                            write(fss_out, mes, strlen(mes));
                        }

                        // Start synchronization
                        if(active_workers < worker_limit) {
                            info->second.active = true;
                            file_pairs[info->first] = info->second;

                            int worker_pipe[2];
                            if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }
    
                            // Find an available slot in fds[]
                            int pipe_idx;
                            for(int i = 0; i < worker_limit; i++)
                                if(fds[i].fd == -1) { pipe_idx = i; break; }
            
                            fds[pipe_idx].fd = worker_pipe[0];
                            fds[pipe_idx].events = POLLIN;
                            active_workers++;
                            worker_process(info->second, "ALL", "FULL", worker_pipe);
                        } else {
                            empty_spot = 0;
                            Operation oper = {
                                info->second,
                                "ALL",
                                "FULL" 
                            };
                            worker_queue.push(oper);
                        }
                    } else {    // If already monitored, print: Already in queue
                        char mes[MAXBUFF];
                        sprintf(mes, "[%s] Already in queue: %s", current_timestamp().c_str(), operation[1]);
                        printf("%s\n", mes);    // Print to stdout
                        strcat(mes, "$\n");
                        write(fss_out, mes, strlen(mes));   // Send to fss_console
                        return 0;
                    }
                } else {
                    // If source_dir is already mapped to a different target_dir, print: Already in queue
                    char mes[MAXBUFF];
                    sprintf(mes, "[%s] Already in queue: %s", current_timestamp().c_str(), operation[1]);
                    printf("%s\n", mes);    // Print to stdout
                    strcat(mes, "$\n");
                    write(fss_out, mes, strlen(mes));   // Send to fss_console
                    return 0;
                }
            } else {    // Directory pair does not exist, so add it
                SyncInfo target_info = {
                    operation[1],
                    operation[2],
                    "Active",
                    current_timestamp(),
                    false,
                    0
                };

                // Add directory to inotify monitoring
                int watch_d = inotify_add_watch(fds[worker_limit + 1].fd, operation[1], IN_CREATE | IN_MODIFY | IN_DELETE);  
                if(watch_d == -1) {
                    if (errno == ENOENT) {
                        write(fss_out, "$\n", 2);   // Ignore for fss_console
                    } else {
                        perror("Watch failed");
                    }
                    return 0;
                } else {
                    // Log and notify that the directory was added and monitoring started
                    char mes[MAXBUFF];
                    sprintf(mes, "[%s] Added directory: %s -> %s\n", current_timestamp().c_str(), operation[1], operation[2]);
                    printf("%s", mes); // Print to stdout
                    write(log_file_fd, mes, strlen(mes));  // Write to manager_logfile
    
                    strcat(mes, "--");     // Format for fss_console
                    write(fss_out, mes, strlen(mes));   // Send to fss_console
                    sleep(0.001);

                    watch_descr[watch_d] = target_info;
                    memset(mes, 0, sizeof(mes));
                    sprintf(mes,"[%s] Monitoring started for %s", current_timestamp().c_str(), operation[1]);
                    printf("%s\n", mes);

                    string log_mes = mes;   // Prepare message for manager logfile
                    log_mes = log_mes + '\n';
                    write(log_file_fd, log_mes.c_str(), log_mes.length());  // Write to manager_logfile

                    strcat(mes, "$\n");
                    write(fss_out, mes, strlen(mes));
                }

                // Synchronize immediately if a worker slot is available
                if(active_workers < worker_limit) {
                    target_info.active = true;
                    int worker_pipe[2];
                    if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }
    
                    // Find an available slot in fds[]
                    int pipe_idx;
                    for(int i = 0; i < worker_limit; i++)
                        if(fds[i].fd == -1) { pipe_idx = i; break; }
    
                    fds[pipe_idx].fd = worker_pipe[0];
                    fds[pipe_idx].events = POLLIN;
                    active_workers++;
                    worker_process(target_info, "ALL", "FULL", worker_pipe);
                } else {    // Otherwise, push to the queue to be processed when a worker becomes available
                    empty_spot = 0;
                    Operation oper = {
                        target_info, 
                        "ALL",
                        "FULL"
                    };
                    worker_queue.push(oper);
                }

                file_pairs[operation[1]] = target_info;
            }
        } else if(!strcmp(operation[0], "sync")) {
            /* source_dir: operation[1] */
            operation = split_word(buff, 2);
            if(operation == NULL) { write(fss_out, "$\n", 2); return 0; } // Ignore invalid command

            SyncMap::iterator info = file_pairs.find(operation[1]);
            if(info == file_pairs.end()) {  // If directory is not found, ignore
                char mes[MAXBUFF];
                snprintf(mes, sizeof(mes),"[%s] Directory not monitored: %s", current_timestamp().c_str(), operation[1]);
                printf("%s\n", mes);
                strcat(mes, "$\n");
                write(fss_out, mes, strlen(mes));
                return 0;
            } else {
                // If a sync task is already running or queued for this directory, notify that sync is in progress
                if(info->second.active == true || find_in_queue(worker_queue, info->second)) {
                    char mes[MAXBUFF];
                    sprintf(mes, "[%s] Sync already in progress: %s", current_timestamp().c_str(), info->first.c_str());
                    printf("%s\n", mes);
                    strcat(mes, "$\n");

                    write(fss_out, mes, strlen(mes));   
                    return 0;
                } else {
                    console_operations[info->first] = "sync";

                    // If not currently monitored, re-enable monitoring
                    if(info->second.status == "Not active") {
                        info->second.status = "Active";
                        file_pairs[info->first] = info->second;

                        int watch_d = inotify_add_watch(fds[worker_limit + 1].fd, operation[1], IN_CREATE | IN_MODIFY | IN_DELETE);  
                        if(watch_d == -1) {
                            write(fss_out, "$\n", 2);   // Ignore on failure
                            return 0;
                        } else {
                            watch_descr[watch_d] = info->second;
                        }
                    }

                    // Start synchronization
                    if(active_workers < worker_limit) {
                        info->second.active = true;
                        file_pairs[info->first] = info->second;

                        char mes[MAXBUFF];
                        snprintf(mes, sizeof(mes),"[%s] Syncing directory: %s -> %s\n", current_timestamp().c_str(), info->first.c_str(), info->second.target_dir.c_str());
                        printf("%s", mes);  // Print to stdout
                        write(log_file_fd, mes, strlen(mes));   // Write to manager_logfile
                        strcat(mes, "--");
                        write(fss_out, mes, strlen(mes));       // Send to fss_console

                        int worker_pipe[2];
                        if(pipe(worker_pipe) == -1) { perror("Pipe failed"); exit(EXIT_FAILURE); }

                        // Find an available slot in fds[]
                        int pipe_idx;
                        for(int i = 0; i < worker_limit; i++)
                            if(fds[i].fd == -1) { pipe_idx = i; break; }

                        fds[pipe_idx].fd = worker_pipe[0];
                        fds[pipe_idx].events = POLLIN;
                        active_workers++;

                        worker_process(info->second, "ALL", "FULL", worker_pipe);
                    } else {
                        empty_spot = 0;
                        Operation oper = {
                            info->second,
                            "ALL",
                            "FULL"
                        };
                        worker_queue.push(oper);
                    }
                    file_pairs[operation[1]] = info->second;
                }
            } 
        } else if(!strcmp(operation[0], "cancel")) {
            /* source_dir: operation[1] */
            operation = split_word(buff, 2);
            if(operation == NULL) { write(fss_out, "$\n", 2); return 0; }   // Ignore invalid command

            SyncMap::iterator info = file_pairs.find(operation[1]);
            if(info == file_pairs.end()) {  // If directory is not found, ignore
                char mes[MAXBUFF];
                sprintf(mes, "[%s] Directory not monitored: %s", current_timestamp().c_str(), operation[1]);
                printf("%s\n", mes);
                strcat(mes, "$\n");
                write(fss_out, mes, strlen(mes));   
                return 0;
            } else {
                if(info->second.status == "Active") {
                    int wd_rm = -1; // Find the watch descriptor and remove the inotify watch
                    for(const auto& pair: watch_descr) {
                        if(!strcmp(pair.second.source_dir.c_str(), operation[1])) {
                            wd_rm = pair.first;
                            break;
                        }
                    } 
                    if(inotify_rm_watch(fds[worker_limit + 1].fd, wd_rm)) {
                        perror("inotify_rm_watch failed!");
                        exit(EXIT_FAILURE);
                    }

                    watch_descr.erase(wd_rm);
                    file_pairs[operation[1]].status = "Not active";

                    char mes[MAXBUFF];
                    snprintf(mes, sizeof(mes),"[%s] Monitoring stopped for %s", current_timestamp().c_str(), operation[1]);
                    printf("%s\n", mes);    // Print to stdout

                    string log_mes = mes;
                    log_mes = log_mes + '\n';
                    write(log_file_fd, log_mes.c_str(), log_mes.length());  // Write to manager_logfile

                    strcat(mes, "$\n");
                    write(fss_out, mes, strlen(mes));   // Send to fss_console
                } else {
                    char mes[MAXBUFF];
                    sprintf(mes, "[%s] Directory not monitored: %s", current_timestamp().c_str(), operation[1]);
                    printf("%s\n", mes);
                    strcat(mes, "$\n");
                    write(fss_out, mes, strlen(mes));   
                    return 0;
                }
            }
        } else if(!strcmp(operation[0], "status")) {
            char **source_dir = split_word(buff, 2);
            if(source_dir == NULL) {
                write(fss_out, "$\n", 2);   // Ignore invalid command
                free_split(source_dir, 2);
                return 0;
            }

            SyncMap::iterator info = file_pairs.find(source_dir[1]);
            if(info != file_pairs.end()) {  // If source_dir exists
                    char mes[MAXBUFF];
                    print_status(mes, info->second);
                    // strcpy(mes, print_status(mes, info->second));
                    strcat(mes, "$\n");
                    write(fss_out, mes, strlen(mes));
            } else {    // If source_dir is not found
                char mes[MAXBUFF];
                sprintf(mes, "[%s] Directory not monitored: %s", current_timestamp().c_str(), source_dir[1]);
                printf("%s\n", mes);
                strcat(mes, "$\n");
                write(fss_out, mes, strlen(mes));
                free_split(source_dir, 2);
                return 0;
            }
        } else if(!strcmp(operation[0], "shutdown")) {
            char mes[MAXBUFF];
            sprintf(mes, "[%s] Shutting down manager ...\n", current_timestamp().c_str());
            printf("%s", mes);    // Print to manager stdout
            strcat(mes, "--"); 
            write(fss_out, mes, strlen(mes));   // Send to fss_console
            free_split(operation, 1);
            sleep(0.001);
            return 1;
        } else { // Ignore unrecognized command
            write(fss_out, "$\n", 2);   // Ignore
            free_split(operation, 1);
            return 0;
        }
        free_split(operation, 1);
    }
    return 0;
}

// Checks if a task exists in the queue 
bool find_in_queue(queue<Operation> queue, SyncInfo info) {
    while(!queue.empty()) {
        if(queue.front().pair.source_dir == info.source_dir) {
            return true;
        }
        queue.pop();    
    }
    return false;
}

// Formats and prints the synchronization status of a directory
char* print_status(char* mes, SyncInfo info) {
    sprintf(mes, 
        "[%s] Status requested for: %s\nDirectory: %s\nTarget: %s\nLast Sync: %s\nErrors: %d\nStatus: %s",
        current_timestamp().c_str(), info.source_dir.c_str(), info.source_dir.c_str(), info.target_dir.c_str(), info.last_sync_time.c_str(), info.eror_count, info.status.c_str());
    printf("%s\n", mes);
    return mes;
}

// Splits a string by spaces (" ") into 'num_parts' tokens
char **split_word(const char* word, const int num_parts) {
    char **parts = new char*[num_parts];
    for(int i = 0; i < num_parts; i++)
        parts[i] = nullptr;

    char *copy = strdup(word);
    int i = 0;

    char *token = strtok(copy, " \n");
    while(token != NULL && i < num_parts) {
        parts[i++] = strdup(token);
        token = strtok(NULL, " \n");
    }

    free(copy);

    // If fewer tokens than 'num_parts' are found, clean up and return nullptr
    if(i < num_parts) {
        for(int j = 0; j < i; j++) 
            free(parts[j]);
        delete[] parts;
        return nullptr;
    }

    return parts;
}

// Frees memory allocated by split_word()
void free_split(char** parts, const int num_parts) {
    for(int i = 0; i < num_parts; i++) {
        if(parts[i] != nullptr)
            free(parts[i]);
    }
    delete[] parts;
}

// Spawns a new worker process using fork() and exec()
void worker_process(SyncInfo info, const string filename, const string operation, const int worker_pipe[2]) {
    pid_t worker = fork();
    if(worker == -1) {
        perror("Fork failed");
        exit(EXIT_FAILURE);
    }
    else if(worker == 0) {
        close(worker_pipe[0]);  // Close unused read end

        // Redirect stdout to the write end of the pipe
        if(dup2(worker_pipe[1], STDOUT_FILENO) == -1) {
            perror("Worker pipe dup2 failed");
            exit(EXIT_FAILURE);
        }
        close(worker_pipe[1]);  // Close the original write file descriptor

        execl("./workers", "workers", (char*)info.source_dir.c_str(), (char*)info.target_dir.c_str(), (char*)filename.c_str(), (char*)operation.c_str(), (char*)NULL);
        perror("Execlp failed for worker");
        exit(EXIT_FAILURE);
    } else { // Parent process
        close(worker_pipe[1]);  // Close unused write end
    }
}

// Reads directory pairs from the config file and stores them in a map
map<string, SyncInfo> load_config_file(const char* filename, const int log_file_fd) {
    map<string, SyncInfo> config_map;
    int fd = open(filename, O_RDONLY);  
    if(fd == -1) { perror("Error opening file"); exit(EXIT_FAILURE); }

    char buff[MAXBUFF];
    string remaining;
    int count_lines = 0;
    
    while(true) {
        // Read a chunk of the file
        ssize_t bytes_read = read(fd, buff, MAXBUFF);
        if(bytes_read == -1) {
            perror("load_config_file: error while reading file");
            exit(EXIT_FAILURE);
        }
        if(bytes_read == 0) break;  // Reached End of File (EOF)

        buff[bytes_read] = '\0';
        string chunk = remaining + string(buff);
        remaining.clear();  // Clear remaining buffer to prepare for the next chunk

        size_t start = 0;   // Starting position for search
        while(true) {
            // Find the next newline character
            size_t newline_pos = chunk.find('\n', start);

            // If no newline is found, keep the remaining part to process with the next chunk
            if(newline_pos == string::npos) {   // string::npos indicates that the character was not found
                remaining = chunk.substr(start); // Store the incomplete line
                break;
            }
            
            // Extract the <source_dir> <target_dir> line
            string line = chunk.substr(start, newline_pos - start);
            start = newline_pos + 1;    // Move index to the start of the next line
            count_lines++;

            // Separate source_dir from target_dir
            size_t space_pos = line.find(' ');
            if(space_pos == string::npos) { fprintf(stderr, "num: %d has wrong format!! \t<source_dir target_dir>", count_lines); continue; }

            string source = line.substr(0, space_pos);
            string target = line.substr(space_pos+1);

            // Add directory pair to the map
            SyncInfo info;
            info = {
                source, 
                target, 
                "Not active",
                current_timestamp(),
                false,
                0
            };
            
            config_map[source] = info;
        }
    }

    // Process any remaining data (if the file does not end with '\n')
    if(!remaining.empty()) {
        size_t space_pos = remaining.find(' ');
        if(space_pos != string::npos) {
            string source = remaining.substr(0, space_pos);
            string target = remaining.substr(space_pos + 1);

            SyncInfo info;
            info = {
                source,
                target, 
                "Not active",
                current_timestamp(),
                false,
                0
            };
            config_map[source] = info;
        }
    }

    close(fd);
    return config_map;
}

// Generates the current timestamp
string current_timestamp() {
    time_t now = time(nullptr);
    tm* t = localtime(&now);
    stringstream ss;
    ss << put_time(t, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

/* Signal handler:
   Sets the flag to true when a worker terminates, 
   indicating that a slot is available to spawn a new worker. */
void signal_handler(int signum) {
    empty_spot = 1;
    for(int i = 0; i < worker_limit; i++) wait(NULL);
}
