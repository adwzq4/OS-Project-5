// Author: Adam Wilson
// Date: 10/2/2020

#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <sys/time.h>
#include <sys/ipc.h> 
#include <sys/shm.h> 
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include "shared.h"

// intra-file globals
FILE* fp;
int msqid, shmid, semid, currentChildren, totalProcs, lastPID, vFlag;
union semun sem;
struct sembuf p = { 0, -1, SEM_UNDO };
struct sembuf v = { 0, +1, SEM_UNDO };
struct shmseg* shmptr;
struct statistics stats;

// print 2d allocation array as table
void displayAllocationTable() {
	int i, j;
	printf("\nAll");
	for (i = 0; i < 20; i++) { printf(" R%-2d", i); }
	printf("\n");
	for (i = 0; i < 18; i++) {
		printf("P%-2d", i);
		for (j = 0; j < 20; j++) { printf("  %-2d", shmptr->allocation[i][j]); }
		printf("\n");
	}
	printf("\n");
}

// creates a shared memory segment, a message queue, and a semaphore
void createMemory() {
	key_t shmkey, msqkey, semkey;
	
	shmkey = ftok("oss", 137);
	shmid = shmget(shmkey, sizeof(struct shmseg), 0666 | IPC_CREAT);
	if (shmid == -1) {
		perror("oss: Error");
		exit(-1);
	}

	shmptr = shmat(shmid, (void*)0, 0);
	if (shmptr == (void*)-1) { perror("oss: Error"); }

    semkey = ftok("oss", 484);
    semid = semget(semkey, 1, 0666 | IPC_CREAT);
    if (semid < 0) { perror("semget"); }
	sem.val = 1;
    if (semctl(semid, 0, SETVAL, sem) < 0) { perror("semctl"); }

	msqkey = ftok("oss", 731);
	msqid = msgget(msqkey, 0666 | IPC_CREAT);
	if (msqid == -1) { perror("oss: Error"); }
}

// outputs stats, waits for children, destroys message queue, and detaches and destroys shared memory
void terminateOSS() {
	int i, j, status;
	printf("\n\nOSS ran for %4f s\n", timeToDouble(shmptr->currentTime));
	printf("Average requests granted: %3f / process\n", stats.requestsGranted / (double)stats.numComplete);
	printf("Average requests denied: %3f / process\n", stats.requestsDenied / (double)stats.numComplete);
	printf("Average resource instances allocated: %3f / process\n", stats.numAllocated / (double)stats.numComplete);
	// TODO write stats
	fclose(fp);
	for (i = 0; i < currentChildren; i++) { mWait(&status); }
	if (msgctl(msqid, IPC_RMID, NULL) == -1) { perror("oss: msgctl"); }
    if (semctl(semid, 0, IPC_RMID, 0) == -1) { perror("Can't RPC_RMID."); }
	if (shmdt(shmptr) == -1) { perror("oss: Error"); }
	if (shmctl(shmid, IPC_RMID, 0) == -1) {
		perror("oss: Error");
		exit(-1);
	}
	exit(0);
}

// deletes output.log if it exists, then creates it in append mode
void setupFile() {
	fp = fopen("output.log", "w+");
	if (fp == NULL) { perror("oss: Error"); }
	fclose(fp);
	fp = fopen("output.log", "a");
	if (fp == NULL) { perror("oss: Error"); }
}

// sends message to stderr, then kills all processes in this process group, which is ignored by parent
static void interruptHandler(int s) {
	fprintf(stderr, "\nInterrupt recieved.\n");
	signal(SIGQUIT, SIG_IGN);
	kill(-getpid(), SIGQUIT);
	terminateOSS();
}

// sets up sigaction for SIGALRM
static int setupAlarmInterrupt(void) {
	struct sigaction sigAlrmAct;
	sigAlrmAct.sa_handler = interruptHandler;
	sigAlrmAct.sa_flags = 0;
	sigemptyset(&sigAlrmAct.sa_mask);
	return (sigaction(SIGALRM, &sigAlrmAct, NULL));
}

