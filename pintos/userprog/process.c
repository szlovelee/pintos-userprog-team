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
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif


static void process_cleanup (void);
static bool load (char *file_name, struct intr_frame *if_);
static void initd (struct initd_arg *arg);
static bool create_pchild(struct thread *current);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();

  current->parent = NULL;
  list_init(&current->children); 
  sema_init(&current->fork_sema, 0);
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

  process_init();
  struct thread *current = thread_current();
  current->fork_success = false;
  current->fork_child = NULL;

  if (!create_pchild(current))
    return TID_ERROR;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		goto fail_clear_pchild;
	strlcpy (fn_copy, file_name, PGSIZE);
  
  char name_buf[16];
  strlcpy (name_buf, file_name, sizeof name_buf);
  char *save_name;
  char *name = strtok_r(name_buf, " ", &save_name);

	/* Create a new thread to execute FILE_NAME. */
  struct initd_arg *arg = malloc(sizeof(struct initd_arg));
  if (arg == NULL)
    goto fail_clear_fn_copy;
  
  arg->f_name = fn_copy;
  arg->parent = current->fork_child;
	tid = thread_create (name, PRI_DEFAULT, initd, (void *)arg);
	if (tid == TID_ERROR)
    goto fail_clear_all;

  current->fork_child->pid = tid;
	return tid;

fail_clear_all:
  free(arg);
fail_clear_fn_copy:
  palloc_free_page(fn_copy);
fail_clear_pchild:
  list_remove(&current->fork_child->elem);
  free(current->fork_child);
  return TID_ERROR;
}

/* A thread function that launches first user process. */
static void
initd (struct initd_arg *arg) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
  const char *f_name = arg->f_name;

	process_init ();
  thread_current()->parent = arg->parent;
  free(arg);

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");

	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
  struct thread *current = thread_current();
  current->fork_success = false;
  current->fork_child = NULL;

  if (!create_pchild(current))
    return TID_ERROR;

  /* Clone current thread to new thread.*/
  tid_t tid = thread_create (name,
			PRI_DEFAULT, __do_fork, thread_current ()); 

  if (tid == TID_ERROR)
    return TID_ERROR;

  /* 복제한 스레드 생성이 완료될 때까지 대기 */
  current->fork_child->pid = tid;
  sema_down(&current->fork_sema);

	return thread_current()->fork_success ? tid : TID_ERROR;
}

static bool 
create_pchild(struct thread *current){
  struct pchild *child = malloc(sizeof (struct pchild));
  if (child == NULL)
    return false;

  child->p_terminated = false;
  child->c_terminated = false;
  sema_init(&child->sema, 0);
  list_push_back(&current->children, &child->elem);
  current->fork_child = child;

  return true;
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

	/* 1. If the parent_page is kernel page, then return immediately. */
  if (is_kernel_vaddr(va))
    return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
  if (parent_page == NULL)
    return false;

	/* 3. Allocate new PAL_USER page for the child and set result to NEWPAGE. */
  newpage = palloc_get_page(PAL_USER);
  if (newpage == NULL)
    return false;

	/* 4. Duplicate parent's page to the new page and
	 *    check whether parent's page is writable or not (set WRITABLE
	 *    according to the result). */
  memcpy(newpage, parent_page, PGSIZE);
  writable = *pte & PTE_W;

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. if fail to insert page, do error handling. */
    palloc_free_page(newpage);
    return false;
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
	struct intr_frame *parent_if = parent->sys_if;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
  if_.R.rax = 0; // fork의 반환값. 자식 스레드에서는 0으로 반환해야 함.

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

	process_init ();
  current->parent = parent->fork_child;

	/* Finally, switch to the newly created process. */
	if (succ)
	{
    parent->fork_success = true;
    sema_up(&parent->fork_sema);
    do_iret (&if_);
  }
error:
  sema_up(&parent->fork_sema);
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
	process_cleanup ();    // 이 이후에 return하면 안 됨. 이 이전에는 return 가능

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
    process_exit(-1);

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
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
  struct thread * current = thread_current();
  struct list_elem *elem = list_begin(&current->children);
  struct pchild *child = NULL;
  
  while (elem != list_end(&current->children)){
    struct pchild *parsed = list_entry(elem, struct pchild, elem);
    if (parsed->pid == child_tid){
      child = parsed;
      break;
    }
    elem = list_next(elem);
  }

  // simple, killed, twice
  if (child == NULL)
    return -1;

  while(!child->c_terminated){
    sema_down(&child->sema);
  }

  int status = child->exit_status;
  // 자식이 이미 종료되었고, 프로세스가 두 번 wait하지 않아야 하므로 기록 제거
  list_remove(&child->elem);
  free(child);

	return status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (int status) {
	struct thread *curr = thread_current ();
  printf ("%s: exit(%d)\n", curr->name, status);
	/* TODO: Your code goes here.
	 * TODO: We recommend you to implement process resource cleanup here. */
  
  /* 내가 누군가의 자식일 때, 부모의 자식 기록을 조작*/
  if (curr->parent){
    struct pchild *parent = curr->parent;
    if (parent->p_terminated){
      /* 부모도 이미 종료된 경우(고아 상태), 엔티티 자체를 삭제 */
      list_remove(&parent->elem);
      free(parent);
    }
    else{
      parent->c_terminated = true;
      parent->exit_status = status;
      sema_up(&parent->sema);
    }
  }

  /* 나의 자식들에 대해 처리 */
  struct list_elem *elem = list_begin(&curr->children);
  while(elem != list_end(&curr->children)){
    struct pchild *child = list_entry(elem, struct pchild, elem);
    
    if (child->c_terminated){
      elem = list_remove(elem);
      free(child);
    }
    else{
      child->p_terminated = true;
      elem = list_next(elem);
    }
  }
	
  process_cleanup ();
  thread_exit();
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

bool is_valid_pointer(void *uptr){
  if (!is_user_vaddr(uptr)) return false;

  if (pml4_get_page(thread_current()->pml4, uptr) == NULL) return false;

  return true;
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
