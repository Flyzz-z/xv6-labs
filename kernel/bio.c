// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
extern uint ticks;

struct entry {
  int key;
  struct buf *buf;
  struct entry *next;
};


struct {
  struct spinlock bklock[NBUCKET];
  struct spinlock lock;
  struct buf buf[NBUF];
  struct entry entrys[NBUF];
  struct entry table[NBUCKET];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  //struct buf head;
} bcache;

void
binit(void)
{

  //initlock(&bcache.lock, "bcache");

  // // Create linked list of buffers，convert array to linklist
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  for(int i=0;i<NBUCKET;i++)
  {
    bcache.table[i].next = 0;
    initlock(&bcache.bklock[i], "bcache.bucket");
  }

  for(int i=0;i<NBUF;i++)
  {
    bcache.entrys[i].buf = &bcache.buf[i];
    initsleeplock(&bcache.buf[i].lock, "buffer");
    bcache.entrys[i].next =  bcache.table[0].next;
    bcache.table[0].next = &bcache.entrys[i];
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b = 0;
  struct entry *e,*en = 0;
  struct spinlock *last_lock = 0;
  uint min_ticks;
  uint id = blockno % NBUCKET;

  //acquire(&bcache.lock);

  // // Is the block already cached?
  // for(b = bcache.head.next; b != &bcache.head; b = b->next){
  //   if(b->dev == dev && b->blockno == blockno){
  //     b->refcnt++;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  // // Not cached.
  // // Recycle the least recently used (LRU) unused buffer.
  // for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  acquire(&bcache.bklock[id]);
  for(e=bcache.table[id].next;e;e = e->next)
  {
    if(e->buf->blockno == blockno && e->buf->dev == dev)
    {
      //printf("get hash blockno %d  dev %d\n",blockno,dev);
      b = e->buf;
      b->refcnt++;
      release(&bcache.bklock[id]);
      acquiresleep(&e->buf->lock);
      return b;
    }
  }

  
  for(int i=0;i<NBUCKET;i++)
  { 
    if(i!=id)
      acquire(&bcache.bklock[i]);
    for(e=bcache.table[i].next;e;e=e->next)
    {
      if(e->buf->refcnt == 0)
      {
        if((en&&min_ticks>e->buf->ticks)||!en)
        {
          min_ticks = e->buf->ticks;
          en = e;
          if(last_lock&&last_lock!=&bcache.bklock[i])
          {
            if(last_lock!=&bcache.bklock[id])
              release(last_lock);
          }
          last_lock = &bcache.bklock[i];
        }
      }
    }
    if(i!=id&&last_lock!=&bcache.bklock[i])
      release(&bcache.bklock[i]);
  }
  //todo 处理last_lock,也可能在同一个桶中
  if(en)
  {
    //printf("blockno %d  dev %d\n",blockno,dev);
    // change bucket
    if(en->key!=id)
    {
      e = &bcache.table[en->key];
      while (e&&e->next != en) {
        e = e->next;
      }
      if(e->next == en)
      {
        e->next = e->next->next;
      }
      en->key = id;
      en->next = bcache.table[id].next;
      bcache.table[id].next = en;
    }

    b = en->buf;
    b->refcnt = 1;
    b->valid = 0;
    b->dev = dev;
    b->blockno = blockno;
    if(last_lock&&last_lock!=&bcache.bklock[id])
      release(last_lock);
    release(&bcache.bklock[id]);
    acquiresleep(&b->lock);
    return b;
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // acquire(&bcache.lock);
  // b->refcnt--;
  // if (b->refcnt == 0) {
  //   // no one is waiting for it.
  //   b->next->prev = b->prev;
  //   b->prev->next = b->next;
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }
  
  // release(&bcache.lock);
  acquire(&bcache.bklock[b->blockno%NBUCKET]);
  b->refcnt--;
  if(b->refcnt == 0)
  {
    b->ticks = ticks;
  }
  release(&bcache.bklock[b->blockno%NBUCKET]);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


