#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include "opt-A2.h"
#include <synch.h>
#include <mips/trapframe.h>
#include <kern/fcntl.h>
#include <vfs.h>


  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */ 
static void threadForkWrapper(void* ptr, unsigned long num);
static void threadForkWrapper(void* ptr, unsigned long num) {
  (void) num;
  enter_forked_process((struct trapframe*) ptr);
}
// implementation of fork 

pid_t sys_fork(struct trapframe* tf, pid_t* retval) {
  // used for determine the result of thread_fork
  int success;

  // create child process 
  struct proc* child = proc_create_runprogram(curproc->p_name);
  if (child == NULL) {
    return ENOMEM;
  }

  // address space 
  as_copy(curproc_getas(), &child->p_addrspace);
  if (child->p_addrspace == NULL) {
    return ENOMEM;
  }

  // parent-child relationship 
  lock_acquire(procLock);
  allProcess[child->procPID].parentPID = curproc->procPID;
  lock_release(procLock);

  // trap frame 
  struct trapframe* temp = kmalloc(sizeof(struct trapframe));
  if (temp == NULL) {
    proc_destroy(child);
    return ENOMEM;
  }
  *temp = *tf;
  success = thread_fork(curthread->t_name, child, threadForkWrapper, temp, 0); 
  if (success != 0) {
    proc_destroy(child);
    kfree(temp);
    return success;
  }
  *retval = child->procPID; 
  return 0;
}

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  #if OPT_A2
    if (p->procPID < PID_MIN) { return; }
    lock_acquire(procLock);
    allProcess[p->procPID].proc = NULL;
    allProcess[p->procPID].exitCode = exitcode;
    // wake up the parent process if it has one 
    pid_t parent = allProcess[p->procPID].parentPID;
    if (parent != -1 && allProcess[parent].proc != NULL) {
      cv_signal(allProcess[p->procPID].procCV, procLock);
    }
    lock_release(procLock);
  #else
    (void)exitcode;
  #endif
  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval) {
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  #if OPT_A2
    *retval = curproc->procPID;
    return (0);
  #else 
    *retval = 1;
    return(0);
  #endif
}

/* stub handler for waitpid() system call                */

int sys_waitpid(pid_t pid, userptr_t status, int options, pid_t *retval) {
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) { return(EINVAL); }
  /* for now, just pretend the exitstatus is 0 */
  #if OPT_A2
    lock_acquire(procLock);
    if (allProcess[pid].parentPID != curproc->procPID) {
      lock_release(procLock);
      return ESRCH;
    }
    while (allProcess[pid].proc != NULL) {
      cv_wait(allProcess[pid].procCV, procLock);
    }
    exitstatus = _MKWAIT_EXIT(allProcess[pid].exitCode);
    lock_release(procLock);
  #else
    exitstatus = 0;
  #endif

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

int sys_execv(const_userptr_t program, userptr_t args) {
  //(void)program;
  (void)args;
	int result;
  size_t actual; 

  char** argv = NULL;
  char** addressOfArgs = NULL;
  char* pathAndArgv = kmalloc(PATH_MAX + ARG_MAX);
  if (pathAndArgv == NULL) { return ENOMEM; }

  // setting the position of path
  char* path = pathAndArgv;
  // setting the postion of arguments
  char* arg = path + PATH_MAX;
  // setting the last position 
  char* end = path + PATH_MAX + ARG_MAX;

  /* copy arguments into kernel*/
  int argc = 0;
  char* currPos = arg;

  while (true) {
    userptr_t currArg;
    result = copyin(args + argc * sizeof(userptr_t), &currArg, sizeof(userptr_t));
    if (result != 0) {
      kfree(pathAndArgv);
      return EFAULT;
    }
    // reaching NULL pointer 
    if (currArg == NULL) { break; }
    result = copyinstr(currArg, currPos, end - currPos, &actual);
    if (result != 0) {
      kfree(pathAndArgv);
      return EFAULT;
    }
    argc += 1;
    currPos += actual; 
  }
  /* copy the program name*/
  result = copyinstr(program, path, PATH_MAX, &actual);
  if (result != 0) { 
    kfree(pathAndArgv);
    return EFAULT;
  }
  
  char* temp = arg;
  argv = kmalloc(argc * sizeof(char*));
  if (argv == NULL) {
    kfree(pathAndArgv);
    return ENOMEM;
  }

  // store C-style strings into argv 
  for (int i = 0; i < argc; i++) {
    argv[i] = temp;
    int length = strlen(temp) + 1;
    temp += length * sizeof(char);
  }

  struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;

	/* Open the file. */
	result = vfs_open(path, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	//KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
    kfree(pathAndArgv);
    kfree(argv);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	struct addrspace* oldAs = curproc_getas();
	if (oldAs != NULL) { as_destroy(oldAs); }
	
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
    kfree(pathAndArgv);
    kfree(argv);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
    kfree(pathAndArgv);
    kfree(argv);
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	// top of stack
	int totalArgsLength = 0;
	for (int i = 0; i < argc; i++) {
		totalArgsLength += strlen(argv[i]); // Length of argument
		totalArgsLength += 1; // for NULL 
	}

	totalArgsLength += (argc + 1) * sizeof(userptr_t); 
	stackptr -= ROUNDUP(totalArgsLength, 8); // top of stack
	vaddr_t top = stackptr; 
	vaddr_t argStart = top + (argc + 1) * sizeof(userptr_t); // where to start store strings
	
  addressOfArgs = kmalloc((argc + 1) * sizeof(char*));
	if (addressOfArgs == NULL) { 
    kfree(pathAndArgv);
    kfree(argv);
    return ENOMEM; 
  }
	addressOfArgs[argc] = NULL;

	for (int i = 0; i < argc; i++) {
		size_t length = strlen(argv[i]) + 1;
		result = copyoutstr(argv[i], (userptr_t)argStart, length, &actual);
		if (result != 0) { 
      kfree(pathAndArgv);
      kfree(argv);
			kfree(addressOfArgs);
			return EFAULT; 
		}
		addressOfArgs[i] = (char*) argStart;
		argStart += actual + 1;
	} 

	for (int i = 0; i <= argc; i++) {
		result = copyout(&addressOfArgs[i], (userptr_t)stackptr, sizeof(userptr_t));
		if (result != 0) {
      kfree(pathAndArgv);
      kfree(argv);
			kfree(addressOfArgs);
			return EFAULT;
		}
		stackptr += sizeof(userptr_t);
	} 
  kfree(pathAndArgv);
  kfree(argv);
  kfree(addressOfArgs);
	stackptr = top;
	/* Warp to user mode. */ 
	enter_new_process(argc /*argc*/, (userptr_t) top /*userspace addr of argv*/,
			  stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
