#include "userprog/syscall.h"
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

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 시스템 콜 처리 함수 */
static void sys_exit (int status);
static tid_t sys_fork (const char *name, struct intr_frame *f);
static void sys_exec (const char *cmd_line);

static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);

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
syscall_handler (struct intr_frame *f) {
	/* rax에 담긴 시스템 콜 번호로 분기한다. */
	switch (f->R.rax) {
		case SYS_HALT:
			/* 종료 */
			power_off ();
			break;

		case SYS_EXIT:
			/* rdi: 종료 상태 */
			sys_exit ((int) f->R.rdi);
			break;

		case SYS_FORK:
			/* rdi: 자식 이름
			 * f: 복제할 부모의 유저 실행 문맥 */
			f->R.rax = sys_fork (
				(const char *) f->R.rdi, f);
			break;

		case SYS_EXEC:
			/* rdi: 실행할 명령줄 */
			sys_exec ((const char *) f->R.rdi);
			break;

		case SYS_WAIT:
			/* rdi: 기다릴 자식 PID */
			f->R.rax = process_wait ((tid_t) f->R.rdi);
			break;

		case SYS_CREATE:
			/* rdi: 파일 이름, rsi: 초기 파일 크기 */
			f->R.rax = sys_create (
				(const char *) f->R.rdi,
				(unsigned) f->R.rsi);
			break;

		case SYS_REMOVE:
			/* rdi: 파일 이름 */
			f->R.rax = sys_remove (
				(const char *) f->R.rdi);
			break;

		case SYS_OPEN:
			/* rdi: 파일 이름 */
			f->R.rax = sys_open (
				(const char *) f->R.rdi);
			break;

		case SYS_FILESIZE:
			/* rdi: 파일 디스크립터 */
			f->R.rax = sys_filesize ((int) f->R.rdi);
			break;

		case SYS_READ:
			/* rdi: fd, rsi: 데이터를 받을 버퍼, rdx: 크기 */
			f->R.rax = sys_read (
				(int) f->R.rdi,
				(void *) f->R.rsi,
				(unsigned) f->R.rdx);
			break;

		case SYS_WRITE:
			/* rdi: fd, rsi: 출력할 버퍼, rdx: 크기 */
			f->R.rax = sys_write (
				(int) f->R.rdi,
				(const void *) f->R.rsi,
				(unsigned) f->R.rdx);
			break;

		case SYS_SEEK:
			/* rdi: fd, rsi: 이동할 파일 위치 */
			sys_seek (
				(int) f->R.rdi,
				(unsigned) f->R.rsi);
			break;

		case SYS_TELL:
			/* rdi: fd */
			f->R.rax = sys_tell ((int) f->R.rdi);
			break;

		case SYS_CLOSE:
			/* rdi: fd */
			sys_close ((int) f->R.rdi);
			break;

		default:
			/* 정의되지 않은 번호면 → 오류 종료 */
			sys_exit (-1);
			break;
	}
}
