# Process Lifecycle Management System

An interactive Operating Systems simulator. It shows how a process moves through
**NEW → READY → RUNNING → WAITING / SUSPENDED → TERMINATED**, with a Process Control Block (PCB)
for every process, Ready/Waiting/Suspended queues, a simulated CPU and OS clock, activity logs and statistics.

The backend is written in **C** (POSIX sockets, no external libraries). The frontend is a single HTML page served by the C program.

## Features
- Create processes (name, priority, CPU burst, memory) with auto-generated PIDs (P001, P002, ...)
- Full PCB per process: PID, state, priority, program counter, burst, remaining time, memory, arrival, start and termination time, parent PID
- State machine enforced by the backend. Invalid transitions (for example TERMINATED → RUNNING) are rejected with a clear error
- Only one process can run at a time. Remaining time drops each clock tick and the process terminates when it reaches zero
- Priority scheduler (automatic, can be switched off to dispatch manually)
- Dashboard, Processes, Visualizer, PCB, Scheduler, Analytics, Logs and OS Concepts pages
- Waiting, turnaround and response time, plus CPU utilization
- Data is saved to `plms.dat` and survives restarts

## Run it

Requires `gcc` and `make` (Linux, macOS, or WSL on Windows).

```bash
git clone <your-repo-url>
cd <repo-folder>
make run
```

Then open **http://localhost:8080**. Use another port with `./plms 9000`. Run from the repo folder so the server finds `public/index.html`.

### Run it temporarily without installing anything
Use **GitHub Codespaces**: on your repo page choose *Code → Codespaces → Create codespace*, then in its terminal run `make run`. Open the forwarded port 8080 when the prompt appears. Delete the codespace when you are done.

If the port does not open in a Codespace or container, bind to all interfaces: `./plms 8080 0.0.0.0`.

## Project layout
```
server.c            C backend: state machine, scheduler, REST API, HTTP server
public/index.html   Frontend (HTML, CSS, JavaScript in one file)
Makefile            Build and run
```

## REST API
| Method | Endpoint | Purpose |
|---|---|---|
| POST / GET | `/api/processes` | Create / list processes |
| GET / PUT / DELETE | `/api/processes/P001` | Read / edit name and priority / delete |
| POST | `/api/processes/P001/{admit,run,wait,suspend,resume,terminate}` | Change state |
| GET | `/api/processes/P001/history` | State history of one process |
| GET | `/api/statistics` | Counts, CPU utilization, average times |
| GET | `/api/logs` | Latest 100 activity log entries |
| GET | `/api/state` | Everything the UI polls once per second |
| POST | `/api/scheduler` | `{"auto": true/false}` |
| POST | `/api/reset` | Clear all processes |

Example:
```bash
curl -X POST localhost:8080/api/processes -d '{"name":"Process A","priority":2,"cpuBurst":10,"memory":128}'
```

## Valid state transitions
`NEW→READY`, `READY→RUNNING`, `RUNNING→WAITING`, `WAITING→READY`, `RUNNING→SUSPENDED`, `READY→SUSPENDED`, `SUSPENDED→READY`, `RUNNING→TERMINATED`

## Notes
This is an educational simulation. It does not manage real operating system processes.
It stores data in a file instead of MongoDB and updates the UI by polling instead of Socket.IO, because the backend is plain C.
There is no login. The server listens on localhost only unless you pass another address.