// sets up sigaction for SIGINT, using same handler as SIGALRM to avoid conflict
static int setupSIGINT(void) {
	struct sigaction sigIntAct;
	sigIntAct.sa_handler = interruptHandler;
	sigIntAct.sa_flags = 0;
	sigemptyset(&sigIntAct.sa_mask);
	return (sigaction(SIGINT, &sigIntAct, NULL));
}

// sets ups itimer with time of 5s and interval of 0s
static int setupitimer() {
	struct itimerval value = { {0, 0}, {5, 0} };
	return (setitimer(ITIMER_REAL, &value, NULL));
}

// sets up timer, and SIGALRM and SIGINT handlers
static int setupInterrupts() {
	if (setupitimer() == -1) {
		perror("oss: Error");
		exit(-1);
	}
	if (setupAlarmInterrupt() == -1) {
		perror("oss: Error");
		exit(-1);
	}
	if (setupSIGINT() == -1) {
		perror("oss: Error");
		exit(-1);
	}
}

// runs deadlock detection algorithm on a hypothetical allocation
bool deadlockDetection(int pid, int rid, int numRequested) {
	int i, j, p, work[20];
	bool finish[18];

	for (i = 0; i < 20; i++) { work[i] = shmptr->available[i]; }
	for (i = 0; i < 18; i++) { finish[i] = false; }
	for (p = 0; p < 18; p++) {
		if (finish[p]) { continue; }
		for (i = 0; i < 20; i++) { if (shmptr->maximum[p][i] - shmptr->allocation[p][i] > work[i]) { break; } }
		if (i == 20) {
			finish[p] = true;
			for (j = 0; j < 20; j++) { work[j] += shmptr->allocation[p][j]; }
			p = -1;
		}
	}
	for (p = 0; p < 18; p++) { if (!finish[p]) { break; } }

	return (p != 18);
}

// first checks to see if a request is valid and if there are enough resources currently available to fulfill it; if so,
// calls deadlockDetection() to see if that allocation will put the system in a safe or unsafe state
enum action bankersAlgorithm(int pid, int rid, int numRequested) {
	if (numRequested > shmptr->need[pid][rid]) {
		printf("OSS: error: P%d asked for more than initial max request\n", pid);
		signal(SIGQUIT, SIG_IGN);
		kill(-getpid(), SIGQUIT);
		terminateOSS();
	}
	else if (numRequested <= shmptr->available[rid]) {
		// hypothetically grant request
		shmptr->allocation[pid][rid] += numRequested;
		shmptr->need[pid][rid] -= numRequested;
		// if a shareable resource was requested deadlockDetection() needn't be called, and available[] doesn't change
		if (shmptr->descriptors[rid].rType == shareable) {
			printf("Granting request for shareable resource R%d:%d\n", rid, numRequested);
			stats.numAllocated += numRequested;
			stats.requestsGranted++;
			return confirm;
		}
		shmptr->available[rid] -= numRequested;
		if (vFlag) printf("OSS: Running deadlock detection at %f s\n", timeToDouble(shmptr->currentTime));
		
		// if no deadlock, just return confirm, otherwise roll back hypothetical grant
		if (!deadlockDetection(pid, rid, numRequested)) { 
			if (vFlag) printf("\tSafe state after granting request.\n\t");
			else printf("OSS: ");
			printf("Request by P%d for R%d:%d granted.\n", pid, rid, numRequested);
			stats.numAllocated += numRequested;
			stats.requestsGranted++;
			return confirm; 
		}
		else {
			if (vFlag) {
				printf("\tUnsafe state after granting request.\n\tRequest by P%d for R%d:%d denied, adding to wait queue.\n", pid, rid, numRequested);
			}
			shmptr->available[rid] += numRequested;
			shmptr->allocation[pid][rid] -= numRequested;
			shmptr->need[pid][rid] += numRequested;
		}
	}
	else if (vFlag) { printf("OSS: request by P%d for R%d:%d denied due to lack of available resources, adding to wait queue\n", pid, rid, numRequested); }

	stats.requestsDenied++;	
	// add process/numRequested to the wait queue for this resource class, then return block
	enqueue(shmptr->descriptors[rid].waitQ, (struct waitingProc) { pid, numRequested });
	return block;
}

