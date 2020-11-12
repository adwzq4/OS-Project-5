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

// creates a shared memory segment and a message queue
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
	int i, status;
	printf("oss ran for %f s\n", timeToDouble(shmptr->currentTime));
	// TODO write stats
	fclose(fp);
	for (i = 0; i < currentChildren; i++) { mWait(&status); }
	if (msgctl(msqid, IPC_RMID, NULL) == -1) { perror("oss: msgctl"); }
    if (semctl(semid, 0, IPC_RMID, 0) == -1) { perror("\nCan't RPC_RMID."); }
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

// sends message to stderr, kills all processes in this process group, which is 
// ignored by parent, then zeros out procMax so no new processes are spawned
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

// sets ups itimer with time of 3s and interval of 0s
static int setupitimer() {
	struct itimerval value = { {0, 0}, {2, 0} };
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

// handle process termination, adding process's cpu usage, lifetime, and blocked time to cumalative totals, setting PIDmap to 0, and updating counters
void terminateProc(int pid){
	int status, i;
	
	if (semop(semid, &p, 1) < 0) { perror("semop p"); }
	printf("Receiving that P%d terminated\n\tResources released :", pid);
	for (i = 0; i < 20; i++) {
		if (shmptr->allocation[pid][i] > 0) { printf("  R%d:%d  ", i, shmptr->allocation[pid][i]); }
		shmptr->available[i] += shmptr->allocation[pid][i];
		shmptr->allocation[pid][i] = shmptr->maximum[pid][i] = shmptr->need[pid][i] = 0;
	}
	printf("\n");
	shmptr->PIDmap[pid] = 0;
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

	else { 
		if (semop(semid, &p, 1) < 0) { perror("semop p"); }
		printf("OSS: generating P%d at time %f s\n", i, timeToDouble(shmptr->currentTime));
		if (semop(semid, &v, 1) < 0) { perror("semop v"); }
	}
}

void displayAllocationTable(){
	int i, j;
	if (semop(semid, &p, 1) < 0) { perror("semop p"); }
	printf("\n   ");
	for (i = 0; i < 20; i++) { printf(" R%-2d", i); }
	printf("\n");
	for (i = 0; i < 18; i++) {
		printf("P%-2d", i);
		for (j = 0; j < 20; j++) { printf("  %-2d", shmptr->allocation[i][j]); }
		printf("\n");
	}
	if (semop(semid, &v, 1) < 0) { perror("semop v"); }
}

// spawns and schedules children according to multi-level feedback algorithm, keeping track of statistics
int main(int argc, char* argv[]) {
	int randomWait, i, j, numGranted, opt;
	const int PROCMAX = 5;
	const int SHAREABLE_RATIO = rand() % 11 + 15;
	struct msgbuf buf;

	// initialize globals, interrupts, file pointer, and shared memory
	//stats = (struct statistics){ { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, 0 };
	currentChildren = totalProcs = numGranted = vFlag = 0;
	lastPID = -1;
	setupInterrupts();
	createMemory();
	setupFile();
	srand(time(0));

	// parses command line arguments
    while ((opt = getopt(argc, argv, "v")) != -1) {
        if (opt == 'v') { vFlag = 1; }
	}

	for (i = 0; i < 20; i++) {
		shmptr->descriptors[i] = (struct resourceDescriptor) { nonshareable, rand() % 10 + 1, createQueue() };
		if (rand() % 100 < SHAREABLE_RATIO) { shmptr->descriptors[i].rClass = shareable; }
		shmptr->available[i] = shmptr->descriptors[i].instances;
	}

	for (i = 0; i < 18; i++) {
		for (j = 0; j < 20; j++) {
			shmptr->allocation[i][j] = shmptr->maximum[i][j] = shmptr->need[i][j] = 0;
		}
	}

	// initialize PIDmap and currentTime
	for (i = 0; i < 18; i++) { shmptr->PIDmap[i] = 0; }
	shmptr->currentTime.sec = shmptr->currentTime.ns = 0;

	// runs OSS until 40 processes have been spawned, and then until all children have terminated
	while (totalProcs < PROCMAX || currentChildren > 0) {
		randomWait = rand() % (BILLION / 2 - 1000) + 1000;

		// decrements the semaphore
		if (semop(semid, &p, 1) < 0) { perror("semop p"); }
		// updates shared clock
		shmptr->currentTime = addTime(shmptr->currentTime, 0, randomWait);
		// increments the semaphore
    	if (semop(semid, &v, 1) < 0) { perror("semop v"); }

		// spawns new process if process table isn't full and PROCMAX hasn't been reached
		if (currentChildren < 18 && totalProcs < PROCMAX) { spawnChildProc(); }

		if (msgrcv(msqid, &buf, sizeof(struct msgbuf), 20, IPC_NOWAIT) >= 0) {
			 //perror("user_proc: Error");
			if (buf.act == terminate) { terminateProc(buf.pid); }
			else if (buf.act == release) {
				if (semop(semid, &p, 1) < 0) { perror("semop p"); }
				printf("Receiving that P%d is releasing %d instance(s) of R%d\n", buf.pid, buf.instances, buf.resource);
				shmptr->allocation[buf.pid][buf.resource] -= buf.instances;
				shmptr->need[buf.pid][buf.resource] += buf.instances;
				shmptr->available[buf.resource] += buf.instances;
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }
				
				buf.type = buf.pid + 1;
				buf.pid = 20;
				buf.act = confirm;
				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }
			}
			else if (buf.act == request) {
				if (semop(semid, &p, 1) < 0) { perror("semop p"); }
				printf("Receiving that P%d is requesting %d instance(s) of R%d\n", buf.pid, buf.instances, buf.resource);
				shmptr->allocation[buf.pid][buf.resource] += buf.instances;
				shmptr->need[buf.pid][buf.resource] -= buf.instances;
				shmptr->available[buf.resource] -= buf.instances;
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }

				buf.type = buf.pid + 1;
				buf.pid = 20;
				buf.act = confirm;
				numGranted++;
				if (numGranted >= 5) {
					displayAllocationTable();
					numGranted = 0;
				}
				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }
			}
		}
	}

	// finish up
	printf("OSS: 40 processes have been spawned and run to completion, now terminating OSS.\n");
	terminateOSS();
}