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
#include <limits.h>
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"

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

struct lock filesys_lock;

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
	lock_init (&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 버퍼 전체를 읽을 수 있으면 true, 문제가 있으면 false. */
static bool
readable_user_buffer (const void *buffer, unsigned size) {
	uintptr_t start = (uintptr_t) buffer;
	uint64_t *pml4 = thread_current ()->pml4;

	/* 읽을 크기가 0이면 → 검사할 필요 없음. */
	if (size == 0)
		return true;

	/* NULL이거나 커널 주소이면 → 실패. */
	if (buffer == NULL || !is_user_vaddr (buffer))
		return false;

	/* 버퍼가 유저 영역을 넘어가거나 페이지 테이블이 없으면 → 실패.
	 * 덧셈 오버플로를 피하려고 남은 공간과 비교한다. */
	if (size > KERN_BASE - start || pml4 == NULL)
		return false;

	uintptr_t last = start + size - 1;

	/* 첫 페이지부터 마지막 페이지까지 하나씩 확인한다. */
	for (uintptr_t page = (uintptr_t) pg_round_down (buffer);
			page <= last; page += PGSIZE) {
		/* 연결된 물리 메모리가 없는 페이지이면 → 실패. */
		if (pml4_get_page (pml4, (const void *) page) == NULL)
			return false;
	}

	/* 모든 페이지를 통과했으면 → 성공. */
	return true;
}

static int
sys_write (int fd, const void *buffer, unsigned size) {
	/* 1. 현재는 표준 출력만 지원한다. */
	if (fd != STDOUT_FILENO)
		return -1;

	/* 2. 반환형 int로 표현할 수 없는 크기이면 → 실패. */
	if (size > INT_MAX)
		return -1;

	/* 3. 출력할 크기가 0이면 → 출력 없이 0. */
	if (size == 0)
		return 0;

	/* 4. 버퍼를 읽을 수 없으면 → 프로세스 오류 종료. */
	if (!readable_user_buffer (buffer, size)) {
		sys_exit (-1);
		return -1;
	}

	/* 5. 검사를 통과했으면 → 출력. */
	putbuf (buffer, size);

	/* 6. 출력한 바이트 수 반환. */
	return (int) size;
}

static void
sys_exit (int status) {
	/* 1. 현재 실행 중인 프로세스를 가져온다. */
	struct thread *curr = thread_current ();

	/* 2. 부모와 공유하는 종료 정보에 종료 상태를 기록한다.
	 * 부모는 wait()에서 이 값을 읽는다. */
	if (curr->child_status != NULL)
		curr->child_status->exit_status = status;

	/* 3. 기존 종료 경로를 실행한다.
	 * thread_exit()이 process_exit()을 호출하므로
	 * 여기서 process_exit()을 따로 호출하지 않는다. */
	thread_exit ();
}

static void
sys_exec (const char *cmd_line) {
	/* 1. 기존 유저 메모리가 사라져도 남아 있을 복사 공간을 만든다. */
	char *copy = palloc_get_page (0);

	if (copy == NULL) {
		sys_exit (-1);
		return;
	}

	size_t i;

	/* 2. 각 문자의 주소를 검사하면서 커널 메모리로 복사한다.
	 * strlen()부터 호출하면 잘못된 주소를 먼저 읽을 수 있다. */
	for (i = 0; i < PGSIZE; i++) {
		uintptr_t address = (uintptr_t) cmd_line + i;

		if (!readable_user_buffer ((const void *) address, 1)) {
			palloc_free_page (copy);
			sys_exit (-1);
			return;
		}

		copy[i] = *(const char *) address;

		if (copy[i] == '\0')
			break;
	}

	/* 3. 한 페이지 안에서 문자열 끝을 찾지 못하면 → 오류 종료. */
	if (i == PGSIZE) {
		palloc_free_page (copy);
		sys_exit (-1);
		return;
	}

	/* 4. 새 프로그램으로 교체한다.
	 * copy는 process_exec()이 해제하므로 여기서 다시 해제하지 않는다.
	 * 성공하면 새 프로그램으로 이동하여 돌아오지 않는다. */
	if (process_exec (copy) < 0)
		sys_exit (-1);
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
