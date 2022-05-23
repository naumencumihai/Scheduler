// Naumencu Mihai 336 CA

#include <stdlib.h>
#include <stdio.h>
#include <semaphore.h>
#include <stdbool.h>
#include "_test/so_scheduler.h"

#define MAXTHREADS 1024
#define NEW 0
#define READY 1
#define RUNNING 2
#define WAITING 3
#define TERMINATED 4

// Structure representing a thread
struct so_thread {
	tid_t id;
	u_int8_t state;
	u_int32_t priority;
	u_int32_t event;
	u_int64_t remaining_time;
	so_handler *handler;
	sem_t t_sem;
};

// Structure representing a scheduler
struct so_scheduler {
	struct so_thread *current;
	struct so_thread *threads[MAXTHREADS];
	bool initialized;
	u_int64_t time_quantum;
	u_int64_t events;
	u_int16_t threads_count;
	int queue_dim;
	struct so_thread *queue[MAXTHREADS];
	sem_t finished;
} so_scheduler;

// ----------	Helper functions	----------

// Function for handling error codes
void DIE(int assertion, char* call_description)
{
	do {
		if (assertion) {
			fprintf(stderr, "Error: %s", call_description);
			exit(EXIT_FAILURE);
		}
	} while (0);
}

// Starts a thread
void start_thread(struct so_thread *thread)
{
	(*thread).state = RUNNING;
	(*thread).remaining_time = so_scheduler.time_quantum;

	// Removes from priority queue
	int pos = so_scheduler.queue_dim--;

	so_scheduler.queue[pos] = NULL;

	int ret = sem_post(&(*thread).t_sem);

	DIE(ret < 0, "Semaphore post failed - 01\n");
}

// Adds a thread to the priority queue
void add_thread(struct so_thread *thread)
{
	int pr = 0;

	// Find specified thread's priority
	for (pr = 0; pr < so_scheduler.queue_dim; pr++) {
		if ((*so_scheduler.queue[pr]).priority >= thread->priority)
			break;
	}
	for (int j = so_scheduler.queue_dim; j > pr; j--)
		so_scheduler.queue[j] = so_scheduler.queue[j - 1];

	// Add thread to priority queue
	so_scheduler.queue[pr] = thread;
	(*so_scheduler.queue[pr]).state = READY;
	so_scheduler.queue_dim++;
}

// Updates the scheduler
void update_scheduler(void)
{
	struct so_thread *current = so_scheduler.current;
	struct so_thread *next = so_scheduler.queue[so_scheduler.queue_dim - 1];

	// Stops the scheduler if priority queue is empty
	if (so_scheduler.queue_dim == 0) {
		if ((*current).state == TERMINATED) {
			int ret = sem_post(&so_scheduler.finished);

			DIE(ret < 0, "Semaphore post failed - 02\n");
		}
		int ret = sem_post(&(*current).t_sem);

		DIE(ret < 0, "Semaphore post failed - 03\n");

		return;
	}

	// Starts next thread if current not initialized or waiting/terminated
	if (!current || (*current).state == WAITING || (*current).state == TERMINATED) {
		so_scheduler.current = next;
		start_thread(next);
		return;
	}

	u_int32_t cp = (*current).priority;
	u_int32_t np = (*next).priority;
	u_int64_t rt = (*current).remaining_time;

	// Adds current thread and starts next if next's priority is bigger than current's
	if (cp < np) {
		add_thread(current);
		so_scheduler.current = next;
		start_thread(next);
		return;
	}

	// Round robin
	if (rt <= 0) {
		if (cp == np) {
			add_thread(current);
			so_scheduler.current = next;
			start_thread(next);
			return;
		}
		(*current).remaining_time = so_scheduler.time_quantum;
	}

	int ret = sem_post(&(*current).t_sem);

	DIE(ret < 0, "Semaphore post failed - 04\n");
}

// Frees a thread
void free_thread(struct so_thread *thread)
{
	int ret = sem_destroy(&(*thread).t_sem);

	DIE(ret < 0, "Semaphore destroy failed - 01\n");

	free(thread);
}

// Start routine (used as pthread_create param)
void *start_routine(void *arg)
{
	struct so_thread *thread;
	thread = (struct so_thread *) arg;

	int ret = sem_wait(&(*thread).t_sem);

	DIE(ret < 0, "Semaphore wait failed - 01\n");

	(*thread).handler((*thread).priority);
	(*thread).state = TERMINATED;

	update_scheduler();

	return 0;
}

