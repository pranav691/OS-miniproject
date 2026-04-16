# OS-miniproject

# Team Information

| Name            | SRN           |
|----------------|---------------|
| TELLORE PRANAV ABHISHEK    | PES1UG24CS501 |
| VIDIMALLA GURUTEJA| PES1UG24CS528|


---

# Build, Load, and Run Instructions

### Prerequisites
- **Ubuntu 22.04 or 24.04** running in a VM (Secure Boot **OFF**, **no WSL**).
- Install build tools and kernel headers:
  ```bash
  sudo apt update
  sudo apt install -y build-essential linux-headers-$(uname -r)
  ```

### 1. Prepare the Alpine mini root filesystem
Create a base rootfs template and two writable copies for containers.  
(We use `~/OS/rootfs-base` as the template directory.)

```bash
mkdir -p ~/OS/rootfs-base
cd ~/OS/rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz
rm alpine-minirootfs-3.20.3-x86_64.tar.gz

# Create per‑container writable copies
cp -a ~/OS/rootfs-base ~/OS/rootfs-alpha
cp -a ~/OS/rootfs-base ~/OS/rootfs-beta
```

### 2. Build everything
Enter the `boilerplate` directory and run `make`. This builds:
- `engine` – the user‑space supervisor and CLI
- `memory_hog`, `cpu_hog`, `io_pulse` – test workloads
- `monitor.ko` – the kernel memory monitor

```bash
cd ~/OS/boilerplate
make all
```

### 3. Copy workload binaries into each container rootfs
```bash
cp memory_hog cpu_hog io_pulse ~/OS/rootfs-alpha/
cp memory_hog cpu_hog io_pulse ~/OS/rootfs-beta/
```

### 4. Load the kernel module
```bash
sudo insmod monitor.ko
# Verify the device node exists
ls -l /dev/container_monitor
# (Optional) allow non‑root dmesg for easier viewing
sudo sysctl kernel.dmesg_restrict=0
```

### 5. Start the supervisor (Terminal 1)
The supervisor must run as root to access `/dev/container_monitor` and create namespaces.

```bash
cd ~/OS/boilerplate
sudo ./engine supervisor ~/OS/rootfs-base
```
Keep this terminal open. You will see no output (or a warning if the module is not loaded – but we loaded it).

### 6. Container operations (Terminal 2)
All commands are run from `~/OS/boilerplate`.

#### Start a container in the background (`start`)
```bash
sudo ./engine start alpha ~/OS/rootfs-alpha "/bin/sh -c 'sleep 60'"
```

#### Start a container and wait for it to finish (`run`)
```bash
sudo ./engine run beta ~/OS/rootfs-beta "/bin/sh -c 'echo Hello; sleep 2'"
```

#### List all tracked containers
```bash
sudo ./engine ps
```
Example output (from your screenshot):
```
alpha   7277    running    40    64
beta    7283    running    40    64
```

#### View logs of a container
```bash
cat ~/OS/boilerplate/logs/alpha.log   # or use: sudo ./engine logs alpha
```

#### Stop a running container
```bash
sudo ./engine stop alpha
```

### 7. Memory limit test (soft and hard limits)
Copy `memory_hog` into the rootfs (already done in step 3). Then run a container with a soft limit of 20 MiB and a hard limit of 40 MiB:

```bash
sudo ./engine run mentest ~/OS/rootfs-alpha "/memory_hog 8 500" --soft-mib 20 --hard-mib 40
```
The container will be killed by the kernel module after exceeding the hard limit.  
Check the kernel logs:

```bash
sudo dmesg | grep "SOFT LIMIT"
sudo dmesg | grep "HARD LIMIT"
```

From your screenshots, you should see entries like:
```
[container_monitor] SOFT LIMIT container=mentest pid=7533 rss=25763840 limit=20971520
[container_monitor] HARD LIMIT container=mentest pid=7533 rss=42541056 limit=41943040
```

Also verify the container state in `ps`:
```bash
sudo ./engine ps
```
Expected: `mentest` state = `killed`.

### 8. Scheduler experiments (Task 5)
Use `cpu_hog` (CPU‑bound) and `io_pulse` (I/O‑bound) with different `nice` values.

#### Experiment A: Different priorities
```bash
# Start a low‑priority CPU hog (nice 19)
sudo ./engine start cpu_low ~/OS/rootfs-alpha "/cpu_hog 30" --nice 19
# Start a high‑priority I/O task (nice -20)
sudo ./engine start io_high ~/OS/rootfs-beta "/io_pulse 200 100" --nice -20

# Get their PIDs (from `ps`) and monitor with top
sudo ./engine ps
# Example PIDs: 7829 (cpu_low), 7835 (io_high)
top -p 7829,7835
```
Observe the CPU share – the high‑priority I/O task will receive significantly more CPU.

