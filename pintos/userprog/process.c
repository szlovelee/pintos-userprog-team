#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif
#include "threads/malloc.h"
#include "userprog/syscall.h"  /* filesys_lock */

#include <limits.h>

/* 최대 개수가 아니라, 처음 확보할 슬롯 수 */
#define FD_INITIAL_CAPACITY 32

/* 부모가 자식에게 전달할 시작 정보 */
struct initd_args {
	char *file_name;
	struct child_status *status;
};
 
static void process_cleanup (void);
static bool load (char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static bool
process_init (void) {
	struct thread *curr = thread_current ();

	/* 1. 이미 테이블이 있으면 → 기존 내용을 유지한다. */
	if (curr->fd_table != NULL)
		return true;

	/* 2. 최초 배열을 확보하고 모든 칸을 NULL로 만든다. */
	curr->fd_table = calloc (
		FD_INITIAL_CAPACITY, sizeof *curr->fd_table);

	/* 3. 할당 실패면 → false. */
	if (curr->fd_table == NULL)
		return false;

	/* 4. 성공한 뒤에만 현재 용량을 기록한다. */
	curr->fd_capacity = FD_INITIAL_CAPACITY;
	return true;
}

/* FD 테이블의 용량을 두 배로 늘린다. */
static bool
process_fd_grow (void) {
	struct thread *curr = thread_current ();
	size_t old_capacity = curr->fd_capacity;

	/* 1. 초기화되지 않은 테이블이면 → 실패. */
	if (curr->fd_table == NULL)
		return false;

	/* 2. 확장 후 FD 번호가 int 범위를 넘지 않게 한다.
	 * FD는 int이므로, 용량이 INT_MAX + 1이면 마지막 번호가 INT_MAX다. */
	if (old_capacity > ((size_t) INT_MAX + 1) / 2)
		return false;

	size_t new_capacity = old_capacity * 2;

	/* 3. 더 큰 배열을 확보한다.
	 * 실패해도 기존 주소를 잃지 않도록 임시 포인터를 사용한다. */
	struct file **new_table = realloc (
		curr->fd_table,
		new_capacity * sizeof *new_table);

	if (new_table == NULL)
		return false;

	/* 4. 기존 파일 정보는 유지하고, 늘어난 칸만 NULL로 만든다. */
	memset (new_table + old_capacity, 0,
			(new_capacity - old_capacity) * sizeof *new_table);

	/* 5. 성공한 새 주소와 용량을 반영한다. */
	curr->fd_table = new_table;
	curr->fd_capacity = new_capacity;

	return true;
}

/* 열린 파일을 등록하고 FD 반환. 실패하면 -1. */
int
process_add_file (struct file *file) {
	struct thread *curr = thread_current ();

	/* 1. 등록할 파일이나 초기화된 테이블이 없으면 → 실패. */
	if (file == NULL || curr->fd_table == NULL)
		return -1;

	/* 2. 표준 입출력용 0·1을 제외하고 빈칸을 찾는다.
	 * 닫힌 FD의 빈칸도 다시 사용한다. */
	for (size_t fd = 2; fd < curr->fd_capacity; fd++) {
		if (curr->fd_table[fd] == NULL) {
			curr->fd_table[fd] = file;
			return (int) fd;
		}
	}

	/* 3. 기존 배열이 가득 찼으면 → 확장한다. */
	size_t fd = curr->fd_capacity;

	if (!process_fd_grow ())
		return -1;

	/* 4. 새로 늘어난 첫 번째 칸에 등록한다. */
	curr->fd_table[fd] = file;
	return (int) fd;
}

/* FD로 일반 파일을 찾는다. 잘못된 FD이면 NULL. */
struct file *
process_get_file (int fd) {
	struct thread *curr = thread_current ();

	/* 음수·표준 입출력·테이블 범위 밖이면 → 조회하지 않는다. */
	if (fd < 2 || curr->fd_table == NULL ||
			(size_t) fd >= curr->fd_capacity)
		return NULL;

	/* 닫힌 FD라면 이 칸 자체가 NULL이다. */
	return curr->fd_table[fd];
}

/* 일반 파일 하나를 닫고 FD 슬롯을 비운다. */
void
process_close_file (int fd) {
	struct thread *curr = thread_current ();
	struct file *file = process_get_file (fd);

	/* 1. 유효한 파일이 없으면 → 할 일 없음. */
	if (file == NULL)
		return;

	/* 2. 같은 파일을 다시 닫지 않도록 슬롯부터 비운다. */
	curr->fd_table[fd] = NULL;

	/* 3. 파일 시스템 lock을 잡고 파일을 닫는다. */
	lock_acquire (&filesys_lock);
	file_close (file);
	lock_release (&filesys_lock);
}

/* 모든 일반 파일을 닫고 FD 테이블 메모리를 해제한다. */
void
process_close_all_files (void) {
	struct thread *curr = thread_current ();

	/* 1. 테이블이 없으면 → 할 일 없음. */
	if (curr->fd_table == NULL)
		return;

	/* 2. 등록된 일반 파일들을 모두 닫는다. */
	lock_acquire (&filesys_lock);

	for (size_t fd = 2; fd < curr->fd_capacity; fd++) {
		if (curr->fd_table[fd] != NULL) {
			file_close (curr->fd_table[fd]);
			curr->fd_table[fd] = NULL;
		}
	}

	lock_release (&filesys_lock);

	/* 3. 파일을 모두 닫은 뒤 배열 자체를 해제한다. */
	free (curr->fd_table);
	curr->fd_table = NULL;
	curr->fd_capacity = 0;
}

/* 부모와 자식이 공유할 종료 정보를 만든다. */
static struct child_status *
child_status_create (void) {
	struct child_status *status = malloc (sizeof *status);

	/* 메모리를 확보하지 못하면 → 실패. */
	if (status == NULL)
		return NULL;

	/* 실제 PID는 thread_create() 성공 후 설정한다. */
	status->tid = TID_ERROR;

	/* 정상 exit() 호출 없이 종료되면 기본 상태는 -1. */
	status->exit_status = -1;

	/* 아직 종료하지 않았으므로, 기다리는 부모는 대기한다. */
	sema_init (&status->exit_sema, 0);

	/* 부모 몫 하나 + 자식 몫 하나. */
	status->ref_count = 2;
	lock_init (&status->ref_lock);

	return status;
}

/* 종료 정보를 사용하는 쪽 하나가 자신의 참조를 놓는다. */
static void
child_status_release (struct child_status *status) {
	bool should_free;

	/* 1. 연결된 종료 정보가 없으면 → 할 일 없음. */
	if (status == NULL)
		return;

	/* 2. 부모와 자식이 동시에 참조 수를 바꾸지 못하게 잠근다. */
	lock_acquire (&status->ref_lock);

	/* 참조를 가지고 있는 쪽만 이 함수를 호출해야 한다. */
	ASSERT (status->ref_count > 0);

	/* 3. 내 몫의 참조 하나를 줄인다. */
	status->ref_count--;

	/* 4. 마지막 참조였는지 잠금을 가진 상태에서 기억한다. */
	should_free = (status->ref_count == 0);

	lock_release (&status->ref_lock);

	/* 5. 아무도 사용하지 않으면 → 메모리 해제. */
	if (should_free)
		free (status);
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy = NULL;
	struct child_status *status = NULL;
	struct initd_args *args = NULL;
	tid_t tid;

	/* 1. 자식에게 전달할 전체 명령줄을 복사한다. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		goto error;

	strlcpy (fn_copy, file_name, PGSIZE);

	/* 2. 부모와 자식이 공유할 종료 정보를 만든다. */
	status = child_status_create ();
	if (status == NULL)
		goto error;

	/* 3. 전체 명령줄과 종료 정보를 전달용 구조체에 넣는다. */
	args = malloc (sizeof *args);
	if (args == NULL)
		goto error;

	args->file_name = fn_copy;
	args->status = status;

	/* 4. 이름을 분리하기 전에 앞쪽 공백을 건너뛴다. */
	const char *name_start = file_name + strspn (file_name, " ");

	/* 5. 원본 명령줄을 바꾸지 않도록 이름용 복사본을 만든다. */
	char process_name[sizeof thread_current ()->name];
	char *save_ptr;

	strlcpy (process_name, name_start, sizeof process_name);

	/* 6. 첫 번째 단어만 스레드 이름으로 사용한다. */
	char *program_name = strtok_r (process_name, " ", &save_ptr);

	/* 빈 명령줄이면 → 생성하지 않고 정리한다. */
	if (program_name == NULL)
		goto error;

	/* 7. 이름은 실행 파일명만, args에는 전체 명령줄을 전달한다. */
	tid = thread_create (program_name, PRI_DEFAULT, initd, args);
	if (tid == TID_ERROR)
		goto error;

	/* 8. 부모의 자식 목록에 종료 정보를 등록한다.
	 * 자식이 먼저 종료해도 부모 몫의 참조가 있어 유지된다. */
	status->tid = tid;
	list_push_back (&thread_current ()->children, &status->elem);

	/* 성공 후 args와 fn_copy는 자식이 관리한다.
	 * 자식이 args를 이미 해제했을 수 있으므로 다시 접근하지 않는다. */
	return tid;

error:
	/* 자식 생성 전 실패했으므로 확보한 메모리를 모두 정리한다. */
	free (args);
	free (status);

	if (fn_copy != NULL)
		palloc_free_page (fn_copy);

	return TID_ERROR;
}

/* A thread function that launches first user process. */
static void
initd (void *aux) {
	struct initd_args *args = aux;
	char *file_name = args->file_name;

	/* 1. 자식 쪽 연결: 부모가 만든 종료 정보를 내 thread에 연결한다.
	 * 실행 파일을 읽기 전에 연결해야 실패 경로에서도 사용할 수 있다. */
	thread_current ()->child_status = args->status;

	/* 2. 전달용 묶음은 더 이상 필요 없다.
	 * file_name과 child_status 자체는 아직 해제하지 않는다. */
	free (args);

#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	/* 1. FD 테이블 초기화 실패 → 명령줄을 해제하고 종료. */
	if (!process_init ()) {
		palloc_free_page (file_name);
		thread_exit ();
	}
	/* 2. 초기화 성공 → 유저 프로그램 실행. */
	if (process_exec (file_name) < 0)
		thread_exit ();

	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	return thread_create (name,
			PRI_DEFAULT, __do_fork, thread_current ());
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	/* Finally, switch to the newly created process. */
	if (!process_init ())
		goto error;

	/* 복제에 성공했으면 → 자식의 유저 실행 문맥으로 이동. */
	if (succ)
		do_iret (&if_);
error:
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current ();
	struct child_status *status = NULL;

	/* 1. 내 자식 목록에서 전달받은 PID를 찾는다. */
	for (struct list_elem *e = list_begin (&curr->children);
			e != list_end (&curr->children);
			e = list_next (e)) {
		struct child_status *child =
			list_entry (e, struct child_status, elem);

		if (child->tid == child_tid) {
			status = child;
			break;
		}
	}

	/* 2. 내 자식이 아니거나 이미 기다린 자식이면 → -1. */
	if (status == NULL)
		return -1;

	/* 3. 목록에서 제거한다.
	 * 같은 PID로 다시 wait()하면 목록에서 찾지 못하게 한다.
	 * 부모 몫의 참조는 아직 유지하므로 메모리는 살아 있다. */
	list_remove (&status->elem);

	/* 4. 자식이 종료를 알릴 때까지 기다린다.
	 * 자식이 이미 알렸다면 기다리지 않고 바로 진행한다. */
	sema_down (&status->exit_sema);

	/* 5. 자식이 남긴 종료 상태를 지역변수에 복사한다. */
	int exit_status = status->exit_status;

	/* 6. 부모는 이 정보를 다 사용했으므로 자신의 참조를 놓는다.
	 * 여기서 메모리가 해제될 수도 있다. */
	child_status_release (status);

	/* 7. 복사해 둔 종료 상태를 반환한다. */
	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	struct child_status *status = curr->child_status;

	/* 유저 프로세스의 종료 정보를 해제하기 전에 한 번 출력한다.
	 * 종료 정보가 없는 일반 커널 스레드는 출력하지 않는다. */
	if (status != NULL)
		printf ("%s: exit(%d)\n",
				curr->name, status->exit_status);
	
	/* 1. 내가 부모인 경우:
	 * 더 이상 기다리지 않을 자식들의 정보를 목록에서 꺼낸다. */
	while (!list_empty (&curr->children)) {
		struct list_elem *e = list_pop_front (&curr->children);
		struct child_status *child =
			list_entry (e, struct child_status, elem);

		/* 부모 몫만 해제한다.
		 * 자식이 아직 살아 있으면 자식 몫의 참조는 유지된다. */
		child_status_release (child);
	}

	/* 열린 파일과 FD 테이블을 정리한 뒤 주소 공간을 정리한다. */
	process_close_all_files ();
	process_cleanup ();

	/* 3. 자신의 종료 정보와의 연결을 끊는다.
	 * 이후 같은 참조를 다시 해제하지 않도록 한다. */
	curr->child_status = NULL;

	/* 4. 내가 부모와 공유하는 종료 정보가 있으면 처리한다.
	 * 일반 커널 스레드는 이 정보가 없으므로 건너뛴다. */
	if (status != NULL) {
		/* 부모에게 자원 정리가 끝났음을 알린다.
		 * 종료 상태는 이 알림 전에 기록되어 있어야 한다. */
		sema_up (&status->exit_sema);

		/* 자식인 자신도 이제 종료 정보를 사용하지 않는다. */
		child_status_release (status);
	}
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* 명령줄 분리 */
	char *argv[64];
	int argc = 0;
	char *save_ptr;
	char *token;

	token = strtok_r (file_name, " ", &save_ptr);

	while (token != NULL) {
    	/* 배열 범위를 넘지 않도록 검사 */
    	if (argc >= 64)
        	goto done;

    	argv[argc] = token;
    	argc++;

    	/* 다음 단어 가져오기 */
    	token = strtok_r (NULL, " ", &save_ptr);
	}

	/* 빈 문자열이나 공백만 있는 명령줄 */
	if (argc == 0)
	    goto done;

	/* 첫 번째 단어가 실행 파일 이름 */
	/* Open executable file. */
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	/* 각 문자열이 복사된 유저 주소를 기억할 임시 배열 */
	char *arg_addr[64];

	/* 현재 할당된 유저 스택 페이지의 아래쪽 경계 */
	uintptr_t stack_bottom = (uintptr_t) USER_STACK - PGSIZE;

	for (int i = argc - 1; i >= 0; i--) {
		/* 문자열 끝의 '\0'도 포함 */
		size_t len = strlen (argv[i]) + 1;

		/* 스택 페이지 밖으로 넘어가지 않도록 검사 */
		if (len > if_->rsp - stack_bottom)
			goto done;

		/* 문자열 길이만큼 스택 공간 확보 */
		if_->rsp -= len;

		/* 확보한 위치에 문자열 복사 */
		memcpy ((void *) if_->rsp, argv[i], len);

		/* 복사한 문자열의 시작 주소 기록 */
		arg_addr[i] = (char *) if_->rsp;
	}
	/* x86-64에서는 주소 하나가 8바이트 */
	size_t ptr_size = sizeof (uintptr_t);

	/* 1. 앞으로 필요한 스택 공간 검사 */
	size_t padding = if_->rsp % ptr_size;

	/* argc개의 주소 + 마지막 NULL + 가짜 반환 주소 */
	size_t needed = padding + (argc + 2) * ptr_size;

	if (if_->rsp < stack_bottom ||
		needed > if_->rsp - stack_bottom)
		goto done;


	/* 스택을 8바이트 경계로 정렬 */
	if_->rsp -= padding;
	memset ((void *) if_->rsp, 0, padding);


	/* argv[argc]에 해당하는 NULL 포인터 넣기 */
	uintptr_t zero = 0;

	if_->rsp -= ptr_size;
	memcpy ((void *) if_->rsp, &zero, ptr_size);


	/* 문자열 주소들을 역순으로 넣기 */
	for (int i = argc - 1; i >= 0; i--) {
		if_->rsp -= ptr_size;

		/* 문자열이 아니라, arg_addr[i]에 저장된 주소 값을 복사 */
		memcpy ((void *) if_->rsp,
				&arg_addr[i],
				ptr_size);
	}


	/* 완성된 argv 배열의 시작 주소 기억 */
	uintptr_t user_argv = if_->rsp;


	/* 가짜 반환 주소 넣기 */
	if_->rsp -= ptr_size;
	memcpy ((void *) if_->rsp, &zero, ptr_size);

	/* _start(argc, argv)에 전달할 레지스터 설정 */
	if_->R.rdi = argc;
	if_->R.rsi = user_argv;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
