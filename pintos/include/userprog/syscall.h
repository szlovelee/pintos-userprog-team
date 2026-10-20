#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

/* 파일 시스템 접근을 보호하는 공통 lock */
extern struct lock filesys_lock;

void syscall_init (void);

#endif /* userprog/syscall.h */