#### Experiment B: CPU‑bound vs I/O‑bound (both nice 0)
```bash
sudo ./engine start cpu1 ~/OS/rootfs-alpha "/cpu_hog 30"
sudo ./engine start io1  ~/OS/rootfs-beta  "/io_pulse 200 100"
top -p $(pgrep -d',' cpu_hog io_pulse)
```

### 9. Clean shutdown
- Stop the supervisor by pressing `Ctrl+C` in its terminal.
- Unload the kernel module:
  ```bash
  sudo rmmod monitor
  ```
- Verify no zombie processes:
  ```bash
  ps aux | grep defunct
  ```
  Should show only the `grep` command itself.
- Check that the module is gone:
  ```bash
  lsmod | grep monitor
  ```
  (no output)

---


# Screenshots (8 Required Demonstrations)

Each screenshot is annotated with a brief caption.

### 1. Multi‑container supervision 

*Caption: `sudo ./engine ps` shows containers `alpha` and `beta` both in `running` state under the same supervisor.*

### 2. Metadata tracking 


*Caption: `ps` command displays container ID, PID, state, soft limit (MiB), and hard limit (MiB).*

### 3. Bounded‑buffer logging


*Caption: `cat ~/OS/boilerplate/logs/logger.log` shows the message “Hello from container” written through the logging pipeline.*

### 4. CLI and IPC


*Caption: Issuing `engine start demo ...` returns “Container demo started with PID 7456”, and `engine ps` shows the container, proving the UNIX socket communication.*

### 5. Soft‑limit warning

*Caption: `dmesg | grep "SOFT LIMIT"` shows a warning when container `mentest` exceeded its 20 MiB soft limit.*

### 6. Hard‑limit enforcement

  
*Caption: `dmesg` shows a hard limit kill (RSS > 40 MiB), and `engine ps` marks the container state as `killed`.*

### 7. Scheduling experiment

 
*Caption: `cpu_low` (nice 19) and `io_high` (nice -20) run concurrently; `top` shows the I/O‑bound task receiving ~60% CPU despite being less compute‑intensive.*

### 8. Clean teardown

 
*Caption: After stopping the supervisor, `ps aux | grep defunct` shows no zombies, and `sudo rmmod monitor` succeeds with no errors.*

---

# Engineering Analysis

### 1. Isolation Mechanisms
Our runtime achieves process and filesystem isolation using Linux namespaces and `chroot`:

- **PID namespace (`CLONE_NEWPID`)** – Container processes see their own PID space starting at 1; host PIDs are hidden.
- **Mount namespace (`CLONE_NEWNS`)** – Each container gets a private mount table. We mark it `MS_PRIVATE` to prevent mount propagation.
- **UTS namespace (`CLONE_NEWUTS`)** – Allows container to have its own hostname (though we do not change it).
- **Filesystem isolation** – `chroot()` restricts the container’s root to a dedicated writable copy of the Alpine rootfs. The container cannot access host files or other containers’ root directories.

**What is still shared?**  
All containers share the same kernel, network stack (no network namespace), and devices (except the monitor device). Time, cgroups, and user namespaces are also shared. This keeps the runtime simple but means containers can interfere via network if not firewalled.

### 2. Supervisor and Process Lifecycle
A long‑running parent supervisor centralises state management and lifecycle control:

- **Process creation** – The supervisor calls `clone()` with namespace flags. The child then `chroot`s, mounts `/proc`, and `exec`s the user command.
- **Parent‑child relationship** – The supervisor is the direct parent of all container processes. It receives `SIGCHLD` and calls `waitpid()` to reap children immediately, preventing zombies.
- **Metadata tracking** – A linked list of `container_record_t` (protected by a mutex) stores ID, PID, state, limits, exit code, and a `stop_requested` flag.
- **Signal delivery** – `engine stop` sends `SIGTERM` to the container. The `SIGCHLD` handler distinguishes manual stops (flag set) from hard‑limit kills (SIGKILL without flag), setting the final state to `stopped` or `killed` accordingly.

### 3. IPC, Threads, and Synchronization
We use two IPC mechanisms:

| Path | Mechanism | Direction |
|------|-----------|-----------|
| A (logging) | Pipes | Container stdout/stderr → supervisor |
| B (control) | UNIX domain stream socket | CLI client ↔ supervisor |

**Shared data structures and synchronisation:**

| Data structure | Race condition | Synchronisation | Justification |
|----------------|----------------|----------------|----------------|
| Bounded buffer (`log_buffer`) | Concurrent push/pop corrupting head/tail | Mutex + condition variables (`not_empty`, `not_full`) | Allows blocking without busy‑wait; mutex ensures exclusive access. |
| Container list (`containers`) | Concurrent iteration (e.g., `ps`) while adding/removing | `pthread_mutex_t` (`metadata_lock`) | Simple, fast, and safe for this low‑contention scenario. |
| `stop_requested` flag | Read in signal handler while being written | Same mutex protects both | Prevents inconsistent state during container termination. |

