# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor process and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
|V Kushal sri sai | PES2UG24CS572 |
| Ujwal K | PES2UG24CS566 |

**Course:** UE24CS242B - Operating Systems  
**Guide:** Prof. Shilpa S, Assistant Professor, PES University  
**Semester:** Jan – May 2026

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 in a VM with Secure Boot OFF. WSL will not work.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
cd boilerplate
make
```

### Prepare Root Filesystem

```bash
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz
sudo tar -xzf alpine-minirootfs-3.20.3-aarch64.tar.gz -C rootfs
```

> Use the `aarch64` tarball on ARM64 machines (e.g. Parallels on Apple Silicon). Use `x86_64` on Intel/AMD machines.

### Copy Workload Binaries into Rootfs

```bash
sudo cp memory_hog rootfs/
sudo cp cpu_hog rootfs/
```

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls /dev/container_monitor
```

### Start Supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs
```

### Use the CLI (Terminal 2)

```bash
# Start containers in background
sudo ./engine start alpha ./rootfs /bin/busybox
sudo ./engine start beta ./rootfs /bin/busybox

# List tracked containers
sudo ./engine ps

# View container logs
sudo ./engine logs alpha

# Run a container in foreground and wait
sudo ./engine run alpha ./rootfs /memory_hog

# Stop a container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Check Kernel Logs

```bash
sudo dmesg | tail -20
```

### Unload Module and Clean Up

```bash
# Ctrl+C the supervisor in Terminal 1, then:
sudo rmmod monitor
sudo dmesg | tail -5
---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime achieves process and filesystem isolation using three Linux namespaces created via the `clone()` system call with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` flags.

The **PID namespace** gives each container its own process ID space. The first process inside the container becomes PID 1 from its own perspective, even though the host sees a different PID. This prevents containers from seeing or signalling each other's processes.

The **UTS namespace** allows each container to have its own hostname, set via `sethostname()` inside `child_fn`. This isolates the container's identity from the host and from other containers.

The **mount namespace** (`CLONE_NEWNS`) gives each container its own view of the filesystem. Combined with `chroot()` into the Alpine rootfs, the container cannot see the host filesystem. `/proc` is then mounted fresh inside the container using `mount("proc", "/proc", "proc", ...)`, giving the container accurate process information scoped to its own PID namespace.

Despite this isolation, all containers still share the same underlying Linux kernel, the same physical memory, and the same CPU scheduler. This is fundamentally different from full virtualization (like QEMU/KVM), where each VM has its own kernel instance. Our approach is lightweight but less secure — a kernel exploit inside a container affects the entire host.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor process is essential for centralized lifecycle management. Without it, each CLI invocation would be independent and unable to track container state across commands.

The supervisor uses `clone()` to spawn each container as a child process. Because `clone()` is used instead of `fork()`, we can pass namespace flags to isolate the child. The supervisor maintains a linked list of `container_record_t` structs protected by a `pthread_mutex_t`, tracking each container's PID, state, start time, memory limits, and log path.

When a container exits, the kernel sends `SIGCHLD` to the supervisor. The `sigchld_handler` calls `waitpid(-1, WNOHANG)` in a loop to reap all exited children without blocking. `WNOHANG` is critical — without it, the supervisor would block on a single child and miss signals for others. Reaped children are looked up by PID in the metadata list and their state is updated to `CONTAINER_EXITED` or `CONTAINER_KILLED` depending on whether they exited normally or were signalled.

`SIGTERM` and `SIGINT` to the supervisor trigger a `should_stop` flag, which causes the event loop to exit gracefully after killing all running containers and joining the logger thread.

### 4.3 IPC, Threads, and Synchronization

The project uses two distinct IPC mechanisms for two different purposes.

**Pipes** are used for log capture. Before `clone()`, a `pipe()` is created. Inside the container, `dup2()` redirects `stdout` and `stderr` to the write end. The supervisor reads from the read end in a dedicated producer thread per container. Pipes are chosen here because they are unidirectional, lightweight, and naturally follow the container lifecycle — when the container exits, the write end closes and the producer thread gets EOF.

**UNIX domain sockets** are used for the control channel between the CLI client and the supervisor. A socket at `/tmp/mini_runtime.sock` accepts `control_request_t` structs and sends back `control_response_t` structs. Sockets are chosen over pipes for the control channel because they are bidirectional — the CLI needs to send a command and receive a response in the same connection.

The **bounded buffer** sits between the producer threads (one per container) and the single consumer (logger) thread. It is a circular array of `log_item_t` structs with capacity 16. Access is protected by a `pthread_mutex_t`. Two `pthread_cond_t` variables — `not_full` and `not_empty` — are used for blocking coordination.

Without the mutex, two producer threads could simultaneously write to the same buffer slot, corrupting data. Without `not_full`, a producer would spin or overflow the buffer when it is full. Without `not_empty`, the consumer would spin-wait when the buffer is empty, wasting CPU. The condition variables allow threads to sleep and be woken precisely when the condition they need becomes true, avoiding both busy-waiting and data loss.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the amount of physical RAM currently occupied by a process's pages. It is read via `get_mm_rss()` on the process's `mm_struct` in the kernel. RSS does not include memory that has been swapped out, nor does it accurately account for shared memory pages (which may be counted multiple times across processes).

