// Author: Adam Wilson
// Date: 11/17/2020

#include <stdlib.h>
#include <limits.h>
#include <stdio.h> 
#include <sys/sem.h>

#define BILLION 1000000000

// resource classes
enum resourceType { shareable, nonshareable };

// process actions
enum action { request, confirm, deny, release, terminate, block, wake };

// declare semaphore union
union semun {
    int val;
    struct semid_ds* buf;
    unsigned short* array;
};

// time struct
struct mtime {
	int sec;
	long int ns;
};

// holds cumulative statistics
struct statistics {
	struct mtime lifeTime;
	struct mtime waitTime;
	int numComplete;
	int requestsGranted;
	int requestsDenied;
	int numAllocated;
};

// holds message contents/info
struct msgbuf {
    long type;
    int pid;
    int resource;
	int instances;
	enum action act;
};

// holds info about resource class
struct resourceDescriptor {
	enum resourceType rType;
	int instances;
	struct Queue* waitQ;
};

// shared memory segment
struct shmseg {
	struct mtime currentTime;
	struct statistics stats;
	struct resourceDescriptor descriptors[20];
	int available[20];
	int maximum[18][20];
	int allocation[18][20];
	int need[18][20];
	int PIDmap[18];
};

// queue holds structs consisting of pid and the quantity of a resource request
struct waitingProc {
	int pid;
	int numRequested;
};

// queue struct to holding waitingProc structs
struct Queue {
    int front, rear, size;
    int capacity;
    struct waitingProc* array;
};

void mWait(int*);
struct mtime addTime(struct mtime, int, long);
struct mtime subtractTime(struct mtime, struct mtime);
int compareTimes(struct mtime, struct mtime);
double timeToDouble(struct mtime);
struct Queue* createQueue();
int isFull(struct Queue*);
int isEmpty(struct Queue*);
void enqueue(struct Queue*, struct waitingProc);
struct waitingProc dequeue(struct Queue*);
int front(struct Queue*);
int rear(struct Queue*);