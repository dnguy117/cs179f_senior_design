#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   30 log blocks - 1 checksum, 1 header, 28 free blocks
//
//   1-checksum block, will hold the checksum in the disk
//     for power failures and crashes
//   2-header block, containing block #s for block A, B, C, ...
//   3-block A
//   4-block B
//   5-block C
//   ...
//   30-block ...
// Log appends are synchronous.



// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  uint checksum;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();
void write_checksum();
int check_checksum();

void
initlog(int dev)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  struct superblock sb;
  initlock(&log.lock, "log");
  readsb(dev, &sb);
  log.start = sb.logstart;
  log.size = sb.nlog;
  log.dev = dev;
  log.checksum = 0;
  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1+1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, (log.start+1));
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, (log.start+1));
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

static void
recover_from_log(void)
{
  uint disk_check, checksum;
  int i, j;
  checksum = 0;
  struct buf *buf = bread(log.dev, log.start); // log block
  disk_check = (buf->data[0]) + (buf->data[1]<<8) + (buf->data[2]<<16) + (buf->data[3]<<24); // read checksum block
  
  // calculate checksum from log header and log free blocks
  for ( i = 0 ; i < log.lh.n ; i++ ) {
    struct buf *logblocks = bread(log.dev, log.start+i+1+1); // log block
    for ( j = 0; j < BSIZE ; j++ ) {
	  checksum += ((j+1)*logblocks->data[j]);
	}
	brelse(logblocks);
  }
  brelse(buf);
  checksum %= BSIZE;
  
  
  if (disk_check == checksum) {
	cprintf("boot log checksum match, proceding with log commit. \n");
    read_head();
    install_trans(); // if committed, copy from log to disk
    log.lh.n = 0;
    write_head(); // clear the log
  }
  else {
    cprintf("boot log checksum mismatch, will not commit log.");
  }
  
}

// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > (LOGSIZE-2)){
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)
    panic("log.committing");
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    write_log();     // Write modified blocks from cache to log
    write_checksum(); // Calculate checksum
	if (check_checksum()) {
	  write_head();    // Write header to disk -- the real commit
      install_trans(); // Now install writes to home locations
      log.lh.n = 0;
      write_head();    // Erase the transaction from the log
	}
	else {
	  panic("log checksum has a missmatch");
	}
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache with B_DIRTY.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= (log.size - 1 - 1))
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorbtion
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n)
    log.lh.n++;
  
  b->flags |= B_DIRTY; // prevent eviction
  release(&log.lock);
}

// Reads through allocated log blocks to calculate the checksum
void write_checksum() {
  int i, j;
  uint checksum = 0;
  uint test_checksum1;
  //uint test_checksum2;
  
  for ( i = 0 ; i < log.lh.n ; i++ ) {
    struct buf *logblocks = bread(log.dev, log.start+i+1); // log block
    for ( j = 0; j < BSIZE ; j++ ) {
	  checksum += ((j+1)*logblocks->data[j]);
	}
	brelse(logblocks);
  }
  checksum %= BSIZE; //Minimize size due to issues with integer overflow
  log.checksum = checksum;
    
  //cprintf("log %d , cal %d \n", log.checksum, checksum); 
  cprintf("write_checksum() - log checksum calculated as: %x \n", log.checksum);  
  
  // write checksum into disk for crash and power failure protection
  struct buf *check_block = bread(log.dev, log.start); // check block
  
  check_block->data[0] = (checksum); // write checksum into buffer
  check_block->data[1] = (checksum>>8);
  check_block->data[2] = (checksum>>16);
  check_block->data[3] = (checksum>>24);
  
  bwrite(check_block); // write buffer to disk
  brelse(check_block); // release buffer
  
  // TEST functionality of checksum on disk
  struct buf *test_check = bread(log.dev, log.start);
  test_checksum1 = (test_check->data[0]) + (test_check->data[1]<<8) + (test_check->data[2]<<16) + (test_check->data[3]<<24);
  //test_checksum2 = test_check->data[BSIZE-1];
  brelse(test_check);
  
  cprintf("disk written checksum data: %x \n" , test_checksum1);

}

// Reads through the allocated log blocks to calculate a new_checksum
// Then compares the new one to the current one to verify log integrity
int check_checksum() {
  int i, j;
  int check = 0;
  uint new_checksum = 0;
  
  for ( i = 0 ; i < log.lh.n ; i++ ) {
    struct buf *logblocks = bread(log.dev, log.start+i+1); // log block
    for ( j = 0; j < BSIZE ; j++ ) {
	  new_checksum += ((j+1)*logblocks->data[j]);
	}
	brelse(logblocks);
  }
  new_checksum %= BSIZE; // Minimize size due to issues integer overflow
  // PRINT BOTH CHECKSUMS FOR VERIFICATION
  cprintf("check_checksum() - log checksum: %x \n", log.checksum);
  cprintf("check_checksum() - new checksum: %x \n", new_checksum);
  
  if (log.checksum == new_checksum) {
	cprintf("check_checksum() - checksum validated prior to commit\n");
	check = 1;
  }
  else {
	cprintf("check_checksum() - ERROR: checksum invalid prior to commit\n");
	check = 0;
  }
  
  return check;
}
