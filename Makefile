INCLUDE = include/
MODULES = modules/
SRC = src/

CC = g++

CXXFLAGS = -Wall -Werror -g -I$(INCLUDE)

# Files .o
OBJ_manager = $(SRC)fss_manager.o $(MODULES)server_manager.o
OBJ_console = $(SRC)fss_console.o # $(MODULES)client_console.o
OBJ_WORKER = $(SRC)workers.o $(MODULES)server_manager.o

EXEC_manager = fss_manager
EXEC_console = fss_console
EXEC_WORKER = workers

# ARGS_MANAGER = data/confige_file.txt 2
ARGS_MANAGER = -l data/manager_logfile.txt -c data/confige_file.txt -n 10
ARGS_CONSOLE = -l data/console_log_file.txt
ARGS_WORKER = data/test/tmp1/source_dir data/test/tmp2/target_dir ALL FULL

all: $(EXEC_manager) $(EXEC_console) $(EXEC_WORKER)

$(EXEC_WORKER): $(OBJ_WORKER)
	$(CC) $(OBJ_WORKER) -o $(EXEC_WORKER) $(LDFLAGS) -I$(INCLUDE)

$(EXEC_manager): $(OBJ_manager)
	$(CC) $(OBJ_manager) -o $(EXEC_manager) $(LDFLAGS) -I$(INCLUDE)

$(EXEC_console): $(OBJ_console)
	$(CC) $(OBJ_console) -o $(EXEC_console) $(LDFLAGS) -I$(INCLUDE)

clean:
	rm -f $(OBJ_manager) $(EXEC_manager) 
	rm -f $(OBJ_console) $(EXEC_console)
	rm -f $(OBJ_WORKER) $(EXEC_WORKER)

manager: $(EXEC_manager)
	./$(EXEC_manager) $(ARGS_MANAGER)

worker: $(EXEC_WORKER)
	./$(EXEC_WORKER) $(ARGS_WORKER)

console: $(EXEC_console)
	./$(EXEC_console) $(ARGS_CONSOLE)

# run: $(EXEC)
# 	./$(EXEC) $(ARGS)

valgrind_manager: $(EXEC_manager)
	valgrind ./$(EXEC_manager) $(ARGS_MANAGER) 
# --leak-check=full --track-origins=yes
valgrind_console: $(EXEC_console)
	valgrind ./$(EXEC_console) $(ARGS_CONSOLE)
# --leak-check=full --track-origins=yes
