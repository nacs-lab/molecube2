#include "../lib/pulser.h"

#include <chrono>
#include <stdio.h>
#include <iostream>
#include <thread>
#include <vector>

#include <nacs-utils/log.h>
#include <nacs-utils/timer.h>
#include <nacs-seq/zynq/dma.h>

using namespace NaCs::Seq::Zynq;


int main()
{
  auto addr = Molecube::Pulser::address();
  Molecube::Pulser p(addr);

  auto dma_buff = (uint8_t*)p.alloc_buffer(4096);
  auto write_buff = (uint8_t*)malloc(4096);
  auto phy_addr = p.buffer_addr(dma_buff);
  printf("virt: %p, phy: %p\n", dma_buff, (void*)phy_addr);

  size_t buff_sz = 0;
  auto add_inst = [&] (auto inst) {
    constexpr size_t inst_sz = sizeof(inst);
    static_assert(inst_sz == 2 || inst_sz == 4 || inst_sz == 6, "");
    assert(buff_sz + inst_sz <= 4096);
    memcpy(write_buff + buff_sz, &inst, inst_sz);
    buff_sz += inst_sz;
  };

  for (uint8_t addr = 0x2c; addr < 0x34; addr += 2) {
      printf("[%x:%x] = %x\n", addr, addr + 1, p.read_dds1(10, addr));
  }
  for (int i = 0; i < 22; i++) {
      printf("DDS %d START ---\n", i);
      p.dump_dds(std::cout, i);
      printf("DDS %d END \n", i);
  }

  add_inst(DMA::Inst_v0::Wait1(10));
  add_inst(DMA::Inst_v0::TTLSet4(0, 8));
  add_inst(DMA::Inst_v0::Wait1(100));
  add_inst(DMA::Inst_v0::TTLSet4(0, 0));
  add_inst(DMA::Inst_v0::DDSSet32(1, 10, 1, 0x2c >> 1, 0x05503507));
  add_inst(DMA::Inst_v0::Wait1(100));
  add_inst(DMA::Inst_v0::DDSSet16(1, 10, 1, 0x32 >> 1, 408));
  add_inst(DMA::Inst_v0::Wait2(10000));
  add_inst(DMA::Inst_v0::DDSSet16(1, 10, 1, 0x32 >> 1, 49));
  add_inst(DMA::Inst_v0::Wait1(100));
  while (true) {
      auto rem = buff_sz % (16 *  8);
      if (rem == 0)
          break;
      rem = 16 *  8 - rem;
      if (rem > sizeof(DMA::Inst_v0::Wait2)) {
          add_inst(DMA::Inst_v0::Wait2(100));
      }
      else {
          add_inst(DMA::Inst_v0::Wait1(100));
      }
  }
  DMA::print(std::cout, std::span(write_buff, buff_sz), 0);
  auto total_time = DMA::total_time(std::span(write_buff, buff_sz), 0);
  printf("Total time: %" PRId64 "\n", total_time);
  memcpy(dma_buff, write_buff, 4096);
  asm volatile ("dmb st" ::: "memory");
  p.set_dma_ttl_mask(0, 0xf);

  auto status0 = p.dma_status();

  NaCs::Timer timer;

  p.start_dma(phy_addr, uint16_t(buff_sz / (16 * 8)), true);

  while ((p.dma_status() & 0xff) != ((status0 + 1) & 0xff)) {
  }

  auto status1 = p.dma_status();
  // printf("%x -> %x\n", status0, status1);

  while ((p.dma_status() & 0x100)) {
  }
  timer.print();
  p.set_dma_ttl_mask(0, 0);

  auto status2 = p.dma_status();
  printf("%x -> %x\n", status1, status2);

  for (uint8_t addr = 0x2c; addr < 0x34; addr += 2) {
      printf("[%x:%x] = %x\n", addr, addr + 1, p.read_dds1(10, addr));
  }
  for (int i = 0; i < 22; i++) {
      printf("DDS %d START ---\n", i);
      p.dump_dds(std::cout, i);
      printf("DDS %d END \n", i);
  }

  free(write_buff);
  p.free_buffer(write_buff, 4096);

  return 0;
}
