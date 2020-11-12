// Author: Adam Wilson
// Date: 10/2/2020

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
    
	// initialize semaphore to have counter value of 1
	// sem.val = 1;
    // if (semctl(semid, 0, SETVAL, sem) < 0) { perror("semctl"); }
}

// simulates an instance of either a user or real time process spawned by OSS
int main(int argc, char* argv[]) {
	struct msgbuf buf;
	struct mtime waitTil;
	struct mtime terminateOK;
	int i, j, pid, term;
	const int TERMRATIO = 5;
	
	// get pid from execl parameter
	pid = atoi(argv[0]);
	attachToSharedMemory();
	srand(pid * shmptr->currentTime.ns);
	terminateOK = addTime(shmptr->currentTime, 1, 0);

	if (semop(semid, &p, 1) < 0) { perror("semop p"); }
	for (j = 0; j < 20; j++) {
		shmptr->need[pid][j] = shmptr->maximum[pid][j] = rand() % (shmptr->descriptors[j].instances + 1);
	}
	if (semop(semid, &v, 1) < 0) { perror("semop v"); }

	// check to see if process has been scheduled until process terminates
	while (1) {
		buf.type = 20;
		buf.pid = pid;
		
		waitTil = addTime(shmptr->currentTime, 0, rand() % (BILLION / 4));
		while (!compareTimes(shmptr->currentTime, waitTil));

		if (compareTimes(shmptr->currentTime, terminateOK) && rand() % TERMRATIO == TERMRATIO - 1) {
			buf.act = terminate;
			buf.resource = buf.instances = 0;
		    if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }
			break;
		}

		if (rand() % 5 == 4) {
			buf.act = release;
			int classes = 0;
			if (semop(semid, &p, 1) < 0) { perror("semop p"); }
			for (i = 0; i < 20; i++) { if (shmptr->allocation[pid][i] > 0) classes++; }
			// picks random resource class from those that are allocated
			if (classes > 0) {
				int r = rand() % classes + 1;
				j = i = 0;
				while (j < r) {
					if (shmptr->allocation[pid][i] > 0) j++;
					if (j != r) i++;
				}
				//i = rand() % 20;
				buf.resource = i;
				buf.instances = rand() % shmptr->allocation[pid][i] + 1;
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }

				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }

				if (msgrcv(msqid, &buf, sizeof(struct msgbuf), pid + 1, 0) == -1) { perror("user_proc: Error"); }	
				
				if (semop(semid, &p, 1) < 0) { perror("semop p"); }
				if (buf.act == confirm) { printf("user_proc: release by P%d confirmed\n", pid); }
				//updates shared clock, protect critical resource with semphore
				shmptr->currentTime = addTime(shmptr->currentTime, 0, rand() % 10000 + 5000);
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }
			}
			else if (semop(semid, &v, 1) < 0) { perror("semop v"); }
		}

		else {
			buf.act = request;
			int classes = 0;
			if (semop(semid, &p, 1) < 0) { perror("semop p"); }
			for (i = 0; i < 20; i++) { if (shmptr->need[pid][i] > 0) classes++; }
			// picks random resource class from those that are needed
			if (classes > 0) {
				int r = rand() % classes + 1;
				j = i = 0;
				while (j < r) {
					if (shmptr->need[pid][i] > 0) j++;
					if (j != r) i++;
				}
				//i = rand() % 20;
				buf.resource = i;
				buf.instances = rand() % shmptr->need[pid][i] + 1;
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }

				if (msgsnd(msqid, &buf, sizeof(struct msgbuf), 0) == -1) { perror("user_proc: Error"); }
				if (msgrcv(msqid, &buf, sizeof(struct msgbuf), pid + 1, 0) == -1) { perror("user_proc: Error"); }
				
				if (semop(semid, &p, 1) < 0) { perror("semop p"); }
				if (buf.act == confirm) { printf("user_proc: request by P%d granted\n", pid); }
				else if (buf.act == block) { printf("user_proc: request by P%d denied\n", pid);	}
				//updates shared clock, protect critical resource with semphore
				shmptr->currentTime = addTime(shmptr->currentTime, 0, rand() % 5000 + 5000);
				if (semop(semid, &v, 1) < 0) { perror("semop v"); }
			}
			else if (semop(semid, &v, 1) < 0) { perror("semop v"); }
		}
	}

	// detaches shmseg from shared memory
	if (shmdt(shmptr) == -1) {
		perror("user_proc: Error");
		exit(-1);
	}
}
