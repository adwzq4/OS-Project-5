// Author: Adam Wilson
// Date: 11/17/2020

#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h> 
#include <sys/shm.h> 
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include "shared.h"

int msqid, shmid, semid;
union semun sem;
struct sembuf p = { 0, -1, SEM_UNDO };
struct sembuf v = { 0, +1, SEM_UNDO };
struct shmseg* shmptr;

void attachToSharedMemory(){
	key_t shmkey, msqkey, semkey;
	// create shared memory segment the same size as struct shmseg and get its shmid
	shmkey = ftok("oss", 137);
	shmid = shmget(shmkey, sizeof(struct shmseg), 0666 | IPC_CREAT);
	if (shmid == -1) {
		perror("user_proc: Error");
		exit(-1);
	}

	// attach struct pointer to shared memory segment and seed rand() with pid * time
	shmptr = shmat(shmid, (void*)0, 0);
	if (shmptr == (void*)-1) { perror("user_proc: Error"); }

	// attach to same message queue as parent
	msqkey = ftok("oss", 731);
	msqid = msgget(msqkey, 0666 | IPC_CREAT);
	if (msqid == -1) { perror("user_proc: Error"); }

	// create semaphore with specified key
    semkey = ftok("oss", 484);
    semid = semget(semkey, 1, 0666 | IPC_CREAT);
    if (semid < 0) { perror("semget"); }
}

// simulates an instance of either a user or real time process spawned by OSS
int main(int argc, char* argv[]) {
	struct msgbuf buf;
	struct mtime waitTil, terminateOK, initTime, blockTime, waiting = {0, 0};
	int i, j, pid, term;
	const int TERMRATIO = 15;
	
	// get pid from execl parameter and set up shared memory/semaphore/message queue
	pid = atoi(argv[0]);
	attachToSharedMemory();
	srand(pid * shmptr->currentTime.ns);
	initTime = shmptr->currentTime;
	terminateOK = addTime(shmptr->currentTime, 1, 0);

	// initialize the max need for each resource to a random number [0, resource instances], and set need to max
	if (semop(semid, &p, 1) < 0) { perror("semop p"); }
	for (j = 0; j < 20; j++) {
		shmptr->need[pid][j] = shmptr->maximum[pid][j] = rand() % (shmptr->descriptors[j].instances + 1);
	}
	if (semop(semid, &v, 1) < 0) { perror("semop v"); }

	// until the process terminates, it will continue to request or release resources
	while (1) {
		buf.type = 20;
		buf.pid = pid;
		
		// wait between 0-250ms
		waitTil = addTime(shmptr->currentTime, 0, rand() % (BILLION / 4));
		while (!compareTimes(shmptr->currentTime, waitTil));

		// if more than 1 second has passed, then 1/15 of the time the process will terminate
		if (compareTimes(shmptr->currentTime, terminateOK) && rand() % TERMRATIO == TERMRATIO - 1) {
			buf.act = terminate;
			buf.resource = buf.instances = 0;
		    if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }
			break;
		}

		// 1/3 of the time, releases a random resource (if any are allocated to this process)
		if (rand() % 3 == 0) {
			buf.act = release;
			int classes = 0;
			if (semop(semid, &p, 1) < 0) { perror("semop p"); }
			// picks random resource class and random number of instances from those that are allocated
			for (i = 0; i < 20; i++) { if (shmptr->allocation[pid][i] > 0) classes++; }
			if (classes > 0) {
				int r = rand() % classes + 1;
				j = i = 0;
				while (j < r) {
					if (shmptr->allocation[pid][i] > 0) j++;
					if (j != r) i++;
				}
				buf.resource = i;
				buf.instances = rand() % shmptr->allocation[pid][i] + 1;
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }
				
				// sends a message to OSS saying it is releasing x instances of resource i
				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }
				// waits for a message back confirming the release
				if (msgrcv(msqid, &buf, sizeof(struct msgbuf), pid + 1, 0) == -1) { perror("user_proc: Error"); }	
				
				// updates clock
				if (semop(semid, &p, 1) < 0) { perror("semop p"); }
				shmptr->currentTime = addTime(shmptr->currentTime, 0, rand() % 10000 + 5000);
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }
			}
			else if (semop(semid, &v, 1) < 0) { perror("semop v"); }
		}

		// otherwise, process sends a message requesting resources
		else {
			buf.act = request;
			int classesNeeded = 0;
			if (semop(semid, &p, 1) < 0) { perror("semop p"); }
			for (i = 0; i < 20; i++) { if (shmptr->need[pid][i] > 0) classesNeeded++; }
			// picks random resource class and number of instances from those that are needed
			if (classesNeeded > 0) {
				int r = rand() % classesNeeded + 1;
				j = i = 0;
				while (j < r) {
					if (shmptr->need[pid][i] > 0) j++;
					if (j != r) i++;
				}
				buf.resource = i;
				buf.instances = rand() % shmptr->need[pid][i] + 1;
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }
				
				// sends a message saying it is requesting x instances of resource i
				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }
				// waits for a response confirming or blocking the request
				if (msgrcv(msqid, &buf, sizeof(struct msgbuf), pid + 1, 0) == -1) { perror("user_proc: Error"); }
				
				// updates clock
				if (buf.act == confirm) {
					if (semop(semid, &p, 1) < 0) { perror("semop p"); }
					shmptr->currentTime = addTime(shmptr->currentTime, 0, rand() % 5000 + 5000);
					if (semop(semid, &v, 1) < 0) { perror("semop v"); }
				}

				// puts the process into a loop until it receives a message waking it up
				else if (buf.act == block) { 
					if (semop(semid, &p, 1) < 0) { perror("semop p"); }
					shmptr->currentTime = addTime(shmptr->currentTime, 0, rand() % 5000 + 5000);
					if (semop(semid, &v, 1) < 0) { perror("semop v"); }		
					blockTime = shmptr->currentTime;			
					while (buf.act != wake) {
						if (msgrcv(msqid, &buf, sizeof(struct msgbuf), pid + 1, 0) == -1) { perror("user_proc: Error"); }
					}
					blockTime = subtractTime(shmptr->currentTime, blockTime);
					waiting = addTime(waiting, blockTime.sec, blockTime.ns);
				}
			}
			else if (semop(semid, &v, 1) < 0) { perror("semop v"); }
		}
	}

	// updates stats
	if (semop(semid, &p, 1) < 0) { perror("semop p"); }
	initTime = subtractTime(shmptr->currentTime, initTime);
	shmptr->stats.lifeTime = addTime(shmptr->stats.lifeTime, initTime.sec, initTime.ns);
	shmptr->stats.waitTime = addTime(shmptr->stats.waitTime, waiting.sec, waiting.ns);	
	if (semop(semid, &v, 1) < 0) { perror("semop v"); }		

	// detaches shmseg from shared memory
	if (shmdt(shmptr) == -1) {
		perror("user_proc: Error");
		exit(-1);
	}
}
