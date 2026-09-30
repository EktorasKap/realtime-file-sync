#include <iostream>
#include <string.h>
#include <fcntl.h>      // For O_RDONLY, O_WRONLY, ...
#include <unistd.h>     // For close, read, write, ...
#include <iomanip>      // Stringstream in current_timestamp
#include <signal.h>     // Sigaction
using namespace std;

#define MAXBUFF 1024
#define FIFO1 "/tmp/fifo.1"
#define FIFO2 "/tmp/fifo.2"
#define PERMS 0666

// Sends the command to fss_manager, reads the response, and writes it to the console logfile
void console_operation(const int console_logfile_fd, const int readfd, const int writefd, const char* operation);

// Closes and unlinks the named pipes
void closeFIFOs();

// Generates the current timestamp
string current_timestamp();

// Splits a string into 'num_parts' tokens based on spaces
char **split_word(const char* word, const int num_parts);

// Validates the command, writes it to the logfile if valid, otherwise returns false
bool store_command(char *buff, const int console_logfile_fd);

// Signal handler
void signal_handler(int signum);

// Configures signal handling with sigaction
void set_signal();

int readfd, writefd;    // Named pipe file descriptors
int console_logfile_fd; // Console logfile file descriptor

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "\n\033[31mUsage: ./fss_console -l <console_logfile> \033[0m\n\n");
        exit(EXIT_FAILURE);
    } 

    // On SIGINT (Ctrl-C), SIGTSTP (Ctrl-Z), or SIGTERM, sends a shutdown command to fss_manager
    set_signal();

    char *console_log_file;
    for(int i = 1; i < argc; i += 2) 
        if(strcmp(argv[i], "-l") == 0) console_log_file = argv[i + 1];

    console_logfile_fd = open(console_log_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(console_logfile_fd == -1) { perror("Failed to open console_logfile"); exit(EXIT_FAILURE); }

    // Open the FIFOs created by the server (fss_manager)
    if((writefd = open(FIFO1, O_WRONLY)) < 0) {
        perror("client: can't open write fifo \n");
        exit(EXIT_FAILURE);
    }

    if((readfd = open(FIFO2, O_RDONLY)) < 0) {
        perror("client: can't open read fifo \n");
        exit(EXIT_FAILURE);
    }

    // Loop until a valid command is provided
    char buff[MAXBUFF];
    while(true) {
        printf("\n>> ");   // Prompt for input
        memset(buff, 0, sizeof(buff)); 
        fflush(stdout);
        read(STDIN_FILENO, buff, MAXBUFF);
        buff[strcspn(buff, "\n")] = '\0';

        // True indicates a valid command format
        if(store_command(buff, console_logfile_fd))
            break;
        else 
            fprintf(stderr, "\033[31mUsage:\033[0m \033[32madd\033[0m <source> <target> or \033[32mstatus\033[0m <directory> or \033[32mcancel\033[0m <source> or \033[32msync\033[0m <directory> or \033[32mshutdown\033[0m\n"); 
    }
    strcat(buff, "$\0");

    // Loop until the "shutdown" command is issued
    while(strcmp(buff, "shutdown$") != 0) {
        char **operation = split_word(buff, 1);

        if(!strcmp(operation[0], "add") || !strcmp(operation[0], "status") || !strcmp(operation[0], "cancel") || !strcmp(operation[0], "sync"))
            console_operation(console_logfile_fd, readfd, writefd, buff);
        else    
            fprintf(stderr, "\033[31mUsage:\033[0m add <source> <target>  or status <directory> or cancel <source> or sync <directory>\n");  
        
        printf("\n\n>> "); // Prompt for input
        memset(buff, 0, sizeof(buff)); 
        fflush(stdout);
        read(STDIN_FILENO, buff, MAXBUFF);
        if(store_command(buff, console_logfile_fd)) {
            buff[strcspn(buff, "\n")] = '\0';
            strcat(buff, "$\0");
        } // If the command is invalid, the loop continues and prints an error message
        free(operation);
    }

    // Process shutdown command
    console_operation(console_logfile_fd, readfd, writefd, buff);
    printf("\n");   // Print a newline in the terminal after the final message

    close(console_logfile_fd);
    close(readfd);
    close(writefd);

    closeFIFOs();
    return 0;
}

// Splits a string into 'num_parts' tokens based on spaces
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

    // If tokens < num_parts, clean up and return NULL
    if( i < num_parts) {
        for(int j = 0; j < i; j++) 
            free(parts[j]);
        delete[] parts;
        return nullptr;
    }
    return parts;
}

