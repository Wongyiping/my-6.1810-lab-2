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

#define NBUCKET 17 //哈希桶大小

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct spinlock bklock[NBUCKET];
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  //struct buf head;
  struct buf bucket[NBUCKET];// 桶的头结点
} bcache;

int hash(uint dev, uint blockno)
{
  return (dev + blockno) % NBUCKET;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.bklock[i], "bcache");
    
    bcache.bucket[i].prev = &bcache.bucket[i];
    bcache.bucket[i].next = &bcache.bucket[i];
  }
  // Create linked list of buffers
  //bcache.head.prev = &bcache.head;
  //bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.bucket[0].next;
    b->prev = &bcache.bucket[0];
    initsleeplock(&b->lock, "buffer");
    bcache.bucket[0].next->prev = b;
    bcache.bucket[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int idx = hash(dev, blockno);
  acquire(&bcache.bklock[idx]);

  // Is the block already cached?
  for(b = bcache.bucket[idx].next; b != &bcache.bucket[idx]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bklock[idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.bucket[idx].next; b != &bcache.bucket[idx]; b = b->next){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.bklock[idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  int newidx = (idx + 1) % NBUCKET;
  while(newidx != idx){
    acquire(&bcache.lock); //获得大锁
    for(b = bcache.bucket[newidx].next; b != &bcache.bucket[newidx]; b = b->next){
      if(b->refcnt == 0) {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        b->next->prev = b->prev;
        b->prev->next = b->next;  // 取出b
        release(&bcache.lock);

        b->next = bcache.bucket[idx].next; // 加入到桶
        b->prev = &bcache.bucket[idx];
        bcache.bucket[idx].next->prev = b;
        bcache.bucket[idx].next = b;
        release(&bcache.bklock[idx]);

        acquiresleep(&b->lock);
        return b;
      }
    }
    newidx = (newidx + 1) % NBUCKET;
    release(&bcache.lock);
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

  int idx = hash(b->dev, b->blockno);

  acquire(&bcache.bklock[idx]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.bucket[idx].next;
    b->prev = &bcache.bucket[idx];
    bcache.bucket[idx].next->prev = b;
    bcache.bucket[idx].next = b;
  }
  
  release(&bcache.bklock[idx]);
}

void
bpin(struct buf *b) {
  int idx = hash(b->dev, b->blockno);
  acquire(&bcache.bklock[idx]);
  b->refcnt++;
  release(&bcache.bklock[idx]);
}

void
bunpin(struct buf *b) {
  int idx = hash(b->dev, b->blockno);
  acquire(&bcache.bklock[idx]);
  b->refcnt--;
  release(&bcache.bklock[idx]);
}


