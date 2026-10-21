#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

struct child_status {
	tid_t tid;
	int exit_status;

	/* 자식의 종료가 끝났을 때 → wait() 중인 부모를 깨운다. */
	struct semaphore exit_sema;

	/* 자식의 복제가 끝났을 때 → fork() 중인 부모를 깨운다. */
	struct semaphore fork_sema;

	/* 복제 성공이면 true, 실패이면 false. */
	bool fork_success;

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
