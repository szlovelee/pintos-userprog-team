#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct initd_arg{
  const char *f_name;
  struct pchild *parent;
};

void init_process(void);
tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (int status);
void process_activate (struct thread *next);

void acquire_filesys(void);
void release_filesys(void);
int open_file(struct file *file);
void close_file(int fd); 
struct file_entry *get_file(int fd);

#endif /* userprog/process.h */
