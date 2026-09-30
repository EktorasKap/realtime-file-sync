#include <iostream>
#include <cstring>      
#include <fcntl.h>      // For O_RDONLY, O_WRONLY, ...
#include <unistd.h>     // For close, read, write, ...
#include <dirent.h>     // DIR
#include <sys/stat.h>   // Struct stat
#include "server_manager.h"
using namespace std;

#define RED     "\033[31m"
#define GREEN     "\033[32m"
#define RESET   "\033[0m"

int worker_limit;
volatile sig_atomic_t empty_spot;
int kill_process;

bool copyFile(const string& source_path, const string& target_path, char *error_mes);

int main(int argc, char** argv) {
    string source_dir = argv[1];
    string target_dir = argv[2];
    string filename = argv[3];
    string operation = argv[4];

    char error_mes[128] = "\0";
    int errors = 0;
    int skipped = 0;
    int copy_files = 0;

    // Copy contents from source_dir to target_dir
    if(filename == "ALL" && operation == "FULL") {
        // Open the directory
        DIR* dir = opendir(source_dir.c_str());
        if(!dir) {
            strcat(error_mes, strerror(errno));
            errors++;
        }

        if(errors == 0) {
            struct stat st; // Check if target directory exists
            if(stat(target_dir.c_str(), &st) == -1) {
                // Create the target directory if it does not exist
                if(mkdir(target_dir.c_str(), 0777) == -1) {
                    strcat(error_mes, strerror(errno));
                    errors++;
                }
            } 

            // Proceed if no errors occurred
            if(errors == 0) {
                struct dirent* entry;
                while((entry = readdir(dir))) {
                    string name = entry->d_name;
                    if(name == "." || name == "..") continue;

                    string source_path = source_dir + "/" + name;
                    string target_path = target_dir + "/" + name;

                    if(entry->d_type == DT_REG) {   // If it is a regular file
                        if(copyFile(source_path, target_path, error_mes))
                            copy_files++;
                        else {
                            errors++;
                            skipped++;
                        }
                    }
                }
            }
        }
    } else if(operation == "ADDED" || operation == "MODIFY" || operation == "DELETE") {
        string source_path = source_dir + "/" + filename;
        string target_path = target_dir + "/" + filename;

        if(operation == "DELETE") {
            if(unlink(target_path.c_str()) < 0) { 
                strcat(error_mes, strerror(errno));
                errors++;
            } else {
                copy_files++;
            }
        }
        else
            // Increment copied files counter upon successful copy
            if(copyFile(source_path, target_path, error_mes))
                copy_files++;
            else {
                skipped++;
                errors++;
            }
    }

    Report report;
    strcpy(report.timestamp, current_timestamp().c_str());
    strcpy(report.source_dir, source_dir.c_str());
    strcpy(report.target_dir, target_dir.c_str());
    strcpy(report.filename, filename.c_str());
    report.worker_pid = getpid();
    strcpy(report.operation, operation.c_str());
    
    if(copy_files == 0 && errors > 0) {   // If no files were copied
        strcpy(report.result, "ERROR");
    } else {
        if(errors == 0) {
            strcpy(report.result, "SUCCESS");
        } else {
            strcpy(report.result, "PARTIAL");
        }
    }
    
    if(operation == "FULL") {
        if(!strcmp(report.result, "SUCCESS")) {
            char mes[256];
            sprintf(mes, "%d files copied", copy_files);
            strcpy(report.details, mes);
        } else if(!strcmp(report.result, "PARTIAL")) {
            char mes[256];
            sprintf(mes, "%d files copied, %d skipped", copy_files, skipped);
            strcpy(report.details, mes);
        } else {
            char mes[256];
            sprintf(mes, "%d files copied, %d skipped", copy_files, skipped);
            strcpy(report.details, mes);
        }
    } else {
        strcpy(report.details, error_mes);
    }
    report.errors = errors;

    write(STDOUT_FILENO, &report, sizeof(Report));
    close(STDOUT_FILENO);
    return 0;
}

bool copyFile(const string& source_path, const string& target_path, char *error_mes) {
    bool copied = true;
    
    int source_fd = open(source_path.c_str(), O_RDONLY);
    if(source_fd < 0) { 
        strcat(error_mes, strerror(errno));
        copied = false;
    }

    int target_fd = open(target_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(target_fd == -1) {
        strcat(error_mes, strerror(errno));
        copied = false;
    }

    if(copied) {
        char buff[1024];
        ssize_t bytes_read;
        while((bytes_read = read(source_fd, buff, sizeof(buff))) > 0) {
            buff[bytes_read] = '\0';
            if(write(target_fd, buff, bytes_read) != bytes_read) {
                strcat(error_mes, strerror(errno));
                copied = false;
            }
        }
    }

    close(source_fd);
    close(target_fd);

    if(copied)
        return true;
    else 
        return false;
}
