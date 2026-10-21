#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "threads/mmu.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"       /* power_off() */
#include "userprog/process.h"   /* process_wait(), tid_t */
#include "filesys/filesys.h"
#include "filesys/file.h"

#define SET_SYSCALL_RETURN(frame, value) ((frame)->R.rax = (value))

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static inline void set_syscall_return(struct intr_frame *f, uint64_t value);
static bool is_valid_pointer(void *ptr);
static bool is_valid_string(char *ptr);
static bool is_valid_string_bounded(char *ptr, size_t *size, size_t max_size);


/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

static void sys_halt();
static void sys_exit(struct intr_frame *f);
static void sys_fork(struct intr_frame *f);
static void sys_exec(struct intr_frame *f);
static void sys_wait(struct intr_frame *f);
static void sys_create(struct intr_frame *f);
static void sys_remove(struct intr_frame *f);
static void sys_open(struct intr_frame *f);
static void sys_filesize(struct intr_frame *f);
static void sys_read(struct intr_frame *f);
static void sys_write(struct intr_frame *f);
static void sys_seek(struct intr_frame *f);
static void sys_tell(struct intr_frame *f);
static void sys_close(struct intr_frame *f);

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 시스템 콜 번호를 확인하고 해당 처리 함수를 호출 */
void
syscall_handler (struct intr_frame *f) {
  thread_current()->sys_if = f;
  uint64_t sysnum = f->R.rax;

  switch (sysnum){
  case SYS_HALT:              /* Halt the operating system. */
    sys_halt();
    break;
  case SYS_EXIT:              /* Terminate this process. */
    sys_exit(f);
    break;
  case SYS_FORK:              /* Clone current process. */
    sys_fork(f);
    break;
  case SYS_EXEC:              /* Switch current process. */
    sys_exec(f);
    break;
  case SYS_WAIT:              /* Wait for a child process to die. */
    sys_wait(f);
    break;
  case SYS_CREATE:            /* Create a file. */
    sys_create(f);
    break;
  case SYS_REMOVE:            /* Delete a file. */
    sys_remove(f);
    break;
  case SYS_OPEN:              /* Open a file. */
    sys_open(f);
    break;
  case SYS_FILESIZE:          /* Obtain a file's size. */
    sys_filesize(f);
    break;
  case SYS_READ:              /* Read from a file. */
    sys_read(f);
    break;
  case SYS_WRITE:             /* Write to a file. */
    sys_write(f);
    break;
  case SYS_SEEK:              /* Change position in a file. */
    sys_seek(f);
    break;
  case SYS_TELL:              /* Report current position in a file. */
    sys_tell(f);
    break;
  case SYS_CLOSE:             /* Close a file. */
    sys_close(f);
    break;
  }
}

static void sys_halt(){
  power_off();
}

static void sys_exit(struct intr_frame *f){
  int exit_status = f->R.rdi;
  process_exit(exit_status);
}

static void sys_fork(struct intr_frame *f){
  int pid;
  const char *name = f->R.rdi;
  
  if (!is_valid_string(name)) pid = TID_ERROR;
  else pid = process_fork(name, f);
  
  set_syscall_return(f, pid);
}

static void sys_exec(struct intr_frame *f){
  int success = -1;
  char *f_name = f->R.rdi;
  size_t size;

  if (!is_valid_string_bounded(f_name, &size, PGSIZE)) 
    goto done;
  
  char *fn_copy = palloc_get_page(0);
  if (fn_copy == NULL) 
    goto done;

  strlcpy(fn_copy, f_name, size);
  success = process_exec(fn_copy);
  
done:
  set_syscall_return(f, success);
}

static void sys_wait(struct intr_frame *f){
  int wait_status = process_wait(f->R.rdi);
  set_syscall_return(f, wait_status);
}

static void sys_create(struct intr_frame *f){
  const char *f_name = f->R.rdi;
  unsigned size = f->R.rsi;
  bool success = false;

  size_t name_size;
  if (!is_valid_string(f_name)) 
    process_exit(-1);
  
  acquire_filesys();
  success = filesys_create(f_name, size);
  release_filesys();
  set_syscall_return(f, success);
}

static void sys_remove(struct intr_frame *f){
  const char *f_name = f->R.rdi;
  bool success = false;

  if (!is_valid_string(f_name)) 
    process_exit(-1);
  
  acquire_filesys();
  success = filesys_remove(f_name);
  release_filesys();

  set_syscall_return(f, success);
}

static void sys_open(struct intr_frame *f){
  const char *f_name = f->R.rdi;
  int fd = -1;

  if (!is_valid_string(f_name)) 
    goto done;
  
done:
  set_syscall_return(f, fd);
}

static void sys_filesize(struct intr_frame *f){
  int fd = f->R.rdi;

}

static void sys_read(struct intr_frame *f){
  int fd = f->R.rdi;
  char *buffer = (char *) f->R.rsi;
  size_t size = f->R.rdx;

  if (!is_valid_pointer(buffer)){
    set_syscall_return(f, -1);
    return;
  }

  if (fd == STDIN_FILENO){
    for (size_t i = 0; i < size; i++){
      buffer[i] = input_getc();
    }
    set_syscall_return(f, size);
  }
  else if (fd == STDOUT_FILENO){
    set_syscall_return(f, -1);
  }
  else{

  }
}

static void sys_write(struct intr_frame *f){
  int fd = f->R.rdi;
  char *buffer = (char *) f->R.rsi;
  size_t size = f->R.rdx;

  if (!is_valid_pointer(buffer))
    set_syscall_return(f, -1);

  if (fd == STDIN_FILENO){
    set_syscall_return(f, -1);
  }
  else if (fd == STDOUT_FILENO){
    putbuf(buffer, size);
    set_syscall_return(f, size);
  }
  else{

  }
}

static void sys_seek(struct intr_frame *f){
  int fd = f->R.rdi;
  unsigned position = f->R.rsi;

}

static void sys_tell(struct intr_frame *f){
  int fd = f->R.rdi;
  
}

static void sys_close(struct intr_frame *f){
  int fd = f->R.rdi;

}

static inline void set_syscall_return(struct intr_frame *f, uint64_t value){
  f->R.rax = value;
}

static bool is_valid_pointer(void *ptr){
  if (!is_user_vaddr(ptr)) return false;

  if (pml4_get_page(thread_current()->pml4, ptr) == NULL) return false;

  return true;
}


static bool is_valid_string(char *ptr){
  size_t size = 0;
  size_t max_size = SIZE_MAX;
  return is_valid_string_bounded(ptr, &size, max_size);
}

static bool is_valid_string_bounded(char *ptr, size_t *size, size_t max_size){
  if (!ptr || !size) return false;

  *size = 0;
  char *cur = ptr;

  while(max_size > *size){
    if (!is_valid_pointer((void *)cur))
      return false;

    (*size)++;

    if (*cur == '\0')
      return true;
    
    cur ++;
  }

  return false;
}