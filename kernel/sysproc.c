#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 
sys_mmap(void)
{
  uint64 addr;
  int length,prot,flags,fd;
  if(argaddr(0, &addr) < 0)
    return -1;
  if(argint(1, &length) < 0)
    return -1;
  if(argint(2, &prot) < 0)
    return -1;
  if(argint(3, &flags) < 0)
    return -1;
  if(argint(4, &fd) < 0)
    return -1;
  
  struct proc *p = myproc();
  if(fd>=NOFILE||p->ofile[fd]==0)
    return -1;
  if((prot&PROT_READ)&&!p->ofile[fd]->readable)
    return -1;
  if((flags&MAP_SHARED)&&(prot&PROT_WRITE)&&!p->ofile[fd]->writable)
    return -1;
  // add record
  if(!p->vmasz)
    p->vmasz = 0x40000000;
  addr = p->vmasz;
  p->vmasz = PGROUNDUP((addr + length));

  //struct vmaaera *vma = 0;
  for(int i=0;i<16;i++) {
    if(!p->vmaaera[i].valid) {
      //printf("i %d\n",i);
      p->vmaaera[i].addr = addr;
      p->vmaaera[i].length = length;
      p->vmaaera[i].prot = prot;
      p->vmaaera[i].flag = flags;
      p->vmaaera[i].file = p->ofile[fd];
      filedup(p->vmaaera[i].file);
      //printf("%x\n",(uint64)p->vmaaera[i].file->ip);
      p->vmaaera[i].valid = 1;
     // vma = &p->vmaaera[i];
      break;
    }
  }

  //check vma file
  // ilock(vma->file->ip);
  // char *b = kalloc();
  // readi(vma->file->ip, 0, (uint64)b, 0, 10);
  // printf("%s\n",b);
  // printf("%x\n",vma->file);
  // iunlock(vma->file->ip);
  //printf("addr %x\n",addr);
  return addr;
}

uint64 
sys_munmap(void)
{
  uint64 addr;
  int length;
  if(argaddr(0, &addr)<0)
    return -1;
  if(argint(1, &length)<0)
    return -1;
  
  struct proc *p = myproc();
  struct  vmaaera *vma;

  for(int i=0;i<16;i++) {
    vma = &p->vmaaera[i];
    if(!vma->valid)
      continue;
    if(addr<vma->addr||addr>=vma->addr+vma->length)
      continue;
    for(uint64 j = addr;j<addr+length;j+=PGSIZE) {
      pte_t *pte = walk(p->pagetable, j,0);
      if(pte==0||!(*pte&PTE_V)) 
        continue;
      if (PTE_FLAGS(*pte) == PTE_V)
        continue;
      // if((*pte&PTE_D)&&(vma->flag&MAP_SHARED))
      //   writei(vma->file->ip, 0, PTE2PA(*pte), j-vma->addr, PGSIZE);
      if((*pte&PTE_D)&&(vma->flag&MAP_SHARED)) {
        int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
        int u = 0,n = PGSIZE,r=0;
        struct file *f = vma->file;
        while(u < n){
          int n1 = n - u;
          if(n1 > max)
            n1 = max;

          begin_op();
          ilock(f->ip);
          r = writei(f->ip, 0, PTE2PA(*pte)+u, j-vma->addr+u, n1);
          iunlock(f->ip);
          end_op();

          if(r != n1){
            // error from writei
            break;
          }
          u += r;
        }
      }
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
      *pte = 0;
    }
    if(addr == vma->addr&&length == vma->length) {
      fileclose(vma->file);
      vma->valid = 0;
    } else if(addr == vma->addr) {
      vma->addr = addr + length;
      vma->length -= length;
    } else if(addr+length == vma->addr+vma->length) {
      vma->length = vma->length - length;
    }
  
  }
  return 0;
}
