#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct initd_arg{
  const char *f_name;
  struct pchild *parent;
};

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (int status);
void process_activate (struct thread *next);
bool is_valid_pointer(void *uptr);

#endif /* userprog/process.h */
