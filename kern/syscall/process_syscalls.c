/**
 * Author: Rakesh Balasubramanian and Ramya Rao
 * Date: 27th Feb 2015
 * Process system calls
 */

#include <syscall.h>
#include <kern/wait.h>
#include <kern/procsys.h>
#include <thread.h>
#include <current.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <copyinout.h>
#include <lib.h>
#include <mips/trapframe.h>
#include <addrspace.h>
#include <vfs.h>
#include <copyinout.h>

struct pdesc* g_pdtable[PID_LIMIT];
void childfork_func(void * ptr, unsigned long data2);

void
sys__exit(int exitcode)
{

	struct pdesc *pd = g_pdtable[curthread->t_pid];
	pd->exitcode = _MKWAIT_EXIT(exitcode);
	pd->exited = true;
	lock_acquire(pd->wait_lock);
	cv_signal(pd->wait_cv,pd->wait_lock);
	lock_release(pd->wait_lock);
	thread_exit();
}



int
sys_waitpid(int pid, userptr_t status, int options, pid_t *ret_pid)
{
	int err;
	if(((uintptr_t)status % 4) !=0 ){
		return EFAULT; // Not aligned
	}
	if (status == NULL)
	{
		return EFAULT;
	}
	
	if (pid < 0 || pid >= PID_LIMIT)
	{
		return ESRCH;
	}

	if (options != 0 &&
		options != 1 &&
		options != 2 )
	{
		return EINVAL;
	}

	struct pdesc *pd = g_pdtable[pid];
	if (pd == NULL)
	{
		return ESRCH;
	}

	if (pd->ppid != curthread->t_pid)
	{
		return ECHILD; //Not parent
	}

	if (pd->exited == false && options == WNOHANG)
	{
		*ret_pid = 0;
		return 0;
	}

	lock_acquire(pd->wait_lock);
	while (pd->exited == false)
	{
		cv_wait(pd->wait_cv, pd->wait_lock);
	}
	lock_release(pd->wait_lock);


	*ret_pid = pid;
	err = copyout(&pd->exitcode, status, sizeof(int));
	if (err)
	{
		return err;
	}
	pdesc_destroy(pd);
	g_pdtable[pid] = NULL;

	return 0;
}

int
sys_fork(struct trapframe *tf, pid_t *ret_pid)
{
	struct trapframe *child_tf = kmalloc(sizeof(struct trapframe));
	if (child_tf == NULL)
	{
		return ENOMEM;
	}
	memcpy(child_tf,tf,sizeof(struct trapframe));
	int err;

	struct thread *child_thread;
	err = thread_fork("child", childfork_func, child_tf, (vaddr_t)NULL, &child_thread);
	if (err)
	{
		return err;
	}

	*ret_pid = child_thread->t_pid;
	return 0;

}


void childfork_func(void * tf_ptr, unsigned long as)
{

	/************ RB:Need trap frame on stack instead of heap ************/
	struct trapframe tf;
	memcpy(&tf,tf_ptr,sizeof(struct trapframe));
	kfree(tf_ptr);
	/************ RB:Prepare trap fame ************/
	tf.tf_v0 = 0;
	tf.tf_a3 = 0;
	tf.tf_epc += 4;
	(void)as;
	mips_usermode(&tf);
}

int sys_execv(char *program, char **uargs, int *ret)
{
	int err = 0;
	int index = 0;
	char* kbuf[ARG_MAX];
	int arg_length;
	int copylength = 0;
	while(uargs[index] != NULL) {
		arg_length = strlen(uargs[index]);
		int padding = 4-((arg_length+1)%4);		//align by 4 (including \0 at the end)
		char temp_arg[arg_length+padding];
		err = copyin((const_userptr_t) uargs[index],temp_arg, (size_t)arg_length-1);
		if (err) {
			return err;
		}
		kbuf[index] = temp_arg;
		for (int i = arg_length; i < arg_length+padding+1; ++i)
		{
			kbuf[index][i] = '\0';
		}
		copylength += arg_length+padding +1;
		index++;
	}
	copylength += 4*(index+1);
	int argc = index-1;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
	result = vfs_open(program, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new thread. */
	KASSERT(curthread->t_addrspace == NULL);
	// /* Create a new address space. */
	curthread->t_addrspace = as_create();
	if (curthread->t_addrspace==NULL) {
		vfs_close(v);
		return ENOMEM;
	}
	/* Activate it. */
	as_activate(curthread->t_addrspace);
	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* thread_exit destroys curthread->t_addrspace */
		vfs_close(v);
		return result;
	}
	/* Done with the file now. */
	vfs_close(v);
	/* Define the user stack in the address space */
	result = as_define_stack(curthread->t_addrspace, &stackptr);
	if (result) {
		/* thread_exit destroys curthread->t_addrspace */
		return result;
	}
	stackptr -= copylength;
	int prev_offset = 0;
	char* ret_buf[index];
	int l = 0;
	/*********** RR:moving contents from kernel buffer to user stack ***********/
	for (int l = 0; l < index+1; ++l)
	{
		arg_length = strlen(kbuf[l]);
		int padding = 4-((arg_length+1)%4);
		int dest = stackptr+(index+1)*4+prev_offset;
		err = copyout(kbuf[l],(userptr_t)dest,(size_t)arg_length+padding);
		if (err) {
			return err;
		}
		ret_buf[l] = (char *)(stackptr+(index+1+l)*4+prev_offset);
		prev_offset += (arg_length+padding);
	}
	ret_buf[l] = NULL;
	err = copyout(ret_buf,(userptr_t)stackptr, sizeof(ret_buf));
	if (err) {
		return err;
	}

	enter_new_process(argc, (userptr_t)stackptr /*userspace addr of argv*/,
			  stackptr, entrypoint);
	/* enter_new_process does not return. */
	*ret = -1;
	panic("enter_new_process returned\n");
	return EINVAL;
}

void pdesc_destroy(struct pdesc * pd)
{
	cv_destroy(pd->wait_cv);
	lock_destroy(pd->wait_lock);
	kfree(pd);
}