Soft and hard limits represent two different enforcement policies. A **soft limit** is a warning threshold — the process is still allowed to run, but an operator is notified that memory usage is approaching a dangerous level. A **hard limit** is a termination threshold — when exceeded, the process is killed with `SIGKILL` immediately.

Enforcement is implemented in kernel space rather than user space for two reasons. First, a user-space monitor would need to periodically read `/proc/<pid>/status`, which is slower and less accurate than directly calling `get_mm_rss()` on the `mm_struct`. Second, a malicious or buggy container process could manipulate its own `/proc` entries or exhaust resources before a user-space monitor reacts. Kernel-space enforcement is atomic and cannot be bypassed by the monitored process.

The kernel module uses a periodic timer firing every second to check all monitored processes. The list is protected by a `mutex` rather than a `spinlock` because `get_task_mm()` can sleep, and spinlocks do not permit sleeping while held.

### 4.5 Scheduling Behavior

The Linux Completely Fair Scheduler (CFS) aims to give each runnable process a fair share of CPU time. It tracks a **virtual runtime** (`vruntime`) for each process — a measure of how much CPU time the process has consumed, weighted by its priority. The process with the lowest `vruntime` is always scheduled next.

`nice` values map to CFS weight multipliers. A process with nice=0 has a higher weight than one with nice=10, meaning the scheduler advances its `vruntime` more slowly per unit of real CPU time. As a result, a nice=0 process is scheduled more frequently and receives more CPU time per unit of wall-clock time.

In our experiment, c2 (nice=10) was launched slightly before c1 (nice=0). c2 completed in 9.25 seconds while c1 took 19.25 seconds. This is because while both were running, c1 (higher priority) received more CPU time, causing c2 to finish its 10-second workload in less wall-clock time. After c2 finished, c1 had the CPU entirely to itself for the remainder. The total wall-clock time of ~19 seconds for c1 reflects the period it spent competing with c2 plus the period it ran alone. This demonstrates that CFS correctly allocates more CPU share to the higher-weight process.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation with chroot

**Choice:** PID + UTS + mount namespaces combined with `chroot()`.  
**Tradeoff:** Simpler than `pivot_root` but slightly less secure, as certain `/proc` escape paths remain possible.  
**Justification:** `chroot()` is sufficient for a project demonstrating isolation concepts. Full container runtimes like Docker use `pivot_root` for production security, which is beyond the scope of this project.

### UNIX Domain Socket for CLI Communication

**Choice:** A stream socket at `/tmp/mini_runtime.sock` for bidirectional CLI-to-supervisor communication.  
**Tradeoff:** Slightly more setup than a named pipe (FIFO), but supports full duplex communication in a single connection.  
**Justification:** The CLI needs to both send a command and receive a response. A unidirectional FIFO would require two FIFOs, adding complexity. A socket cleanly handles this with one `connect()`.

### Producer-Consumer Logging Design

**Choice:** One producer thread per container, one shared consumer thread, bounded circular buffer with mutex + condvar.  
**Tradeoff:** Added complexity from thread management and synchronization, but no log data is lost under high output rates.  
**Justification:** Direct writes from the supervisor's main loop would block on slow disk I/O and delay handling of incoming CLI commands. Decoupling with a bounded buffer keeps the main loop responsive.

### Kernel-Space Memory Enforcement

**Choice:** Linux Kernel Module with a periodic timer, linked list of monitored PIDs, and `SIGKILL` for hard limit.  
**Tradeoff:** More complex than user-space polling, and requires careful handling of kernel locking and memory allocation.  
**Justification:** Kernel-space enforcement is accurate and cannot be bypassed. User-space polling via `/proc` introduces latency and can be fooled. For a memory enforcement mechanism to be reliable, it must operate at the same privilege level as the memory subsystem itself.

### Separate IPC Channels for Logging and Control

**Choice:** Pipes for log data, sockets for control commands.  
**Tradeoff:** Two IPC mechanisms to implement and maintain instead of one.  
**Justification:** Mixing log data and control commands on the same channel would require framing and demultiplexing logic. Keeping them separate makes each channel simple and purpose-built, reducing the chance of bugs.

---

## 6. Scheduler Experiment Results

### Experiment: CPU-bound containers with different nice values

Two containers ran the same CPU-bound workload (`cpu_hog`, which runs for 10 seconds of pure computation) concurrently with different nice values.

```bash
time sudo ./engine run c1 ./rootfs /cpu_hog --nice 0 &
time sudo ./engine run c2 ./rootfs /cpu_hog --nice 10 &
wait
```

### Results

| Container | Nice Value | Priority | Execution Time |
|-----------|------------|----------|----------------|
| c1 | 0 | Higher | 19.25 seconds |
| c2 | 10 | Lower | 9.25 seconds |

### Observation

c2 (nice=10, lower priority) completed faster because it was launched slightly earlier and received CPU time while c1 had not yet started. Once both were running concurrently, c1 (nice=0) received a larger share of CPU time due to its higher CFS weight, causing c2 to finish its workload sooner in wall-clock time. c1 then ran alone for the remaining time, completing at 19.25 seconds total.

This demonstrates the core behavior of CFS: higher-weight (lower nice) processes receive proportionally more CPU time when competing, but all processes eventually make forward progress. The scheduler does not starve lower-priority processes — c2 still completed its full workload, just with a smaller CPU share.

---

## GitHub Repository

https://github.com/kushal/OS-Jackfruit.git