// Sends the command to the manager, reads the response, and outputs it to both terminal and logfile
void console_operation(const int console_logfile_fd, const int readfd, const int writefd, const char* operation) {
    char buff[MAXBUFF];
    int n = strlen(operation);
    if(operation[n-1] == '\n')
        n--;

    // Περνάει το operation στον manager
    if(write(writefd, operation, n) != n) {
        perror("fss_console: filename write error");
        closeFIFOs();
        exit(EXIT_FAILURE);
    }
        
    bool ignore = false;
    while((n = read(readfd, buff, MAXBUFF)) > 0) {
        if(n == 2) {    // Το αγνοούμε
            ignore = true;
            break;   
        }

        if(write(1, buff, n-2) != (n-2)) {  // write in terminal
            perror("fss_console: data write error\n");
            closeFIFOs();
            exit(EXIT_FAILURE);
        } else {
            // write in console log file
            write(console_logfile_fd, buff, n-2);
        }
        if(buff[n-2] == '$') {
            break;
        } 
    }
    if(!ignore)
        write(console_logfile_fd, "\n", 1);
    
    if(n < 0) {
        perror("fss_console: data read error\n");
        closeFIFOs();
        exit(EXIT_FAILURE);
    }
}

// Validates the command, writes it to the logfile if valid, otherwise returns false
bool store_command(char *buff, const int console_logfile_fd) {
    char mes[MAXBUFF];
    char **split = split_word(buff, 1);

    if(!strcmp(split[0], "add")) {
        // buff[strlen(buff)-1] = '\0';
        split = split_word(buff, 3);
        if(split != NULL) {
            sprintf(mes, "[%s] Command %s %s -> %s\n", current_timestamp().c_str(), split[0], split[1], split[2]);
            write(console_logfile_fd, mes, strlen(mes));
            return true;
        }
    } else if(!strcmp(split[0], "status") || !strcmp(split[0], "cancel") || !strcmp(split[0], "sync")) {
        split = split_word(buff, 2);
        if(split != NULL) {
            sprintf(mes, "[%s] Command %s %s\n", current_timestamp().c_str(), split[0], split[1]);
            write(console_logfile_fd, mes, strlen(mes));
            return true;
        }
    } else if(!strcmp(split[0], "shutdown")) {
        split = split_word(buff, 1);
        if(split != NULL) {
            sprintf(mes, "[%s] Command %s\n", current_timestamp().c_str(), split[0]);
            write(console_logfile_fd, mes, strlen(mes));
            return true;
        }
    }
    return false;
}

// Unlinks the named pipes
void closeFIFOs() {
    if(unlink(FIFO1) < 0) {
        perror("fss_console: can't unlink\n");
        exit(EXIT_FAILURE);
    }

    if(unlink(FIFO2) < 0) {
        perror("fss_console: can't unlink\n");
        exit(EXIT_FAILURE);
    }
}

// Generates the current timestamp
string current_timestamp() {
    time_t now = time(nullptr);
    tm* t = localtime(&now);
    stringstream ss;
    ss << put_time(t, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// Signal handler
void signal_handler(int signum) {
    char mes[MAXBUFF];
    strcpy(mes, "shutdown");
    write(writefd, mes, sizeof(mes));
    store_command(mes, console_logfile_fd); // Write shutdown command to logfile
    printf("\n");

    // Read shutdown messages from the manager
    int read_bytes;
    char buff[MAXBUFF];
    while((read_bytes = read(readfd, buff, MAXBUFF)) > 0) {
        if(write(1, buff, read_bytes-2) != (read_bytes-2)) {
            perror("fss_console: data write error\n");
            closeFIFOs();
            exit(EXIT_FAILURE);
        } else {
            // Write to console logfile
            write(console_logfile_fd, buff, read_bytes-2);
        }
        if(buff[read_bytes-2] == '$') {
            break;
        } 
    }
    write(console_logfile_fd, "\n", 1);

    if(read_bytes < 0) {
        perror("fss_console: data read error\n");
        closeFIFOs();
        exit(EXIT_FAILURE);
    }

    close(console_logfile_fd);
    close(readfd);
    close(writefd);
    closeFIFOs();
    exit(EXIT_SUCCESS);
}

// Configures signal handling with sigaction
void set_signal() {
    // Setup the signal handler configuration
    static struct sigaction act;
    act.sa_handler = signal_handler;

    sigemptyset(&act.sa_mask);         // Initialize an empty signal set
    // Add signals to the mask to be blocked during handler execution
    sigaddset(&act.sa_mask, SIGINT);    // Ctrl -c
    sigaddset(&act.sa_mask, SIGTERM);   // kill
    sigaddset(&act.sa_mask, SIGTSTP);   // Ctrl -z

    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGTSTP, &act, NULL);
}
