#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

struct journalheader {
  int n;
  int block[JOURNALSIZE];
};

struct journal {
  struct spinlock lock;
  int start;
  int size;
  int dev;
  struct journalheader jh;
};

struct journal journal;

static void recover_from_journal(void);
void journal_write();

void initjournal(int dev) {
  if (sizeof(struct journalheader) >= BSIZE)
    panic("initjournal: too big journalheader");

  struct superblock sb;
  initlock(&journal.lock, "journal");
  readsb(dev, &sb);
  journal.start = sb.journalstart;
  journal.size = sb.njournal;
  journal.dev = dev;
  recover_from_journal();
};

static void recover_from_journal(void) {
	
};

void journal_write() {
	
};