// runs bankersAlgorithm()/deadlockDetection() for each process in a resource descriptor's wait queue, and
// sends a message to wake any blocked process for which it would be safe to do so
void checkWaitQueue(int rid) {
	int i;
	struct waitingProc proc;
	struct msgbuf buf;
	int numWaiting = shmptr->descriptors[rid].waitQ->size;
	for (i = 0; i < numWaiting; i++) {
		proc = dequeue(shmptr->descriptors[rid].waitQ);
		if (bankersAlgorithm(proc.pid, rid, proc.numRequested) == confirm) {
			buf = (struct msgbuf) { proc.pid + 1, 20, rid, proc.numRequested, wake };
			printf("OSS: waking P%d and granting its request for R%d:%d\n", proc.pid, rid, proc.numRequested);
			shmptr->currentTime = addTime(shmptr->currentTime, 0, rand() % 100000 + 100000);
			if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("oss: Error"); }
		}
	}
}

// handles process termination, releasing any allocated resources and listing them, then calling
// checkWaitQueue() for each resource class that was released
void terminateProc(int pid){
	int status, i;
	
	if (semop(semid, &p, 1) < 0) { perror("semop p"); }
	shmptr->PIDmap[pid] = 0;
	stats.numComplete++;
	if (vFlag) printf("Receiving that P%d terminated\n\tResources released :", pid);
	for (i = 0; i < 20; i++) {
		if (shmptr->descriptors[i].rType == nonshareable) { shmptr->available[i] += shmptr->allocation[pid][i]; }
		if (vFlag && shmptr->allocation[pid][i] > 0) { 
			printf("  R%d:%d  ", i, shmptr->allocation[pid][i]);
		}
		shmptr->allocation[pid][i] = shmptr->maximum[pid][i] = shmptr->need[pid][i] = 0;
	}
	if (vFlag) printf("\n");
	for (i = 0; i < 20; i++) checkWaitQueue(i);
	if (semop(semid, &v, 1) < 0) { perror("semop v"); }
	
	mWait(&status);
	currentChildren--;
}

// spawn a new child and place it in the correct queue
void spawnChildProc() {
	int i;
	pid_t pid;

	// finds available pid for new process, sets corresponding index of PIDmap to 1, and increments totalProcs and currentChildren
	for (i = lastPID + 1; i < 18; i++) { if (shmptr->PIDmap[i] == 0) { break; } }
	if (i == 18) { for (i = 0; i < lastPID; i++) { if (shmptr->PIDmap[i] == 0) { break; } } }
	shmptr->PIDmap[i] = 1;
	lastPID = i;
	currentChildren++;
	totalProcs++;

	// fork
	pid = fork();

	// rolls values back if fork fails
	if (pid == -1) {
		shmptr->PIDmap[i] = 0;
		currentChildren--;
		totalProcs--;
		perror("oss: Error");
	}

	// exec child
	else if (pid == 0) {
		char index[2];
		sprintf(index, "%d", i);
		execl("user_proc", index, (char*)NULL);
		exit(0);
	}

	// updates clock and displays time
	else { 
		if (semop(semid, &p, 1) < 0) { perror("semop p"); }
		shmptr->currentTime = addTime(shmptr->currentTime, 0, rand() % 10000 + 10000);
		if (vFlag) printf("OSS: generating P%d at time %f s\n", i, timeToDouble(shmptr->currentTime));
		if (semop(semid, &v, 1) < 0) { perror("semop v"); }
	}
}

