#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

struct child_status {
	tid_t tid;
	int exit_status;

	struct semaphore exit_sema;

	int ref_count;
	struct lock ref_lock;

	struct list_elem elem;
};
tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
int process_add_file (struct file *file);
struct file *process_get_file (int fd);
void process_close_file (int fd);
void process_close_all_files (void);

#endif /* userprog/process.h */
