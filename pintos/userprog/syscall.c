#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"       /* power_off() */
#include "userprog/process.h"   /* process_wait(), tid_t */

#define SET_SYSCALL_RETURN(frame, value) ((frame)->R.rax = (value))

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static inline void set_syscall_return(struct intr_frame *f, uint64_t value);


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
syscall_handler (struct intr_frame *f UNUSED) {
  thread_current()->sys_if = f;
  uint64_t sysnum = f->R.rax;

  switch (sysnum){
  case SYS_HALT:              /* Halt the operating system. */
    power_off();
    break;
  case SYS_EXIT:              /* Terminate this process. */
    int exit_status = f->R.rdi;
    process_exit(exit_status);
    break;
  case SYS_FORK:              /* Clone current process. */
    int pid = process_fork(f->R.rdi, f);
    set_syscall_return(f, pid);
    break;
  case SYS_EXEC:              /* Switch current process. */
    int success = process_exec(f->R.rdi);
    set_syscall_return(f, success);
    break;
  case SYS_WAIT:              /* Wait for a child process to die. */
    int wait_status = process_wait(f->R.rdi);
    set_syscall_return(f, wait_status);
    break;
  case SYS_CREATE:            /* Create a file. */
    break;
  case SYS_REMOVE:            /* Delete a file. */
    break;
  case SYS_OPEN:              /* Open a file. */
    break;
  case SYS_FILESIZE:          /* Obtain a file's size. */
    break;
  case SYS_READ:              /* Read from a file. */
    break;
  case SYS_WRITE:             /* Write to a file. */
    int fd = f->R.rdi;
    const char *buffer = (const char *) f->R.rsi;
    size_t size = f->R.rdx;

    if (fd == STDOUT_FILENO){
      putbuf(buffer, size);
      set_syscall_return(f, size);
    }
    break;
  case SYS_SEEK:              /* Change position in a file. */
    break;
  case SYS_TELL:              /* Report current position in a file. */
    break;
  case SYS_CLOSE:             /* Close a file. */
    break;
  }
}

static inline void set_syscall_return(struct intr_frame *f, uint64_t value){
  f->R.rax = value;
}