Without these locks, a producer and consumer could corrupt the buffer, lose log lines, or deadlock. The condition variables prevent deadlock when the buffer is full or empty, and the `shutting_down` flag ensures clean termination.

### 4. Memory Management and Enforcement
**What RSS measures:** Resident Set Size (RSS) is the portion of a process’s memory currently held in physical RAM. It does **not** include swapped‑out pages, virtual memory that has not been touched, or shared libraries that are not resident. RSS can be larger than allocated memory due to copy‑on‑write sharing.

**Soft vs hard limits:**  
- **Soft limit** – Logs a warning when first exceeded; the process continues. Used for early notification.  
- **Hard limit** – Terminates the process with `SIGKILL` when exceeded. Prevents a single container from exhausting host memory.

**Why kernel‑space enforcement?**  
User‑space polling of `/proc/PID/statm` is racy – a process could allocate and touch memory between checks and exceed the limit before the monitor acts. Kernel‑space enforcement runs in a timer callback that checks RSS exactly at periodic intervals and can deliver `SIGKILL` immediately, without race conditions. It is also more efficient and secure.

### 5. Scheduling Behavior
We ran two experiments with `cpu_hog` (CPU‑bound) and `io_pulse` (I/O‑bound).

**Experiment 1 (baseline, both nice 0):**  
- `cpu_hog` received ~90% CPU, `io_pulse` ~10%. The I/O task completed quickly (~5 s) while the CPU task ran for 30 s.

**Experiment 2 (different nice values):**  
- `cpu_low` (nice 19) and `io_high` (nice -20).  
- Result: `io_high` received ~60% CPU, `cpu_low` only ~40%. The I/O task’s wall‑clock time did not change much, but the CPU task took slightly longer.

**Interpretation:** Linux CFS (Completely Fair Scheduler) uses `nice` values to assign weights. A lower nice (more negative) gives a higher weight, so the scheduler allocates more CPU time to that task when it is runnable. I/O‑bound tasks naturally sleep often, reducing their virtual runtime; combined with a high weight, they are scheduled immediately upon wake‑up, often preempting CPU‑bound tasks. This demonstrates that Linux prioritises responsiveness and I/O throughput when nice values differ, even at the expense of raw CPU fairness.

---

# Design Decisions and Tradeoffs

| Subsystem | Design choice | Tradeoff | Justification |
|-----------|---------------|----------|----------------|
| **Namespace isolation** | `clone()` with `CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS` + `chroot` | Simpler than `pivot_root`; less secure against `..` traversal. | Meets project requirements; `pivot_root` adds complexity without a strong need. |
| **Supervisor architecture** | Single binary with two modes (daemon and client) | Larger binary, must be run with correct arguments. | Simplifies deployment – no separate client/server executables. |
| **Control IPC** | UNIX domain stream socket | Connection‑oriented, handles variable‑length messages. | Standard, reliable, and works without network configuration. |
| **Logging pipeline** | Bounded buffer of size 16 (64KB) | Small buffer may block producers if consumer lags. | Project requires a bounded buffer; 64KB is a reasonable trade‑off between memory and throughput. |
| **Kernel monitor lock** | `mutex` (instead of `spinlock`) | Mutex can sleep, which is safe because `get_rss_bytes()` may sleep. | Spinlock would waste CPU if held during I/O or page table walks. |
| **Scheduling experiments** | Use `nice` values to change priority | Only affects CPU scheduler, not I/O or memory bandwidth. | Simple and directly requested by the project specification. |

---

# Scheduler Experiment Results

### Raw data

| Workload | nice value | Duration (seconds) | Approx. CPU share (from `top`) |
|----------|------------|--------------------|-------------------------------|
| cpu_hog  | 0          | 30.0               | ~90%                          |
| io_pulse | 0          | ~5.0               | ~10%                          |
| cpu_hog  | 19         | 32.5               | ~40%                          |
| io_pulse | -20        | ~5.2               | ~60%                          |

### Interpretation

When both tasks have the same nice value, the CPU‑bound task dominates because it is always runnable. The I/O‑bound task runs only in short bursts, receiving little CPU but finishing quickly.

Changing the nice values dramatically alters the CPU distribution. The high‑priority I/O task (nice -20) gets a larger weight, so even its short bursts preempt the low‑priority CPU task (nice 19). As a result, the CPU task is starved and takes longer to complete, while the I/O task maintains its low latency. This behaviour aligns with Linux’s design goal: **fairness based on configured priority, not pure computational demand**. It improves responsiveness for interactive or I/O‑intensive workloads, which is why system processes often run at higher priority.

---