// spawns and schedules children according to multi-level feedback algorithm, keeping track of statistics
int main(int argc, char* argv[]) {
	int randomWait, i, j, numGranted, opt;
	const int PROCMAX = 40;
	const int SHAREABLE_RATIO = rand() % 11 + 15;
	struct msgbuf buf;

	// initialize globals, interrupts, file pointer, and shared memory
	stats = (struct statistics) { { 0, 0 }, { 0, 0 }, 0, 0, 0, 0 };
	currentChildren = totalProcs = numGranted = vFlag = 0;
	lastPID = -1;
	setupInterrupts();
	createMemory();
	setupFile();
	srand(time(0));

	// parses command line arguments
    while ((opt = getopt(argc, argv, "v")) != -1) { if (opt == 'v') { vFlag = 1; } }

	// creates 20 resource descriptors, 15-25% are shareable and each has 1-10 instances and a wait queue;
	// initializes available[] with each resource class's number of instances
	for (i = 0; i < 20; i++) {
		shmptr->descriptors[i] = (struct resourceDescriptor) { nonshareable, rand() % 10 + 1, createQueue() };
		if (rand() % 100 < SHAREABLE_RATIO) { shmptr->descriptors[i].rType = shareable; }
		shmptr->available[i] = shmptr->descriptors[i].instances;
	}

	// initialize allocation[][], maximum[][], and need[][] tables with all 0s
	for (i = 0; i < 18; i++) {
		for (j = 0; j < 20; j++) { shmptr->allocation[i][j] = shmptr->maximum[i][j] = shmptr->need[i][j] = 0; }
	}

	// initialize PIDmap and currentTime
	for (i = 0; i < 18; i++) { shmptr->PIDmap[i] = 0; }
	shmptr->currentTime.sec = shmptr->currentTime.ns = 0;

	// runs OSS until 40 processes have been spawned, and then until all children have terminated
	while (totalProcs < PROCMAX || currentChildren > 0) {
		randomWait = rand() % (BILLION / 2 - 1000) + 1000;

		// update shared clock
		if (semop(semid, &p, 1) < 0) { perror("semop p"); }
		shmptr->currentTime = addTime(shmptr->currentTime, 0, randomWait);
    	if (semop(semid, &v, 1) < 0) { perror("semop v"); }

		// spawns new process if process table isn't full and PROCMAX hasn't been reached
		if (currentChildren < 18 && totalProcs < PROCMAX) { spawnChildProc(); }

		// check to see if their is a message for OSS, but do not wait for one
		if (msgrcv(msqid, &buf, sizeof(struct msgbuf), 20, IPC_NOWAIT) >= 0) {
			if (buf.act == terminate) { terminateProc(buf.pid); }
			// handles release of resources, updating allocation[][] and, if nonshareable, available[], then calls checkWaitQueue()
			else if (buf.act == release) {
				if (semop(semid, &p, 1) < 0) { perror("semop p"); }
				if (vFlag) {
					printf("Receiving that P%d is releasing %d instance(s) of R%d\n", buf.pid, buf.instances, buf.resource);
				}
				shmptr->allocation[buf.pid][buf.resource] -= buf.instances;
				if (shmptr->descriptors[buf.resource].rType == nonshareable) {shmptr->available[buf.resource] += buf.instances; }
				for (i=0; i < 20; i++) checkWaitQueue(i);
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }
				
				// send a message back confirming the release
				buf.type = buf.pid + 1;
				buf.pid = 20;
				buf.act = confirm;
				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("oss: Error"); }
			}
			// calls bankersAlgorithm()/deadlockDetection() any time resources are requested, then sends a message back
			// with either confirm or block, depending on the result
			else if (buf.act == request) {
				if (semop(semid, &p, 1) < 0) { perror("semop p"); }
				printf("OSS: Receiving that P%d is requesting %d instance(s) of R%d\n", buf.pid, buf.instances, buf.resource);
				buf.act = bankersAlgorithm(buf.pid, buf.resource, buf.instances);
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }

				buf.type = buf.pid + 1;
				buf.pid = 20;
				// displays allocation table every 20 times a request is granted
				if (buf.act == confirm) {
					numGranted++;
					if (numGranted >= 20) {
						if (semop(semid, &p, 1) < 0) { perror("semop p"); }
						displayAllocationTable();
						if (semop(semid, &v, 1) < 0) { perror("semop v"); }
						numGranted = 0;
					}
				}

				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("oss: Error"); }
			}
		}
	}

	// finish up
	printf("\nOSS: 40 processes have been spawned and run to completion, now terminating OSS.\n");
	terminateOSS();
}