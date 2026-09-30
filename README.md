# Real-Time File Synchronization System (FSS)

This project implements a multi-process, real-time file synchronization system for Linux environments. Utilizing low-level system calls, inter-process communication (IPC), and event-driven I/O, the system ensures autonomous directory mirroring, dynamic query handling, and execution reporting.

## System Architecture

The architecture relies on a decoupled, client-server model consisting of four primary components:
*   **fss_manager:** The core daemon responsible for orchestrating synchronization. It utilizes the `inotify` API to monitor specific source directories for creation, modification, or deletion events without polling overhead. It maintains internal state utilizing `std::map` for efficient directory tracking.
*   **fss_console:** An interactive command-line interface allowing users to dynamically submit administrative commands (`add`, `cancel`, `status`, `sync`, `shutdown`). It communicates with the manager bidirectionally via designated named pipes (FIFOs).
*   **Worker Processes:** Ephemeral child processes spawned by the manager utilizing `fork()` and `exec()`. Workers execute atomic synchronization operations using low-level I/O system calls (`open`, `read`, `write`, `unlink`) and report execution outcomes back to the manager via anonymous pipes.
*   **fss_script.sh:** A supplementary Bash utility designed for parsing generated log files to produce system status reports (`listAll`, `listMonitored`, `listStopped`) and purging obsolete synchronization targets (`purge`).

## Implementation Details

*   **Concurrency & Task Queuing:** The manager enforces a strict concurrency threshold (`worker_limit`) to prevent system resource exhaustion. If the active worker count reaches this limit, incoming `inotify` events or explicit `sync` requests are deferred to a standard FIFO queue (`std::queue`) and processed sequentially upon subsequent worker termination.
*   **Event Multiplexing:** To avoid threading complexity and blocking I/O, the manager employs `poll()` to concurrently monitor multiple file descriptors. This includes the `inotify` instance, the incoming named pipe from the console, and the read-ends of active worker anonymous pipes.
*   **Data Integrity & Error Reporting:** Workers validate every I/O operation (utilizing `strerror` on failure) and transmit detailed execution reports (`SUCCESS`, `PARTIAL`, `ERROR`) to the manager prior to exiting. The manager captures the `SIGCHLD` signal, reads the report, updates in-memory structures, and writes formatted entries to a persistent log file.

## Compilation & Usage

Compile the system executables using the provided Makefile:
`make`

**Manager Execution:**
`./fss_manager -l <manager_logfile> -c <config_file> -n <worker_limit>`

**Console Execution:**
`./fss_console -l <console_logfile>`
