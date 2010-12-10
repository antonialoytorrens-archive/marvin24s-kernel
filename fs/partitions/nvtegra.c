#include "check.h"
#include "nvtegra.h"


typedef struct {
  unsigned id;
  char name[4];
  unsigned type;
  unsigned unk1[2];
  char name2[4];
  unsigned unk2[4];
  unsigned start;
  unsigned unk3;
  unsigned size;
  unsigned unk4[7];
} t_nvtegra_partinfo;


typedef struct {
  unsigned unknown[18];
  t_nvtegra_partinfo partinfo[24];
} t_nvtegra_ptable;




static size_t read_dev_bytes(struct block_device *bdev, unsigned sector, char* buffer, size_t count)
{
  size_t totalreadcount = 0;

  if (!bdev || !buffer)
    return 0;

  while (count) {
    int copied = 512;
    Sector sect;
    unsigned char *data = read_dev_sector(bdev, sector++, &sect);
    if (!data)
      break;
    if (copied > count)
      copied = count;
    memcpy(buffer, data, copied);
    put_dev_sector(sect);
    buffer += copied;
    totalreadcount += copied;
    count -= copied;
  }
  return totalreadcount;
}



int nvtegra_partition(struct parsed_partitions *state)
{
  t_nvtegra_ptable *pt;
  t_nvtegra_partinfo* p;
  int count;
  unsigned sector;

  //int sector_size = bdev_hardsect_size(bdev) / 512;
  printk(KERN_WARNING "gg: nvtegra_partition()\n");
  printk(KERN_WARNING "gg: bdev_hardsect_size() = %d\n", bdev_logical_block_size(state->bdev));

  pt = kzalloc(2048, GFP_KERNEL);
  if (!pt) {
//    printk(KERN_ERR "gg: error kzalloc\n");
    return -1;
  }

  sector = 1536*4-0x1000;
  if (read_dev_bytes(state->bdev, sector, (char*)pt, 2048) != 2048) {
    printk(KERN_WARNING "gg: error read_dev_bytes\n");
    kfree(pt);
    return 0;
  }

  p = pt->partinfo;
  count = 1;

  while ((p->id < 128) && (count<=24)) {
    unsigned id;
    char name[5];
    unsigned type;
    unsigned start;
    unsigned size;

    id = p->id;
    name[0] = p->name[0];
    name[1] = p->name[1];
    name[2] = p->name[2];
    name[3] = p->name[3];
    name[4] = 0;
    start = p->start;
    size = p->size;
    type = p->type;

    printk(KERN_WARNING "gg: nvtegrapart: %d [%s] %d %d %d\n", id, name, type, start, size);

    start = start*4 - 0x1000;
    size = size*4;

    printk(KERN_WARNING "gg: --> put_partition(%d,%d,%d)\n", count, start, size);
    //put_partition(state, count, start, size);

    count++;
    p++;
  }


  kfree(pt);
  return 0; // don't succeed for now...
}