// Creates new thread
struct so_thread *new_thread(so_handler *func, unsigned int priority)
{
	struct so_thread *thread = calloc(1, sizeof(*thread));

	DIE(!thread, "Allocating memory for thread failed - 01\n");

	// Initialize thread values
	(*thread).id = INVALID_TID;
	(*thread).event = SO_MAX_NUM_EVENTS;
	(*thread).state = NEW;
	(*thread).handler = func;
	(*thread).priority = priority;
	(*thread).remaining_time = so_scheduler.time_quantum;

	// Init thread's semaphore
	int ret = sem_init(&(*thread).t_sem, 0, 0);

	DIE(ret < 0, "Semaphore initialization failed - 01\n");

	// Start thread
	ret = pthread_create(&(*thread).id, NULL, &start_routine, (void *)thread);
	DIE(ret < 0, "Pthread creation failed - 01\n");

	return thread;
}

// -------------------------------------------

int so_init(unsigned int time_quantum, unsigned int io)
{
	// Error checking
	if (so_scheduler.initialized == true || io > SO_MAX_NUM_EVENTS || time_quantum <= 0)
		return -1;

	// Initialize scheduler values
	so_scheduler.initialized = true;
	so_scheduler.current = NULL;
	so_scheduler.time_quantum = time_quantum;
	so_scheduler.events = io;
	so_scheduler.queue_dim = 0;
	so_scheduler.threads_count = 0;

	// Init scheduler's sempahore (it signals when scheduler is finished)
	int ret = sem_init(&so_scheduler.finished, 0, 1);

	DIE(ret < 0, "Semaphore initialization failed - 02\n");

	return 0;
}

tid_t so_fork(so_handler *func, unsigned int priority)
{
	// Error checking
	if (!func || priority > SO_MAX_PRIO)
		return INVALID_TID;

	// Set semaphore to wait, if it is the first thread
	if (!so_scheduler.threads_count) {
		int ret = sem_wait(&so_scheduler.finished);

		DIE(ret < 0, "Semaphore wait failed - 02\n");
	}

	u_int16_t last = so_scheduler.threads_count;

	// Thread to be returned
	struct so_thread *thread_ret = new_thread(func, priority);

	// Add thread to scheduler
	add_thread(thread_ret);
	so_scheduler.threads[last] = thread_ret;
	so_scheduler.threads_count++;

	if (!so_scheduler.current)
		update_scheduler();
	else
		so_exec();

	return thread_ret->id;
}

int so_wait(unsigned int io)
{
	// Error checking
	if (io >= so_scheduler.events || io < 0)
		return -1;

	// Current thread set to waiting
	(*so_scheduler.current).event = io;
	(*so_scheduler.current).state = WAITING;

	so_exec();

	return 0;
}

int so_signal(unsigned int io)
{
	// Error checking
	if (io >= so_scheduler.events || io < 0)
		return -1;

	u_int16_t t_unlocked = 0;
	u_int16_t count = so_scheduler.threads_count;
	struct so_thread *tmp;

	// Wake up threads
	for (int i = 0; i < count; i++) {
		tmp = so_scheduler.threads[i];

		if ((*tmp).event == io && (*tmp).state == WAITING) {
			t_unlocked++;
			(*tmp).state = READY;
			(*tmp).event = SO_MAX_NUM_EVENTS;
			add_thread(tmp);
		}
	}

	so_exec();

	return t_unlocked;
}

void so_exec(void)
{
	struct so_thread *tmp;

	tmp = so_scheduler.current;

	// Consume time
	(*tmp).remaining_time = (*tmp).remaining_time - 1;

	// Update scheduler
	update_scheduler();

	int ret = sem_wait(&(*tmp).t_sem);

	DIE(ret < 0, "Semaphore wait failed - 03\n");
}

void so_end(void)
{
	// Error checking
	if (so_scheduler.initialized == false)
		return;

	int ret = sem_wait(&so_scheduler.finished);

	DIE(ret < 0, "Semaphore wait failed - 04\n");

	u_int16_t t_count = so_scheduler.threads_count;

	// Free threads
	for (int i = 0; i < t_count; i++) {
		ret = pthread_join((*so_scheduler.threads[i]).id, NULL);
		DIE(ret < 0, "Pthread join failed - 01\n");

		free_thread(so_scheduler.threads[i]);
	}

	so_scheduler.initialized = false;

	ret = sem_destroy(&so_scheduler.finished);
	DIE(ret < 0, "Semaphore destroy failed - 02\n");
}

