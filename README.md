
# Building a mini Docker with a boss process (supervisor) and tiny command tools (CLI clients)
A lightweight container runtime with kernel-level memory monitoring, designed to explore core operating system concepts including process isolation, scheduling, IPC, and kernel-user interaction.

---

# Team Information

- PES1UG24AM288
- PES1UG24AM295

---

# Build, Load and Run Instructions

### Build
```bash
make
```

### Load Kernel Module
```bash
sudo insmod monitor.ko
sudo dmesg | tail
```

### Verify Control Device
```bash
ls -l /dev/container_monitor
sudo chmod 666 /dev/container_monitor
```

### Start Supervisor
```bash
sudo ./engine supervisor
```

### Start Containers
```bash
sudo ./engine start alpha
sudo ./engine start beta
```

### List Containers
```bash
sudo ./engine ps
```

### View Logs
```bash
sudo ./engine logs alpha
```

### Memory Monitoring
```bash
sudo ./engine start alpha
sudo dmesg -w
```

### Scheduling Experiment
```bash
sudo ./cpu_hog
sudo ./io_pulse
```

### Clean Teardown
```bash
sudo pkill engine
ps aux | grep engine
```

### Unload Module
```bash
sudo rmmod monitor
```
---

# Engineering Analysis

## Isolation Mechanisms
Isolation is achieved using Linux namespaces, primarily the PID namespace via clone(CLONE_NEWPID). Each container operates in its own process ID space, preventing visibility of host or other container processes.
Filesystem isolation is approximated using separate root directories per container. While not a full pivot_root implementation, this approach restricts processes to a confined filesystem subtree.
All containers still share:
- the same kernel
- global scheduling
- physical memory
This shared-kernel model makes containers lightweight compared to virtual machines.

## Supervisor and Processor Lifecycles
A persistent supervisor process manages all containers. It creates containers using clone() and maintains metadata such as container ID and PID.
The supervisor:
- tracks active containers
- handles termination
- reaps processes to prevent zombies
Signals like SIGCHLD notify the supervisor when containers exit, ensuring correct lifecycle handling. This design mirrors real-world container orchestration systems.

## IPC, Threads and Synchronization
A persistent supervisor process manages all containers. It creates containers using clone() and maintains metadata such as container ID and PID.
The supervisor:
- tracks active containers
- handles termination
- reaps processes to prevent zombies
Signals like SIGCHLD notify the supervisor when containers exit, ensuring correct lifecycle handling. This design mirrors real-world container orchestration systems.

## Memory Management and Enforcement
Memory usage is measured using RSS (Resident Set Size), which represents physical memory currently used by a process. RSS does not include swapped-out memory or unused allocations.
Two limits are enforced:
- Soft limit: generates a warning
- Hard limit: terminates the process
The soft limit allows monitoring, while the hard limit ensures strict enforcement.
Enforcement is implemented in kernel space to ensure:
- accurate tracking
- immediate response
- system-level control
User-space alone cannot reliably enforce such constraints.

## Scheduling Behaviour
Scheduling experiments compare CPU-bound and I/O-bound workloads.
- CPU-bound (cpu_hog) continuously uses CPU
- I/O-bound (io_pulse) frequently yields
The Linux scheduler balances:
- fairness
- responsiveness
- throughput
I/O-bound processes receive more frequent scheduling opportunities, while CPU-bound processes receive longer execution slices. This behavior aligns with the Completely Fair Scheduler (CFS).

---

# Design Decision and Tradeoffs
## Namespace Isolation
- Choice: PID namespace via clone()
- Tradeoff: Limited isolation compared to full container runtimes
Justification: Simpler implementation while demonstrating core concepts

## Supervisor Architecture
- Choice: Central supervisor process
- Tradeoff: Single point of failure
- Justification: Simplifies lifecycle management and coordination

## IPC and Logging
- Choice: UNIX sockets + bounded buffer
- Tradeoff: Increased complexity
- Justification: Demonstrates real-world IPC and synchronization

## Kernel Monitor
- Choice: Loadable kernel module with ioctl
- Tradeoff: Requires root privileges and careful debugging
- Justification: Enables accurate memory enforcement

## Scheduling Experiments
- Choice: Custom workloads
- Tradeoff: Not highly precise benchmarking
- Justification: Clearly demonstrates scheduler behavior

---

# Scheduler Experiment Results
| Workload | Behavior |
| :--- | :--- |
| `cpu_hog` | Continuous CPU usage |
| `io_pulse` | Periodic bursts with idle time |

The scheduler ensures responsiveness for I/O-bound tasks while maintaining fairness for CPU-bound processes, illustrating balanced CPU allocation.

---
 
 # Conclusion
This project demonstrates the interaction between user-space control and kernel-space enforcement in a containerized system. By combining namespaces, IPC, synchronization, and kernel modules, it provides a practical exploration of core operating system principles.

---
